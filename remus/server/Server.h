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

#ifndef remus_server_Server_h
#define remus_server_Server_h

#include <remus/common/SignalCatcher.h>
#include <remus/proto/zmqSocketIdentity.h>

#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#ifndef _MSC_VER
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <boost/uuid/random_generator.hpp>
#ifndef _MSC_VER
  #pragma GCC diagnostic pop
#endif

#include <remus/server/WorkerFactory.h>
#include <remus/server/ServerPorts.h>

//included for export symbols
#include <remus/server/ServerExports.h>

namespace remus {
  //forward declaration of classes only the implementation needs
  namespace proto {
  class Message;
  }

  namespace worker {
  class Job;
  }
}

namespace remus{
namespace server{
  namespace detail
    {
    //forward declaration of classes only the implementation needs
    class ActiveJobs;
    class JobQueue;
    class SocketMonitor;
    class WorkerPool;
    struct ThreadManagement;
    struct ZmqManagement;
    }

//helper class that allows users to set and get the polling rates for
//a server instance
class REMUSSERVER_EXPORT PollingRates
{
public:
  PollingRates(boost::int64_t min_millisec, boost::int64_t max_mill):
    MinRateMillisec(min_millisec),
    MaxRateMillisec(max_mill)
    {
    }

  const boost::int64_t& minRate() const { return MinRateMillisec; }
  const boost::int64_t& maxRate() const { return MaxRateMillisec; }

private:
  boost::int64_t MinRateMillisec;
  boost::int64_t MaxRateMillisec;
};


//Server is the broker of Remus. It handles accepting client
//connections, worker connections, and manages the life cycle of submitted jobs.
//We inherit from SignalCatcher so that we can properly handle
//segfaults and other abnormal termination conditions
//The Server class doesn't support copy or move semantics
class REMUSSERVER_EXPORT Server : public remus::common::SignalCatcher
{
public:
  friend struct remus::server::detail::ThreadManagement;
  enum SignalHandling {NONE, CAPTURE};
  //construct a new server using the default worker factory
  //and default loopback ports
  Server();

  //construct a new server with a custom factory
  //and the default loopback ports
  explicit Server(const boost::shared_ptr<remus::server::WorkerFactory>& factory);

  //construct a new server using the given loop back ports
  //and the default factory
  explicit Server(const remus::server::ServerPorts& ports);

  //construct a new server using the given loop back ports
  //and the default factory
  Server(const remus::server::ServerPorts& ports,
         const boost::shared_ptr<remus::server::WorkerFactory>& factory);

  //cleanup the server
  virtual ~Server();

  //Modify the polling interval rates for the server. The server uses a
  //a dynamic polling monitor that adjusts the frequency of the polling rate
  //based on the amount of traffic it receives. These controls allow users
  //to determine what the floor and ceiling are on the polling timeout rates.
  //If you are creating a server that requires a short lifespan or
  //needs to be highly responsive, changing these will be required.
  //
  //Note: All rates are in milliseconds
  //Note: will return false if rates are non positive values
  void pollingRates( const remus::server::PollingRates& rates );
  remus::server::PollingRates pollingRates() const;

  //when you call start brokering the server will actually start accepting
  //worker and client requests.
  bool startBrokering(SignalHandling sh = CAPTURE);

  //when you call stop brokering, the server will stop accepting worker
  //and client requests. This will also tell all active workers that we
  //are shutting down, so they themselves will terminate. You can't stop
  //the server and expect 'good' things to happen.
  void stopBrokering();

  //Returns if the server is still brokering client and worker requests
  bool isBrokering() const;

  //Waits until the thread is up and running
  void waitForBrokeringToStart();

  //Waits until brokering finishes
  void waitForBrokeringToFinish();

  //get back the port information that this server bound too. Since multiple
  //remus servers can be running at a single time this is a way for the server
  //to report which port it bound it self too.
  const remus::server::ServerPorts& serverPortInfo() const {return PortInfo;}

  //control how we handle abnormal signals that we catch
  virtual void signalCaught( SignalCatcher::SignalType signal );

protected:
  //The main brokering loop, called by thread
  virtual bool Brokering(SignalHandling sh = CAPTURE);

  //processes all job queries
  void DetermineJobQueryResponse(const zmq::SocketIdentity &clientIdentity,
                                 const remus::proto::Message& msg);

  //These methods are all to do with sending responses to clients
  std::string canMesh(const remus::proto::Message& msg);
  std::string canMeshRequirements(const remus::proto::Message& msg);
  std::string meshRequirements(const remus::proto::Message& msg);
  std::string meshStatus(const remus::proto::Message& msg);
  std::string queueJob(const remus::proto::Message& msg);
  std::string retrieveMesh(const remus::proto::Message& msg);
  std::string terminateJob(const remus::proto::Message& msg);

  //Methods for processing Worker queries
  void DetermineWorkerResponse(const zmq::SocketIdentity &workerIdentity,
                               const remus::proto::Message& msg);

  //These methods are all to do with sending/recving to workers
  void storeMeshStatus(const remus::proto::Message& msg);
  void storeMesh(const remus::proto::Message& msg);
  void assignJobToWorker(const zmq::SocketIdentity &workerIdentity,
                         const remus::worker::Job& job);

  //see if we have a worker in the pool for the next job in the queue,
  //otherwise ask the factory to generate a new worker to handle that job
  //virtual so that people using custom factories can decide the lifespan
  //of workers
  //overriding this will also allow custom servers to change the priority
  //of queued jobs and workers
  virtual void FindWorkerForQueuedJob();

  //terminate all workers that are doing jobs or waiting for jobs
  void TerminateAllWorkers();

private:
  //explicitly state the server doesn't support copy or move semantics
  Server(const Server&);
  void operator=(const Server&);

  remus::server::ServerPorts PortInfo;
  boost::scoped_ptr<detail::ZmqManagement> Zmq;

protected:
  //allow subclasses to override these detail containers
  boost::uuids::random_generator UUIDGenerator;
  boost::scoped_ptr<remus::server::detail::JobQueue> QueuedJobs;
  boost::scoped_ptr<remus::server::detail::SocketMonitor> SocketMonitor;
  boost::scoped_ptr<remus::server::detail::WorkerPool> WorkerPool;
  boost::scoped_ptr<remus::server::detail::ActiveJobs> ActiveJobs;
  boost::scoped_ptr<detail::ThreadManagement> Thread;

  //needs to be a shared_ptr since we can be passed in a WorkerFactory
  boost::shared_ptr<remus::server::WorkerFactory> WorkerFactory;
};

}

typedef remus::server::Server Server;
}


#endif
