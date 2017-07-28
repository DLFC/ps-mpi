#include "ps/ps.h"

int main(int argc, char *argv[]) {

    int comm_sz;
    int my_rank;

    MPI_Init(NULL, NULL);
    MPI_Comm_size(MPI_COMM_WORLD, &comm_sz);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    char env_rank[15];
    sprintf(env_rank, "MY_RANK=%d",my_rank);
    putenv(env_rank);

  ps::Start();
  // do nothing

  std::cout << "ps start!" << std::endl;
  ps::Finalize();
  MPI_Finalize();
  return 0;
}
