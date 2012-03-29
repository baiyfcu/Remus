/*=========================================================================

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include <meshserver/broker/Worker.h>

int main ()
{
  meshserver::Worker w(meshserver::MESH2D);
  meshserver::common::JobDetails jd = w.getJob();

  meshserver::common::JobStatus status(jd.JobId,meshserver::IN_PROGRESS);
  for(int i=1; i <= 100; ++i)
    {
    status.setProgress(i);
    w.updatesStatus(status);
    }

  status = meshserver::common::JobStatus(jd.JobId,meshserver::FINISHED);
  w.updateStatus(status);

  w.returnMeshResults("FAKE RESULTS");

  return 1;
}
