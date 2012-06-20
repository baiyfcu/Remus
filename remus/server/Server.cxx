//=============================================================================
//
//  Copyright (c) Kitware, Inc.
//  All rights reserved.
//  See LICENSE.txt for details.
//
//  This software is distributed WITHOUT ANY WARRANTY; without even
//  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
//  PURPOSE.  See the above copyright notice for more information.
//
//=============================================================================

#include <remus/server/Server.h>

#include <boost/uuid/uuid.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <remus/JobDetails.h>
#include <remus/JobResult.h>
#include <remus/JobStatus.h>

#include <remus/common/JobMessage.h>
#include <remus/common/JobResponse.h>

#include <remus/common/meshServerGlobals.h>
#include <remus/common/zmqHelper.h>

#include <remus/server/internal/uuidHelper.h>
#include <remus/server/internal/ActiveJobs.h>
#include <remus/server/internal/JobQueue.h>
#include <remus/server/internal/WorkerPool.h>


namespace remus{
namespace server{

//------------------------------------------------------------------------------
Server::Server():
  Context(1),
  ClientQueries(Context,ZMQ_ROUTER),
  WorkerQueries(Context,ZMQ_ROUTER),
  UUIDGenerator(), //use default random number generator
  QueuedJobs(new remus::server::internal::JobQueue() ),
  WorkerPool(new remus::server::internal::WorkerPool() ),
  ActiveJobs(new remus::server::internal::ActiveJobs () ),
  WorkerFactory()
  {
  //attempts to bind to a tcp socket, with a prefered port number
  //For accurate information on what the socket actually bond to,
  //check the Job Socket Info class
  this->ClientSocketInfo = zmq::bindToTCPSocket(
                             this->ClientQueries,
                             remus::BROKER_CLIENT_PORT);
  this->WorkerSocketInfo = zmq::bindToTCPSocket(
                             this->WorkerQueries,
                             remus::BROKER_WORKER_PORT);

  //give to the worker factory the endpoint information needed to connect
  //to myself. The WorkerSocketInfo host name is not what we want to
  //give to the factory as it might not
  //be the external name we are interested in
  zmq::socketInfo<zmq::proto::tcp> externalInfo("127.0.0.1",
                                                this->WorkerSocketInfo.port());
  this->WorkerFactory.addCommandLineArgument(externalInfo.endpoint());
  }

//------------------------------------------------------------------------------
Server::Server(const remus::server::WorkerFactory& factory):
  Context(1),
  ClientQueries(Context,ZMQ_ROUTER),
  WorkerQueries(Context,ZMQ_ROUTER),
  UUIDGenerator(), //use default random number generator
  QueuedJobs(new remus::server::internal::JobQueue() ),
  WorkerPool(new remus::server::internal::WorkerPool() ),
  ActiveJobs(new remus::server::internal::ActiveJobs () ),
  WorkerFactory(factory)
  {
  //attempts to bind to a tcp socket, with a prefered port number
  //For accurate information on what the socket actually bond to,
  //check the Job Socket Info class
  this->ClientSocketInfo = zmq::bindToTCPSocket(
                             this->ClientQueries,
                             remus::BROKER_CLIENT_PORT);
  this->WorkerSocketInfo = zmq::bindToTCPSocket(
                             this->WorkerQueries,
                             remus::BROKER_WORKER_PORT);

  //give to the worker factory the endpoint information needed to connect
  //to myself. The WorkerSocketInfo host name is not what we want to
  //give to the factory as it might not
  //be the external name we are interested in
  zmq::socketInfo<zmq::proto::tcp> externalInfo("127.0.0.1",
                                                this->WorkerSocketInfo.port());
  this->WorkerFactory.addCommandLineArgument(externalInfo.endpoint());
  }

//------------------------------------------------------------------------------
Server::~Server()
{

}

//------------------------------------------------------------------------------
bool Server::startBrokering()
  {
  zmq::pollitem_t items[2] = {
      { this->ClientQueries,  0, ZMQ_POLLIN, 0 },
      { this->WorkerQueries, 0, ZMQ_POLLIN, 0 } };

  //  Process messages from both sockets
  while (true)
    {
    zmq::poll (&items[0], 2, remus::HEARTBEAT_INTERVAL);
    boost::posix_time::ptime hbTime = boost::posix_time::second_clock::local_time();
    if (items[0].revents & ZMQ_POLLIN)
      {
      //we need to strip the client address from the message
      zmq::socketIdentity clientIdentity = zmq::address_recv(this->ClientQueries);

      //Note the contents of the message isn't valid
      //after the DetermineJobQueryResponse call
      remus::common::JobMessage message(this->ClientQueries);
      this->DetermineJobQueryResponse(clientIdentity,message); //NOTE: this will queue jobs
      }
    if (items[1].revents & ZMQ_POLLIN)
      {
      //a worker is registering
      //we need to strip the worker address from the message
      zmq::socketIdentity workerIdentity = zmq::address_recv(this->WorkerQueries);

      //Note the contents of the message isn't valid
      //after the DetermineWorkerResponse call
      remus::common::JobMessage message(this->WorkerQueries);
      this->DetermineWorkerResponse(workerIdentity,message);

      //refresh all jobs for a given worker with a new expiry time
      this->ActiveJobs->refreshJobs(workerIdentity);

      //refresh the worker if it is actuall in the pool instead of doing a job
      this->WorkerPool->refreshWorker(workerIdentity);
      }

    //mark all jobs whose worker haven't sent a heartbeat in time
    //as a job that failed.
    this->ActiveJobs->markFailedJobs(hbTime);

    //purge all pending workers with jobs that haven't sent a heartbeat
    this->WorkerPool->purgeDeadWorkers(hbTime);

    //see if we have a worker in the pool for the next job in the queue,
    //otherwise as the factory to generat a new worker to handle that job
    this->FindWorkerForQueuedJob();
    }
  return true;
  }

//------------------------------------------------------------------------------
void Server::DetermineJobQueryResponse(const zmq::socketIdentity& clientIdentity,
                                  const remus::common::JobMessage& msg)
{
  //msg.dump(std::cout);
  //server response is the general response message type
  //the client can than convert it to the expected type
  remus::common::JobResponse response(clientIdentity);
  if(!msg.isValid())
    {
    response.setData(remus::INVALID_MSG);
    response.send(this->ClientQueries);
    return; //no need to continue
    }

  //we have a valid job, determine what to do with it
  switch(msg.serviceType())
    {
    case remus::MAKE_MESH:
      response.setData(this->queueJob(msg));
      break;
    case remus::MESH_STATUS:
      response.setData(this->meshStatus(msg));
      break;
    case remus::CAN_MESH:
      response.setData(this->canMesh(msg));
      break;
    case remus::RETRIEVE_MESH:
      response.setData(this->retrieveMesh(msg));
      break;
    default:
      response.setData(remus::INVALID_STATUS);
    }
  response.send(this->ClientQueries);
  return;
}

//------------------------------------------------------------------------------
bool Server::canMesh(const remus::common::JobMessage& msg)
{
  //ToDo: add registration of mesh type
  //how is a generic worker going to register its type? static method?
  return this->WorkerFactory.haveSupport(msg.meshType());
}

//------------------------------------------------------------------------------
std::string Server::meshStatus(const remus::common::JobMessage& msg)
{
  const boost::uuids::uuid id = remus::to_uuid(msg);

  remus::JobStatus js(id,INVALID_STATUS);
  if(this->QueuedJobs->haveUUID(id))
    {
    js.Status = remus::QUEUED;
    }
  else if(this->ActiveJobs->haveUUID(id))
    {
    js = this->ActiveJobs->status(id);
    }
  return remus::to_string(js);
}

//------------------------------------------------------------------------------
std::string Server::queueJob(const remus::common::JobMessage& msg)
{
  if(this->canMesh(msg))
  {
    //generate an UUID
    boost::uuids::uuid jobUUID = this->UUIDGenerator();
    //create a new job to place on the queue
    //This call will invalidate the msg as we are going to move the data
    //to another message to await sending to the worker
    this->QueuedJobs->addJob(jobUUID,msg);
    //return the UUID
    return remus::to_string(jobUUID);
  }
  return remus::INVALID_MSG;
}

//------------------------------------------------------------------------------
std::string Server::retrieveMesh(const remus::common::JobMessage& msg)
{
  //go to the active jobs list and grab the mesh result if it exists
  const boost::uuids::uuid id = remus::to_uuid(msg);

  remus::JobResult result(id);
  if(this->ActiveJobs->haveUUID(id) && this->ActiveJobs->haveResult(id))
    {
    result = this->ActiveJobs->result(id);
    }

  //for now we remove all references from this job being active
  this->ActiveJobs->remove(id);

  return remus::to_string(result);
}

//------------------------------------------------------------------------------
void Server::DetermineWorkerResponse(const zmq::socketIdentity &workerIdentity,
                                     const remus::common::JobMessage& msg)
{
  //we have a valid job, determine what to do with it
  switch(msg.serviceType())
    {
    case remus::CAN_MESH:
      this->WorkerPool->addWorker(workerIdentity,msg.meshType());
      break;
    case remus::MAKE_MESH:
      //the worker will block while it waits for a response.
      if(!this->WorkerPool->haveWorker(workerIdentity))
        {
        this->WorkerPool->addWorker(workerIdentity,msg.meshType());
        }
      this->WorkerPool->readyForWork(workerIdentity);
      break;
    case remus::MESH_STATUS:
      //store the mesh status msg,  no response needed
      this->storeMeshStatus(msg);
      break;
    case remus::RETRIEVE_MESH:
      //we need to store the mesh result, no response needed
      this->storeMesh(msg);
      break;
    default:
      break;
    }
}

//------------------------------------------------------------------------------
void Server::storeMeshStatus(const remus::common::JobMessage& msg)
{
  //the string in the data is actualy a job status object
  remus::JobStatus js = remus::to_JobStatus(msg.data(),msg.dataSize());
  this->ActiveJobs->updateStatus(js);
}

//------------------------------------------------------------------------------
void Server::storeMesh(const remus::common::JobMessage& msg)
{
  remus::JobResult jr = remus::to_JobResult(msg.data(),msg.dataSize());
  this->ActiveJobs->updateResult(jr);
}

//------------------------------------------------------------------------------
void Server::assignJobToWorker(const zmq::socketIdentity &workerIdentity,
                               const remus::JobDetails& job )
{
  this->ActiveJobs->add( workerIdentity, job.JobId );

  remus::common::JobResponse response(workerIdentity);
  response.setData(remus::to_string(job));

  std::cout << "assigning job to worker " <<
               zmq::to_string(workerIdentity) << std::endl;

  response.send(this->WorkerQueries);
}

//see if we have a worker in the pool for the next job in the queue,
//otherwise as the factory to generat a new worker to handle that job
//------------------------------------------------------------------------------
void Server::FindWorkerForQueuedJob()
{
  typedef std::set<remus::MESH_TYPE>::const_iterator it;
  this->WorkerFactory.updateWorkerCount();
  std::set<remus::MESH_TYPE> types;

  //now if we have room in our worker pool for more pending workers create some
  //todo, make sure we ask the worker pool what its limit on number of pending
  //workers is before creating more
  types = this->QueuedJobs->queuedJobTypes();
  for(it type = types.begin(); type != types.end(); ++type)
    {
    //check if we have a waiting worker, if we don't than try
    //ask the factory to create a worker of that type.
    bool workerReady = this->WorkerPool->haveWaitingWorker(*type);
    workerReady = workerReady || this->WorkerFactory.createWorker(*type);
    if(workerReady)
      {
      this->QueuedJobs->workerDispatched(*type);
      }
    }

  //find all the jobs that have been marked as waiting for a worker
  //and ask if we have a worker in the poll that can mesh that job
  types = this->QueuedJobs->waitingForWorkerTypes();
  for(it type = types.begin(); type != types.end(); ++type)
    {
    if(this->WorkerPool->haveWaitingWorker(*type))
      {
      //give this job to that worker
      this->assignJobToWorker(this->WorkerPool->takeWorker(*type),
                              this->QueuedJobs->takeJob(*type));
      }
    }


}

}
}