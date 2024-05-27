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

    void Postoffice::InitEnvironment() {//初始化操作
        DBG_PRINTF("Intial Environment");
        const char* val = NULL;
        std::string van_type = GetEnv("DMLC_PS_VAN_TYPE", "quic");//获取van类型
        van_ = Van::Create(van_type);//创建对应van
        val = CHECK_NOTNULL(Environment::Get()->find("DMLC_NUM_WORKER"));
        num_workers_ = atoi(val);//获取worker数量
        val = CHECK_NOTNULL(Environment::Get()->find("DMLC_NUM_SERVER"));
        num_servers_ = atoi(val);//获取server数量
        val = CHECK_NOTNULL(Environment::Get()->find("DMLC_ROLE"));
        std::string role(val);
        is_worker_ = role == "worker";//设置当前节点的（因为每一个节点维护一个postoffice类）角色
        is_server_ = role == "server";
        is_scheduler_ = role == "scheduler";
        verbose_ = GetEnv("PS_VERBOSE", 0);
        DBG_PRINTF("|Server_Nums:%d|Worker_Nums:%d|Current_Role:%s|", num_servers_, num_workers_, role.c_str());
    }

    void Postoffice::Start(int customer_id, const char* argv0, const bool do_barrier) {
        start_mu_.lock();
        if (init_stage_ == 0) {
            InitEnvironment();//调用初始化操作完成van的创建以及节点数量和节点角色的获取
            // init glog
            if (argv0) {
                dmlc::InitLogging(argv0);
            }
            else {
                dmlc::InitLogging("ps-lite\0");
            }

            // init node info.
            DBG_PRINTF("Initial and Classify different nodes");
            for (int i = 0; i < num_workers_; ++i) {//对所有的worker节点进行设置，使其能够发送消息到任一节点
                int id = WorkerRankToID(i);//首先获取worker的id
                for (int g : {id, kWorkerGroup, kWorkerGroup + kServerGroup, //其次遍历所有包含worker的组，比如单纯的worker组，或者是worker加server的组等
                    kWorkerGroup + kScheduler,
                    kWorkerGroup + kServerGroup + kScheduler}) {
                    node_ids_[g].push_back(id);//在这些组内添加当前设置的workerid，相当于是分类，对应的组包含所有对应的节点，比如worker组就包含所有的worker节点
                }
            }

            for (int i = 0; i < num_servers_; ++i) {//对所有的Server节点进行设置
                int id = ServerRankToID(i);
                for (int g : {id, kServerGroup, kWorkerGroup + kServerGroup,
                    kServerGroup + kScheduler,
                    kWorkerGroup + kServerGroup + kScheduler}) {
                    node_ids_[g].push_back(id);
                }
            }

            for (int g : {kScheduler, kScheduler + kServerGroup + kWorkerGroup,
                kScheduler + kWorkerGroup, kScheduler + kServerGroup}) {
                node_ids_[g].push_back(kScheduler);//设置scheduler节点，因为只有一个所以无需循环
            }
            init_stage_++;
        }
        start_mu_.unlock();

        // start van
        DBG_PRINTF("Start Van conmunication");
        van_->Start(customer_id);//开启Van通信

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
            //Barrier(customer_id, kWorkerGroup + kServerGroup + kScheduler);//当前节点告知scheduler自己已完成计算并进入阻塞状态
            Barrier(customer_id, kWorkerGroup + kServerGroup);//当前节点告知scheduler自己已完成计算并进入阻塞状态
        }
        //注意，这里第二个参数是三个group，是一个定值，也就是说scheduler也要向自身发送一个阻塞消息，等所有阻塞全部接收后才解除
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

    void Postoffice::Barrier(int customer_id, int node_group) {//这一步封装来自customer节点的barrier信息，传递给scheduler节点
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
        req.meta.recver = kScheduler;//barrier接收对象，即scheduler节点
        req.meta.request = true; //是否是req请求，是的话才会进行后续处理
        req.meta.control.cmd = Control::BARRIER; //表明是barrier控制信息
        req.meta.app_id = 0;
        req.meta.customer_id = customer_id;
        req.meta.control.barrier_group = node_group; //从传入的节点来看，这应该是三个节点组都需要
        req.meta.timestamp = van_->GetTimestamp();
        //DBG_PRINTF("Send Barrier Msg to scheduler");
        //if (role != Node::SCHEDULER ) {
        //    van_->Send(req, nullptr, 0); //发送信息
        //}
        if (role == Node::SERVER) {
            van_->Send(req, nullptr, 0); //发送信息
        }
        else if (role == Node::WORKER) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            van_->Send(req, nullptr, 0); //发送信息
        }
        DBG_PRINTF("Send Barrier Msg to scheduler successfully");
        barrier_cond_.wait(ulk, [this, customer_id] {
            return barrier_done_[0][customer_id];
            });
    }

    /*在0~KMaxKey之间根据servers的数量进行均分，分出对应数量的区间*/
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

    void Postoffice::Manage(const Message& recv) { //用于解除非scheduler端的barrier阻塞
        CHECK(!recv.meta.control.empty());
        const auto& ctrl = recv.meta.control;
        if (ctrl.cmd == Control::BARRIER && !recv.meta.request) {
            barrier_mu_.lock();
            auto size = barrier_done_[recv.meta.app_id].size();
            for (size_t customer_id = 0; customer_id < size; customer_id++) {
                barrier_done_[recv.meta.app_id][customer_id] = true;
            }
            barrier_mu_.unlock();
            barrier_cond_.notify_all();//这一步便是解除barrier
        }
    }

    void Postoffice::ManageForScheduler(int app_id) { //用于解除scheduler端的barrier阻塞
        barrier_mu_.lock();
        auto size = barrier_done_[app_id].size();
        for (size_t customer_id = 0; customer_id < size; customer_id++) {
            barrier_done_[app_id][customer_id] = true;
        }
        barrier_mu_.unlock();
        barrier_cond_.notify_all();//这一步便是解除barrier
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
