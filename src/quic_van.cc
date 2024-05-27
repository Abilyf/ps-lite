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

#include "ps/internal/van.h"
#include "netinet/tcp.h"
#include "ps/internal/threadsafe_queue.h"
#include "ps/internal/threadsafe_priority_queues.h"
#include "quic_van.h"

#define PICOQUIC_DEMO_STREAM_ID_INITIAL (uint64_t)((int64_t)-1)

namespace ps {

    QUICVan::QUICVan() { //构造函数，完成对一些基本参数的配置
        m_from_length = sizeof(struct sockaddr_storage);
        m_to_length = sizeof(struct sockaddr_storage);
        m_if_index_to = 0;
        memset(&m_packet_from, 0, sizeof(m_packet_from));
        memset(&m_packet_to, 0, sizeof(m_packet_to));
        m_packet_from.ss_family = AF_INET;
        m_packet_to.ss_family = AF_INET;
        m_alpn = "h3-29";
    }

    QUICVan::~QUICVan() {}

    void QUICVan::Start(int customer_id) {
        // start quic
        start_mu_.lock();
        CreateQUICContext();
        //CHECK(m_quic_send_ctx != nullptr) << "create quic context failed!";       
        start_mu_.unlock();
        Van::Start(customer_id);
    }

    void QUICVan::Stop() {
        LG << my_node_.ShortDebugString() << " is stopping";
        Van::Stop();
        // close sockets
        picoquic_close_one_server_sockets(m_quic_receiver);
    }

    void QUICVan::CreateQUICContext() {
        /*create server callback ctx*/
        picohttp_server_parameters_t* picoquic_file_param = (picohttp_server_parameters_t*)malloc(sizeof(picohttp_server_parameters_t));
        //create post callback ctx
        //char* post = "/post";
        picohttp_server_path_item_t path_item_list[] =
        {
            {
                "/post",
                5,
                demoserver_post_callback
                //nullptr
            },
        };
        memset(picoquic_file_param, 0, sizeof(picohttp_server_parameters_t));
        picoquic_file_param->path_table = path_item_list;
        picoquic_file_param->path_table_nb = 1;
        cnx_id_cb_fn cnx_id_callback = nullptr;
        void* cnx_id_callbacl_ctx = nullptr;

        m_quic_rcv_ctx = (picoquic_quic_t*)malloc(sizeof(picoquic_quic_t));
        m_quic_send_ctx = (picoquic_quic_t*)malloc(sizeof(picoquic_quic_t));

        /*创建QUIC server连接端的上下文，其中包含了用于传输的参数*/
        m_quic_rcv_ctx = picoquic_create(8, server_cert_file, server_key_file, nullptr, nullptr, picoquic_demo_server_callback, picoquic_file_param,
            cnx_id_callback, cnx_id_callbacl_ctx, nullptr, picoquic_current_time(), nullptr, nullptr, nullptr, 0, nullptr);
        if (m_quic_rcv_ctx == nullptr) {
            printf("Couldn't create quic server context.\n");
        }
        else {
            picoquic_set_alpn_select_fn(m_quic_rcv_ctx, picoquic_demo_server_callback_select_alpn);
        }
        /*创建worker端的上下文*/
        uint64_t current_time = 0;
        current_time = picoquic_current_time();
        m_quic_send_ctx = picoquic_create(8, nullptr, nullptr, nullptr, m_alpn, nullptr, nullptr, nullptr, nullptr, nullptr,
            current_time, nullptr, ticket_store_filename, nullptr, 0, nullptr);
        if (m_quic_send_ctx == nullptr) {
            printf("Couldn't create quic worker context.\n");
        }
        else {
            picoquic_set_null_verifier(m_quic_send_ctx);
        }
    }

    int QUICVan::CreateQUICSocket(int* qsocket)
    {
        int ret = 0;
        *qsocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);//创建UDPsocket
        int val = 1;

#ifdef IP_PKTINFO
        ret = setsockopt(*qsocket, IPPROTO_IP, IP_PKTINFO, (char*)&val, sizeof(int));//表示返回分组信息
#else
        /* The IP_PKTINFO structure is not defined on BSD */
        ret = setsockopt(quic_socket, IPPROTO_IP, IP_RECVDSTADDR, (char*)&val, sizeof(int));
#endif
        if (ret != 0) {
            printf("set socket option failed!");
            return 0;
        }
        //*qsocket = &quic_socket;
        return ret;
    }

    int QUICVan::Bind(const Node& node, int max_retry) {
        int ret = 0;
        int port = node.port;
        unsigned seed = static_cast<unsigned>(time(nullptr) + port);//用于绑定端口失败时，分配随机端口

        struct sockaddr_storage sa;
        //int addr_length = 0;
        memset(&sa, 0, sizeof(sa));
        struct sockaddr_in* s4 = (struct sockaddr_in*)&sa;

        s4->sin_family = AF_INET;//设定为ipv4阈
        s4->sin_port = htons((unsigned short)port);//设定端口
        //s4->sin_addr.s_addr = inet_addr(node.hostname.c_str());
        m_quic_receiver = new int;
        ret = CreateQUICSocket(m_quic_receiver);
        if (ret != 0) {
            DBG_PRINTF("Create socket faliled!");
            return ret;
        }
        //DBG_PRINTF("Receiving socket %d has been created", *m_quic_receiver);
        for (int i = 0; i < max_retry + 1; ++i) {//max_retry为最大的尝试次数，直到成功绑定为止
            //LOG(INFO) << "start bing port" << std::endl;
            ret = bind(*m_quic_receiver, (struct sockaddr*)&sa, sizeof(struct sockaddr_in));//绑定端口
            //LG << "ret: " << ret << std::endl;
            if (ret == 0) break;
            if (i == max_retry) {
                port = -1;
            }
            else {
                port = 10000 + rand_r(&seed) % 40000;//绑定不成功则随机分配
            }
        }
        DBG_PRINTF("Binding socket to port %d for node %d", port, node.id);
        return port;
    }

    void QUICVan::Connect(const Node& node) {
        CHECK_NE(node.id, node.kEmpty);
        CHECK_NE(node.port, node.kEmpty);
        CHECK(node.hostname.size());
        picoquic_connection_id_t cnx_id = picoquic_null_connection_id;
        //int node_id = node.id;
        //std::string node_id = node.hostname + ":" + std::to_string(node.port);
        int ret = 0;
        //Don't need to connect to the node with same role
        if ((node.role == my_node_.role) && (node.id != my_node_.id)) {
            return;
        }

        struct sockaddr_storage peer_address;
        int peer_addr_length = 0;
        int is_name = 0;
        uint64_t current_time = picoquic_current_time();
        picoquic_cnx_t* cnx_tmp;
        /*Get server address and create connection*/
        if (picoquic_get_server_address(node.hostname.c_str(), node.port, &peer_address, &peer_addr_length, &is_name) == 0) {
            //the peer_address is now network sequence
            cnx_tmp = picoquic_create_cnx(m_quic_send_ctx, picoquic_null_connection_id, picoquic_null_connection_id,
                (struct sockaddr*)&peer_address, current_time, m_proposed_version, m_sni, m_alpn, 1);
            {
                std::lock_guard<std::mutex> lock(m_mu_);
                LG << "Send cnx_ctx: " << cnx_tmp << "has created" << std::endl;
                LG << "Destination address: " << node.hostname.c_str() << ", Port: " << node.port << ",  CNX-STATE: " << cnx_tmp->cnx_state << std::endl;;
            }
        }
        cnx_id = cnx_tmp->path[0]->local_cnxid;
        std::string node_id = ByteArrayToString(cnx_id.id, cnx_id.id_len);
        snd_nid2cnx[node.id] = node_id;
        snd_cnx2nid[node_id] = node.id;
        LG << "The node_id: " << node_id << std::endl;

        /*Check whether the cnx exists*/
        auto it = m_qsnd.find(node_id);
        if (it == m_qsnd.end()) {
            m_qsnd[node_id] = new QUICCnxParameter;
            memset(m_qsnd[node_id], 0, sizeof(QUICCnxParameter));
            m_qsnd[node_id]->packet_from.ss_family = AF_INET;
            m_qsnd[node_id]->from_length = sizeof(sockaddr_storage);
            m_qsnd[node_id]->packet_to.ss_family = AF_INET;
            m_qsnd[node_id]->to_length = sizeof(sockaddr_storage);
            m_qsnd[node_id]->peer_address.ss_family = AF_INET;
            m_qsnd[node_id]->peer_addr_length = sizeof(sockaddr_storage);
            m_qsnd[node_id]->cnx = cnx_tmp;
            m_qsnd[node_id]->current_time = current_time;
            m_qsnd[node_id]->peer_address = peer_address;
            //m_qsnd[node_id]->qpath = new picoquic_path_t;
        }

        /*Check whether the sender socket exists*/
        auto its = m_qsnd.find(node_id);
        if (its != m_qsnd.end()) {
            if (!its->second->qsocket) {
                its->second->qsocket = new int;
                //std::cout << "Creating qsocket..." << std::endl;
                ret = CreateQUICSocket(m_qsnd[node_id]->qsocket);
                {
                    std::lock_guard<std::mutex> lock(m_mu_);
                    LG << "Send socket: " << *m_qsnd[node_id]->qsocket << " has created" << std::endl;
                }
                //std::cout << "the qsocket: " << *m_qsnd[node_id]->qsocket << std::endl;
            }
        }

        /*create connection parameters*/
        //picoquic_demo_callback_ctx_t callback_ctx;
        m_qsnd[node_id]->send_cb_ctx = new picoquic_demo_callback_ctx_t;
        memset(m_qsnd[node_id]->send_cb_ctx, 0, sizeof(picoquic_demo_callback_ctx_t));
        //uint64_t current_time = 0;
        //int zero_rtt_avaliable = 0;
        int bytes_sent = 0;
        //struct sockaddr_storage peer_address{};
        //int peer_address_length = 0;

        m_qsnd[node_id]->current_time = picoquic_current_time();
        m_qsnd[node_id]->send_cb_ctx->last_interaction_time = m_qsnd[node_id]->current_time;
        m_qsnd[node_id]->client_receive_loop = 0;
        m_qsnd[node_id]->post_file = (picoquic_demo_stream_desc_t*)malloc(sizeof(picoquic_demo_stream_desc_t));
        memset(m_qsnd[node_id]->post_file, 0, sizeof(picoquic_demo_stream_desc_t));
        ret = picoquic_demo_client_initialize_context(m_qsnd[node_id]->send_cb_ctx, m_qsnd[node_id]->post_file, 0, m_alpn, 0, 0);

        //const char* ip = node.hostname.c_str();

        picoquic_set_callback(m_qsnd[node_id]->cnx, picoquic_demo_client_callback, m_qsnd[node_id]->send_cb_ctx);

        if (m_qsnd[node_id]->cnx->alpn == NULL) {
            picoquic_demo_client_set_alpn_from_tickets(m_qsnd[node_id]->cnx, m_qsnd[node_id]->send_cb_ctx, m_qsnd[node_id]->current_time);
            if (m_qsnd[node_id]->cnx->alpn != NULL) {
                fprintf(stdout, "Set ALPN to %s based on stored ticket\n", m_qsnd[node_id]->cnx->alpn);
            }
        }

        /*Start tls initializing*/
        ret = picoquic_start_client_cnx(m_qsnd[node_id]->cnx);

        /*start init packet sending*/
        if (ret == 0) {
            if (picoquic_is_0rtt_available(m_qsnd[node_id]->cnx) && (m_proposed_version & 0x0a0a0a0a) != 0x0a0a0a0a) {
                m_qsnd[node_id]->zero_rtt_avaliable = 1;
                ret = picoquic_demo_client_start_streams(m_qsnd[node_id]->cnx, m_qsnd[node_id]->send_cb_ctx, PICOQUIC_DEMO_STREAM_ID_INITIAL);
            }

            /*Prepare init packet for connection creation*/
            if (ret == 0) {
                ret = picoquic_prepare_packet(m_qsnd[node_id]->cnx, picoquic_current_time(),
                    m_qsnd[node_id]->send_buffer, sizeof(m_qsnd[node_id]->send_buffer), &m_qsnd[node_id]->send_length, &m_qsnd[node_id]->qpath);
            }
            {
                std::lock_guard<std::mutex> lock(m_mu_);
                LG << "CNX-STATE: " << m_qsnd[node_id]->cnx->cnx_state << std::endl;
            }
            /*Send init packet*/
            if (ret == 0 && m_qsnd[node_id]->send_length > 0) {
                //std::cout << "m_qsnd[node_id]->cnx: " << m_qsnd[node_id]->cnx << std::endl;
                {
                    std::lock_guard<std::mutex> lock(m_mu_);
                    LG << "Prepare to send sender's initial packet using socket: " << *m_qsnd[node_id]->qsocket << std::endl;
                    LG << m_qsnd[node_id]->send_length << " bytes need to be sent" << std::endl;
                }
                picoquic_before_sending_packet(m_qsnd[node_id]->cnx, *m_qsnd[node_id]->qsocket);
                /* The first packet must be a sendto, next ones, not necessarily! */
                bytes_sent = sendto(*m_qsnd[node_id]->qsocket, m_qsnd[node_id]->send_buffer, (int)m_qsnd[node_id]->send_length, 0,
                    (struct sockaddr*)&m_qsnd[node_id]->peer_address, m_qsnd[node_id]->peer_addr_length);
                if (bytes_sent == -1) {
                    std::cerr << "Sendto failed with error: " << strerror(errno) << std::endl;
                    // 可根据 errno 的值进行不同的处理
                }
                else {
                    std::lock_guard<std::mutex> lock(m_mu_);
                    LG << "Sending " << bytes_sent << " bytes successfully." << std::endl;
                }
            }
        }

        /*Wait for packet from node*/
        while (ret == 0 && picoquic_get_cnx_state(m_qsnd[node_id]->cnx) != picoquic_state_disconnected) {

            //LG << "waiting for server init packet" << std::endl;
            int bytes_recv;

            m_qsnd[node_id]->from_length = m_qsnd[node_id]->to_length = sizeof(struct sockaddr_storage);
            uint64_t select_time = picoquic_current_time();

            /*Receiving packet from node*/
            bytes_recv = picoquic_select(m_qsnd[node_id]->qsocket, 1, &m_qsnd[node_id]->packet_from, &m_qsnd[node_id]->from_length,
                &m_qsnd[node_id]->packet_to, &m_qsnd[node_id]->to_length, &m_qsnd[node_id]->if_index_to, m_qsnd[node_id]->buffer, sizeof(m_qsnd[node_id]->buffer),
                m_qsnd[node_id]->delta_t, &m_qsnd[node_id]->current_time, m_quic_send_ctx);

            if (bytes_recv < 0) {
                ret = -1;
            }
            else {
                if (bytes_recv > 0) {
                    /*
                    {
                        std::lock_guard<std::mutex> lock(m_mu_);
                        LG << "server first init&hsk packet received!" << std::endl;
                    }*/
                    /*Decode packet*/
                    ret = picoquic_incoming_packet(m_quic_send_ctx, m_qsnd[node_id]->buffer, (size_t)bytes_recv,
                        (struct sockaddr*)&m_qsnd[node_id]->packet_from, (struct sockaddr*)&m_qsnd[node_id]->packet_to, m_qsnd[node_id]->if_index_to,
                        picoquic_current_time(), &m_qsnd[node_id]->new_context_created, &cnx_id);
                    /*
                    {
                        std::lock_guard<std::mutex> lock(m_mu_);
                        LG << " cnx state: " << m_qsnd[node_id]->cnx->cnx_state << std::endl;
                    }*/

                    m_qsnd[node_id]->client_receive_loop++;

                    if (picoquic_get_cnx_state(m_qsnd[node_id]->cnx) == picoquic_state_client_almost_ready && m_qsnd[node_id]->notified_ready == 0) {
                        if (picoquic_tls_is_psk_handshake(m_qsnd[node_id]->cnx)) {
                            fprintf(stdout, "The session was properly resumed!\n");
                        }

                        if (m_qsnd[node_id]->cnx->zero_rtt_data_accepted) {
                            fprintf(stdout, "Zero RTT data is accepted!\n");
                        }

                        if (m_qsnd[node_id]->cnx->alpn != nullptr) {
                            fprintf(stdout, "Negotiated ALPN: %s\n", m_qsnd[node_id]->cnx->alpn);
                            m_qsnd[node_id]->saved_alpn = picoquic_string_duplicate(m_qsnd[node_id]->cnx->alpn);
                        }
                        fprintf(stdout, "Almost ready!\n");
                        m_qsnd[node_id]->notified_ready = 1;
                    }

                    m_qsnd[node_id]->delta_t = 0;
                }

                //if (bytes_recv == 0 || (ret == 0 && m_qsnd[node_id]->client_receive_loop > 1)) {
                //if (bytes_recv == 0 || (ret == 0 && m_qsnd[node_id]->client_receive_loop > 4 )) {
                if (bytes_recv == 0 || (ret == 0 && m_qsnd[node_id]->client_receive_loop > 4)) {
                    m_qsnd[node_id]->client_receive_loop = 0;

                    if (ret == 0 && picoquic_get_cnx_state(m_qsnd[node_id]->cnx) == picoquic_state_client_ready) {
                        LG << "Connection has been built, the CNX-STATE is: " << picoquic_get_cnx_state(m_qsnd[node_id]->cnx) << endl;
                        printf("Connection established. Version = %x, I-CID: %" PRIx64 "\n",
                            picoquic_supported_versions[m_qsnd[node_id]->cnx->version_index].version,
                            picoquic_val64_connection_id(picoquic_get_logging_cnxid(m_qsnd[node_id]->cnx)));//获得connectionID
                        m_qsnd[node_id]->established = 1;//将连接建立标志位置为1
                        break;
                    }

                    //send init ack and handshake ack
                    if (ret == 0) {
                        /* {
                            std::lock_guard<std::mutex> lock(m_mu_);
                            LG << "send client handshake packet" << std::endl;
                        }*/
                        //m_qsnd[node_id]->send_length = 1536;
                        {
                            //std::lock_guard<std::mutex> lock(m_mu_);
                            //LG << "send cnx state（befor client init&handshake）: " << m_qsnd[node_id]->cnx->cnx_state << std::endl;
                        }
                        ret = picoquic_prepare_packet(m_qsnd[node_id]->cnx, picoquic_current_time(),
                            m_qsnd[node_id]->send_buffer, sizeof(m_qsnd[node_id]->send_buffer), &m_qsnd[node_id]->send_length, &m_qsnd[node_id]->qpath);
                        //LG << "client: ret=" << ret << " and send_length=" << m_qsnd[node_id]->send_length << std::endl;
                        if (ret == 0 && m_qsnd[node_id]->send_length > 0) {
                            int peer_addr_len = 0;
                            struct sockaddr* peer_addr;
                            int local_addr_len = 0;
                            struct sockaddr* local_addr;
                            {
                                std::lock_guard<std::mutex> lock(m_mu_);
                                LG << "After Preparing CNX-STATE: " << m_qsnd[node_id]->cnx->cnx_state << std::endl;
                            }
                            picoquic_before_sending_packet(m_qsnd[node_id]->cnx, *m_qsnd[node_id]->qsocket);

                            picoquic_get_peer_addr(m_qsnd[node_id]->qpath, &peer_addr, &peer_addr_len);
                            picoquic_get_local_addr(m_qsnd[node_id]->qpath, &local_addr, &local_addr_len);

                            bytes_sent = picoquic_sendmsg(*m_qsnd[node_id]->qsocket, peer_addr, peer_addr_len, local_addr,
                                local_addr_len, picoquic_get_local_if_index(m_qsnd[node_id]->qpath),
                                (const char*)m_qsnd[node_id]->send_buffer, (int)m_qsnd[node_id]->send_length);
                            if (bytes_sent == -1) {
                                std::cerr << "Sending failed with error: " << strerror(errno) << std::endl;
                                // 可根据 errno 的值进行不同的处理
                            }
                            else {
                                std::lock_guard<std::mutex> lock(m_mu_);
                                LG << "Sending " << bytes_sent << " bytes successfully." << std::endl;
                            }
                        }
                    }

                    m_qsnd[node_id]->delta_t = picoquic_get_next_wake_delay(m_quic_send_ctx, picoquic_current_time(), m_qsnd[node_id]->delay_max);

                }
            }
        }
    }

    void QUICVan::print_address(struct sockaddr* address, const char* label, picoquic_connection_id_t cnx_id)
    {
        char hostname[256];

        const char* x = inet_ntop(address->sa_family,
            (address->sa_family == AF_INET) ? (void*)&(((struct sockaddr_in*)address)->sin_addr) : (void*)&(((struct sockaddr_in6*)address)->sin6_addr),
            hostname, sizeof(hostname));

        printf("%016llx : ", (unsigned long long)picoquic_val64_connection_id(cnx_id));

        if (x != NULL) {
            printf("%s %s, port %d\n", label, x,
                (address->sa_family == AF_INET) ? ((struct sockaddr_in*)address)->sin_port : ((struct sockaddr_in6*)address)->sin6_port);
        }
        else {
            printf("%s: inet_ntop failed with error # %ld\n", label, WSA_LAST_ERROR(errno));
        }
    }

    int QUICVan::QUICCnxRcv(uint64_t current_time, int64_t delta_t, std::string node_id, int bytes_recv) {
        int ret = 0;
        //receiving init packet from peer and preparing init and handshake packets 
        if (bytes_recv < 0) { ret = -1; }
        else {
            if (bytes_recv > 0) {

                m_qrcv[node_id] = new QUICCnxParameter;
                memset(m_qrcv[node_id], 0, sizeof(QUICCnxParameter));
                m_qrcv[node_id]->packet_from.ss_family = AF_INET;
                m_qrcv[node_id]->from_length = sizeof(sockaddr_storage);
                m_qrcv[node_id]->packet_to.ss_family = AF_INET;
                m_qrcv[node_id]->to_length = sizeof(sockaddr_storage);
                m_qrcv[node_id]->peer_address.ss_family = AF_INET;
                m_qrcv[node_id]->peer_addr_length = sizeof(sockaddr_storage);
                m_qrcv[node_id]->new_context_created = m_new_context_created;

                if (ret != 0) { ret = 0; }

                //create cnx_ctx through initial packet
                if (m_qrcv[node_id]->new_context_created) {
                    m_qrcv[node_id]->cnx = picoquic_get_first_cnx(m_quic_rcv_ctx);//这一步是否会出现问题。quic中存在多个连接时还能用这套方案取出链接吗？
                    //LG << "server cnx: " << m_qrcv[node_id]->cnx << std::endl;
                    //{
                    //    std::lock_guard<std::mutex> lock(m_mu_);
                    //    LG << "server cnx state: " << m_qrcv[node_id]->cnx->cnx_state << std::endl;
                    //}

                    printf("%" PRIx64 ": ", picoquic_val64_connection_id(picoquic_get_logging_cnxid(m_qrcv[node_id]->cnx)));
                    picoquic_log_time(stdout, m_qrcv[node_id]->cnx, picoquic_current_time(), "", " : ");
                    printf("Connection established, state = %d, from length: %u\n",
                        picoquic_get_cnx_state(picoquic_get_first_cnx(m_quic_rcv_ctx)), m_from_length);
                    memset(&m_qrcv[node_id]->peer_address, 0, sizeof(m_qrcv[node_id]->peer_address));
                    memcpy(&m_qrcv[node_id]->peer_address, &m_packet_from, m_from_length);

                    /*
                    {
                        LG << "QUICCnxRcv->peer_address" << std::endl;
                        bool i = (m_qrcv[node_id]->peer_address.ss_family == AF_INET);
                        if (i) {
                            LG << "this is an ipv4 address" << std::endl;
                        }
                        else {
                            LG << "this is an ipv6 address" << std::endl;
                        }
                        char ipStr[INET_ADDRSTRLEN];
                        struct sockaddr_in* ipv4 = reinterpret_cast<struct sockaddr_in*>(&m_qrcv[node_id]->peer_address);
                        inet_ntop(AF_INET, &(ipv4->sin_addr), ipStr, INET_ADDRSTRLEN);
                        int port = ipv4->sin_port;
                        std::string adrs = std::string(ipStr) + ":" + std::to_string(port);
                        std::lock_guard<std::mutex> lock(m_mu_);
                        LG << "packet from: " << adrs << std::endl;
                    }
                    */

                    print_address((struct sockaddr*)&m_qrcv[node_id]->peer_address, "Client address:",
                        picoquic_get_logging_cnxid(m_qrcv[node_id]->cnx));
                    {
                        std::lock_guard<std::mutex> lock(m_mu_);
                        std::cerr << "address error: " << strerror(errno) << std::endl;
                    }
                    picoquic_log_transport_extension(stdout, m_qrcv[node_id]->cnx, 1);
                }
            }

            if (ret == 0) {
                uint64_t loop_time = picoquic_current_time();
                /*
                {
                    std::lock_guard<std::mutex> lock(m_mu_);
                    LG << "server cnx state: " << m_qrcv[node_id]->cnx->cnx_state << std::endl;
                }*/

                //prepare stateless packet for connection rebuild or something else
                while ((m_qrcv[node_id]->sp = picoquic_dequeue_stateless_packet(m_quic_rcv_ctx)) != nullptr) {
                    {
                        std::lock_guard<std::mutex> lock(m_mu_);
                        LG << "Sending stateless packets" << std::endl;
                    }
                    int bytes_sent = picoquic_send_through_server_sockets_for_ps(m_quic_receiver, (struct sockaddr*)&m_qrcv[node_id]->sp->addr_to,
                        (m_qrcv[node_id]->sp->addr_to.ss_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
                        (struct sockaddr*)&m_qrcv[node_id]->sp->addr_local,
                        (m_qrcv[node_id]->sp->addr_local.ss_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
                        m_qrcv[node_id]->sp->if_index_local,
                        (const char*)m_qrcv[node_id]->sp->bytes, (int)m_qrcv[node_id]->sp->length);
                    if (bytes_sent == -1) {
                        std::cerr << "Sending stateless pkt failed with error: " << strerror(errno) << std::endl;
                        // 可根据 errno 的值进行不同的处理
                    }
                    else {
                        LG << "Sending " << bytes_sent << " bytes successfully." << std::endl;
                    }
                    fflush(stdout);
                    picoquic_delete_stateless_packet(m_qrcv[node_id]->sp);
                }

                //prepare initial and handshake packets
                //while (ret == 0 && (m_qrcv[node_id]->cnx_next = picoquic_get_earliest_cnx_to_wake(m_quic_rcv_ctx, loop_time)) != nullptr &&
                    //m_qrcv[node_id]->cnx_next->cnx_state < picoquic_state_server_ready) {
                while (ret == 0 && (m_qrcv[node_id]->cnx_next = m_quic_rcv_ctx->cnx_wake_first) != nullptr &&
                    m_qrcv[node_id]->cnx_next->cnx_state < picoquic_state_server_ready) {
                    //LG << "prepare to send server init pkt and handshake pkt" << std::endl;
                    LG << "The current CNX-STATE: " << m_qrcv[node_id]->cnx_next->cnx_state << std::endl;
                    ret = picoquic_prepare_packet(m_qrcv[node_id]->cnx_next, picoquic_current_time(),
                        m_qrcv[node_id]->send_buffer, sizeof(m_qrcv[node_id]->send_buffer),
                        &m_qrcv[node_id]->send_length,
                        &m_qrcv[node_id]->qpath);
                    {
                        std::lock_guard<std::mutex> lock(m_mu_);
                        LG << "After preparing CNX-STATE: " << m_qrcv[node_id]->cnx->cnx_state << std::endl;
                    }
                    if (m_qrcv[node_id]->cnx_next->cnx_state == picoquic_state_server_ready) {
                        m_qrcv[node_id]->established = 1;
                        LG << "Established: " << m_qrcv[node_id]->established;
                    }
                    //if sth wrong
                    if (ret == PICOQUIC_ERROR_DISCONNECTED) {
                        {
                            std::lock_guard<std::mutex> lock(m_mu_);
                            LG << "quic error disconnected" << std::endl;
                        }
                        ret = 0;

                        printf("%" PRIx64 ": ", picoquic_val64_connection_id(picoquic_get_logging_cnxid(m_qrcv[node_id]->cnx_next)));
                        picoquic_log_time(stdout, m_qrcv[node_id]->cnx_next, picoquic_current_time(), "", " : ");
                        printf("Closed. Retrans= %d, spurious= %d, max sp gap = %d, max sp delay = %d\n",
                            (int)m_qrcv[node_id]->cnx_next->nb_retransmission_total, (int)m_qrcv[node_id]->cnx_next->nb_spurious,
                            (int)m_qrcv[node_id]->cnx_next->path[0]->max_reorder_gap, (int)m_qrcv[node_id]->cnx_next->path[0]->max_spurious_rtt);


                        if (m_qrcv[node_id]->cnx_next == m_qrcv[node_id]->cnx) {
                            m_qrcv[node_id]->cnx = nullptr;
                        }
                        //write_stats(cnx_next, stats_filename);
                        picoquic_delete_cnx(m_qrcv[node_id]->cnx_next);

                        fflush(stdout);
                        break;
                    }
                    //send those packets
                    else if (ret == 0) {
                        int peer_addr_len = 0;
                        struct sockaddr* peer_addr = new sockaddr;
                        int local_addr_len = 0;
                        struct sockaddr* local_addr = new sockaddr;

                        if (m_qrcv[node_id]->send_length > 0) {
                            picoquic_get_peer_addr(m_qrcv[node_id]->qpath, &peer_addr, &peer_addr_len);
                            picoquic_get_local_addr(m_qrcv[node_id]->qpath, &local_addr, &local_addr_len);
                            {
                                /*
                                char ipStr[INET_ADDRSTRLEN];
                                char ipStr6[INET_ADDRSTRLEN];
                                struct sockaddr_in* ipv4 = reinterpret_cast<struct sockaddr_in*>(&peer_addr);
                                struct sockaddr_in* ipv6 = reinterpret_cast<struct sockaddr_in*>(&local_addr);
                                inet_ntop(AF_INET, &(ipv4->sin_addr), ipStr, INET_ADDRSTRLEN);
                                inet_ntop(AF_INET, &(ipv6->sin_addr), ipStr6, INET_ADDRSTRLEN);
                                int port = ipv4->sin_port;
                                int port6 = ipv6->sin_port;
                                std::string adrs = std::string(ipStr) + ":" + std::to_string(port);
                                std::string adrs6 = std::string(ipStr6) + ":" + std::to_string(port6);
                                */
                                std::lock_guard<std::mutex> lock(m_mu_);
                                //LG << "QuicCnxRcv()" << std::endl;
                                LG << m_qrcv[node_id]->send_length << " bytes need to be sent" << std::endl;
                                /*
                                LG << "the receiver: " << adrs << std::endl;
                                LG << "the sender: " << adrs6 << std::endl;
                                for (int j = 0; j < 10; ++j) {
                                    //std::lock_guard<std::mutex> lock(m_mu_);
                                    LG << "buffer[" << j << "] = " << static_cast<int>(m_qrcv[node_id]->send_buffer[j]) << std::endl;
                                }
                                */
                            }
                            picoquic_before_sending_packet(m_qrcv[node_id]->cnx_next, *m_quic_receiver);
                            int bytes_sent = picoquic_send_through_server_sockets_for_ps(m_quic_receiver,
                                peer_addr, peer_addr_len, local_addr, local_addr_len,
                                picoquic_get_local_if_index(m_qrcv[node_id]->qpath),
                                (const char*)m_qrcv[node_id]->send_buffer, (int)m_qrcv[node_id]->send_length);
                            /*int bytes_sent = picoquic_send_through_server_sockets_for_ps(m_quic_receiver,
                                (sockaddr*)&m_packet_from, m_from_length, (sockaddr*)&m_packet_to, m_to_length,
                                picoquic_get_local_if_index(m_qrcv[node_id]->qpath),
                                (const char*)m_qrcv[node_id]->send_buffer, (int)m_qrcv[node_id]->send_length);*/
                            if (bytes_sent == -1) {
                                std::cerr << "Sending normal pkt with error: " << strerror(errno) << std::endl;
                                // 可根据 errno 的值进行不同的处理
                            }
                            else {
                                std::lock_guard<std::mutex> lock(m_mu_);
                                LG << "Sending " << bytes_sent << " bytes successfully." << std::endl;
                            }

                        }
                        else {
                            LG << "Send length: " << m_qrcv[node_id]->send_length << ", break" << std::endl;
                            break;
                        }
                    }
                    else { break; }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_mu_);
            //if(m_qrcv[node_id]->cnx_next->cnx_state) LG << "CNX STATE: " << m_qrcv[node_id]->cnx_next->cnx_state << std::endl;
            //LG << "CNX STATE: " << m_qrcv[node_id]->cnx_next->cnx_state << std::endl;
            LG << "The loop has been breaked! something wrong." << std::endl;
        }
        return ret;
    }

    int QUICVan::QUICMsgSend(char* msg, int* length, std::string node_id) {
        int ret = 0;
        //DBG_PRINTF("msg_length:%d", *length);
        int bytes_sent = 0;
        int post_sent = 0;
        size_t post_file_nb = 0;
        //create postfile for pushing data
        m_qsnd[node_id]->post_file = (picoquic_demo_stream_desc_t*)malloc(sizeof(picoquic_demo_stream_desc_t));
        ret = demo_client_parse_scenario_desc(test_scenario_default, &post_file_nb, &m_qsnd[node_id]->post_file);
        m_qsnd[node_id]->post_file->msg_data = &msg;//传入二级指针，因为最后会更新msg的地址，故而传入了二级指针；
        m_qsnd[node_id]->post_file->post_size = (uint64_t)*length;
        m_qsnd[node_id]->post_file->post_sent = new size_t;
        *m_qsnd[node_id]->post_file->post_sent = 0;
        m_qsnd[node_id]->post_file->previous_stream_id = m_cur_stream_id;
        m_qsnd[node_id]->post_file->stream_id = m_qsnd[node_id]->post_file->previous_stream_id + 4;
        m_previous_stream_id = m_qsnd[node_id]->post_file->previous_stream_id;
        m_cur_stream_id = m_qsnd[node_id]->post_file->stream_id;
        m_qsnd[node_id]->send_cb_ctx->last_interaction_time = picoquic_current_time();
        m_qsnd[node_id]->post_file->repeat_count = 0;

        if (ret != 0) {
            DBG_PRINTF("Cannot parse the specified scenario");
            return -1;
        }
        else {
            //LG << "initial again" << std::endl;
            //initialize callback ctx demo_stream for post file
            ret = picoquic_demo_client_initialize_context(m_qsnd[node_id]->send_cb_ctx, m_qsnd[node_id]->post_file,
                1, m_alpn, 1, 0);
        }

        if (ret == 0) {
            //reset callback
            picoquic_set_callback(m_qsnd[node_id]->cnx, picoquic_demo_client_callback, m_qsnd[node_id]->send_cb_ctx);
            //LG << "post_szie: " << m_qsnd[node_id]->send_cb_ctx->demo_stream->post_size << std::endl;
            if (m_qsnd[node_id]->cnx->alpn == NULL) {
                picoquic_demo_client_set_alpn_from_tickets(m_qsnd[node_id]->cnx, m_qsnd[node_id]->send_cb_ctx, m_qsnd[node_id]->current_time);
                if (m_qsnd[node_id]->cnx->alpn != NULL) {
                    DBG_PRINTF("Set ALPN to %s based on stored ticket", m_qsnd[node_id]->cnx->alpn);
                }
            }

            /*Start tls initializing*/
            //ret = picoquic_start_client_cnx(m_qsnd[node_id]->cnx);

            if (ret == 0) {
                if (picoquic_is_0rtt_available(m_qsnd[node_id]->cnx) && (m_proposed_version & 0x0a0a0a0a) != 0x0a0a0a0a) {
                    //if ((m_proposed_version & 0x0a0a0a0a) != 0x0a0a0a0a) {
                    m_qsnd[node_id]->zero_rtt_avaliable = 1;
                    DBG_PRINTF("start 0-rtt transmission");
                    ret = picoquic_demo_client_start_streams(m_qsnd[node_id]->cnx, m_qsnd[node_id]->send_cb_ctx, m_qsnd[node_id]->post_file->previous_stream_id);
                }
                ret = picoquic_demo_client_start_streams(m_qsnd[node_id]->cnx, m_qsnd[node_id]->send_cb_ctx, m_qsnd[node_id]->post_file->previous_stream_id);
            }
        }
        /*Wait for packet from node*/
        while (ret == 0 && picoquic_get_cnx_state(m_qsnd[node_id]->cnx) != picoquic_state_disconnected) {
            int bytes_recv;

            m_qsnd[node_id]->from_length = m_qsnd[node_id]->to_length = sizeof(struct sockaddr_storage);
            uint64_t time_before = picoquic_current_time();
            uint64_t current_time = picoquic_current_time();
            int64_t delta_t = picoquic_get_next_wake_delay(m_quic_send_ctx, picoquic_current_time(), m_qsnd[node_id]->delay_max);

            /*Receiving packet from node*/
            bytes_recv = picoquic_select(m_qsnd[node_id]->qsocket, 1, &m_qsnd[node_id]->packet_from, &m_qsnd[node_id]->from_length,
                &m_qsnd[node_id]->packet_to, &m_qsnd[node_id]->to_length, &m_qsnd[node_id]->if_index_to, m_qsnd[node_id]->buffer, sizeof(m_qsnd[node_id]->buffer),
                delta_t, &current_time, m_quic_send_ctx);

            if (bytes_recv < 0) {
                ret = -1;
            }
            else {
                if (bytes_recv > 0) {
                    /*Decode packet*/
                    ret = picoquic_incoming_packet(m_quic_send_ctx, m_qsnd[node_id]->buffer, (size_t)bytes_recv,
                        (struct sockaddr*)&m_qsnd[node_id]->packet_from, (struct sockaddr*)&m_qsnd[node_id]->packet_to, m_qsnd[node_id]->if_index_to,
                        current_time, &m_qsnd[node_id]->new_context_created, nullptr);
                    if (ret != 0) ret = 0;

                    if (picoquic_get_cnx_state(m_qsnd[node_id]->cnx) == picoquic_state_client_almost_ready && m_qsnd[node_id]->notified_ready == 0) {
                        if (picoquic_tls_is_psk_handshake(m_qsnd[node_id]->cnx)) {
                            fprintf(stdout, "The session was properly resumed!\n");
                        }

                        if (m_qsnd[node_id]->cnx->zero_rtt_data_accepted) {
                            fprintf(stdout, "Zero RTT data is accepted!\n");
                        }

                        if (m_qsnd[node_id]->cnx->alpn != nullptr) {
                            fprintf(stdout, "Negotiated ALPN: %s\n", m_qsnd[node_id]->cnx->alpn);
                            m_qsnd[node_id]->saved_alpn = picoquic_string_duplicate(m_qsnd[node_id]->cnx->alpn);
                        }
                        fprintf(stdout, "Almost ready!\n");
                        m_qsnd[node_id]->notified_ready = 1;
                    }

                }
                if (ret == 0) {
                    uint64_t loop_time = picoquic_current_time();
                    while (ret == 0 && (m_qsnd[node_id]->cnx_next = picoquic_get_earliest_cnx_to_wake(m_quic_send_ctx, &m_qsnd[node_id]->cnx, loop_time)) != nullptr) {

                        //LG << "check out if stream file has been sent" << std::endl;
                        picoquic_stream_head* stream_tmp = picoquic_find_stream(m_qsnd[node_id]->cnx_next, m_qsnd[node_id]->post_file->stream_id, 0);
                        if (stream_tmp && stream_tmp->fin_sent) {
                            //LG << "stream file has been sent" << std::endl;
                            break;
                        }

                        ret = picoquic_prepare_packet(m_qsnd[node_id]->cnx_next, picoquic_current_time(),
                            m_qsnd[node_id]->send_buffer, sizeof(m_qsnd[node_id]->send_buffer),
                            &m_qsnd[node_id]->send_length, &m_qsnd[node_id]->qpath);
                        //DBG_PRINTF("prepare success");
                        if (ret == PICOQUIC_ERROR_DISCONNECTED) {
                            ret = 0;
                            printf("%" PRIx64 ": ", picoquic_val64_connection_id(picoquic_get_logging_cnxid(m_qsnd[node_id]->cnx_next)));
                            picoquic_log_time(stdout, m_qsnd[node_id]->cnx_next, picoquic_current_time(), "", " : ");
                            printf("Closed. Retrans= %d, spurious= %d, max sp gap = %d, max sp delay = %d\n",
                                (int)m_qsnd[node_id]->cnx_next->nb_retransmission_total, (int)m_qsnd[node_id]->cnx_next->nb_spurious,
                                (int)m_qsnd[node_id]->cnx_next->path[0]->max_reorder_gap, (int)m_qsnd[node_id]->cnx_next->path[0]->max_spurious_rtt);

                            if (m_qsnd[node_id]->cnx_next == m_qsnd[node_id]->cnx) {
                                m_qsnd[node_id]->cnx = nullptr;
                            }
                            picoquic_delete_cnx(m_qsnd[node_id]->cnx_next);
                            fflush(stdout);
                            break;
                        }
                        else if (ret == 0) {
                            int peer_addr_len = 0;
                            struct sockaddr* peer_addr;
                            int local_addr_len = 0;
                            struct sockaddr* local_addr;
                            if (m_qsnd[node_id]->send_length > 0) {
                                /*
                                {
                                    std::lock_guard<std::mutex> lock(m_mu_);
                                    //LG << "After Preparing CNX-STATE: " << m_qsnd[node_id]->cnx->cnx_state << std::endl;
                                }
                                */
                                picoquic_get_peer_addr(m_qsnd[node_id]->qpath, &peer_addr, &peer_addr_len);
                                picoquic_get_local_addr(m_qsnd[node_id]->qpath, &local_addr, &local_addr_len);
                                picoquic_before_sending_packet(m_qsnd[node_id]->cnx_next, *m_qsnd[node_id]->qsocket);
                                bytes_sent = picoquic_sendmsg(*m_qsnd[node_id]->qsocket, peer_addr, peer_addr_len, local_addr,
                                    local_addr_len, picoquic_get_local_if_index(m_qsnd[node_id]->qpath),
                                    (const char*)m_qsnd[node_id]->send_buffer, (int)m_qsnd[node_id]->send_length);
                                if (bytes_sent == -1) {
                                    std::cerr << "Sending failed with error: " << strerror(errno) << std::endl;
                                    // 可根据 errno 的值进行不同的处理
                                }
                                else {
                                    std::lock_guard<std::mutex> lock(m_mu_);
                                    //LG << "Sending " << bytes_sent << " bytes successfully." << std::endl;
                                    //DBG_PRINTF("%d bytes have been sent", bytes_sent);
                                }
                            }
                            else {
                                //LG << "send_length: " << m_qsnd[node_id]->send_length << ", and break" << std::endl;
                                DBG_PRINTF("|Break-Info|Send_length:%d|", m_qsnd[node_id]->send_length);
                                break;
                            }
                        }
                        else {
                            //LG << "return:" << ret << ", error and break" << std::endl;
                            DBG_PRINTF("|Break-Info|Return:%d|", ret);
                            break;
                        }
                    }
                }
            }
            if (*m_qsnd[node_id]->post_file->post_sent >= m_qsnd[node_id]->post_file->post_size) {
                //LG << "check out if ack of msg has been rcvd" << std::endl;
                //DBG_PRINTF("check out if ack of msg has been rcvd");
                picoquic_stream_head* stream_tmp = picoquic_find_stream(m_qsnd[node_id]->cnx, m_qsnd[node_id]->post_file->stream_id, 0);
                if (stream_tmp && stream_tmp->fin_received) {
                    //LG << m_qsnd[node_id]->post_file->post_size << " bytes have been sent successfully" << std::endl;
                    DBG_PRINTF("|Success-Info|Post Size:%d|", *m_qsnd[node_id]->post_file->post_sent);
                    post_sent = *m_qsnd[node_id]->post_file->post_sent;
                    free(m_qsnd[node_id]->post_file);
                    //LG << "CNX STATE: " << m_qsnd[node_id]->cnx->cnx_state;
                    break;
                }
            }
        }
        return post_sent;
    }

    int QUICVan::SendMsg(Message& msg, Node* node, bool connect) {
        std::lock_guard<std::mutex> lk(m_mu_tmp_);
        int send_bytes = 0;
        int bytes_sent = 0;

        if (connect) {
            int post_sent = 0;
            CHECK_NE(node->id, node->kEmpty);
            CHECK_NE(node->port, node->kEmpty);
            CHECK(node->hostname.size());
            //Don't need to connect to the node with same role
            if ((node->role == my_node_.role) && (node->id != my_node_.id)) {
                DBG_PRINTF("Dest_node has the same role with cur_node, return");
                return 0;
            }
            DBG_PRINTF("Start connecting to node %d", node->id);
            int ret = 0;
            int peer_addr_length = 0;
            int is_name = 0;
            size_t post_file_nb = 0;
            uint64_t current_time = picoquic_current_time();
            picoquic_cnx_t* cnx_tmp;
            picoquic_connection_id_t cnx_id = picoquic_null_connection_id;
            struct sockaddr_storage peer_address;

            ret = picoquic_get_server_address(node->hostname.c_str(), node->port, &peer_address, &peer_addr_length, &is_name);

            /*Get server address and create connection*/
            if (ret == 0) {
                //the peer_address is now network sequence
                cnx_tmp = picoquic_create_cnx(m_quic_send_ctx, picoquic_null_connection_id, picoquic_null_connection_id,
                    (struct sockaddr*)&peer_address, current_time, m_proposed_version, m_sni, m_alpn, 1);
                //DBG_PRINTF("Send cnx_ctx %p has been created", cnx_tmp);
                DBG_PRINTF("|Destination_address:%s|Destination_port:%d|Connection_state:%d|", node->hostname.c_str(), node->port, cnx_tmp->cnx_state);
                DBG_PRINTF("|Current_address:%s|Current_port:%d|Connection_state:%d|", my_node_.hostname.c_str(), my_node_.port, cnx_tmp->cnx_state);
                cnx_tmp->dml = 0;
            }
            cnx_id = cnx_tmp->path[0]->local_cnxid;
            std::string node_id = ByteArrayToString(cnx_id.id, cnx_id.id_len);
            snd_nid2cnx[node->id] = node_id;
            snd_cnx2nid[node_id] = node->id;
            DBG_PRINTF("|node_id:%s|", node_id.c_str());

            /*Check whether the cnx exists*/
            if (!m_qsnd.count(node_id)) {
                m_qsnd[node_id] = new QUICCnxParameter;
                memset(m_qsnd[node_id], 0, sizeof(QUICCnxParameter));
                m_qsnd[node_id]->packet_from.ss_family = AF_INET;
                m_qsnd[node_id]->from_length = sizeof(sockaddr_storage);
                m_qsnd[node_id]->packet_to.ss_family = AF_INET;
                m_qsnd[node_id]->to_length = sizeof(sockaddr_storage);
                m_qsnd[node_id]->peer_address.ss_family = AF_INET;
                m_qsnd[node_id]->peer_addr_length = peer_addr_length;
                m_qsnd[node_id]->cnx = cnx_tmp;
                m_qsnd[node_id]->current_time = current_time;
                m_qsnd[node_id]->peer_address = peer_address;
                m_qsnd[node_id]->delay_max = 10000000;
                m_qsnd[node_id]->if_index_to = 0;
                m_qsnd[node_id]->new_context_created = 0;
                m_qsnd[node_id]->client_receive_loop = 0;
                m_qsnd[node_id]->qsocket = new int;
                ret = CreateQUICSocket(m_qsnd[node_id]->qsocket);
                //DBG_PRINTF("Send_Socket %d has been created", *m_qsnd[node_id]->qsocket);
            }

            // prepare msg
            int meta_size; char* meta_buf;
            msg.meta.sender = my_node_.id;
            int n = msg.data.size();
            msg.meta.more = n;
            PackMeta(msg.meta, &meta_buf, &meta_size);
            DBG_PRINTF("|Msg-Info|CMD:%d|Receiver:%d|Sender:%d|Node_id:%s|Meta Size:%d|Data Size:%d|", msg.meta.control.cmd, msg.meta.recver, msg.meta.sender, node_id.c_str(), meta_size, n);

            /*create connection parameters*/
            m_qsnd[node_id]->send_cb_ctx = new picoquic_demo_callback_ctx_t;
            memset(m_qsnd[node_id]->send_cb_ctx, 0, sizeof(picoquic_demo_callback_ctx_t));
            m_qsnd[node_id]->send_cb_ctx->last_interaction_time = m_qsnd[node_id]->current_time;
            m_qsnd[node_id]->client_receive_loop = 0;

            //create postfile for pushing data
            m_qsnd[node_id]->post_file = (picoquic_demo_stream_desc_t*)malloc(sizeof(picoquic_demo_stream_desc_t));
            ret = demo_client_parse_scenario_desc(test_scenario_default, &post_file_nb, &m_qsnd[node_id]->post_file);
            m_qsnd[node_id]->post_file->msg_data = &meta_buf;//传入二级指针，因为最后会更新msg的地址，故而传入了二级指针；
            m_qsnd[node_id]->post_file->post_size = (uint64_t)meta_size;
            m_qsnd[node_id]->post_file->post_sent = new size_t;
            *m_qsnd[node_id]->post_file->post_sent = 0;
            m_qsnd[node_id]->post_file->previous_stream_id = PICOQUIC_DEMO_STREAM_ID_INITIAL;
            m_qsnd[node_id]->post_file->stream_id = 0;
            m_previous_stream_id = m_qsnd[node_id]->post_file->previous_stream_id;
            m_cur_stream_id = m_qsnd[node_id]->post_file->stream_id;
            m_qsnd[node_id]->post_file->repeat_count = 0;

            if (ret != 0) {
                DBG_PRINTF("Cannot parse the specified scenario");
                return -1;
            }
            else {
                //initialize callback ctx demo_stream for post file
                ret = picoquic_demo_client_initialize_context(m_qsnd[node_id]->send_cb_ctx, m_qsnd[node_id]->post_file,
                    1, m_alpn, 1, 0);
            }

            /*start init packet sending*/
            if (ret == 0) {

                picoquic_set_callback(m_qsnd[node_id]->cnx, picoquic_demo_client_callback, m_qsnd[node_id]->send_cb_ctx);

                if (m_qsnd[node_id]->cnx->alpn == NULL) {
                    picoquic_demo_client_set_alpn_from_tickets(m_qsnd[node_id]->cnx, m_qsnd[node_id]->send_cb_ctx, m_qsnd[node_id]->current_time);
                    if (m_qsnd[node_id]->cnx->alpn != NULL) {
                        DBG_PRINTF("Set ALPN to %s based on stored ticket", m_qsnd[node_id]->cnx->alpn);
                    }
                }

                /*Start tls initializing*/
                ret = picoquic_start_client_cnx(m_qsnd[node_id]->cnx);

                if (ret == 0) {
                    if (picoquic_is_0rtt_available(m_qsnd[node_id]->cnx) && (m_proposed_version & 0x0a0a0a0a) != 0x0a0a0a0a) {
                        m_qsnd[node_id]->zero_rtt_avaliable = 1;
                        DBG_PRINTF("start 0-rtt transmission");
                        ret = picoquic_demo_client_start_streams(m_qsnd[node_id]->cnx, m_qsnd[node_id]->send_cb_ctx, PICOQUIC_DEMO_STREAM_ID_INITIAL);
                    }

                    /*Prepare init packet for connection creation*/
                    if (ret == 0) {
                        ret = picoquic_prepare_packet(m_qsnd[node_id]->cnx, picoquic_current_time(),
                            m_qsnd[node_id]->send_buffer, sizeof(m_qsnd[node_id]->send_buffer), &m_qsnd[node_id]->send_length, &m_qsnd[node_id]->qpath);
                    }
                    //DBG_PRINTF("|Connection_state:%d|", m_qsnd[node_id]->cnx->cnx_state);
                    /*Send init packet*/
                    if (ret == 0 && m_qsnd[node_id]->send_length > 0) {
                        //std::cout << "m_qsnd[node_id]->cnx: " << m_qsnd[node_id]->cnx << std::endl;
                            /*
                            {
                                std::lock_guard<std::mutex> lock(m_mu_);
                                LG << "Prepare to send sender's initial packet using socket: " << *m_qsnd[node_id]->qsocket << std::endl;
                                LG << m_qsnd[node_id]->send_length << " bytes need to be sent" << std::endl;
                            }
                            */
                            //DBG_PRINTF("%d bytes need to be sent", m_qsnd[node_id]->send_length);
                        picoquic_before_sending_packet(m_qsnd[node_id]->cnx, *m_qsnd[node_id]->qsocket);
                        /* The first packet must be a sendto, next ones, not necessarily! */
                        bytes_sent = sendto(*m_qsnd[node_id]->qsocket, m_qsnd[node_id]->send_buffer, (int)m_qsnd[node_id]->send_length, 0,
                            (struct sockaddr*)&m_qsnd[node_id]->peer_address, m_qsnd[node_id]->peer_addr_length);
                        if (bytes_sent == -1) {
                            std::cerr << "Sendto failed with error: " << strerror(errno) << std::endl;
                            // 可根据 errno 的值进行不同的处理
                        }
                        else {
                            //DBG_PRINTF("%d bytes have been sent successfully", bytes_sent);
                        }
                    }
                }
            }

            /*Wait for packet from node*/
            while (ret == 0 && picoquic_get_cnx_state(m_qsnd[node_id]->cnx) != picoquic_state_disconnected) {
                //LG << "waiting for server init packet" << std::endl;
                int bytes_recv;

                m_qsnd[node_id]->from_length = m_qsnd[node_id]->to_length = sizeof(struct sockaddr_storage);
                uint64_t time_before = picoquic_current_time();
                uint64_t current_time = picoquic_current_time();
                int64_t delta_t = picoquic_get_next_wake_delay(m_quic_send_ctx, picoquic_current_time(), m_qsnd[node_id]->delay_max);

                /*Receiving packet from node*/
                bytes_recv = picoquic_select(m_qsnd[node_id]->qsocket, 1, &m_qsnd[node_id]->packet_from, &m_qsnd[node_id]->from_length,
                    &m_qsnd[node_id]->packet_to, &m_qsnd[node_id]->to_length, &m_qsnd[node_id]->if_index_to, m_qsnd[node_id]->buffer, sizeof(m_qsnd[node_id]->buffer),
                    delta_t, &current_time, m_quic_send_ctx);

                if (bytes_recv < 0) {
                    ret = -1;
                }
                else {
                    if (bytes_recv > 0) {
                        /*Decode packet*/
                        ret = picoquic_incoming_packet(m_quic_send_ctx, m_qsnd[node_id]->buffer, (size_t)bytes_recv,
                            (struct sockaddr*)&m_qsnd[node_id]->packet_from, (struct sockaddr*)&m_qsnd[node_id]->packet_to, m_qsnd[node_id]->if_index_to,
                            current_time, &m_qsnd[node_id]->new_context_created, &cnx_id);
                        if (ret != 0) ret = 0;

                        if (picoquic_get_cnx_state(m_qsnd[node_id]->cnx) == picoquic_state_client_almost_ready && m_qsnd[node_id]->notified_ready == 0) {
                            if (picoquic_tls_is_psk_handshake(m_qsnd[node_id]->cnx)) {
                                fprintf(stdout, "The session was properly resumed!\n");
                            }

                            if (m_qsnd[node_id]->cnx->zero_rtt_data_accepted) {
                                fprintf(stdout, "Zero RTT data is accepted!\n");
                            }

                            if (m_qsnd[node_id]->cnx->alpn != nullptr) {
                                fprintf(stdout, "Negotiated ALPN: %s\n", m_qsnd[node_id]->cnx->alpn);
                                m_qsnd[node_id]->saved_alpn = picoquic_string_duplicate(m_qsnd[node_id]->cnx->alpn);
                            }
                            fprintf(stdout, "Almost ready!\n");
                            m_qsnd[node_id]->notified_ready = 1;
                        }
                    }
                    if (ret == 0) {
                        uint64_t loop_time = picoquic_current_time();
                        while (ret == 0 && (m_qsnd[node_id]->cnx_next = picoquic_get_earliest_cnx_to_wake(m_quic_send_ctx, &m_qsnd[node_id]->cnx, loop_time)) != nullptr) {

                            //LG << "check out if stream file has been sent" << std::endl;
                            picoquic_stream_head* stream_tmp = picoquic_find_stream(m_qsnd[node_id]->cnx_next, m_qsnd[node_id]->post_file->stream_id, 0);
                            if (stream_tmp && stream_tmp->fin_sent) {
                                //LG << "stream file has been sent" << std::endl;
                                break;
                            }

                            if (ret == 0 && picoquic_get_cnx_state(m_qsnd[node_id]->cnx_next) == picoquic_state_client_ready) {
                                //LG << "established:%d" << m_qsnd[node_id]->established << std::endl;
                                if (m_qsnd[node_id]->established == 0) {
                                    DBG_PRINTF("|Connection established|Version:%x|I-CID:%|" PRIx64,
                                        picoquic_supported_versions[m_qsnd[node_id]->cnx_next->version_index].version,
                                        picoquic_val64_connection_id(picoquic_get_logging_cnxid(m_qsnd[node_id]->cnx_next)));//获得connectionID
                                    m_qsnd[node_id]->established = 1;
                                    if (m_qsnd[node_id]->zero_rtt_avaliable == 0) {
                                        ret = picoquic_demo_client_start_streams(m_qsnd[node_id]->cnx_next, m_qsnd[node_id]->send_cb_ctx, PICOQUIC_DEMO_STREAM_ID_INITIAL);//开启预设的所有流
                                        if (picoquic_is_0rtt_available(m_qsnd[node_id]->cnx_next)) DBG_PRINTF("0_RTT available");
                                    }
                                }
                            }

                            ret = picoquic_prepare_packet(m_qsnd[node_id]->cnx_next, picoquic_current_time(),
                                m_qsnd[node_id]->send_buffer, sizeof(m_qsnd[node_id]->send_buffer),
                                &m_qsnd[node_id]->send_length, &m_qsnd[node_id]->qpath);

                            if (ret == PICOQUIC_ERROR_DISCONNECTED) {
                                ret = 0;
                                printf("%" PRIx64 ": ", picoquic_val64_connection_id(picoquic_get_logging_cnxid(m_qsnd[node_id]->cnx_next)));
                                picoquic_log_time(stdout, m_qsnd[node_id]->cnx_next, picoquic_current_time(), "", " : ");
                                printf("Closed. Retrans= %d, spurious= %d, max sp gap = %d, max sp delay = %d\n",
                                    (int)m_qsnd[node_id]->cnx_next->nb_retransmission_total, (int)m_qsnd[node_id]->cnx_next->nb_spurious,
                                    (int)m_qsnd[node_id]->cnx_next->path[0]->max_reorder_gap, (int)m_qsnd[node_id]->cnx_next->path[0]->max_spurious_rtt);

                                if (m_qsnd[node_id]->cnx_next == m_qsnd[node_id]->cnx) {
                                    m_qsnd[node_id]->cnx = nullptr;
                                }
                                picoquic_delete_cnx(m_qsnd[node_id]->cnx_next);
                                fflush(stdout);
                                break;
                            }
                            else if (ret == 0) {
                                int peer_addr_len = 0;
                                struct sockaddr* peer_addr;
                                int local_addr_len = 0;
                                struct sockaddr* local_addr;
                                if (m_qsnd[node_id]->send_length > 0) {
                                    /*
                                    {
                                        std::lock_guard<std::mutex> lock(m_mu_);
                                        LG << "After Preparing CNX-STATE: " << m_qsnd[node_id]->cnx_next->cnx_state << std::endl;
                                    }
                                    */
                                    //DBG_PRINTF("|Connection_state:%d|", m_qsnd[node_id]->cnx_next->cnx_state);
                                    //DBG_PRINTF("%d bytes need to be sent", m_qsnd[node_id]->send_length);
                                    picoquic_get_peer_addr(m_qsnd[node_id]->qpath, &peer_addr, &peer_addr_len);
                                    picoquic_get_local_addr(m_qsnd[node_id]->qpath, &local_addr, &local_addr_len);
                                    picoquic_before_sending_packet(m_qsnd[node_id]->cnx_next, *m_qsnd[node_id]->qsocket);
                                    bytes_sent = picoquic_sendmsg(*m_qsnd[node_id]->qsocket, peer_addr, peer_addr_len, local_addr,
                                        local_addr_len, picoquic_get_local_if_index(m_qsnd[node_id]->qpath),
                                        (const char*)m_qsnd[node_id]->send_buffer, (int)m_qsnd[node_id]->send_length);
                                    if (bytes_sent == -1) {
                                        std::cerr << "Sending failed with error: " << strerror(errno) << std::endl;
                                        // 可根据 errno 的值进行不同的处理
                                    }
                                    else {
                                        //std::lock_guard<std::mutex> lock(m_mu_);
                                        //DBG_PRINTF("%d bytes have been sent successfully", bytes_sent);
                                    }
                                }
                                else {
                                    break;
                                }
                            }
                            else {
                                break;
                            }
                        }
                    }
                }
                if (*m_qsnd[node_id]->post_file->post_sent >= m_qsnd[node_id]->post_file->post_size) {
                    //LG << "check out if ack of msg has been rcvd" << std::endl;
                    picoquic_stream_head* stream_tmp = picoquic_find_stream(m_qsnd[node_id]->cnx, m_qsnd[node_id]->post_file->stream_id, 0);
                    if (stream_tmp && stream_tmp->fin_received) {
                        //DBG_PRINTF("%d bytes have been post successfully", *m_qsnd[node_id]->post_file->post_sent);
                        DBG_PRINTF("|Success-Info|Connecting Node:%d|", node->id);
                        post_sent = *m_qsnd[node_id]->post_file->post_sent;
                        free(m_qsnd[node_id]->post_file);
                        //LG << "CNX STATE: " << m_qsnd[node_id]->cnx->cnx_state;
                        break;
                    }
                }
            }
            return post_sent;
        }
        else
        {
            int id = msg.meta.recver;
            CHECK_NE(id, Meta::kEmpty);
            auto it = snd_nid2cnx.find(id);
            if (it == snd_nid2cnx.end()) {
                //LG << "There is no connection to node " << id;
                DBG_PRINTF("No connection to node %d", id);
                return -1;
            }
            DBG_PRINTF("Start sending msg to node %d", id);
            std::string node_id = it->second;
            //LG << "Start sending msg using socket: " << *m_qsnd[node_id]->qsocket << std::endl;
            //LG << "cmd: "<< cmd << std::endl;
            //LG << "recver: " << id << std::endl;
            // prepare msg meta
            int meta_size; char* meta_buf;
            msg.meta.sender = my_node_.id;
            int n = msg.data.size();
            msg.meta.more = n;
            if (msg.meta.pull) {

            }
            PackMeta(msg.meta, &meta_buf, &meta_size);
            //LG << "There are " << meta_size << " bytes of Meta need to be sent" << std::endl;
            //LG << "And " << n << " bytes of msg data need too" << std::endl;
            DBG_PRINTF("|Msg-Info|CMD:%d|Receiver:%d|Sender:%d|Node_id:%s|Meta Size:%d|Data Size:%d|", msg.meta.control.cmd, msg.meta.recver, msg.meta.sender, node_id.c_str(), meta_size, n);

            bytes_sent += QUICMsgSend(meta_buf, &meta_size, node_id);
            send_bytes += meta_size;

            // send kvpairs data one by one
            for (int i = 0; i < n; ++i) {
                SArray<char>* data = new SArray<char>(msg.data[i]);
                int data_size = data->size();
                DBG_PRINTF("Sending data[%d]", i);
                bytes_sent += QUICMsgSend(data->data(), &data_size, node_id);
                send_bytes += data_size;
            }

            //If the actual number of bytes sent is not equal to the number of bytes that need to be sent
            if (bytes_sent != send_bytes) {
                //std::cout << "actual number of bytes sent is not equal to the number of bytes that need to be sent" << std::endl;
                DBG_PRINTF("|Break-Info|Actual number of bytes sent is not equal to the number of bytes that need to be sent");
                return -1;
            }
            return bytes_sent;
        }
    }

    int QUICVan::QUICMsgRcv(char** msg_data, int& msg_rcvd) {
        DBG_PRINTF("|Receive-Info|");
        picoquic_connection_id_t cnx_id = picoquic_null_connection_id;
        std::string node_id;
        int ret = 0;
        int bytes_recv = 0;
        int msg_length = 0;
        int rcv_loop = 0;

        //start receving
        while (true)
        {
            uint64_t time_before = picoquic_current_time();
            uint64_t current_time = picoquic_current_time();
            int64_t delta_t = 0;
            m_from_length = m_to_length = sizeof(struct sockaddr_storage);
            m_if_index_to = 0;

            //receive singal packet
            bytes_recv = picoquic_select(m_quic_receiver, 1, &m_packet_from, &m_from_length, &m_packet_to, &m_to_length,
                &m_if_index_to, m_buffer, sizeof(m_buffer), delta_t, &current_time, m_quic_rcv_ctx);
            if (bytes_recv < 0) {
                ret = -1;
            }
            else {
                if (bytes_recv > 0) {
                    //handling buffer data to packet
                    //DBG_PRINTF("Test 1");
                    ret = picoquic_incoming_packet(m_quic_rcv_ctx, m_buffer, (size_t)bytes_recv,
                        (struct sockaddr*)&m_packet_from, (struct sockaddr*)&m_packet_to,
                        m_if_index_to, current_time, &m_new_context_created, &cnx_id);
                    rcv_loop++;

                    //build a cnx if it doesn't exist 
                    node_id = ByteArrayToString(cnx_id.id, cnx_id.id_len);
                    if (!m_qrcv.count(node_id)) {
                        /*
                        {
                            std::lock_guard<std::mutex> lock(m_mu_);
                            LG << "Didn't find cnx_ctx: " << node_id << ", start building it" << std::endl;
                        }
                        */
                        DBG_PRINTF("Creating receive cnx_ctx for node_id %s", node_id.c_str());

                        m_qrcv[node_id] = new QUICCnxParameter;
                        memset(m_qrcv[node_id], 0, sizeof(QUICCnxParameter));
                        m_qrcv[node_id]->packet_from.ss_family = AF_INET;
                        m_qrcv[node_id]->from_length = sizeof(sockaddr_storage);
                        m_qrcv[node_id]->packet_to.ss_family = AF_INET;
                        m_qrcv[node_id]->to_length = sizeof(sockaddr_storage);
                        m_qrcv[node_id]->peer_address.ss_family = AF_INET;
                        m_qrcv[node_id]->peer_addr_length = sizeof(sockaddr_storage);
                        m_qrcv[node_id]->new_context_created = m_new_context_created;

                        //create cnx_ctx through initial packet
                        if (m_qrcv[node_id]->new_context_created) {
                            m_qrcv[node_id]->cnx = picoquic_get_first_cnx(m_quic_rcv_ctx);//这一步是否会出现问题。quic中存在多个连接时还能用这套方案取出链接吗？
                            m_qrcv[node_id]->cnx->dml = 0;
                            printf("%" PRIx64 ": ", picoquic_val64_connection_id(picoquic_get_logging_cnxid(m_qrcv[node_id]->cnx)));
                            picoquic_log_time(stdout, m_qrcv[node_id]->cnx, picoquic_current_time(), "", " : ");
                            printf("Connection established, state = %d, from length: %u\n",
                                picoquic_get_cnx_state(picoquic_get_first_cnx(m_quic_rcv_ctx)), m_from_length);
                            memset(&m_qrcv[node_id]->peer_address, 0, sizeof(m_qrcv[node_id]->peer_address));
                            memcpy(&m_qrcv[node_id]->peer_address, &m_packet_from, m_from_length);
                            print_address((struct sockaddr*)&m_qrcv[node_id]->peer_address, "Client address:",
                                picoquic_get_logging_cnxid(m_qrcv[node_id]->cnx));
                            //picoquic_log_transport_extension(stdout, m_qrcv[node_id]->cnx, 1);
                        }
                    }
                    delta_t = 0;
                }
                if (m_qrcv.count(node_id) && (picoquic_get_cnx_state(m_qrcv[node_id]->cnx) == picoquic_state_server_almost_ready
                    || bytes_recv == 0 || (ret == 0 && rcv_loop > 4))) {
                    rcv_loop = 0;

                    if (ret == 0) {
                        m_qrcv[node_id]->send_length = 1536;
                        ret = picoquic_prepare_packet(m_qrcv[node_id]->cnx, picoquic_current_time(),
                            m_qrcv[node_id]->send_buffer, sizeof(m_qrcv[node_id]->send_buffer), &m_qrcv[node_id]->send_length,
                            &m_qrcv[node_id]->qpath);

                        if (ret == 0 && m_qrcv[node_id]->send_length > 0) {
                            int peer_addr_len = 0;
                            struct sockaddr* peer_addr = new sockaddr;
                            int local_addr_len = 0;
                            struct sockaddr* local_addr = new sockaddr;
                            picoquic_get_peer_addr(m_qrcv[node_id]->qpath, &peer_addr, &peer_addr_len);
                            picoquic_get_local_addr(m_qrcv[node_id]->qpath, &local_addr, &local_addr_len);
                            picoquic_before_sending_packet(m_qrcv[node_id]->cnx, *m_quic_receiver);
                            int bytes_sent = picoquic_send_through_server_sockets_for_ps(m_quic_receiver,
                                peer_addr, peer_addr_len, local_addr, local_addr_len,
                                picoquic_get_local_if_index(m_qrcv[node_id]->qpath),
                                (const char*)m_qrcv[node_id]->send_buffer, (int)m_qrcv[node_id]->send_length);
                            if (bytes_sent == -1) {
                                std::cerr << "Sending normal pkt with error: " << strerror(errno) << std::endl;
                                // 可根据 errno 的值进行不同的处理
                            }
                            else {
                                std::lock_guard<std::mutex> lock(m_mu_);
                                //LG << "Sending " << bytes_sent << " bytes successfully." << std::endl;
                            }
                        }
                    }
                    delta_t = picoquic_get_next_wake_delay(m_quic_rcv_ctx, picoquic_current_time(), m_qrcv[node_id]->delay_max);
                }
                //if msg all received
                if (m_qrcv.count(node_id) && picoquic_get_msg_state(m_qrcv[node_id]->cnx)) {
                    *msg_data = (char*)malloc(m_qrcv[node_id]->cnx->msg_size);
                    memcpy(*msg_data, m_qrcv[node_id]->cnx->msg_data, m_qrcv[node_id]->cnx->msg_size);
                    //*msg_data = m_qrcv[node_id]->cnx->msg_data;
                    msg_length = m_qrcv[node_id]->cnx->msg_size;
                    //DBG_PRINTF("msg_data addr:%p | data:%c", *msg_data, *msg_data);
                    picoquic_set_msg_state(m_qrcv[node_id]->cnx, 0);
                    /*
                    {
                        std::lock_guard<std::mutex> lock(m_mu_);
                        LG << "Msg " << msg_length << " bytes received!" << std::endl;
                    }
                    */
                    DBG_PRINTF("|Msg Received:%d|", msg_length);
                    //free(m_qrcv[node_id]->cnx->msg_data);
                    //DBG_PRINTF("old msg_data addr:%p", m_qrcv[node_id]->cnx->msg_data);
                    memset(m_qrcv[node_id]->cnx->msg_data, 0, m_qrcv[node_id]->cnx->msg_capacity);
                    //m_qrcv[node_id]->cnx->msg_data = (char*)malloc(1536);
                    //DBG_PRINTF("new msg_data addr:%p", m_qrcv[node_id]->cnx->msg_data);
                    picoquic_reset_msg_size(m_qrcv[node_id]->cnx);
                    msg_rcvd = 1;
                    break;
                }
            }
        }
        return msg_length;
    }

    int QUICVan::RecvMsg(Message* msg) {
        msg->data.clear();
        int msg_length;
        int msg_rcvd = 0;

        for (int i = 0; ; ++i) {
            msg_length = 0;
            //char* msg_data = new char;
            char* msg_data = nullptr;
            //DBG_PRINTF("msg_data:%p", msg_data);

            msg_length = QUICMsgRcv(&msg_data, msg_rcvd);
            if (msg_length > 0 || msg_rcvd == 1) {
                if (i == 0) {
                    //LG << "prepare unpack meta" << std::endl;
                    DBG_PRINTF("Unpacking Meta");
                    UnpackMeta(msg_data, msg_length, &(msg->meta));
                    msg->meta.recver = my_node_.id;
                    if (msg_data) {
                        free(msg_data);
                        msg_data = nullptr;
                    }
                    DBG_PRINTF("|Msg Number:%d|Sender:%d|", msg->meta.more, msg->meta.sender);
                    if (msg->meta.more == 0) break;
                }
                else {
                    SArray<char> data;
                    data.reset(msg_data, msg_length, [msg_data, msg_length](char* buf) {
                        if (msg_data) free(msg_data);
                        });
                    msg_data = nullptr;
                    msg->data.push_back(data);
                    DBG_PRINTF("|Msg Number:%d|Current Number:%d|", msg->meta.more, i);
                    if (i == msg->meta.more) {
                        break;
                    }
                }
            }
        }
        return msg_length;
    }

    std::string QUICVan::ByteArrayToString(const uint8_t* byte_array, size_t length) {
        std::string result;

        // 将字节数组的每个字节转换成字符并拼接
        for (size_t i = 0; i < length; ++i) {
            result += std::to_string(byte_array[i]);
        }

        return result;
    }

    void QUICVan::UpdateNodeID(int old_id, int new_id) {
        if (snd_nid2cnx.count(old_id)) {
            snd_nid2cnx[new_id] = snd_nid2cnx[old_id];
        }
        else {
            LG << "There is no connection to node " << old_id;
        }
    }
}