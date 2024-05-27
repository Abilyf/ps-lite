/**
 *  Copyright (c) 2015 by Contributors
 */

#include <chrono>
#include <thread>

#include "ps/base.h"
#include "ps/internal/customer.h"
#include "ps/internal/postoffice.h"
#include "ps/internal/van.h"
#include "ps/sarray.h"

#include "./meta.pb.h"
#include "network_utils.h"
#include "ibverbs_van.h"
#include "resender.h"
#include "zmq_van.h"
#include "p3_van.h"
#include "quic_van.h"

namespace ps {

    // interval in second between to heartbeast signals. 0 means no heartbeat.
    // don't send heartbeast in default. because if the scheduler received a
    // heartbeart signal from a node before connected to that node, then it could be
    // problem.
    static const int kDefaultHeartbeatInterval = 0;

    Van* Van::Create(const std::string& type) {
        if (type == "zmq") {
            return new ZMQVan();
        }
        else if (type == "p3") {
            return new P3Van();
#ifdef DMLC_USE_IBVERBS
        }
        else if (type == "ibverbs") {
            return new IBVerbsVan();
#endif
        }
        else if (type == "quic") {
            return new QUICVan();
        }
        else {
            LOG(FATAL) << "Unsupported van type: " << type;
            return nullptr;
        }
    }

    void Van::ProcessTerminateCommand() {
        PS_VLOG(1) << my_node().ShortDebugString() << " is stopped";
        ready_ = false;//这一步是关键，将其设为false即意味着van有问题需终止
    }

    /*
    void Van::ProcessAddNodeCommandAtScheduler(Message* msg, Meta* nodes,
        Meta* recovery_nodes) {//这一步后续的执行，是要等到nodes中的数量等于预设数量时
        recovery_nodes->control.cmd = Control::ADD_NODE;
        time_t t = time(NULL);
        //这里的num_nodes指的是初始化时所指定的server和worker数量
        size_t num_nodes = Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();
        if (nodes->control.node.size() == num_nodes) { //这里的nodes是传进来的参数，系统启动阶段时，经过update之后，里面存有的数量应和配置时一致
            // sort the nodes according their ip and port,
            std::sort(nodes->control.node.begin(), nodes->control.node.end(),//根据节点的ip和port进行排序
                [](const Node& a, const Node& b) {
                    return (a.hostname.compare(b.hostname) | (a.port < b.port)) > 0;
                });
            // assign node rank
            for (auto& node : nodes->control.node) {
                std::string node_host_ip =
                    node.hostname + ":" + std::to_string(node.port);
                if (connected_nodes_.find(node_host_ip) == connected_nodes_.end()) {//判断该节点是否没有连接
                    CHECK_EQ(node.id, Node::kEmpty);
                    int id = node.role == Node::SERVER //分配一个正式的id
                        ? Postoffice::ServerRankToID(num_servers_)
                        : Postoffice::WorkerRankToID(num_workers_);
                    //LG << "assign rank=" << id << " to node " << node.DebugString() << std::endl;
                    DBG_PRINTF("assign rank %d to node %s", id, node.DebugString().c_str());
                    node.id = id;//分配该id
                    Message msg;
                    msg.meta.recver = node.id;
                    msg.meta.sender = my_node_.id;
                    msg.meta.control.cmd = Control::CONNECT; //给scheduler发送ADD_NODE控制信息，让scheduler节点注册Msg中携带的节点信息
                    //Connect(node);//scheduler节点连接该节点
                    //LG << "prepare to connect to node " << id << std::endl;
                    DBG_PRINTF("Prepare to connect to node %d", id);
                    Send(msg, &node, 1);
                    Postoffice::Get()->UpdateHeartbeat(node.id, t);
                    connected_nodes_[node_host_ip] = id;//更新该节点为已经连接的节点
                }
                else {//如果已经连接
                    int id = node.role == Node::SERVER
                        ? Postoffice::ServerRankToID(num_servers_)
                        : Postoffice::WorkerRankToID(num_workers_);
                    shared_node_mapping_[id] = connected_nodes_[node_host_ip];
                    node.id = connected_nodes_[node_host_ip];//更新该节点
                }
                if (node.role == Node::SERVER) num_servers_++;
                if (node.role == Node::WORKER) num_workers_++;
            }
            nodes->control.node.push_back(my_node_);
            nodes->control.cmd = Control::ADD_NODE;
            Message back;
            back.meta = *nodes;
            for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {//向所有的节点发送add_node信息
                int recver_id = r;
                if (shared_node_mapping_.find(r) == shared_node_mapping_.end()) {
                    back.meta.recver = recver_id;
                    back.meta.timestamp = timestamp_++;
                    //LG << "prepare to send back msg to node " << back.meta.recver << std::endl;
                    DBG_PRINTF("Prepare to send back msg to node %d", back.meta.recver);
                    Send(back, nullptr, 0);
                }
            }
            //LG << "the scheduler is connected to " << num_workers_ << " workers and " << num_servers_ << " servers";
            DBG_PRINTF("The scheduler is connected to %d workers and %d servers", num_workers_, num_servers_);
            ready_ = true;
        }
        else if (!recovery_nodes->control.node.empty()) { //如果节点没有收集完，
            auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_);//查出超时而被设定为dead的节点
            std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
            // send back the recovery node
            CHECK_EQ(recovery_nodes->control.node.size(), 1);
            Connect(recovery_nodes->control.node[0]);
            Postoffice::Get()->UpdateHeartbeat(recovery_nodes->control.node[0].id, t);
            Message back;
            for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {
                if (r != recovery_nodes->control.node[0].id &&
                    dead_set.find(r) != dead_set.end()) {
                    // do not try to send anything to dead node
                    continue;
                }
                // only send recovery_node to nodes already exist
                // but send all nodes to the recovery_node
                back.meta =
                    (r == recovery_nodes->control.node[0].id) ? *nodes : *recovery_nodes;
                back.meta.recver = r;
                back.meta.timestamp = timestamp_++;
                Send(back, nullptr, 0);
            }
        }
    }
    */

    void Van::ProcessAddNodeCommandAtScheduler(Message* msg, Meta* nodes, Meta* recovery_nodes) {//这一步后续的执行，是要等到nodes中的数量等于预设数量时
        recovery_nodes->control.cmd = Control::ADD_NODE;
        time_t t = time(NULL);
        //这里的num_nodes指的是初始化时所指定的server和worker数量
        size_t num_nodes = Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();

        //处理worker和server的back add node消息
        if (msg->meta.control.node.size() == 0) {//只有在sch节点返回node信息，且节点处理完毕后，发送的那个完毕信息里node.size才会等于0
            DBG_PRINTF("back add_node msg");
            add_node_record_[msg->meta.sender]++;
            if (add_node_record_.size() == num_nodes) {
                DBG_PRINTF("The scheduler is connected to %d workers and %d servers", num_workers_, num_servers_);
                ready_ = true;
            }
        }
        if (nodes->control.node.size() == num_nodes) { //这里的nodes是传进来的参数，系统启动阶段时，经过update之后，里面存有的数量应和配置时一致
            // sort the nodes according their ip and port,
            std::sort(nodes->control.node.begin(), nodes->control.node.end(),//根据节点的ip和port进行排序
                [](const Node& a, const Node& b) {
                    return (a.hostname.compare(b.hostname) | (a.port < b.port)) > 0;
                });
            // assign node rank
            for (auto& node : nodes->control.node) {
                std::string node_host_ip =
                    node.hostname + ":" + std::to_string(node.port);
                if (connected_nodes_.find(node_host_ip) == connected_nodes_.end()) {//判断该节点是否没有连接
                    CHECK_EQ(node.id, Node::kEmpty);
                    int id = node.role == Node::SERVER //分配一个正式的id
                        ? Postoffice::ServerRankToID(num_servers_)
                        : Postoffice::WorkerRankToID(num_workers_);
                    //LG << "assign rank=" << id << " to node " << node.DebugString() << std::endl;
                    DBG_PRINTF("assign rank %d to node %s", id, node.DebugString().c_str());
                    node.id = id;//分配该id
                    Message msg;
                    msg.meta.recver = node.id;
                    msg.meta.sender = my_node_.id;
                    msg.meta.control.cmd = Control::CONNECT; //给scheduler发送ADD_NODE控制信息，让scheduler节点注册Msg中携带的节点信息
                    //Connect(node);//scheduler节点连接该节点
                    //LG << "prepare to connect to node " << id << std::endl;
                    DBG_PRINTF("Prepare to connect to node %d", id);
                    Send(msg, &node, 1);
                    Postoffice::Get()->UpdateHeartbeat(node.id, t);
                    connected_nodes_[node_host_ip] = id;//更新该节点为已经连接的节点
                }
                else {//如果已经连接
                    int id = node.role == Node::SERVER
                        ? Postoffice::ServerRankToID(num_servers_)
                        : Postoffice::WorkerRankToID(num_workers_);
                    shared_node_mapping_[id] = connected_nodes_[node_host_ip];
                    node.id = connected_nodes_[node_host_ip];//更新该节点
                }
                if (node.role == Node::SERVER) num_servers_++;
                if (node.role == Node::WORKER) num_workers_++;
            }
            nodes->control.node.push_back(my_node_);//经过这一步之后，nodes中的节点数量将会大于num_nodes，因此后续将不会进入这个分支
            nodes->control.cmd = Control::ADD_NODE;

            //发送第一个back信息，从而得到后续的add_node信息
            int recver_id;
            Message back;
            back.meta = *nodes;
            for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {
                if (!add_node_record_.count(r)) {
                    recver_id = r;
                    break;
                }
            }
            if (shared_node_mapping_.find(recver_id) == shared_node_mapping_.end()) {
                back.meta.recver = recver_id;
                back.meta.timestamp = timestamp_++;
                //LG << "prepare to send back msg to node " << back.meta.recver << std::endl;
                DBG_PRINTF("Prepare to send back msg to node %d", back.meta.recver);
                Send(back, nullptr, 0);
                add_node_record_[recver_id]++;
            }
        }
        else if (connected_nodes_.size() == num_nodes && add_node_record_.size() != num_nodes) {//这一部分用来依次发送back消息给不同节点,//向所有的节点发送add_node信息
            int recver_id;
            Message back;
            back.meta = *nodes;
            for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {
                if (!add_node_record_.count(r)) {
                    recver_id = r;
                    break;
                }
            }
            if (shared_node_mapping_.find(recver_id) == shared_node_mapping_.end()) {
                back.meta.recver = recver_id;
                back.meta.timestamp = timestamp_++;
                //LG << "prepare to send back msg to node " << back.meta.recver << std::endl;
                DBG_PRINTF("Prepare to send back msg to node %d", back.meta.recver);
                Send(back, nullptr, 0);
            }
        }
        else if (!recovery_nodes->control.node.empty()) { //如果节点没有收集完，
            auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_);//查出超时而被设定为dead的节点
            std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
            // send back the recovery node
            CHECK_EQ(recovery_nodes->control.node.size(), 1);
            Connect(recovery_nodes->control.node[0]);
            Postoffice::Get()->UpdateHeartbeat(recovery_nodes->control.node[0].id, t);
            Message back;
            for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {
                if (r != recovery_nodes->control.node[0].id &&//这一步指的是不能发给超时的节点，但不包括恢复的节点
                    dead_set.find(r) != dead_set.end()) {
                    // do not try to send anything to dead node
                    continue;
                }
                // only send recovery_node to nodes already exist
                // but send all nodes to the recovery_node
                back.meta =
                    (r == recovery_nodes->control.node[0].id) ? *nodes : *recovery_nodes;
                back.meta.recver = r;
                back.meta.timestamp = timestamp_++;
                Send(back, nullptr, 0);
            }
        }
    }

    void Van::UpdateLocalID(Message* msg, std::unordered_set<int>* deadnodes_set,
        Meta* nodes, Meta* recovery_nodes) {
        auto& ctrl = msg->meta.control;
        size_t num_nodes =
            Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();
        // assign an id
        if (msg->meta.sender == Meta::kEmpty) {//当msg的发送端是初始id时
            CHECK(is_scheduler_);
            CHECK_EQ(ctrl.node.size(), 1);
            if (nodes->control.node.size() < num_nodes) {//如果nodes数目小于配置的num_nodes数目，说明是系统启动阶段，即将该节点加入到nodes中，完成注册
                nodes->control.node.push_back(ctrl.node[0]);//将msg中的node信息push到nodes中
            }
            else {//如果不是，说明此时是系统的运行阶段，运行阶段如果有add控制信号，这说明是节点dead之后重新连接重新注册
                // some node dies and restarts
                CHECK(ready_.load());
                for (size_t i = 0; i < nodes->control.node.size() - 1; ++i) {
                    const auto& node = nodes->control.node[i];//取出第i个节点信息
                    if (deadnodes_set->find(node.id) != deadnodes_set->end() &&
                        node.role == ctrl.node[0].role) { //从deadnode_set中找到一个和当前节点的role一致的node id
                        auto& recovery_node = ctrl.node[0];
                        // assign previous node id
                        recovery_node.id = node.id;
                        recovery_node.is_recovery = true;
                        PS_VLOG(1) << "replace dead node " << node.DebugString()
                            << " by node " << recovery_node.DebugString();
                        nodes->control.node[i] = recovery_node;//更新nodes，恢复的节点也要放置在nodes中
                        recovery_nodes->control.node.push_back(recovery_node);//同时恢复的节点也要重新注册在recovery_nodes中以便标记
                        break;//找到后停止
                    }
                }
            }
        }

        // update my id
        for (size_t i = 0; i < ctrl.node.size(); ++i) {//这一步是针对普通节点
            const auto& node = ctrl.node[i];//取出第i个节点信息
            if (my_node_.hostname == node.hostname && my_node_.port == node.port) {//若该节点信息中的ip和port与当前节点一致
                if (getenv("DMLC_RANK") == nullptr || my_node_.id == Meta::kEmpty) {
                    //int old_id = my_node_.id;
                    //int new_id = node.id;
                    my_node_ = node;//更新本地节点信息，主要是有scheduler统一设置的node_id信息
                    std::string rank = std::to_string(Postoffice::IDtoRank(node.id));
#ifdef _MSC_VER
                    _putenv_s("DMLC_RANK", rank.c_str());
#else
                    setenv("DMLC_RANK", rank.c_str(), true);
#endif
                    //UpdateNodeID(old_id, new_id);
                }
            }
        }
    }

    void Van::ProcessHearbeat(Message* msg) {
        auto& ctrl = msg->meta.control;
        time_t t = time(NULL);
        for (auto& node : ctrl.node) {
            Postoffice::Get()->UpdateHeartbeat(node.id, t);
            if (is_scheduler_) {
                Message heartbeat_ack;
                heartbeat_ack.meta.recver = node.id;
                heartbeat_ack.meta.control.cmd = Control::HEARTBEAT;
                heartbeat_ack.meta.control.node.push_back(my_node_);
                heartbeat_ack.meta.timestamp = timestamp_++;
                // send back heartbeat
                Send(heartbeat_ack, nullptr, 0);
            }
        }
    }

    void Van::ProcessBarrierCommand(Message* msg) {
        auto& ctrl = msg->meta.control;
        if (msg->meta.request) {
            if (barrier_count_.empty()) {
                barrier_count_.resize(8, 0);
            }
            int group = ctrl.barrier_group;
            ++barrier_count_[group]; //对barrier计数
            //LG << "Barrier count for " << group << " : " << barrier_count_[group];
            DBG_PRINTF("Barrier count for %d:%d, all:%d, init stage:%d", group, barrier_count_[group], static_cast<int>(Postoffice::Get()->GetNodeIDs(group).size()), init_stage);
            if (barrier_count_[group] == static_cast<int>(Postoffice::Get()->GetNodeIDs(group).size())
                && init_stage == 2) { //判断是否等于该节点组节点数量，是的话进入下一步，不是则退出并继续计数
                barrier_count_[group] = 0; //清楚计数信息
                Message res; //包装respond信息
                res.meta.request = false; //表明该信息并非request
                res.meta.app_id = msg->meta.app_id;
                res.meta.customer_id = msg->meta.customer_id;
                res.meta.control.cmd = Control::BARRIER; //依旧是barrier信息，但是由于上面的request是false，所以这里表明这是一个关于barrier的反应信息
                for (int r : Postoffice::Get()->GetNodeIDs(group)) {//这里是讲该respond信息发回给每个节点
                    int recver_id = r;//设置该次循环的接收方id
                    if (shared_node_mapping_.find(r) == shared_node_mapping_.end()) {
                        res.meta.recver = recver_id;
                        res.meta.timestamp = timestamp_++;
                        Send(res, nullptr, 0);
                    }
                }
                Postoffice::Get()->ManageForScheduler(msg->meta.app_id);
            }
        }
        else { //这一步是非sch节点接收到barrier信息，
            Postoffice::Get()->Manage(*msg); //此时说明所有节点都已工作完毕，可以解除barrier
        }
    }

    void Van::ProcessDataMsg(Message* msg) {
        // data msg
        CHECK_NE(msg->meta.sender, Meta::kEmpty);
        CHECK_NE(msg->meta.recver, Meta::kEmpty);
        CHECK_NE(msg->meta.app_id, Meta::kEmpty);
        int app_id = msg->meta.app_id;
        int customer_id =
            Postoffice::Get()->is_worker() ? msg->meta.customer_id : app_id;
        auto* obj = Postoffice::Get()->GetCustomer(app_id, customer_id, 5);
        CHECK(obj) << "timeout (5 sec) to wait App " << app_id << " customer "
            << customer_id << " ready at " << my_node_.role;
        obj->Accept(*msg);
    }

    void Van::ProcessAddNodeCommand(Message* msg, Meta* nodes,
        Meta* recovery_nodes) {
        auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_); //心跳包超时的id讲存储到dead_node中
        std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
        auto& ctrl = msg->meta.control;
        if (ctrl.node.size() != 0) {
            DBG_PRINTF("normal add_node msg");
            //更新节点信息，主要完成两个工作，对于scheduler，
            //注册节点信息或者注册恢复的节点信息，对于普通节点，则是根据返回的节点信息将本地节点进行更新
            UpdateLocalID(msg, &dead_set, nodes, recovery_nodes);
        }

        if (is_scheduler_) {//如果当前节点是scheduler节点，则需要分配对应的节点id信息，要执行这个分支的函数，基本上是要等到上面更新节点信息知道数量相等才行
            ProcessAddNodeCommandAtScheduler(msg, nodes, recovery_nodes);
        }
        else {//如果当前节点是普通节点，说明收到了scheduler回答的add_node信息
            for (auto& node : ctrl.node) {
                std::string addr_str = node.hostname + ":" + std::to_string(node.port);
                if (connected_nodes_.find(addr_str) == connected_nodes_.end()) {
                    Message msg;
                    msg.meta.recver = node.id;
                    msg.meta.sender = my_node_.id;
                    msg.meta.control.cmd = Control::CONNECT;
                    if (my_node_.id != node.id) {
                        //Connect(node);//当前节点连接nodes中的所有节点
                        //LG << "prepare to connect to node" << msg.meta.recver << std::endl;
                        DBG_PRINTF("Prepare to connect to node %d", msg.meta.recver);
                        Send(msg, &node, 1);
                        connected_nodes_[addr_str] = node.id;
                    }
                    else {
                        //LG << "prepare to connect to node" << msg.meta.recver << std::endl;
                        DBG_PRINTF("Prepare to connect to node %d", msg.meta.recver);
                        //LG << "there is no need to connect to itself" << std::endl;
                        DBG_PRINTF("there is no need to connect to itself");
                        //connected_nodes_[addr_str] = node.id;
                    }
                    /*
                    //Connect(node);//当前节点连接nodes中的所有节点
                    LG << "prepare to connect to node" << msg.meta.recver << std::endl;
                    Send(msg, &node, 1);
                    connected_nodes_[addr_str] = node.id;
                    */
                }
                if (!node.is_recovery && node.role == Node::SERVER) ++num_servers_;
                if (!node.is_recovery && node.role == Node::WORKER) ++num_workers_;
            }
            //完成当前节点的连接任务后，发送back消息告知scheduler节点发送下一个back消息至其他节点
            Message back;
            back.meta.recver = kScheduler;
            back.meta.sender = my_node_.id;
            back.meta.control.cmd = Control::ADD_NODE;
            Send(back, nullptr, 0);
            //LG << my_node_.ShortDebugString() << " is connected to others";
            DBG_PRINTF("%s is connected to others", my_node_.ShortDebugString().c_str());
            ready_ = true;
        }
    }

    void Van::Start(int customer_id) {
        // get scheduler info
        start_mu_.lock();

        if (init_stage == 0) {//初始化scheduler节点，获取IP和PORT信，每个节点都会保存自己和sch的信息
            scheduler_.hostname = std::string(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_URI")));//从环境变量中取出相关信息进行赋值
            scheduler_.port = atoi(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_PORT")));
            scheduler_.role = Node::SCHEDULER;
            scheduler_.id = kScheduler;
            is_scheduler_ = Postoffice::Get()->is_scheduler();

            // get my node info
            if (is_scheduler_) {//todo:
                my_node_ = scheduler_; //当前节点是scheduler节点的话将该节点信息赋值给my_node_变量
                DBG_PRINTF("|Current_Role:Scheduler|ID:%d|", scheduler_.id);
            }
            else {
                auto role = Postoffice::Get()->is_worker() ? Node::WORKER : Node::SERVER; //判断server或者worker
                const char* nhost = Environment::Get()->find("DMLC_NODE_HOST");//取得IP地址
                std::string ip;
                if (nhost) ip = std::string(nhost);
                if (ip.empty()) { //若无法从环境变量中取出host地址
                    const char* itf = Environment::Get()->find("DMLC_INTERFACE");//则从接口处取得对应IP
                    std::string interface;
                    if (itf) interface = std::string(itf);
                    if (interface.size()) {
                        GetIP(interface, &ip);
                    }
                    else {
                        GetAvailableInterfaceAndIP(&interface, &ip);
                    }
                    CHECK(!interface.empty()) << "failed to get the interface";
                }
                int port = GetAvailablePort();//分配可用的端口
                const char* pstr = Environment::Get()->find("PORT");
                if (pstr) port = atoi(pstr);
                CHECK(!ip.empty()) << "failed to get ip";
                CHECK(port) << "failed to get a port";
                my_node_.hostname = ip;
                my_node_.role = role;
                my_node_.port = port;
                // cannot determine my id now, the scheduler will assign it later
                // set it explicitly to make re-register within a same process possible
                my_node_.id = Node::kEmpty;
                my_node_.customer_id = customer_id;
                if (role == Node::WORKER) DBG_PRINTF("|Current_Role:Worker|ID:%d|", my_node_.id);
                if (role == Node::SERVER) DBG_PRINTF("|Current_Role:Server|ID:%d|", my_node_.id);
            }

            // bind.
            my_node_.port = Bind(my_node_, is_scheduler_ ? 0 : 40);//在派生类的实现中将Socket绑定到该节点的IP和Port上
            //LG << "current port is " << my_node_.port << std::endl;
            PS_VLOG(1) << "Bind to " << my_node_.DebugString();
            CHECK_NE(my_node_.port, -1) << "bind failed";

            // for debug use
            if (Environment::Get()->find("PS_DROP_MSG")) {
                drop_rate_ = atoi(Environment::Get()->find("PS_DROP_MSG"));
            }
            // start receiver
            receiver_thread_ = std::unique_ptr<std::thread>(new std::thread(&Van::Receiving, this)); //启动Receiving线程

            // start cnx building
            //cnxBuild_thread_ = std::unique_ptr<std::thread>(new std::thread(&Van::CnxBuilding, this));
            if (!is_scheduler_) {
                Message msg;
                msg.meta.recver = kScheduler;
                msg.meta.sender = my_node_.id;
                msg.meta.control.cmd = Control::CONNECT;
                //if(!is_scheduler_) Send(msg, &scheduler_, 1);
                Send(msg, &scheduler_, 1);
            }

            // connect to the scheduler
            //Connect(scheduler_);//将当前节点连接到scheduler节点，scheduler节点也连接到这个指定的端口
            init_stage++;
        }
        start_mu_.unlock();

        if (!is_scheduler_) {//若当前节点并非scheduler节点，则将本地node信息注册到scheduler节点，实际上就是一个信息的发送，只不过发送的不是Tensor数据而是节点信息
            // let the scheduler know myself
            //LG << "Send connect message to sch node from sch node" << std::endl;
            Message msg;
            Node customer_specific_node = my_node_;
            customer_specific_node.customer_id = customer_id;
            msg.meta.recver = kScheduler;
            msg.meta.control.cmd = Control::ADD_NODE; //给scheduler发送ADD_NODE控制信息，让scheduler节点注册Msg中携带的节点信息
            msg.meta.control.node.push_back(customer_specific_node);
            msg.meta.timestamp = timestamp_++;
            Send(msg, nullptr, 0);//将连接和发送消息放到一起
            //Connect(scheduler_);//将当前节点连接到scheduler节点，scheduler节点也连接到这个指定的端口
            //LG << "Connect to sch successful! is this sch?:" << is_scheduler_ << std::endl;
        }
        //LG << "waiting for all nodes ready" << std::endl;
        DBG_PRINTF("Waiting for all nodes ready");
        // wait until ready
        while (!ready_.load()) {//等待scheduler节点通知ready，即所有节点信息均注册完毕之后
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        start_mu_.lock();
        if (init_stage == 1) {
            // resender
            if (Environment::Get()->find("PS_RESEND") &&
                atoi(Environment::Get()->find("PS_RESEND")) != 0) {
                int timeout = 1000;
                if (Environment::Get()->find("PS_RESEND_TIMEOUT")) {
                    timeout = atoi(Environment::Get()->find("PS_RESEND_TIMEOUT"));
                }
                resender_ = new Resender(timeout, 10, this); //新建一个resender，用于处理重传数据
            }

            if (!is_scheduler_) {
                // start heartbeat thread
                DBG_PRINTF("Start heartbeat thread");
                heartbeat_thread_ =
                    std::unique_ptr<std::thread>(new std::thread(&Van::Heartbeat, this)); //启动心跳线程，周期性发送节点状态
            }
            init_stage++;
        }
        start_mu_.unlock();
    }

    void Van::Stop() {
        // stop threads
        DBG_PRINTF("Start stop");
        Message exit;
        exit.meta.control.cmd = Control::TERMINATE;//创建一个终止控制信号
        exit.meta.recver = my_node_.id;
        // only customer 0 would call this method
        exit.meta.customer_id = 0;
        int ret = Send(exit, &my_node_, 1);//发送该终止信息
        CHECK_NE(ret, -1);
        receiver_thread_->join();
        init_stage = 0;
        if (!is_scheduler_) heartbeat_thread_->join();
        if (resender_) delete resender_;
        ready_ = false;
        connected_nodes_.clear();
        shared_node_mapping_.clear();
        send_bytes_ = 0;
        timestamp_ = 0;
        my_node_.id = Meta::kEmpty;
        barrier_count_.clear();
    }

    //int Van::Send(const Message& msg) {
    int Van::Send(Message& msg, Node* node, int connect) {
        int send_bytes = SendMsg(msg, node, connect);
        CHECK_NE(send_bytes, -1);
        if (connect) {
            return send_bytes;
        }
        send_bytes_ += send_bytes;
        if (resender_) resender_->AddOutgoing(msg);
        if (Postoffice::Get()->verbose() >= 2) {
            PS_VLOG(2) << msg.DebugString();
        }
        return send_bytes;
    }

    //void Van::CnxBuilding() {
      //  int ret = QUICCnxRecv();
    //}

    void Van::Receiving() {
        DBG_PRINTF("Start receiving thread");
        Meta nodes; //用于存储scheduler节点内部所拥有的所有节点信息
        Meta recovery_nodes;  // store recovery nodes
        recovery_nodes.control.cmd = Control::ADD_NODE;
        int rcv_loop = 0;
        int ready_loop = 0;

        while (true) {
            Message msg;
            int recv_bytes = RecvMsg(&msg); //调用派生类所实现的RecvMsg，接收来自节点的信息
            //std::cout << "msg received!" << std::endl;
            // For debug, drop received message
            if (ready_.load() && drop_rate_ > 0) {
                unsigned seed = time(NULL) + my_node_.id;
                if (rand_r(&seed) % 100 < drop_rate_) {
                    LOG(WARNING) << "Drop message " << msg.DebugString();
                    continue;
                }
            }

            CHECK_NE(recv_bytes, -1);
            recv_bytes_ += recv_bytes;
            if (Postoffice::Get()->verbose() >= 2) {
                PS_VLOG(2) << msg.DebugString();
            }
            // duplicated message
            if (resender_ && resender_->AddIncomming(msg)) continue; //信息的重传

            if (!msg.meta.control.empty()) { //判断接收到的是否是控制信息
                // control msg
                auto& ctrl = msg.meta.control;
                if (ctrl.cmd == Control::TERMINATE) {
                    //LG << "terminate cmd" << std::endl;
                    DBG_PRINTF("|Msg Type:TERMINATE|");
                    ProcessTerminateCommand(); //处理节点的退出信号
                    break;
                }
                else if (ctrl.cmd == Control::ADD_NODE) {
                    //LG << "add node cmd" << std::endl;
                    DBG_PRINTF("|Msg Type:ADD_NODE|");
                    ProcessAddNodeCommand(&msg, &nodes, &recovery_nodes); //处理节点的注册信号
                }
                else if (ctrl.cmd == Control::BARRIER) {
                    //LG << "barrier cmd" << std::endl;
                    DBG_PRINTF("|Msg Type:BARRIER|");
                    ProcessBarrierCommand(&msg); //处理节点间的同步阻塞信号
                }
                else if (ctrl.cmd == Control::HEARTBEAT) {
                    //LG << "heartbeat cmd" << std::endl;
                    DBG_PRINTF("|Msg Type:HEARTBEAT|");
                    ProcessHearbeat(&msg); //处理节点间的心跳信号
                }
                else if (ctrl.cmd == Control::CONNECT) {
                    //LG << "Connect cmd" << std::endl;
                    DBG_PRINTF("|Msg Type:CONNECT|");
                    continue;
                }
                else {
                    LOG(WARNING) << "Drop unknown typed message " << msg.DebugString();
                }
            }
            else {
                DBG_PRINTF("|Msg Type:DATA|");
                ProcessDataMsg(&msg); //处理非控制信息，即Tensor数据
            }
        }
    }

    void Van::PackMetaPB(const Meta& meta, PBMeta* pb) {
        pb->set_head(meta.head);
        if (meta.app_id != Meta::kEmpty) pb->set_app_id(meta.app_id);
        if (meta.timestamp != Meta::kEmpty) pb->set_timestamp(meta.timestamp);
        if (meta.body.size()) pb->set_body(meta.body);
        pb->set_push(meta.push);
        pb->set_request(meta.request);
        pb->set_simple_app(meta.simple_app);
        pb->set_priority(meta.priority);
        pb->set_customer_id(meta.customer_id);
        for (auto d : meta.data_type) pb->add_data_type(d);
        if (!meta.control.empty()) {
            auto ctrl = pb->mutable_control();
            ctrl->set_cmd(meta.control.cmd);
            if (meta.control.cmd == Control::BARRIER) {
                ctrl->set_barrier_group(meta.control.barrier_group);
            }
            else if (meta.control.cmd == Control::ACK) {
                ctrl->set_msg_sig(meta.control.msg_sig);
            }
            for (const auto& n : meta.control.node) {
                auto p = ctrl->add_node();
                p->set_id(n.id);
                p->set_role(n.role);
                p->set_port(n.port);
                p->set_hostname(n.hostname);
                p->set_is_recovery(n.is_recovery);
                p->set_customer_id(n.customer_id);
            }
        }
        pb->set_data_size(meta.data_size);
    }

    void Van::PackMeta(const Meta& meta, char** meta_buf, int* buf_size) {
        // convert into protobuf
        PBMeta pb;
        pb.set_head(meta.head);
        pb.set_sender(meta.sender);
        pb.set_more(meta.more);
        if (meta.app_id != Meta::kEmpty) pb.set_app_id(meta.app_id);
        if (meta.timestamp != Meta::kEmpty) pb.set_timestamp(meta.timestamp);
        if (meta.body.size()) pb.set_body(meta.body);
        pb.set_push(meta.push);
        pb.set_pull(meta.pull);
        pb.set_request(meta.request);
        pb.set_simple_app(meta.simple_app);
        pb.set_priority(meta.priority);
        pb.set_customer_id(meta.customer_id);
        for (auto d : meta.data_type) pb.add_data_type(d);
        if (!meta.control.empty()) {
            auto ctrl = pb.mutable_control();
            ctrl->set_cmd(meta.control.cmd);
            if (meta.control.cmd == Control::BARRIER) {
                ctrl->set_barrier_group(meta.control.barrier_group);
            }
            else if (meta.control.cmd == Control::ACK) {
                ctrl->set_msg_sig(meta.control.msg_sig);
            }
            for (const auto& n : meta.control.node) {
                auto p = ctrl->add_node();
                p->set_id(n.id);
                p->set_role(n.role);
                p->set_port(n.port);
                p->set_hostname(n.hostname);
                p->set_is_recovery(n.is_recovery);
                p->set_customer_id(n.customer_id);
            }
        }

        // to string
        *buf_size = pb.ByteSize();
        *meta_buf = new char[*buf_size + 1];
        CHECK(pb.SerializeToArray(*meta_buf, *buf_size))
            << "failed to serialize protobuf";
    }

    void Van::UnpackMeta(const char* meta_buf, int buf_size, Meta* meta) {
        // to protobuf
        PBMeta pb;
        CHECK(pb.ParseFromArray(meta_buf, buf_size))
            << "failed to parse string into protobuf";

        // to meta
        meta->head = pb.head();
        meta->sender = pb.sender();
        meta->more = pb.more();
        meta->app_id = pb.has_app_id() ? pb.app_id() : Meta::kEmpty;
        meta->timestamp = pb.has_timestamp() ? pb.timestamp() : Meta::kEmpty;
        meta->request = pb.request();
        meta->push = pb.push();
        meta->pull = pb.pull();
        meta->simple_app = pb.simple_app();
        meta->priority = pb.priority();
        meta->body = pb.body();
        meta->customer_id = pb.customer_id();
        meta->data_type.resize(pb.data_type_size());
        for (int i = 0; i < pb.data_type_size(); ++i) {
            meta->data_type[i] = static_cast<DataType>(pb.data_type(i));
        }
        if (pb.has_control()) {
            const auto& ctrl = pb.control();
            meta->control.cmd = static_cast<Control::Command>(ctrl.cmd());
            meta->control.barrier_group = ctrl.barrier_group();
            meta->control.msg_sig = ctrl.msg_sig();
            for (int i = 0; i < ctrl.node_size(); ++i) {
                const auto& p = ctrl.node(i);
                Node n;
                n.role = static_cast<Node::Role>(p.role());
                n.port = p.port();
                n.hostname = p.hostname();
                n.id = p.has_id() ? p.id() : Node::kEmpty;
                n.is_recovery = p.is_recovery();
                n.customer_id = p.customer_id();
                meta->control.node.push_back(n);
            }
        }
        else {
            meta->control.cmd = Control::EMPTY;
        }
    }

    void Van::Heartbeat() {
        LG << "start heartbeating" << std::endl;
        const char* val = Environment::Get()->find("PS_HEARTBEAT_INTERVAL");
        const int interval = val ? atoi(val) : kDefaultHeartbeatInterval;
        while (interval > 0 && ready_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(interval));
            Message msg;
            msg.meta.recver = kScheduler;
            msg.meta.control.cmd = Control::HEARTBEAT;
            msg.meta.control.node.push_back(my_node_);
            msg.meta.timestamp = timestamp_++;
            Send(msg, nullptr, 0);
        }
    }

}//namespace ps
