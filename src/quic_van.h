/**
 *  Copyright (c) 2015 by Contributors
 */

 //QUIC VAN COMMUNICATION

#ifndef PS_QUIC_VAN_H_
#define PS_QUIC_VAN_H_

#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <zmq.h>
#include <stdlib.h>
#include <thread>
#include <string>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <strings.h>
#include <ctype.h>

#include "ps/internal/van.h"
#include "netinet/tcp.h"
#include "ps/internal/threadsafe_queue.h"
#include "ps/internal/threadsafe_priority_queues.h"
#include "ps/sarray.h"
#include "ps/base.h"
#include "picoquic.h"
#include "picosplay.h"
#include "picoquic_internal.h"
#include "picosocks.h"
#include "ps/internal/utils.h"
#include "h3zero.h"
#include "democlient.h"
#include "demoserver.h"
#include "ccpara.h"


#ifndef __USE_XOPEN2K
#define __USE_XOPEN2K
#endif
#ifndef __USE_POSIX
#define __USE_POSIX
#endif
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>

#ifndef SOCKET_TYPE
#define SOCKET_TYPE int
#endif
#ifndef INVALID_SOCKET
#define INVALID_SOCKET -1
#endif
#ifndef SOCKET_CLOSE
#define SOCKET_CLOSE(x) close(x)
#endif
#ifndef WSA_LAST_ERROR
#define WSA_LAST_ERROR(x) ((long)(x))
#endif
static const char* server_cert_file = "../3rdparty/quic/certs/cert.pem";
static const char* server_key_file = "../3rdparty/quic/certs/key.pem";
static const char* ticket_store_filename = "../3rdparty/quic/certs/demo_ticket_store.bin";

#if _MSC_VER
#define rand_r(x) rand()
#endif

using namespace std;

namespace ps {
    /*A packet which header and data stored in a discontinuous memory segment. Used for transmission*/
    struct PackedPacket {
        /*Header store four kinds of information
        - key
        - offset
        - valsize
        - timestamp*/
        SArray<char> header;
        /*The size of date is equal to m_packet_size_ minus m_packet_header_size_, which is a fixed value, made all unpackedPackets the same size*/
        SArray<char> data;

        Key dataLen = 0;
        bool valid = true; // when server finish all merging, it sets valid to false so that this pkt is dumped in next iteration
        bool stop = false;
    };

    /*A packet which header and data stored in a continuous memory segment. Used for processing*/
    struct UnpackedPacket {
        /*header_data includes the packet header and data*/
        SArray<char> header_data;

        Key dataLen = 0;
        bool valid = true; // when server finish all merging, it sets valid to false so that this pkt is dumped in next iteration
        bool stop = false;
        int merge_num = 1;
    };

    struct QUICCnxParameter {
        picoquic_cnx_t* cnx = nullptr;
        picoquic_cnx_t* cnx_next = nullptr;
        picoquic_path_t* qpath = nullptr;
        int* qsocket = nullptr;

        int zero_rtt_avaliable = 0;
        int bytes_sent = 0;
        int client_receive_loop = 0;
        int client_ready_loop = 0;
        int new_context_created = 0;
        int notified_ready = 0;
        unsigned long if_index_to = 0;
        char const* saved_alpn = nullptr;
        int established = 0;
        int64_t delay_max = 10000000;
        uint64_t current_time = 0;
        int64_t delta_t = 0;
        picoquic_stateless_packet_t* sp = nullptr;
        picoquic_demo_stream_desc_t* post_file = nullptr;//推送数据的相关记录

        struct sockaddr_storage peer_address;
        struct sockaddr_storage packet_from;
        struct sockaddr_storage packet_to;
        socklen_t from_length = 0;
        socklen_t to_length = 0;
        int peer_addr_length = 0;
        uint8_t buffer[1536];
        uint8_t send_buffer[1536];
        size_t send_length = 0;
        uint64_t loop_time = 0;

        picoquic_demo_callback_ctx_t* send_cb_ctx = nullptr;
        picohttp_server_parameters_t* rcv_cb_ctx = nullptr;
    };

    /*An array of unpackedPackets*/
    struct Tensor {
        std::vector<UnpackedPacket> unpackedPackets;
    };


    /**
     * \brief ZMQ based implementation
     */
    class QUICVan : public Van {
    public:
        QUICVan();
        virtual ~QUICVan();

        /******************* socket related util funcs ********************/
    public:

        /*
        @brief Test socket fd, function write() and read() work fine or not
        @param fd: file descriptor, which means socket
        @return Null
        */
        //void test_rtt_send(int fd);
        /********************* socket related util funcs ***************************/

    protected:
        /*
        @brief Create quic context and run van->start()
        @param customer_id: the node identity
        @return Null
        */
        void Start(int customer_id) override;

        /*
        @brief Waiting for all the threads stopped using function join()
        @param Null
        @return Null
        */
        void Stop() override;

        /*
        @brief Create a context for quic connection.
        @param null
        @return null
        */
        void CreateQUICContext();

        /*
        @brief Create a quic socket and set some options.
        @param qsocket:quic socket
        @return the result: the socket(succeed) or -1(failed)
        */
        int CreateQUICSocket(int* qsocket);

        /*
        @brief Bind the quic socket to the port provided from node
        @param node: the server or worker node which neet to be connect
        @param max_retry: the maximum number of retry
        @return port number
        */
        int Bind(const Node& node, int max_retry) override;

        /*
        @brief Create socket and cnx and send init packet for creating quic connection to the node given.
        @param node: the server or worker node which neet to be connect
        @return Null
        */
        void Connect(const Node& node) override;

        /*
        @brief Create socket and cnx and send init packet for creating quic connection to the node given.
        @param time_before:
        @param current_time:
        @param delta_t:
        @param node_id: a string "ip:port" represent for node id
        @return 1 if succeed，others if failed
        */
        int QUICCnxRcv(uint64_t current_time, int64_t delta_t, std::string node_id, int bytes_recv);

        /*
        @brief Send msg through quic socket.
        @param msg: the message that need to be sent.
        @param length: the length of msg.
        @param node_id: node id to which msg is sent
        @return the number of bytes that have been sent currently
        */
        int QUICMsgSend(char* msg, int* length, std::string node_id);

        // Sending starts from this func
        // thread safe
        /*取出传入的msg中的id和data、priority等信息，并将data分出key和value分别存储，将这些信息送入sendpacket进行打包*/
        //int SendMsg(const Message& msg) override;
        int SendMsg(Message& msg, Node* node, bool connect) override;//not const

        /*
        @brief Create socket and cnx and send init packet for creating quic connection to the node given.
        @param node: the server or worker node which neet to be connect
        @return Null
        */
        int QUICMsgRcv(char** msg_data, int& msg_rcvd);

        int RecvMsg(Message* msg) override;

        void UpdateNodeID(int old_id, int new_id) override;

        std::string ByteArrayToString(const uint8_t* byte_array, size_t length);

        //void append_rtt_to_json(const cc_info_minrtt_t* cc_minrtt, double finished_time, double throughput, const char* filename);
        void append_rtt_to_json(const cc_info_minrtt_t* cc_minrtt, double finished_time, double throughput, const char* filename);

    private:
        Van* m_zmq_van_;

        //int GetNodeID(const char* buf, size_t size);
        void print_address(struct sockaddr* address, const char* label, picoquic_connection_id_t cnx_id);

    protected:
        std::mutex m_mu_;
        std::mutex m_mu_tmp_;

        /*quic parameters*/
        //quic ctx, used for managing all the cnx
        picoquic_quic_t* m_quic_send_ctx = nullptr;
        picoquic_quic_t* m_quic_rcv_ctx = nullptr;

        //quic cnx, used for supporting per connection
        std::unordered_map<int, std::string> rcv_nid2cnx;
        std::unordered_map<std::string, int> rcv_cnx2nid;
        std::unordered_map<int, std::string> snd_nid2cnx;
        std::unordered_map<std::string, int> snd_cnx2nid;
        std::unordered_map<std::string, QUICCnxParameter*> m_qsnd;//初始化的时候要创建这个变量
        std::unordered_map<std::string, QUICCnxParameter*> m_qrcv;

        //quic socket for receiving or connecting
        int* m_quic_receiver = nullptr;
        //int* m_quic_connecter = nullptr;

        //quic rcv parameter
        struct sockaddr_storage m_packet_from;
        struct sockaddr_storage m_packet_to;
        socklen_t m_from_length = 0;
        socklen_t m_to_length = 0;
        unsigned long m_if_index_to = 0;
        int64_t m_delay_max = 10000000;
        uint8_t m_buffer[1536];
        int m_new_context_created = 0;
        const char* test_scenario_default = "0:index.html";


        const char* m_sni = nullptr;
        char* m_alpn = nullptr;
        uint64_t  m_previous_stream_id;
        uint64_t  m_cur_stream_id;
        uint32_t  m_proposed_version = 0xff00000b;
        //picoquic_stateless_packet_t* m_sp;
    };
}  // namespace ps
#endif  // PS_QUIC_VAN_H_