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

    //���ﷵ�ص�Ӧ���Ǳ��������ڵ�ǰ�ڵ������Ѽ�¼�������е��±�
    int Customer::NewRequest(int recver) {
        std::lock_guard<std::mutex> lk(tracker_mu_);
        int num = Postoffice::Get()->GetNodeIDs(recver).size();//���ص��Ƕ�Ӧ��Ľڵ�����
        tracker_.push_back(std::make_pair(num, 0));
        //��������(number,0)�ԣ�����ÿ�յ�һ����������Ľڵ㷵�ص����ݣ���ô�ڶ�����������ֱ��������ȣ��������������Ѿ����
        return tracker_.size() - 1;//���������Ϊsize��ʾ�����������������ô���صľ��Ǵ�0��ʼ�������������
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
            recv_queue_.WaitAndPop(&recv);//��accept()��ȡ����Ϣ����recv_queue��ȡ����Ϣ
            if (!recv.meta.control.empty() &&
                recv.meta.control.cmd == Control::TERMINATE) {
                break;
            }
            recv_handle_(recv);//����Ϣ���д���
            if (!recv.meta.request) {//����յ���data��Ϣ����һ��request��˵������������ϣ���˿��ԽӴ��������͵��߳�����
                std::lock_guard<std::mutex> lk(tracker_mu_);
                tracker_[recv.meta.timestamp].second++;//������++�ܺ���⣬��Ϊ�ڸ�ֵ��ʱ��first�������server�����������������Ҫ�ȵ�����server����������
                tracker_cond_.notify_all();
            }
        }
    }

}  // namespace ps
