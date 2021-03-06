/*=========================================================================

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include <remus/client/Client.h>
#include <remus/common/Timer.h>
#include <remus/testing/Testing.h>

#include <remus/common/CompilerInformation.h>

REMUS_THIRDPARTY_PRE_INCLUDE
#include <boost/cstdint.hpp>
REMUS_THIRDPARTY_POST_INCLUDE

#include <vector>
#include <iostream>

int main(int argc, char* argv[])
{
  //create a default server connection
  remus::client::ServerConnection conn;
  if(argc>=2)
    {
    //let the server connection handle parsing the command arguments
    conn = remus::client::make_ServerConnection(std::string(argv[1]));
    }
  //create a client object that will connect to the remus server
  remus::Client c(conn);


  //keep track of the number of queries to do a rough test
  const std::size_t num_submitted_jobs = 18;
  int queries = 0;
  int failed_jobs = 0;

  remus::common::Timer timer;

  remus::common::MeshIOType requestIOType( (remus::meshtypes::Edges()),
                                           (remus::meshtypes::Mesh2D()) );

  bool valid_mesher_found = false;
  remus::proto::JobSubmission sub;

  if(c.canMesh(requestIOType))
    {
    remus::proto::JobRequirementsSet reqSet =
                                  c.retrieveRequirements(requestIOType);

    //for each matching worker make a job submission for it
    for(remus::proto::JobRequirementsSet::const_iterator i=reqSet.begin();
        i != reqSet.end();
        ++i)
      {
      const std::string& name = i->workerName();
      if(name == "BasicWorker")
        {
        valid_mesher_found = true;
        sub = remus::proto::JobSubmission(*i);
        sub["data"] = remus::proto::make_JobContent(
                              remus::testing::AsciiStringGenerator(2097152) );
        }
      }
    }

  //we create a basic job submission for a mesh2d job, with the data contents of "TEST"
  if(valid_mesher_found)
    {
    timer.reset();
    //if we can mesh 2D mesh jobs, we are going to submit 18 jobs to the server
    //for meshing

    std::vector<remus::proto::Job> jobs;
    for(std::size_t i=0; i < num_submitted_jobs; ++i, ++queries)
      {
      //balance job submissions between the different matching
      //workers
      remus::proto::Job job = c.submitJob(sub);
      jobs.push_back(job);
      }

    //get the initial status of all the jobs that we have
    std::vector<remus::proto::JobStatus> js;
    for(std::size_t i=0; i < jobs.size(); ++i, ++queries)
      {
      remus::proto::JobStatus s = c.jobStatus(jobs.at(i));
      std::cout << jobs.at(i).id() << " status is: " << remus::to_string(s.status()) << std::endl;
      js.push_back(s);
      }

    //While we have jobs still running on the server report back
    //each time the status of the job changes
    while(jobs.size() > 0)
      {
      for(std::size_t i=0; i < jobs.size(); ++i, ++queries)
        {
        //update the status with the latest status and compare it too
        //the previously held status
        remus::proto::JobStatus newStatus = c.jobStatus(jobs.at(i));
        remus::proto::JobStatus oldStatus = js.at(i);
        js[i]=newStatus;

        //if the status or progress value has changed report it to the cout stream
        if(newStatus.status() != oldStatus.status() ||
           newStatus.progress() != oldStatus.progress())
          {
          std::cout << "job id " << newStatus.id() << std::endl;
          std::cout << " status of job is: " << remus::to_string(newStatus.status())  << std::endl;
          if(!newStatus.progress().message().empty())
            {
            std::cout << " Progress Msg: " << newStatus.progress().message() << std::endl;
            }
          if(newStatus.progress().value() >= 0)
            {
            std::cout << " Progress Value: " << newStatus.progress().value() << std::endl;
            }
          }

        //when the job has entered any of the finished states we remove it
        //from the jobs we are checking
        if( !newStatus.good() )
          {
          if(newStatus.finished())
            {
            remus::proto::JobResult r = c.retrieveResults(jobs.at(i));
            std::cout << std::string(r.data(),r.dataSize()) << std::endl;
            }
          else
            {
            ++failed_jobs;
            }
          jobs.erase(jobs.begin()+i);
          js.erase(js.begin()+i);

          std::cout << "outstanding jobs are: " << std::endl;
          for(std::size_t j=0; j < jobs.size(); ++j)
            { std::cout << "  " << jobs.at(j).id() << std::endl; }
          }
        }
      }
    std::cout << "Number of jobs submitted: " << num_submitted_jobs << std::endl;
    std::cout << "Number of jobs failed: " << failed_jobs << std::endl;
    std::cout << "We issued " << queries <<  " queries to the server " << std::endl;
    std::cout << "Time to issue all queries " << timer.elapsed() <<  "ms" << std::endl;
    std::cout << "Number of queries per millisecond is " << queries / timer.elapsed() << std::endl;
    }
  else
    {
    std::cout << "server doesn't support 2d meshes of raw triangles" << std::endl;
    }
  return 1;
}
