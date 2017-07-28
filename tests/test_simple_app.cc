#include "ps/ps.h"
using namespace ps;

int num = 0;

void ReqHandle(const SimpleData& req, SimpleApp* app) {
  CHECK_EQ(req.head, 1);
  CHECK_EQ(req.body, "test");
  app->Response(req);
  ++ num;
}

int main(int argc, char *argv[]) {

    int comm_sz;
    int my_rank;

    MPI_Init(NULL, NULL);
    MPI_Comm_size(MPI_COMM_WORLD, &comm_sz);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    char env_rank[15];
    sprintf(env_rank, "MY_RANK=%d",my_rank);
    putenv(env_rank);

  int n = 10;
  SimpleApp app(0);
  app.set_request_handle(ReqHandle);

  Start();
  if (IsScheduler()) {
    std::vector<int> ts;
    for (int i = 0; i < n; ++i) {
      int recver = kScheduler + kServerGroup + kWorkerGroup;
      ts.push_back(app.Request(1, "test", recver));
    }

    for (int t : ts) {
      app.Wait(t);
    }
    std::cout << num <<std::endl;
  }

  Finalize();
  MPI_Finalize();
  CHECK_EQ(num, n);
  return 0;
}
