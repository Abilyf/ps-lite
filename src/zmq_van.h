/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_ZMQ_VAN_H_
#define PS_ZMQ_VAN_H_
#include <stdio.h>
#include <stdlib.h>
#include <zmq.h>
#include <string>
#include <unordered_map>
#include "ps/internal/van.h"
#if _MSC_VER
#define rand_r(x) rand()
#endif

namespace ps {
    /**
     * \brief be smart on freeing recved data
     */
    inline void FreeData(void* data, void* hint) {
        if (hint == NULL) {
            delete[] static_cast<char*>(data);
        }
        else {
            delete static_cast<SArray<char>*>(hint);
        }
    }

    /**
     * \brief ZMQ based implementation
     */
    class ZMQVan : public Van {
    public:
        ZMQVan() {}
        virtual ~ZMQVan() {}

    protected:
        void Start(int customer_id) override {//建立上下文
            // start zmq
            start_mu_.lock();
            if (context_ == nullptr) {
                context_ = zmq_ctx_new();
                CHECK(context_ != NULL) << "create 0mq context failed";
                zmq_ctx_set(context_, ZMQ_MAX_SOCKETS, 65536);
            }
            start_mu_.unlock();
            // zmq_ctx_set(context_, ZMQ_IO_THREADS, 4);
            Van::Start(customer_id);
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
            senders_.clear();
            zmq_ctx_destroy(context_);
            context_ = nullptr;
        }

        int Bind(const Node& node, int max_retry) override {
            receiver_ = zmq_socket(context_, ZMQ_ROUTER);//创建socket，这里和context有关，估计是保存到context里的sockets_中
            CHECK(receiver_ != NULL)
                << "create receiver socket failed: " << zmq_strerror(errno);
            int local = GetEnv("DMLC_LOCAL", 0);
            std::string hostname = node.hostname.empty() ? "*" : node.hostname;
            int use_kubernetes = GetEnv("DMLC_USE_KUBERNETES", 0);
            if (use_kubernetes > 0 && node.role == Node::SCHEDULER) {
                hostname = "0.0.0.0";
            }
            std::string addr = local ? "ipc:///tmp/" : "tcp://" + hostname + ":";
            int port = node.port;
            unsigned seed = static_cast<unsigned>(time(NULL) + port);
            for (int i = 0; i < max_retry + 1; ++i) {
                auto address = addr + std::to_string(port);
                if (zmq_bind(receiver_, address.c_str()) == 0) break;
                if (i == max_retry) {
                    port = -1;
                }
                else {
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
            // worker doesn't need to connect to the other workers. same for server
            if ((node.role == my_node_.role) && (node.id != my_node_.id)) {
                return;
            }
            void* sender = zmq_socket(context_, ZMQ_DEALER);//这里创建的是发送端的socket，同样是保存到context_中
            CHECK(sender != NULL)
                << zmq_strerror(errno)
                << ". it often can be solved by \"sudo ulimit -n 65536\""
                << " or edit /etc/security/limits.conf";
            if (my_node_.id != Node::kEmpty) {
                std::string my_id = "ps" + std::to_string(my_node_.id);
                zmq_setsockopt(sender, ZMQ_IDENTITY, my_id.data(), my_id.size());
                const char* watermark = Environment::Get()->find("DMLC_PS_WATER_MARK");
                if (watermark) {
                    const int hwm = atoi(watermark);
                    zmq_setsockopt(sender, ZMQ_SNDHWM, &hwm, sizeof(hwm));
                }
            }
            // connect
            std::string addr = "tcp://" + node.hostname + ":" + std::to_string(node.port);
            if (GetEnv("DMLC_LOCAL", 0)) {
                addr = "ipc:///tmp/" + std::to_string(node.port);
            }
            if (zmq_connect(sender, addr.c_str()) != 0) {//发起连接。即将socket和ip地址做一个映射
                LOG(FATAL) << "connect to " + addr + " failed: " + zmq_strerror(errno);
            }
            senders_[id] = sender;//将设置好连接的socket按照id分类保存到发送的socket列表中
        }

        //int SendMsg(const Message& msg) override {
        int SendMsg(Message& msg, Node* node, bool connect) override {
            std::lock_guard<std::mutex> lk(mu_);
            // find the socket
            int id = msg.meta.recver;
            CHECK_NE(id, Meta::kEmpty);
            auto it = senders_.find(id);
            if (it == senders_.end()) {
                LOG(WARNING) << "there is no socket to node " << id;
                return -1;
            }
            void* socket = it->second;

            // send meta
            int meta_size; char* meta_buf;
            PackMeta(msg.meta, &meta_buf, &meta_size);
            int tag = ZMQ_SNDMORE;
            int n = msg.data.size();//这里的n便是指msg中data的数据个数，因为data是一个存储char类型的vector容器
            if (n == 0) tag = 0;
            zmq_msg_t meta_msg;
            zmq_msg_init_data(&meta_msg, meta_buf, meta_size, FreeData, NULL);//这一步是将meta数据打包到meta_msg中
            while (true) {
                if (zmq_msg_send(&meta_msg, socket, tag) == meta_size) break;//可以看到，这里首先发送的是包头meta信息，而非整个一起发送
                //从接收端的处理也能看出，第一个收到的消息是meta信息
                if (errno == EINTR) continue;
                return -1;
            }
            // zmq_msg_close(&meta_msg);
            int send_bytes = meta_size;//更新已发送字节数
            // send data
            for (int i = 0; i < n; ++i) {
                zmq_msg_t data_msg;
                SArray<char>* data = new SArray<char>(msg.data[i]);//取出第一个数据，从前面来看是keys数据
                int data_size = data->size();//获取本次数据大小
                zmq_msg_init_data(&data_msg, data->data(), data->size(), FreeData, data);//初始化zmq_msg
                if (i == n - 1) tag = 0;
                while (true) {
                    if (zmq_msg_send(&data_msg, socket, tag) == data_size) break;//发送本次数据，这里是第二次发送，发送的是keys数据，循环过后则是第三次发送vals数据
                    if (errno == EINTR) continue;
                    LOG(WARNING) << "failed to send message to node [" << id
                        << "] errno: " << errno << " " << zmq_strerror(errno)
                        << ". " << i << "/" << n;
                    return -1;
                }
                // zmq_msg_close(&data_msg);
                send_bytes += data_size;//更新总大小
            }
            return send_bytes;//返回本次发送总数据量
        }

        int RecvMsg(Message* msg) override {
            msg->data.clear();//首先清空msg内容
            size_t recv_bytes = 0;//创建接受数据总量的变量
            for (int i = 0; ; ++i) {
                zmq_msg_t* zmsg = new zmq_msg_t;
                CHECK(zmq_msg_init(zmsg) == 0) << zmq_strerror(errno);
                while (true) {
                    if (zmq_msg_recv(zmsg, receiver_, 0) != -1) break;
                    if (errno == EINTR) {
                        std::cout << "interrupted";
                        continue;
                    }
                    LOG(WARNING) << "failed to receive message. errno: "
                        << errno << " " << zmq_strerror(errno);
                    return -1;
                }
                char* buf = CHECK_NOTNULL((char*)zmq_msg_data(zmsg));
                size_t size = zmq_msg_size(zmsg);
                recv_bytes += size;

                if (i == 0) {
                    // identify
                    msg->meta.sender = GetNodeID(buf, size);
                    msg->meta.recver = my_node_.id;
                    CHECK(zmq_msg_more(zmsg));
                    zmq_msg_close(zmsg);
                    delete zmsg;
                }
                else if (i == 1) {
                    // task
                    UnpackMeta(buf, size, &(msg->meta));
                    zmq_msg_close(zmsg);
                    bool more = zmq_msg_more(zmsg);
                    delete zmsg;
                    if (!more) break;
                }
                else {
                    // zero-copy
                    SArray<char> data;
                    data.reset(buf, size, [zmsg, size](char* buf) {
                        zmq_msg_close(zmsg);
                        delete zmsg;
                        });
                    msg->data.push_back(data);
                    if (!zmq_msg_more(zmsg)) {
                        break;
                    }
                }
            }
            return recv_bytes;
        }

        void UpdateNodeID(int old_id, int new_id) {}

    private:
        /**
         * return the node id given the received identity
         * \return -1 if not find
         */
        int GetNodeID(const char* buf, size_t size) {
            if (size > 2 && buf[0] == 'p' && buf[1] == 's') {
                int id = 0;
                size_t i = 2;
                for (; i < size; ++i) {
                    if (buf[i] >= '0' && buf[i] <= '9') {
                        id = id * 10 + buf[i] - '0';
                    }
                    else {
                        break;
                    }
                }
                if (i == size) return id;
            }
            return Meta::kEmpty;
        }

        void* context_ = nullptr;
        /**
         * \brief node_id to the socket for sending data to this node
         */
        std::unordered_map<int, void*> senders_;
        std::mutex mu_;
        void* receiver_ = nullptr;
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