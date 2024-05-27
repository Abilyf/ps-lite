/**
 *  Copyright (c) 2015 by Contributors
 */
#include "ps/internal/customer.h"
#include "ps/internal/postoffice.h"
namespace ps {

    const int Node::kEmpty = std::numeric_limits<int>::max();
    const int Meta::kEmpty = std::numeric_limits<int>::max();

    Customer::Customer(int app_id, int customer_id, const Customer::RecvHandle& recv_handle)
        : app_id_(app_id), customer_id_(customer_id), recv_handle_(recv_handle) {
        Postoffice::Get()->AddCustomer(this);
        recv_thread_ = std::unique_ptr<std::thread>(new std::thread(&Customer::Receiving, this));
    }

    Customer::~Customer() {
        Postoffice::Get()->RemoveCustomer(this);
        Message msg;
        msg.meta.control.cmd = Control::TERMINATE;
        recv_queue_.Push(msg);
        recv_thread_->join();
    }

    //这里返回的应该是本次请求在当前节点所有已记录的请求中的下标
    int Customer::NewRequest(int recver) {
        std::lock_guard<std::mutex> lk(tracker_mu_);
        int num = Postoffice::Get()->GetNodeIDs(recver).size();//返回的是对应组的节点数量
        tracker_.push_back(std::make_pair(num, 0));
        //这里推入(number,0)对，后续每收到一个本次请求的节点返回的数据，那么第二个数自增，直到两者相等，表明本次请求已经完成
        return tracker_.size() - 1;//这里可以认为size表示的是请求的数量，那么返回的就是从0开始计数的请求序号
    }

    void Customer::WaitRequest(int timestamp) {
        std::unique_lock<std::mutex> lk(tracker_mu_);
        tracker_cond_.wait(lk, [this, timestamp] {
            return tracker_[timestamp].first == tracker_[timestamp].second;
            });
    }

    int Customer::NumResponse(int timestamp) {
        std::lock_guard<std::mutex> lk(tracker_mu_);
        return tracker_[timestamp].second;
    }

    void Customer::AddResponse(int timestamp, int num) {
        std::lock_guard<std::mutex> lk(tracker_mu_);
        tracker_[timestamp].second += num;
    }

    void Customer::Receiving() {
        while (true) {
            Message recv;
            recv_queue_.WaitAndPop(&recv);//从accept()存取的消息队列recv_queue中取出消息
            if (!recv.meta.control.empty() &&
                recv.meta.control.cmd == Control::TERMINATE) {
                break;
            }
            recv_handle_(recv);//对消息进行处理
            if (!recv.meta.request) {//如果收到的data信息不是一个request，说明本次推送完毕，因此可以接触本次推送的线程阻塞
                std::lock_guard<std::mutex> lk(tracker_mu_);
                tracker_[recv.meta.timestamp].second++;//这里用++很好理解，因为在赋值的时候，first代表的是server的数量，因此这里是要等到所有server都返回数据
                tracker_cond_.notify_all();
            }
        }
    }

}  // namespace ps
