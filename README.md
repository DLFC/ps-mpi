# ps-mpi

> code is based on [ps-lite](https://github.com/dmlc/ps-lite). 

### Build

Clone and build

```bash
git clone https://github.com/shenggan/ps-lite
cd ps-lite && make -j4
```

### Run example

```shell
cd tests
./local_mpi.sh
```

在 `./local_mpi.sh` 中修改，可运行三个示例。

在将 `data` 换成 `mpi` 传递后， `test_kv_app` 和 `test_simple_app` 在重复次数比较大的情况下不稳定。（不稳定主要是段错误和 `mpi` 的错）。

```shell
Assertion failed in file src/mpid/ch3/channels/nemesis/src/ch3_progress.c at line 267: !GENERIC_Q_EMPTY (*(&MPIDI_CH3I_shm_sendq))
internal ABORT - process 1
```

### 修改细节

1. 修改 `Makefile` 修改编译器 `g++` 为 `mpic++` 。
2. 修改运行脚本和测试样例，使得其适合 `mpi` 启动。
3. 修改 `postoffice.cc` ，初始化时按 `mpi` 的 `rank` 分配 `role` 。
4. 增加 `message.h` 中结构 `Node` 的成员 `rank_mpi` 记录节点的 `mpi_rank` 。并修改 `meta.proto` 。
5. 修改 `zmq_van.h` 。将 `meta` 和 `data` 传递的方式改为 `mpi` 。

### TODO

1. 现在的代码依然是阻塞式传递，在节点自我传递时，会发生死锁，故现在节点自我传递依然为 `zmq` 。
2. 现在依然使用 `zmq`来给 `receiver` 传递 `sender` 的 `rank`  。
3. 传递的检测，和传递失败的处理。