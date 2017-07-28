/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_ZMQ_VAN_H_
#define PS_ZMQ_VAN_H_
#include <zmq.h>
#include <stdlib.h>
#include <thread>
#include <string>

#include <mpi.h>
#include <unistd.h>

#include "ps/internal/van.h"
#if _MSC_VER
#define rand_r(x) rand()
#endif

namespace ps {
/**
 * \brief be smart on freeing recved data
 */
inline void FreeData(void *data, void *hint) {
  if (hint == NULL) {
    delete [] static_cast<char*>(data);
  } else {
    delete static_cast<SArray<char>*>(hint);
  }
}

/**
 * \brief ZMQ based implementation
 */
class ZMQ_MPI_Van : public Van {
 public:
  ZMQ_MPI_Van() { }
  virtual ~ZMQ_MPI_Van() { }

 protected:
  void Start() override {
    // start zmq
    context_ = zmq_ctx_new();
    CHECK(context_ != NULL) << "create 0mq context failed";
    zmq_ctx_set(context_, ZMQ_MAX_SOCKETS, 65536);
    // zmq_ctx_set(context_, ZMQ_IO_THREADS, 4);
    Van::Start();
  }

  void Stop() override {
    PS_VLOG(1) << my_node_.ShortDebugString() << " is stopping";
    Van::Stop();
    // close sockets
    int linger = 0;
    int rc = zmq_setsockopt(receiver_, ZMQ_LINGER, &linger, sizeof(linger));
    CHECK(rc == 0 || errno == ETERM);
    CHECK_EQ(zmq_close(receiver_), 0);
    for (auto& it : senders_) {
      int rc = zmq_setsockopt(it.second, ZMQ_LINGER, &linger, sizeof(linger));
      CHECK(rc == 0 || errno == ETERM);
      CHECK_EQ(zmq_close(it.second), 0);
    }
    zmq_ctx_destroy(context_);
  }

  int Bind(const Node& node, int max_retry) override {
    receiver_ = zmq_socket(context_, ZMQ_ROUTER);
    CHECK(receiver_ != NULL)
        << "create receiver socket failed: " << zmq_strerror(errno);
    int local = GetEnv("DMLC_LOCAL", 0);
    std::string addr = local ? "ipc:///tmp/" : "tcp://*:";
    int port = node.port;
    unsigned seed = static_cast<unsigned>(time(NULL)+port);
    for (int i = 0; i < max_retry+1; ++i) {
      auto address = addr + std::to_string(port);
      if (zmq_bind(receiver_, address.c_str()) == 0) break;
      if (i == max_retry) {
        port = -1;
      } else {
        port = 10000 + rand_r(&seed) % 40000;
      }
    }
    return port;
  }

  void Connect(const Node& node) override {
    CHECK_NE(node.id, node.kEmpty);
    CHECK_NE(node.port, node.kEmpty);
    CHECK(node.hostname.size());
    int id = node.id;
    auto it = senders_.find(id);
    if (it != senders_.end()) {
      zmq_close(it->second);
    }

    const char* val = NULL;
    val = CHECK_NOTNULL(Environment::Get()->find("MY_RANK"));
    int mpi_rank_ = atoi(val);
    my_node_.rank_mpi = mpi_rank_;

    // worker doesn't need to connect to the other workers. same for server
    if ((node.role == my_node_.role) &&
        (node.id != my_node_.id)) {
      return;
    }
    void *sender = zmq_socket(context_, ZMQ_DEALER);
    CHECK(sender != NULL)
        << zmq_strerror(errno)
        << ". it often can be solved by \"sudo ulimit -n 65536\""
        << " or edit /etc/security/limits.conf";
    if (my_node_.id != Node::kEmpty) {
      std::string my_id = "ps" + std::to_string(my_node_.id) + "mpi" + std::to_string(my_node_.rank_mpi);
      zmq_setsockopt(sender, ZMQ_IDENTITY, my_id.data(), my_id.size());
    } else {
      std::string my_id = "psmpi" + std::to_string(my_node_.rank_mpi);
      zmq_setsockopt(sender, ZMQ_IDENTITY, my_id.data(), my_id.size());
    }
    // connect
    std::string addr = "tcp://" + node.hostname + ":" + std::to_string(node.port);
    if (GetEnv("DMLC_LOCAL", 0)) {
      addr = "ipc:///tmp/" + std::to_string(node.port);
    }
    if (zmq_connect(sender, addr.c_str()) != 0) {
      LOG(FATAL) <<  "connect to " + addr + " failed: " + zmq_strerror(errno);
    }
    senders_[id] = sender;
    senders_mpi_[id] = node.rank_mpi;
  }

  int SendMsg(const Message& msg) override {
    std::lock_guard<std::mutex> lk(mu_);
    // find the socket
    int id = msg.meta.recver;
    CHECK_NE(id, Meta::kEmpty);
    auto it = senders_.find(id);
    if (it == senders_.end()) {
      LOG(WARNING) << "there is no socket to node " << id;
      return -1;
    }
    void *socket = it->second;
    // find the rank
    auto it_mpi = senders_mpi_.find(id);
    if (it_mpi == senders_mpi_.end()) {
      LOG(WARNING) << "there is no mpi rank to node " << id;
      return -1;
    }
    rank_dis = it_mpi->second;

    int send_bytes = 0;
    int tmp = msg.data.size();

    PS_VLOG(1) << "Send: " << my_node_.rank_mpi << "->" << rank_dis;
    if (my_node_.rank_mpi != rank_dis) {

        // send meta
        int meta_size; char* meta_buf;
        PackMeta(msg.meta, &meta_buf, &meta_size);

        int tag = 0;

        // if (tmp == 0) tag = 0;
        zmq_msg_t meta_msg;
        zmq_msg_init_data(&meta_msg, meta_buf, meta_size, FreeData, NULL);
        while (true) {
          if (zmq_msg_send(&meta_msg, socket, tag) == meta_size) break;
          if (errno == EINTR) continue;
          LOG(WARNING) << "failed to send message to node [" << id
                       << "] errno: " << errno << " " << zmq_strerror(errno);
          return -1;
        }
        zmq_msg_close(&meta_msg);

        int meta_size_mpi; char* meta_buf_mpi;
        PackMeta(msg.meta, &meta_buf_mpi, &meta_size_mpi);

        send_bytes = meta_size_mpi;

        // PS_VLOG(1) << "Send: " << my_node_.rank_mpi << "->" << rank_dis << " ---- " << strlen(meta_buf_mpi) << "???" << meta_size_mpi;
        MPI_Send(&meta_size_mpi, 1, MPI_INT, rank_dis, 0, MPI_COMM_WORLD);
        MPI_Send(meta_buf_mpi, meta_size_mpi, MPI_CHAR, rank_dis, 1, MPI_COMM_WORLD);
        // PS_VLOG(1) << "Send: " << my_node_.rank_mpi << "->" << rank_dis << "  --DONE--";

        MPI_Send(&tmp, 1, MPI_INT, rank_dis, 2, MPI_COMM_WORLD);


        // PS_VLOG(1) << "[SEND] tmp= " << tmp;
        for (int i = 0; i < tmp; ++i) {
            // zmq_msg_t data_msg;

            SArray<char>* data = new SArray<char>(msg.data[i]);
            int data_size = data->size();
            send_bytes += data_size;
            //if (i==1) PS_VLOG(1) << "[SEND DATA]" << SArray<float>(*data);
            // if (my_node_.rank_mpi != rank_dis){
            PS_VLOG(1) << "DATA Send: " << my_node_.rank_mpi << "->" << rank_dis;
            MPI_Send(&data_size, 1, MPI_INT, rank_dis, 3, MPI_COMM_WORLD);
            MPI_Send(data->data(), data_size, MPI_CHAR, rank_dis, 4, MPI_COMM_WORLD);
            PS_VLOG(1) << "DATA Send: " << my_node_.rank_mpi << "->" << rank_dis << "  --DONE--";
        }
    } else {

        // send meta
        int meta_size; char* meta_buf;
        PackMeta(msg.meta, &meta_buf, &meta_size);
        send_bytes = meta_size;
        int tag = ZMQ_SNDMORE;

        if (tmp == 0) tag = 0;
        zmq_msg_t meta_msg;
        zmq_msg_init_data(&meta_msg, meta_buf, meta_size, FreeData, NULL);
        while (true) {
          if (zmq_msg_send(&meta_msg, socket, tag) == meta_size) break;
          if (errno == EINTR) continue;
          LOG(WARNING) << "failed to send message to node [" << id
                       << "] errno: " << errno << " " << zmq_strerror(errno);
          return -1;
        }
        zmq_msg_close(&meta_msg);

        int n = tmp;
        for (int i = 0; i < n; ++i) {
          zmq_msg_t data_msg;
          SArray<char>* data = new SArray<char>(msg.data[i]);
          int data_size = data->size();
          zmq_msg_init_data(&data_msg, data->data(), data->size(), FreeData, data);
          int tag = ZMQ_SNDMORE;
          if (i == n - 1) tag = 0;
          while (true) {
            if (zmq_msg_send(&data_msg, socket, tag) == data_size) break;
            if (errno == EINTR) continue;
            LOG(WARNING) << "failed to send message to node [" << id
                         << "] errno: " << errno << " " << zmq_strerror(errno)
                         << ". " << i << "/" << n;
            return -1;
          }
          zmq_msg_close(&data_msg);
          send_bytes += data_size;
      }

    }
    return send_bytes;
  }

  int RecvMsg(Message* msg) override {
    msg->data.clear();
    size_t recv_bytes = 0;

    int meta_size;
    char meta_buf[500];
    // if (my_node_.rank_mpi == 2) PS_VLOG(1) << "Hello111111111111111111111";
    zmq_msg_t* zmsg = new zmq_msg_t;
    CHECK(zmq_msg_init(zmsg) == 0) << zmq_strerror(errno);

    while (true) {
        if (zmq_msg_recv(zmsg, receiver_, 0) != -1) break;
        if (errno == EINTR) continue;
        LOG(WARNING) << "failed to receive message. errno: "
                     << errno << " " << zmq_strerror(errno);
        return -1;
    }

    // if (my_node_.rank_mpi == 2) PS_VLOG(1) << "Hello22222222222222222222222222";

      char* buf = CHECK_NOTNULL((char *)zmq_msg_data(zmsg));
      size_t size = zmq_msg_size(zmsg);
      recv_bytes += size;


        // identify
        msg->meta.sender = GetNodeID(buf, size);
        msg->meta.recver = my_node_.id;
        CHECK(zmq_msg_more(zmsg));
        zmq_msg_close(zmsg);
        delete zmsg;

        PS_VLOG(1) << "Recv: " << my_node_.rank_mpi << "<-" << receiver_mpi_;
        // task
        if (my_node_.rank_mpi == receiver_mpi_) {
            for (int i=0;;i++) {
                zmq_msg_t* zmsg = new zmq_msg_t;
                CHECK(zmq_msg_init(zmsg) == 0) << zmq_strerror(errno);
                while (true) {
                    if (zmq_msg_recv(zmsg, receiver_, 0) != -1) break;
                    if (errno == EINTR) continue;
                    LOG(WARNING) << "failed to receive message. errno: "
                                 << errno << " " << zmq_strerror(errno);
                    return -1;
                }

                  char* buf = CHECK_NOTNULL((char *)zmq_msg_data(zmsg));
                  size_t size = zmq_msg_size(zmsg);
                  recv_bytes += size;
                  if (i==0) {
                      UnpackMeta(buf, size, &(msg->meta));

                      zmq_msg_close(zmsg);
                      bool more = zmq_msg_more(zmsg);
                      delete zmsg;
                      if (!more) break;
                  } else {
                      // zero-copy
                      SArray<char> data;
                      data.reset(buf, size, [zmsg, size](char* buf) {
                          zmq_msg_close(zmsg);
                          delete zmsg;
                      });
                      msg->data.push_back(data);
                      if (!zmq_msg_more(zmsg)) { break; }
                }
            }
        } else {

            zmq_msg_t* zmsg = new zmq_msg_t;
            CHECK(zmq_msg_init(zmsg) == 0) << zmq_strerror(errno);
            while (true) {
                if (zmq_msg_recv(zmsg, receiver_, 0) != -1) break;
                if (errno == EINTR) continue;
                LOG(WARNING) << "failed to receive message. errno: "
                             << errno << " " << zmq_strerror(errno);
                return -1;
            }

            //char* buf = CHECK_NOTNULL((char *)zmq_msg_data(zmsg));
            size_t size = zmq_msg_size(zmsg);
            recv_bytes += size;

            // UnpackMeta(buf, size, &(msg->meta));

            zmq_msg_close(zmsg);
            // bool more = zmq_msg_more(zmsg);
            delete zmsg;

            // PS_VLOG(1) << "Recv: " << my_node_.rank_mpi << "<-" << receiver_mpi_ << " ---- " << strlen(meta_buf) << "???" << meta_size;
            MPI_Recv(&meta_size, 1, MPI_INT, receiver_mpi_, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Recv(meta_buf, meta_size+1000, MPI_CHAR, receiver_mpi_, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // PS_VLOG(1) << "Recv: " << my_node_.rank_mpi << "<-" << receiver_mpi_ << "  --DONE--";

            recv_bytes += meta_size;
            UnpackMeta(meta_buf, meta_size, &(msg->meta));

            int data_n = 0;
            MPI_Recv(&data_n, 1, MPI_INT, receiver_mpi_, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for (int i = 0; i < data_n; i++){
                // zero-copy
                SArray<char> data;

                int data_size;
                char *data_buf;

                PS_VLOG(1) << "DATA Recv: " << my_node_.rank_mpi << "<-" << receiver_mpi_;
                MPI_Recv(&data_size, 1, MPI_INT, receiver_mpi_, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                data_buf = new char[data_size*8+100];
                MPI_Recv(data_buf, 200000, MPI_CHAR, receiver_mpi_, 4, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                PS_VLOG(1) << "DATA Recv: " << my_node_.rank_mpi << "<-" << receiver_mpi_ << "  --DONE--";
                SArray<char> data2(data_buf, data_size);
                data.reset(data_buf, data_size, [](char* data_buf) {
                    //zmq_msg_close(zmsg);
                    //delete zmsg;
                });
                //if (i==0) PS_VLOG(1) << "[RECV DATA]" << SArray<uint64_t>(data);
                // if (i==1) PS_VLOG(1) << "[RECV DATA]" << SArray<float>(data);
                msg->data.push_back(data2);
                // if (msg->data.size()>0) PS_VLOG(1) << "[RECV DATA]" << SArray<uint64_t>(msg->data[0]);
            }
        }


    return recv_bytes;
  }

 private:
  /**
   * return the node id given the received identity
   * \return -1 if not find
   */
  int GetNodeID(const char* buf, size_t size) {
    if (size > 2 && buf[0] == 'p' && buf[1] == 's') {
      int id = 0;
      receiver_mpi_ = 0;
      size_t i = 2;
      for (; i < size; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9') {
          id = id * 10 + buf[i] - '0';
        } else {
          break;
        }
      }
      i += 3;
      for (; i < size; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9') {
          receiver_mpi_ = receiver_mpi_ * 10 + buf[i] - '0';
        } else {
          break;
        }
      }
      if (id == 0) return Meta::kEmpty;
      if (i == size) return id;
    }
    return Meta::kEmpty;
  }

  void *context_ = nullptr;
  /**
   * \brief node_id to the socket for sending data to this node
   */
  std::unordered_map<int, void*> senders_;
  std::mutex mu_;
  void *receiver_ = nullptr;
  // add mpi
  int receiver_mpi_;
  int rank_dis;
  std::unordered_map<int, int> senders_mpi_;
  MPI_Request handle1,handle2;
  MPI_Status status1,status2;
};
}  // namespace ps

#endif  // PS_ZMQ_VAN_H_





// monitors the liveness other nodes if this is
// a schedule node, or monitors the liveness of the scheduler otherwise
// aliveness monitor
// CHECK(!zmq_socket_monitor(
//     senders_[kScheduler], "inproc://monitor", ZMQ_EVENT_ALL));
// monitor_thread_ = std::unique_ptr<std::thread>(
//     new std::thread(&Van::Monitoring, this));
// monitor_thread_->detach();

// void Van::Monitoring() {
//   void *s = CHECK_NOTNULL(zmq_socket(context_, ZMQ_PAIR));
//   CHECK(!zmq_connect(s, "inproc://monitor"));
//   while (true) {
//     //  First frame in message contains event number and value
//     zmq_msg_t msg;
//     zmq_msg_init(&msg);
//     if (zmq_msg_recv(&msg, s, 0) == -1) {
//       if (errno == EINTR) continue;
//       break;
//     }
//     uint8_t *data = static_cast<uint8_t*>(zmq_msg_data(&msg));
//     int event = *reinterpret_cast<uint16_t*>(data);
//     // int value = *(uint32_t *)(data + 2);

//     // Second frame in message contains event address. it's just the router's
//     // address. no help

//     if (event == ZMQ_EVENT_DISCONNECTED) {
//       if (!is_scheduler_) {
//         PS_VLOG(1) << my_node_.ShortDebugString() << ": scheduler is dead. exit.";
//         exit(-1);
//       }
//     }
//     if (event == ZMQ_EVENT_MONITOR_STOPPED) {
//       break;
//     }
//   }
//   zmq_close(s);
// }
