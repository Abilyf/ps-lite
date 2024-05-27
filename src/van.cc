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
        ready_ = false;//��һ���ǹؼ���������Ϊfalse����ζ��van����������ֹ
    }

    /*
    void Van::ProcessAddNodeCommandAtScheduler(Message* msg, Meta* nodes,
        Meta* recovery_nodes) {//��һ��������ִ�У���Ҫ�ȵ�nodes�е���������Ԥ������ʱ
        recovery_nodes->control.cmd = Control::ADD_NODE;
        time_t t = time(NULL);
        //�����num_nodesָ���ǳ�ʼ��ʱ��ָ����server��worker����
        size_t num_nodes = Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();
        if (nodes->control.node.size() == num_nodes) { //�����nodes�Ǵ������Ĳ�����ϵͳ�����׶�ʱ������update֮��������е�����Ӧ������ʱһ��
            // sort the nodes according their ip and port,
            std::sort(nodes->control.node.begin(), nodes->control.node.end(),//���ݽڵ��ip��port��������
                [](const Node& a, const Node& b) {
                    return (a.hostname.compare(b.hostname) | (a.port < b.port)) > 0;
                });
            // assign node rank
            for (auto& node : nodes->control.node) {
                std::string node_host_ip =
                    node.hostname + ":" + std::to_string(node.port);
                if (connected_nodes_.find(node_host_ip) == connected_nodes_.end()) {//�жϸýڵ��Ƿ�û������
                    CHECK_EQ(node.id, Node::kEmpty);
                    int id = node.role == Node::SERVER //����һ����ʽ��id
                        ? Postoffice::ServerRankToID(num_servers_)
                        : Postoffice::WorkerRankToID(num_workers_);
                    //LG << "assign rank=" << id << " to node " << node.DebugString() << std::endl;
                    DBG_PRINTF("assign rank %d to node %s", id, node.DebugString().c_str());
                    node.id = id;//�����id
                    Message msg;
                    msg.meta.recver = node.id;
                    msg.meta.sender = my_node_.id;
                    msg.meta.control.cmd = Control::CONNECT; //��scheduler����ADD_NODE������Ϣ����scheduler�ڵ�ע��Msg��Я���Ľڵ���Ϣ
                    //Connect(node);//scheduler�ڵ����Ӹýڵ�
                    //LG << "prepare to connect to node " << id << std::endl;
                    DBG_PRINTF("Prepare to connect to node %d", id);
                    Send(msg, &node, 1);
                    Postoffice::Get()->UpdateHeartbeat(node.id, t);
                    connected_nodes_[node_host_ip] = id;//���¸ýڵ�Ϊ�Ѿ����ӵĽڵ�
                }
                else {//����Ѿ�����
                    int id = node.role == Node::SERVER
                        ? Postoffice::ServerRankToID(num_servers_)
                        : Postoffice::WorkerRankToID(num_workers_);
                    shared_node_mapping_[id] = connected_nodes_[node_host_ip];
                    node.id = connected_nodes_[node_host_ip];//���¸ýڵ�
                }
                if (node.role == Node::SERVER) num_servers_++;
                if (node.role == Node::WORKER) num_workers_++;
            }
            nodes->control.node.push_back(my_node_);
            nodes->control.cmd = Control::ADD_NODE;
            Message back;
            back.meta = *nodes;
            for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {//�����еĽڵ㷢��add_node��Ϣ
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
        else if (!recovery_nodes->control.node.empty()) { //����ڵ�û���ռ��꣬
            auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_);//�����ʱ�����趨Ϊdead�Ľڵ�
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

    void Van::ProcessAddNodeCommandAtScheduler(Message* msg, Meta* nodes, Meta* recovery_nodes) {//��һ��������ִ�У���Ҫ�ȵ�nodes�е���������Ԥ������ʱ
        recovery_nodes->control.cmd = Control::ADD_NODE;
        time_t t = time(NULL);
        //�����num_nodesָ���ǳ�ʼ��ʱ��ָ����server��worker����
        size_t num_nodes = Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();

        //����worker��server��back add node��Ϣ
        if (msg->meta.control.node.size() == 0) {//ֻ����sch�ڵ㷵��node��Ϣ���ҽڵ㴦����Ϻ󣬷��͵��Ǹ������Ϣ��node.size�Ż����0
            DBG_PRINTF("back add_node msg");
            add_node_record_[msg->meta.sender]++;
            if (add_node_record_.size() == num_nodes) {
                DBG_PRINTF("The scheduler is connected to %d workers and %d servers", num_workers_, num_servers_);
                ready_ = true;
            }
        }
        if (nodes->control.node.size() == num_nodes) { //�����nodes�Ǵ������Ĳ�����ϵͳ�����׶�ʱ������update֮��������е�����Ӧ������ʱһ��
            // sort the nodes according their ip and port,
            std::sort(nodes->control.node.begin(), nodes->control.node.end(),//���ݽڵ��ip��port��������
                [](const Node& a, const Node& b) {
                    return (a.hostname.compare(b.hostname) | (a.port < b.port)) > 0;
                });
            // assign node rank
            for (auto& node : nodes->control.node) {
                std::string node_host_ip =
                    node.hostname + ":" + std::to_string(node.port);
                if (connected_nodes_.find(node_host_ip) == connected_nodes_.end()) {//�жϸýڵ��Ƿ�û������
                    CHECK_EQ(node.id, Node::kEmpty);
                    int id = node.role == Node::SERVER //����һ����ʽ��id
                        ? Postoffice::ServerRankToID(num_servers_)
                        : Postoffice::WorkerRankToID(num_workers_);
                    //LG << "assign rank=" << id << " to node " << node.DebugString() << std::endl;
                    DBG_PRINTF("assign rank %d to node %s", id, node.DebugString().c_str());
                    node.id = id;//�����id
                    Message msg;
                    msg.meta.recver = node.id;
                    msg.meta.sender = my_node_.id;
                    msg.meta.control.cmd = Control::CONNECT; //��scheduler����ADD_NODE������Ϣ����scheduler�ڵ�ע��Msg��Я���Ľڵ���Ϣ
                    //Connect(node);//scheduler�ڵ����Ӹýڵ�
                    //LG << "prepare to connect to node " << id << std::endl;
                    DBG_PRINTF("Prepare to connect to node %d", id);
                    Send(msg, &node, 1);
                    Postoffice::Get()->UpdateHeartbeat(node.id, t);
                    connected_nodes_[node_host_ip] = id;//���¸ýڵ�Ϊ�Ѿ����ӵĽڵ�
                }
                else {//����Ѿ�����
                    int id = node.role == Node::SERVER
                        ? Postoffice::ServerRankToID(num_servers_)
                        : Postoffice::WorkerRankToID(num_workers_);
                    shared_node_mapping_[id] = connected_nodes_[node_host_ip];
                    node.id = connected_nodes_[node_host_ip];//���¸ýڵ�
                }
                if (node.role == Node::SERVER) num_servers_++;
                if (node.role == Node::WORKER) num_workers_++;
            }
            nodes->control.node.push_back(my_node_);//������һ��֮��nodes�еĽڵ������������num_nodes����˺�����������������֧
            nodes->control.cmd = Control::ADD_NODE;

            //���͵�һ��back��Ϣ���Ӷ��õ�������add_node��Ϣ
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
        else if (connected_nodes_.size() == num_nodes && add_node_record_.size() != num_nodes) {//��һ�����������η���back��Ϣ����ͬ�ڵ�,//�����еĽڵ㷢��add_node��Ϣ
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
        else if (!recovery_nodes->control.node.empty()) { //����ڵ�û���ռ��꣬
            auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_);//�����ʱ�����趨Ϊdead�Ľڵ�
            std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
            // send back the recovery node
            CHECK_EQ(recovery_nodes->control.node.size(), 1);
            Connect(recovery_nodes->control.node[0]);
            Postoffice::Get()->UpdateHeartbeat(recovery_nodes->control.node[0].id, t);
            Message back;
            for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {
                if (r != recovery_nodes->control.node[0].id &&//��һ��ָ���ǲ��ܷ�����ʱ�Ľڵ㣬���������ָ��Ľڵ�
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
        if (msg->meta.sender == Meta::kEmpty) {//��msg�ķ��Ͷ��ǳ�ʼidʱ
            CHECK(is_scheduler_);
            CHECK_EQ(ctrl.node.size(), 1);
            if (nodes->control.node.size() < num_nodes) {//���nodes��ĿС�����õ�num_nodes��Ŀ��˵����ϵͳ�����׶Σ������ýڵ���뵽nodes�У����ע��
                nodes->control.node.push_back(ctrl.node[0]);//��msg�е�node��Ϣpush��nodes��
            }
            else {//������ǣ�˵����ʱ��ϵͳ�����н׶Σ����н׶������add�����źţ���˵���ǽڵ�dead֮��������������ע��
                // some node dies and restarts
                CHECK(ready_.load());
                for (size_t i = 0; i < nodes->control.node.size() - 1; ++i) {
                    const auto& node = nodes->control.node[i];//ȡ����i���ڵ���Ϣ
                    if (deadnodes_set->find(node.id) != deadnodes_set->end() &&
                        node.role == ctrl.node[0].role) { //��deadnode_set���ҵ�һ���͵�ǰ�ڵ��roleһ�µ�node id
                        auto& recovery_node = ctrl.node[0];
                        // assign previous node id
                        recovery_node.id = node.id;
                        recovery_node.is_recovery = true;
                        PS_VLOG(1) << "replace dead node " << node.DebugString()
                            << " by node " << recovery_node.DebugString();
                        nodes->control.node[i] = recovery_node;//����nodes���ָ��Ľڵ�ҲҪ������nodes��
                        recovery_nodes->control.node.push_back(recovery_node);//ͬʱ�ָ��Ľڵ�ҲҪ����ע����recovery_nodes���Ա���
                        break;//�ҵ���ֹͣ
                    }
                }
            }
        }

        // update my id
        for (size_t i = 0; i < ctrl.node.size(); ++i) {//��һ���������ͨ�ڵ�
            const auto& node = ctrl.node[i];//ȡ����i���ڵ���Ϣ
            if (my_node_.hostname == node.hostname && my_node_.port == node.port) {//���ýڵ���Ϣ�е�ip��port�뵱ǰ�ڵ�һ��
                if (getenv("DMLC_RANK") == nullptr || my_node_.id == Meta::kEmpty) {
                    //int old_id = my_node_.id;
                    //int new_id = node.id;
                    my_node_ = node;//���±��ؽڵ���Ϣ����Ҫ����schedulerͳһ���õ�node_id��Ϣ
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
            ++barrier_count_[group]; //��barrier����
            //LG << "Barrier count for " << group << " : " << barrier_count_[group];
            DBG_PRINTF("Barrier count for %d:%d, all:%d, init stage:%d", group, barrier_count_[group], static_cast<int>(Postoffice::Get()->GetNodeIDs(group).size()), init_stage);
            if (barrier_count_[group] == static_cast<int>(Postoffice::Get()->GetNodeIDs(group).size())
                && init_stage == 2) { //�ж��Ƿ���ڸýڵ���ڵ��������ǵĻ�������һ�����������˳�����������
                barrier_count_[group] = 0; //���������Ϣ
                Message res; //��װrespond��Ϣ
                res.meta.request = false; //��������Ϣ����request
                res.meta.app_id = msg->meta.app_id;
                res.meta.customer_id = msg->meta.customer_id;
                res.meta.control.cmd = Control::BARRIER; //������barrier��Ϣ���������������request��false�����������������һ������barrier�ķ�Ӧ��Ϣ
                for (int r : Postoffice::Get()->GetNodeIDs(group)) {//�����ǽ���respond��Ϣ���ظ�ÿ���ڵ�
                    int recver_id = r;//���øô�ѭ���Ľ��շ�id
                    if (shared_node_mapping_.find(r) == shared_node_mapping_.end()) {
                        res.meta.recver = recver_id;
                        res.meta.timestamp = timestamp_++;
                        Send(res, nullptr, 0);
                    }
                }
                Postoffice::Get()->ManageForScheduler(msg->meta.app_id);
            }
        }
        else { //��һ���Ƿ�sch�ڵ���յ�barrier��Ϣ��
            Postoffice::Get()->Manage(*msg); //��ʱ˵�����нڵ㶼�ѹ�����ϣ����Խ��barrier
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
        auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_); //��������ʱ��id���洢��dead_node��
        std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
        auto& ctrl = msg->meta.control;
        if (ctrl.node.size() != 0) {
            DBG_PRINTF("normal add_node msg");
            //���½ڵ���Ϣ����Ҫ�����������������scheduler��
            //ע��ڵ���Ϣ����ע��ָ��Ľڵ���Ϣ��������ͨ�ڵ㣬���Ǹ��ݷ��صĽڵ���Ϣ�����ؽڵ���и���
            UpdateLocalID(msg, &dead_set, nodes, recovery_nodes);
        }

        if (is_scheduler_) {//�����ǰ�ڵ���scheduler�ڵ㣬����Ҫ�����Ӧ�Ľڵ�id��Ϣ��Ҫִ�������֧�ĺ�������������Ҫ�ȵ�������½ڵ���Ϣ֪��������Ȳ���
            ProcessAddNodeCommandAtScheduler(msg, nodes, recovery_nodes);
        }
        else {//�����ǰ�ڵ�����ͨ�ڵ㣬˵���յ���scheduler�ش��add_node��Ϣ
            for (auto& node : ctrl.node) {
                std::string addr_str = node.hostname + ":" + std::to_string(node.port);
                if (connected_nodes_.find(addr_str) == connected_nodes_.end()) {
                    Message msg;
                    msg.meta.recver = node.id;
                    msg.meta.sender = my_node_.id;
                    msg.meta.control.cmd = Control::CONNECT;
                    if (my_node_.id != node.id) {
                        //Connect(node);//��ǰ�ڵ�����nodes�е����нڵ�
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
                    //Connect(node);//��ǰ�ڵ�����nodes�е����нڵ�
                    LG << "prepare to connect to node" << msg.meta.recver << std::endl;
                    Send(msg, &node, 1);
                    connected_nodes_[addr_str] = node.id;
                    */
                }
                if (!node.is_recovery && node.role == Node::SERVER) ++num_servers_;
                if (!node.is_recovery && node.role == Node::WORKER) ++num_workers_;
            }
            //��ɵ�ǰ�ڵ����������󣬷���back��Ϣ��֪scheduler�ڵ㷢����һ��back��Ϣ�������ڵ�
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

        if (init_stage == 0) {//��ʼ��scheduler�ڵ㣬��ȡIP��PORT�ţ�ÿ���ڵ㶼�ᱣ���Լ���sch����Ϣ
            scheduler_.hostname = std::string(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_URI")));//�ӻ���������ȡ�������Ϣ���и�ֵ
            scheduler_.port = atoi(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_PORT")));
            scheduler_.role = Node::SCHEDULER;
            scheduler_.id = kScheduler;
            is_scheduler_ = Postoffice::Get()->is_scheduler();

            // get my node info
            if (is_scheduler_) {//todo:
                my_node_ = scheduler_; //��ǰ�ڵ���scheduler�ڵ�Ļ����ýڵ���Ϣ��ֵ��my_node_����
                DBG_PRINTF("|Current_Role:Scheduler|ID:%d|", scheduler_.id);
            }
            else {
                auto role = Postoffice::Get()->is_worker() ? Node::WORKER : Node::SERVER; //�ж�server����worker
                const char* nhost = Environment::Get()->find("DMLC_NODE_HOST");//ȡ��IP��ַ
                std::string ip;
                if (nhost) ip = std::string(nhost);
                if (ip.empty()) { //���޷��ӻ���������ȡ��host��ַ
                    const char* itf = Environment::Get()->find("DMLC_INTERFACE");//��ӽӿڴ�ȡ�ö�ӦIP
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
                int port = GetAvailablePort();//������õĶ˿�
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
            my_node_.port = Bind(my_node_, is_scheduler_ ? 0 : 40);//���������ʵ���н�Socket�󶨵��ýڵ��IP��Port��
            //LG << "current port is " << my_node_.port << std::endl;
            PS_VLOG(1) << "Bind to " << my_node_.DebugString();
            CHECK_NE(my_node_.port, -1) << "bind failed";

            // for debug use
            if (Environment::Get()->find("PS_DROP_MSG")) {
                drop_rate_ = atoi(Environment::Get()->find("PS_DROP_MSG"));
            }
            // start receiver
            receiver_thread_ = std::unique_ptr<std::thread>(new std::thread(&Van::Receiving, this)); //����Receiving�߳�

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
            //Connect(scheduler_);//����ǰ�ڵ����ӵ�scheduler�ڵ㣬scheduler�ڵ�Ҳ���ӵ����ָ���Ķ˿�
            init_stage++;
        }
        start_mu_.unlock();

        if (!is_scheduler_) {//����ǰ�ڵ㲢��scheduler�ڵ㣬�򽫱���node��Ϣע�ᵽscheduler�ڵ㣬ʵ���Ͼ���һ����Ϣ�ķ��ͣ�ֻ�������͵Ĳ���Tensor���ݶ��ǽڵ���Ϣ
            // let the scheduler know myself
            //LG << "Send connect message to sch node from sch node" << std::endl;
            Message msg;
            Node customer_specific_node = my_node_;
            customer_specific_node.customer_id = customer_id;
            msg.meta.recver = kScheduler;
            msg.meta.control.cmd = Control::ADD_NODE; //��scheduler����ADD_NODE������Ϣ����scheduler�ڵ�ע��Msg��Я���Ľڵ���Ϣ
            msg.meta.control.node.push_back(customer_specific_node);
            msg.meta.timestamp = timestamp_++;
            Send(msg, nullptr, 0);//�����Ӻͷ�����Ϣ�ŵ�һ��
            //Connect(scheduler_);//����ǰ�ڵ����ӵ�scheduler�ڵ㣬scheduler�ڵ�Ҳ���ӵ����ָ���Ķ˿�
            //LG << "Connect to sch successful! is this sch?:" << is_scheduler_ << std::endl;
        }
        //LG << "waiting for all nodes ready" << std::endl;
        DBG_PRINTF("Waiting for all nodes ready");
        // wait until ready
        while (!ready_.load()) {//�ȴ�scheduler�ڵ�֪ͨready�������нڵ���Ϣ��ע�����֮��
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
                resender_ = new Resender(timeout, 10, this); //�½�һ��resender�����ڴ����ش�����
            }

            if (!is_scheduler_) {
                // start heartbeat thread
                DBG_PRINTF("Start heartbeat thread");
                heartbeat_thread_ =
                    std::unique_ptr<std::thread>(new std::thread(&Van::Heartbeat, this)); //���������̣߳������Է��ͽڵ�״̬
            }
            init_stage++;
        }
        start_mu_.unlock();
    }

    void Van::Stop() {
        // stop threads
        DBG_PRINTF("Start stop");
        Message exit;
        exit.meta.control.cmd = Control::TERMINATE;//����һ����ֹ�����ź�
        exit.meta.recver = my_node_.id;
        // only customer 0 would call this method
        exit.meta.customer_id = 0;
        int ret = Send(exit, &my_node_, 1);//���͸���ֹ��Ϣ
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
        Meta nodes; //���ڴ洢scheduler�ڵ��ڲ���ӵ�е����нڵ���Ϣ
        Meta recovery_nodes;  // store recovery nodes
        recovery_nodes.control.cmd = Control::ADD_NODE;
        int rcv_loop = 0;
        int ready_loop = 0;

        while (true) {
            Message msg;
            int recv_bytes = RecvMsg(&msg); //������������ʵ�ֵ�RecvMsg���������Խڵ����Ϣ
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
            if (resender_ && resender_->AddIncomming(msg)) continue; //��Ϣ���ش�

            if (!msg.meta.control.empty()) { //�жϽ��յ����Ƿ��ǿ�����Ϣ
                // control msg
                auto& ctrl = msg.meta.control;
                if (ctrl.cmd == Control::TERMINATE) {
                    //LG << "terminate cmd" << std::endl;
                    DBG_PRINTF("|Msg Type:TERMINATE|");
                    ProcessTerminateCommand(); //����ڵ���˳��ź�
                    break;
                }
                else if (ctrl.cmd == Control::ADD_NODE) {
                    //LG << "add node cmd" << std::endl;
                    DBG_PRINTF("|Msg Type:ADD_NODE|");
                    ProcessAddNodeCommand(&msg, &nodes, &recovery_nodes); //����ڵ��ע���ź�
                }
                else if (ctrl.cmd == Control::BARRIER) {
                    //LG << "barrier cmd" << std::endl;
                    DBG_PRINTF("|Msg Type:BARRIER|");
                    ProcessBarrierCommand(&msg); //����ڵ���ͬ�������ź�
                }
                else if (ctrl.cmd == Control::HEARTBEAT) {
                    //LG << "heartbeat cmd" << std::endl;
                    DBG_PRINTF("|Msg Type:HEARTBEAT|");
                    ProcessHearbeat(&msg); //����ڵ��������ź�
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
                ProcessDataMsg(&msg); //����ǿ�����Ϣ����Tensor����
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
