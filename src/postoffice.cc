/**
 *  Copyright (c) 2015 by Contributors
 */
#include <unistd.h>
#include <thread>
#include <chrono>
#include "ps/internal/postoffice.h"
#include "ps/internal/message.h"
#include "ps/base.h"
#include "util.h"

namespace ps {
    Postoffice::Postoffice() {
        env_ref_ = Environment::_GetSharedRef();
    }

    void Postoffice::InitEnvironment() {//��ʼ������
        DBG_PRINTF("Intial Environment");
        const char* val = NULL;
        std::string van_type = GetEnv("DMLC_PS_VAN_TYPE", "quic");//��ȡvan����
        van_ = Van::Create(van_type);//������Ӧvan
        val = CHECK_NOTNULL(Environment::Get()->find("DMLC_NUM_WORKER"));
        num_workers_ = atoi(val);//��ȡworker����
        val = CHECK_NOTNULL(Environment::Get()->find("DMLC_NUM_SERVER"));
        num_servers_ = atoi(val);//��ȡserver����
        val = CHECK_NOTNULL(Environment::Get()->find("DMLC_ROLE"));
        std::string role(val);
        is_worker_ = role == "worker";//���õ�ǰ�ڵ�ģ���Ϊÿһ���ڵ�ά��һ��postoffice�ࣩ��ɫ
        is_server_ = role == "server";
        is_scheduler_ = role == "scheduler";
        verbose_ = GetEnv("PS_VERBOSE", 0);
        DBG_PRINTF("|Server_Nums:%d|Worker_Nums:%d|Current_Role:%s|", num_servers_, num_workers_, role.c_str());
    }

    void Postoffice::Start(int customer_id, const char* argv0, const bool do_barrier) {
        start_mu_.lock();
        if (init_stage_ == 0) {
            InitEnvironment();//���ó�ʼ���������van�Ĵ����Լ��ڵ������ͽڵ��ɫ�Ļ�ȡ
            // init glog
            if (argv0) {
                dmlc::InitLogging(argv0);
            }
            else {
                dmlc::InitLogging("ps-lite\0");
            }

            // init node info.
            DBG_PRINTF("Initial and Classify different nodes");
            for (int i = 0; i < num_workers_; ++i) {//�����е�worker�ڵ�������ã�ʹ���ܹ�������Ϣ����һ�ڵ�
                int id = WorkerRankToID(i);//���Ȼ�ȡworker��id
                for (int g : {id, kWorkerGroup, kWorkerGroup + kServerGroup, //��α������а���worker���飬���絥����worker�飬������worker��server�����
                    kWorkerGroup + kScheduler,
                    kWorkerGroup + kServerGroup + kScheduler}) {
                    node_ids_[g].push_back(id);//����Щ������ӵ�ǰ���õ�workerid���൱���Ƿ��࣬��Ӧ����������ж�Ӧ�Ľڵ㣬����worker��Ͱ������е�worker�ڵ�
                }
            }

            for (int i = 0; i < num_servers_; ++i) {//�����е�Server�ڵ��������
                int id = ServerRankToID(i);
                for (int g : {id, kServerGroup, kWorkerGroup + kServerGroup,
                    kServerGroup + kScheduler,
                    kWorkerGroup + kServerGroup + kScheduler}) {
                    node_ids_[g].push_back(id);
                }
            }

            for (int g : {kScheduler, kScheduler + kServerGroup + kWorkerGroup,
                kScheduler + kWorkerGroup, kScheduler + kServerGroup}) {
                node_ids_[g].push_back(kScheduler);//����scheduler�ڵ㣬��Ϊֻ��һ����������ѭ��
            }
            init_stage_++;
        }
        start_mu_.unlock();

        // start van
        DBG_PRINTF("Start Van conmunication");
        van_->Start(customer_id);//����Vanͨ��

        start_mu_.lock();
        if (init_stage_ == 1) {
            // record start time
            start_time_ = time(NULL);
            init_stage_++;
        }
        start_mu_.unlock();
        // do a barrier here
        if (do_barrier) {
            DBG_PRINTF("Barrier in start");
            //Barrier(customer_id, kWorkerGroup + kServerGroup + kScheduler);//��ǰ�ڵ��֪scheduler�Լ�����ɼ��㲢��������״̬
            Barrier(customer_id, kWorkerGroup + kServerGroup);//��ǰ�ڵ��֪scheduler�Լ�����ɼ��㲢��������״̬
        }
        //ע�⣬����ڶ�������������group����һ����ֵ��Ҳ����˵schedulerҲҪ��������һ��������Ϣ������������ȫ�����պ�Ž��
    }

    void Postoffice::Finalize(const int customer_id, const bool do_barrier) {
        if (do_barrier) {
            DBG_PRINTF("Barrier in finalize");
            //Barrier(customer_id, kWorkerGroup + kServerGroup + kScheduler);
            Barrier(customer_id, kWorkerGroup + kServerGroup);
        }
        if (customer_id == 0) {
            num_workers_ = 0;
            num_servers_ = 0;
            van_->Stop();
            init_stage_ = 0;
            customers_.clear();
            node_ids_.clear();
            barrier_done_.clear();
            server_key_ranges_.clear();
            heartbeats_.clear();
            if (exit_callback_) exit_callback_();
        }
    }


    void Postoffice::AddCustomer(Customer* customer) {
        std::lock_guard<std::mutex> lk(mu_);
        int app_id = CHECK_NOTNULL(customer)->app_id();
        // check if the customer id has existed
        int customer_id = CHECK_NOTNULL(customer)->customer_id();
        CHECK_EQ(customers_[app_id].count(customer_id), (size_t)0) << "customer_id " \
            << customer_id << " already exists\n";
        customers_[app_id].insert(std::make_pair(customer_id, customer));
        std::unique_lock<std::mutex> ulk(barrier_mu_);
        barrier_done_[app_id].insert(std::make_pair(customer_id, false));
    }


    void Postoffice::RemoveCustomer(Customer* customer) {
        std::lock_guard<std::mutex> lk(mu_);
        int app_id = CHECK_NOTNULL(customer)->app_id();
        int customer_id = CHECK_NOTNULL(customer)->customer_id();
        customers_[app_id].erase(customer_id);
        if (customers_[app_id].empty()) {
            customers_.erase(app_id);
        }
    }


    Customer* Postoffice::GetCustomer(int app_id, int customer_id, int timeout) const {
        Customer* obj = nullptr;
        for (int i = 0; i < timeout * 1000 + 1; ++i) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                const auto it = customers_.find(app_id);
                if (it != customers_.end()) {
                    std::unordered_map<int, Customer*> customers_in_app = it->second;
                    obj = customers_in_app[customer_id];
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return obj;
    }

    void Postoffice::Barrier(int customer_id, int node_group) {//��һ����װ����customer�ڵ��barrier��Ϣ�����ݸ�scheduler�ڵ�
        if (GetNodeIDs(node_group).size() <= 0) return;
        DBG_PRINTF("Start Barrier");
        auto role = van_->my_node().role;
        if (role == Node::SCHEDULER) {
            //CHECK(node_group & kScheduler);
        }
        else if (role == Node::WORKER) {
            CHECK(node_group & kWorkerGroup);
        }
        else if (role == Node::SERVER) {
            CHECK(node_group & kServerGroup);
        }

        std::unique_lock<std::mutex> ulk(barrier_mu_);
        barrier_done_[0][customer_id] = false;
        Message req;
        req.meta.recver = kScheduler;//barrier���ն��󣬼�scheduler�ڵ�
        req.meta.request = true; //�Ƿ���req�����ǵĻ��Ż���к�������
        req.meta.control.cmd = Control::BARRIER; //������barrier������Ϣ
        req.meta.app_id = 0;
        req.meta.customer_id = customer_id;
        req.meta.control.barrier_group = node_group; //�Ӵ���Ľڵ���������Ӧ���������ڵ��鶼��Ҫ
        req.meta.timestamp = van_->GetTimestamp();
        //DBG_PRINTF("Send Barrier Msg to scheduler");
        //if (role != Node::SCHEDULER ) {
        //    van_->Send(req, nullptr, 0); //������Ϣ
        //}
        if (role == Node::SERVER) {
            van_->Send(req, nullptr, 0); //������Ϣ
        }
        else if (role == Node::WORKER) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            van_->Send(req, nullptr, 0); //������Ϣ
        }
        DBG_PRINTF("Send Barrier Msg to scheduler successfully");
        barrier_cond_.wait(ulk, [this, customer_id] {
            return barrier_done_[0][customer_id];
            });
    }

    /*��0~KMaxKey֮�����servers���������о��֣��ֳ���Ӧ����������*/
    const std::vector<Range>& Postoffice::GetServerKeyRanges() {
        server_key_ranges_mu_.lock();
        if (server_key_ranges_.empty()) {
            for (int i = 0; i < num_servers_; ++i) {
                server_key_ranges_.push_back(Range(
                    kMaxKey / num_servers_ * i,
                    kMaxKey / num_servers_ * (i + 1)));
            }
        }
        server_key_ranges_mu_.unlock();
        return server_key_ranges_;
    }

    void Postoffice::Manage(const Message& recv) { //���ڽ����scheduler�˵�barrier����
        CHECK(!recv.meta.control.empty());
        const auto& ctrl = recv.meta.control;
        if (ctrl.cmd == Control::BARRIER && !recv.meta.request) {
            barrier_mu_.lock();
            auto size = barrier_done_[recv.meta.app_id].size();
            for (size_t customer_id = 0; customer_id < size; customer_id++) {
                barrier_done_[recv.meta.app_id][customer_id] = true;
            }
            barrier_mu_.unlock();
            barrier_cond_.notify_all();//��һ�����ǽ��barrier
        }
    }

    void Postoffice::ManageForScheduler(int app_id) { //���ڽ��scheduler�˵�barrier����
        barrier_mu_.lock();
        auto size = barrier_done_[app_id].size();
        for (size_t customer_id = 0; customer_id < size; customer_id++) {
            barrier_done_[app_id][customer_id] = true;
        }
        barrier_mu_.unlock();
        barrier_cond_.notify_all();//��һ�����ǽ��barrier
    }

    std::vector<int> Postoffice::GetDeadNodes(int t) {
        std::vector<int> dead_nodes;
        if (!van_->IsReady() || t == 0) return dead_nodes;

        time_t curr_time = time(NULL);
        const auto& nodes = is_scheduler_
            ? GetNodeIDs(kWorkerGroup + kServerGroup)
            : GetNodeIDs(kScheduler);
        {
            std::lock_guard<std::mutex> lk(heartbeat_mu_);
            for (int r : nodes) {
                auto it = heartbeats_.find(r);
                if ((it == heartbeats_.end() || it->second + t < curr_time)
                    && start_time_ + t < curr_time) {
                    dead_nodes.push_back(r);
                }
            }
        }
        return dead_nodes;
    }
}  // namespace ps
