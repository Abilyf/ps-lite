/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_INTERNAL_FastTHREADSAFE_QUEUE_H_
#define PS_INTERNAL_FastTHREADSAFE_QUEUE_H_
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unistd.h>
#include <vector>
namespace ps {

    //FPT Impl.
/**
 * \brief thread-safe priority queues allowing push and waited pop, for packet-level preemption
 */
    template<typename T> class ThreadsafePriorityQueues {
    public:

        ThreadsafePriorityQueues() {}

        ~ThreadsafePriorityQueues() {}

        /**
       * \brief init.
       * \param num number of queues
       */
        void Init(int num) {
            num_ = num;
            for (int i = 0; i < num_; i++) {
                std::queue <T> queue;
                priority_queues_.push_back(queue);
            }
            std::vector <std::mutex> list(num_);
            mus_.swap(list);
        }

        /**
         * \brief push an value into one queue. threadsafe.
         * \param new_value the value
         * \param priority  always <=0, and value with bigger priority will be placed in the front of priority_queues_
         */
        void Push(T new_value, int priority) {
            CHECK_LE(priority, 0);
            int index = -priority;
            CHECK_LE(index, num_ - 1);
            mus_[index].lock();
            priority_queues_[index].push(std::move(new_value));
            mus_[index].unlock();
            //printf("push a value to queue with index %d, time_us: %d ; ",index, clock() * 1e6 / CLOCKS_PER_SEC);
            //printf("it empty now ? %d \n",priority_queues_[index].empty());
            //cond_.notify_all();
        }

        /**
         * \brief return false if empty, threadsafe
         * \param value the poped value
         */
        bool Pop(T* value) {
            int index = -1;
            for (int i = 0; i < num_; i++) {
                mus_[i].lock();
                if (!priority_queues_[i].empty()) {
                    index = i;
                    mus_[i].unlock();
                    break;
                }
                mus_[i].unlock();
            }

            //queues all empty
            if (index == -1) {
                return false;
            }

            mus_[index].lock();
            *value = std::move(priority_queues_[index].front());
            priority_queues_[index].pop();
            mus_[index].unlock();
            return true;

        }

    private:
        std::vector <std::mutex> mus_;
        std::vector <std::queue<T>> priority_queues_;
        int num_ = 0;
    };

}  // namespace ps

#endif  // PS_INTERNAL_THREADSAFE_QUEUE_H_
