/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_KV_APP_H_
#define PS_KV_APP_H_
#include <algorithm>
#include <utility>
#include <vector>
#include <unordered_map>
#include "ps/base.h"
#include "ps/simple_app.h"
#include "ps/internal/utils.h"
namespace ps {

    /**
     * \brief the structure for a list of key-value pairs
     *
     * The keys must be unique and sorted in an increasing order.  The length of a
     * value can be more than one. If \a lens is empty, then the length
     * of a value is determined by `k=vals.size()/keys.size()`.  The \a i-th KV pair
     * is then
     *
     * \verbatim {keys[i], (vals[i*k], ..., vals[(i+1)*k-1])} \endverbatim
     *
     * If \a lens is given, then `lens[i]` is the length of the \a i-th
     * value. Let
     *
     * \verbatim n = lens[0] + .. + lens[i-1]  \endverbatim
     *
     * then the \a i-th KV pair is presented as
     *
     * \verbatim {keys[i], (vals[n], ..., vals[lens[i]+n-1])} \endverbatim
     */
    template <typename Val>
    struct KVPairs {
        // /** \brief empty constructor */
        // KVPairs() {}
        /** \brief the list of keys */
        SArray<Key> keys;
        /** \brief the according values */
        SArray<Val> vals; //这里的val表示泛型，后续类似KVPairs<Val>中val则表示value的类型
        /** \brief the according value lengths (could be empty) */
        SArray<int> lens;
        /** \brief priority */
        int priority = 0;
    };

    /**
     * \brief A worker node that can \ref Push (\ref Pull) key-value pairs to (from) server
     * nodes
     *
     * \tparam Val the type of value, which should be primitive types such as
     * int32_t and float
     */
    template<typename Val>
    class KVWorker : public SimpleApp {
    public:
        /** avoid too many this-> */
        using SimpleApp::obj_;
        /**
         * \brief callback function for \ref Push and \ref Pull
         *
         * It is called by the data receiving thread of this instance when the push or
         * pull is actually finished. Namely the kv pairs have already written into
         * servers' data structure or the kv pairs have already pulled back.
         */
        using Callback = std::function<void()>;

        /**
         * \brief constructor
         *
         * \param app_id the app id, should match with \ref KVServer's id
         * \param customer_id the customer id which is unique locally
         */
        explicit KVWorker(int app_id, int customer_id) : SimpleApp() {
            using namespace std::placeholders;
            slicer_ = std::bind(&KVWorker<Val>::DefaultSlicer, this, _1, _2, _3);
            obj_ = new Customer(app_id, customer_id, std::bind(&KVWorker<Val>::Process, this, _1));
        }

        /** \brief deconstructor */
        virtual ~KVWorker() { delete obj_; obj_ = nullptr; }

        /**
         * \brief Pushes a list of key-value pairs to all server nodes.
         *
         * This function pushes a KV list specified by \a keys and \a vals to all
         * server nodes.
         *
         * Sample usage: the following codes push two KV pairs `{1, (1.1, 1.2)}` and `{3,
         * (3.1,3.2)}` to server nodes, where the value is a length-2 float vector
         * \code
         *   KVWorker<float> w;
         *   std::vector<Key> keys = {1, 3};
         *   std::vector<float> vals = {1.1, 1.2, 3.1, 3.2};
         *   w.Push(keys, vals);
         * \endcode
         *
         * If \a lens is given, then the value can be various length. See
         * \ref KVPairs for more information.
         *
         * The KV list is partitioned and sent based on the key range each server
         * maintaining. This function returns without waiting the data are sent
         * actually. Instead, use either \ref Wait or the callback to know when
         * finished. This function is thread-safe.
         *
         * @param keys a list of keys, must be unique and sorted in increasing order
         * @param vals the according values
         * @param lens optional, lens[i] stores the value length of the \a
         * i-th KV pair
         * @param cmd an optional command sent to the servers
         * @param cb the callback which is called when the push is finished.
         * @return the timestamp of this request
         */
        int Push(const std::vector<Key>& keys,
            const std::vector<Val>& vals,
            const std::vector<int>& lens = {},
            int cmd = 0,
            const Callback& cb = nullptr,
            int priority = 0) {
            return ZPush(
                SArray<Key>(keys), SArray<Val>(vals), SArray<int>(lens), cmd, cb,
                priority);
        }

        /**
         * \brief Pulls the values associated with the keys from the server nodes
         *
         * This function pulls the values of the keys specified in \a keys from the
         * server nodes. The format is same to \ref KVPairs
         *
         * Sample usage: the following codes pull the values of keys \a 1 and \a 3
         * from the server nodes.
         * \code
         *   KVWorker<float> w;
         *   std::vector<Key> keys = {1, 3};
         *   std::vector<float> vals;
         *   w.Pull(keys, &vals);
         * \endcode
         *
         * It's a non-blocking call. The actual pulling is finished,
         * namely \a vals (and \a lens) is filled with pulled values, only
         * if \ref Wait returns or the callback is called.
         *
         * @param keys a list of keys, must be unique and sorted in increasing order
         * @param vals the buffer for the pulled values. It can be 0 size.
         * @param lens optional buffer for the value length. If set, it can be 0 size.
         * @param cmd an optional command sent to the servers
         * @param cb the callback which is called when the pull is finished.
         * @return the timestamp of this request
         */
        int Pull(const std::vector<Key>& keys,
            std::vector<Val>* vals,
            std::vector<int>* lens = nullptr,
            int cmd = 0,
            const Callback& cb = nullptr,
            int priority = 0)
        {
            SArray<Key> skeys(keys);
            int ts = AddPullCB(skeys, vals, lens, cmd, cb);
            KVPairs<Val> kvs;
            kvs.keys = skeys;
            kvs.priority = priority;
            Send(ts, false, true, cmd, kvs);
            return ts;
        }

        /**
         * \brief Pushes and Pulls a list of key-value pairs to and from the server
         * nodes.
         *
         * This function pushes the values of the keys specified in \a keys to the
         * server nodes and subsequently pulls and updates the values in \a vals.
         *
         * Sample usage: the following code pushes and pulls the values of keys
         * \a 1 and \a 3 to and from the server nodes.
         * \code
         *   KVWorker<float> w;
         *   std::vector<Key> keys = {1, 3};
         *   std::vector<float> vals;
         *   w.PushPull(keys, &vals);
         * \endcode
         *
         * It's a non-blocking call. The actual pulling is finished,
         * namely \a vals (and \a lens) is filled with pulled values, only
         * if \ref Wait returns or the callback is called.
         *
         * @param keys a list of keys, must be unique and sorted in increasing order
         * @param vals the according values
         * @param outs the buffer for the pulled values. It can be 0 size.
         * @param lens optional buffer for the value length. If set, it can be 0 size.
         * @param cmd an optional command sent to the servers
         * @param cb the callback which is called when the pull is finished.
         * @return the timestamp of this request
         */
        int PushPull(const std::vector<Key>& keys,
            const std::vector<Val>& vals,
            std::vector<Val>* outs,
            std::vector<int>* lens = nullptr,
            int cmd = 0,
            const Callback& cb = nullptr,
            int priority = 0) {
            CHECK_NOTNULL(outs);
            if (outs->empty())
                outs->resize(vals.size());
            else
                CHECK_EQ(vals.size(), outs->size());

            SArray<Key> skeys(keys);
            SArray<Val> svals(vals);
            auto souts = new SArray<Val>(outs->data(), outs->size());
            SArray<int>* slens = lens ?
                new SArray<int>(lens->data(), lens->size()) : nullptr;
            int ts = ZPushPull(skeys, svals, souts, slens, cmd,
                [this, cb, souts, slens]() {
                    delete souts;
                    delete slens;
                    if (cb) cb();
                }, priority);
            return ts;
        }

        /**
         * \brief Waits until a push or pull has been finished
         *
         * Sample usage:
         * \code
         *   int ts = w.Pull(keys, &vals);
         *   Wait(ts);
         *   // now vals is ready for use
         * \endcode
         *
         * \param timestamp the timestamp returned by the push or pull
         */
        void Wait(int timestamp) { obj_->WaitRequest(timestamp); }

        /**
         * \brief zero-copy Push
         *
         * This function is similar to \ref Push except that all data
         * will not be copied into system for better performance. It is the caller's
         * responsibility to keep the content to be not changed before actually
         * finished.
         */
        int ZPush(const SArray<Key>& keys,
            const SArray<Val>& vals,
            const SArray<int>& lens = {},
            int cmd = 0,
            const Callback& cb = nullptr,
            int priority = 0) {
            int ts = obj_->NewRequest(kServerGroup);//这里返回的应该是本次请求在当前节点已记录请求中的下标
            AddCallback(ts, cb);
            KVPairs<Val> kvs;
            kvs.keys = keys;
            kvs.vals = vals;
            kvs.lens = lens;
            kvs.priority = priority;
            Send(ts, true, false, cmd, kvs);
            return ts;
        }

        /**
         * \brief zero-copy Pull
         *
         * This function is similar to \ref Pull except that all data
         * will not be copied into system for better performance. It is the caller's
         * responsibility to keep the content to be not changed before actually
         * finished.
         */
        int ZPull(const SArray<Key>& keys,
            SArray<Val>* vals,
            SArray<int>* lens = nullptr,
            int cmd = 0,
            const Callback& cb = nullptr,
            int priority = 0) {
            int ts = AddPullCB(keys, vals, lens, cmd, cb);
            KVPairs<Val> kvs;
            kvs.keys = keys;
            kvs.priority = priority;
            Send(ts, false, true, cmd, kvs);
            return ts;
        }

        /**
         * \brief zero-copy PushPull
         *
         * This function is similar to \ref PushPull except that all data
         * will not be copied into system for better performance. It is the caller's
         * responsibility to keep the content to be not changed before actually
         * finished.
         */
        int ZPushPull(const SArray<Key>& keys,
            const SArray<Val>& vals,
            SArray<Val>* outs,
            SArray<int>* lens = nullptr,
            int cmd = 0,
            const Callback& cb = nullptr,
            int priority = 0) {
            int ts = AddPullCB(keys, outs, lens, cmd, cb);
            KVPairs<Val> kvs;
            kvs.keys = keys;
            kvs.vals = vals;
            kvs.priority = priority;
            if (lens)
                kvs.lens = *lens;
            Send(ts, true, true, cmd, kvs);
            return ts;
        }
        using SlicedKVs = std::vector<std::pair<bool, KVPairs<Val>>>;
        /**
         * \brief a slicer partitions a key-value list according to the key ranges
         * \param send the kv list for partitioning
         * \param ranges the key ranges, ranges[i] is the key range of server i
         * \param sliced the sliced lists. slices[i] should only contains keys in
         * ranges[i] and the according values
         */
        using Slicer = std::function<void(
            const KVPairs<Val>& send, const std::vector<Range>& ranges,
            SlicedKVs* sliced)>;

        /**
         * \brief set a user-defined slicer
         */
        void set_slicer(const Slicer& slicer) {
            CHECK(slicer); slicer_ = slicer;
        }

    private:
        /**
         * \brief internal pull, C/D can be either SArray or std::vector
         */
        template <typename C, typename D>
        int AddPullCB(const SArray<Key>& keys, C* vals, D* lens,
            int cmd, const Callback& cb);
        /**
         * \brief add a callback for a request. threadsafe.
         * @param cb callback
         * @param timestamp the timestamp of the request
         */
        void AddCallback(int timestamp, const Callback& cb) {
            if (!cb) return;
            std::lock_guard<std::mutex> lk(mu_);
            callbacks_[timestamp] = cb;
        }

        /**
         * \brief run and delete the callback
         * \param timestamp the timestamp of the callback
         */
        void RunCallback(int timestamp);
        /**
         * \brief send the kv list to all servers
         * @param timestamp the timestamp of the request
         * @param push whether or not it is a push request
         * @param push whether or not it is a pull request
         * @param cmd command
         */
        void Send(int timestamp, bool push, bool pull, int cmd, const KVPairs<Val>& kvs);
        /** \brief internal receive handle */
        void Process(const Message& msg);
        /** \brief default kv slicer */
        void DefaultSlicer(const KVPairs<Val>& send,
            const std::vector<Range>& ranges,
            SlicedKVs* sliced);

        /** \brief data buffer for received kvs for each timestamp */
        std::unordered_map<int, std::vector<KVPairs<Val>>> recv_kvs_;
        /** \brief callbacks for each timestamp */
        std::unordered_map<int, Callback> callbacks_;
        /** \brief lock */
        std::mutex mu_;
        /** \brief kv list slicer */
        Slicer slicer_;
    };

    /** \brief meta information about a kv request */
    struct KVMeta {
        /** \brief the int cmd */
        int cmd;
        /** \brief whether or not this is a push request */
        bool push;
        /** \brief whether or not this is a pull request */
        bool pull;
        /** \brief sender's node id */
        int sender;
        /** \brief the associated timestamp */
        int timestamp;
        /** \brief the customer id of worker */
        int customer_id;
    };

    /**
     * \brief A server node for maintaining key-value pairs
     */
    template <typename Val>
    class KVServer : public SimpleApp {
    public:
        /**
         * \brief constructor
         * \param app_id the app id, should match with \ref KVWorker's id
         */
        explicit KVServer(int app_id) : SimpleApp() {
            using namespace std::placeholders;
            obj_ = new Customer(app_id, app_id, std::bind(&KVServer<Val>::Process, this, _1));
        }

        /** \brief deconstructor */
        virtual ~KVServer() { delete obj_; obj_ = nullptr; }

        /**
         * \brief the handle to process a push/pull request from a worker
         * \param req_meta meta-info of this request
         * \param req_data kv pairs of this request
         * \param server this pointer
         */
        using ReqHandle = std::function<void(const KVMeta& req_meta,
            const KVPairs<Val>& req_data,
            KVServer* server)>;
        void set_request_handle(const ReqHandle& request_handle) {
            CHECK(request_handle) << "invalid request handle";
            request_handle_ = request_handle;
        }

        /**
         * \brief response to the push/pull request
         * \param req the meta-info of the request
         * \param res the kv pairs that will send back to the worker
         */
        void Response(const KVMeta& req, const KVPairs<Val>& res = KVPairs<Val>());

    private:
        /** \brief internal receive handle */
        void Process(const Message& msg);
        /** \brief request handle */
        ReqHandle request_handle_;
    };


    /**
     * \brief an example handle adding pushed kv into store
     */
    template <typename Val>
    struct KVServerDefaultHandle {//第二步：server端默认的msg处理函数，这一步分push和pull两部分处理process函数中输入的meta和data数据
        void operator()(
            const KVMeta& req_meta, const KVPairs<Val>& req_data, KVServer<Val>* server) {
            size_t n = req_data.keys.size();//获取keys数量
            KVPairs<Val> res;
            if (!req_meta.pull) {
                CHECK_EQ(n, req_data.vals.size());//如果是push，则检查keys数量和values数量是否对应
            }
            else {
                res.keys = req_data.keys; res.vals.resize(n);//如果是pull，则将keys赋予给res，并分配res中values的大小，res是响应的数据
            }
            for (size_t i = 0; i < n; ++i) {//依据keys数量进入循环
                Key key = req_data.keys[i];//取得第一个key
                if (req_meta.push) {
                    //DBG_PRINTF("|Msg-Type:Push|");
                    store[key] += req_data.vals[i];//如果是push指令，则将上传的数据聚合到到store对应的key中
                }
                if (req_meta.pull) {
                    //DBG_PRINTF("|Msg-Type:Pull|");
                    res.vals[i] = store[key];//如果是pull指令，则从store中取得与key对应的value存到res中以便发送
                }
            }
            server->Response(req_meta, res);//将传入的meta和包装后的res（data）数据送入响应函数Response中
        }
        std::unordered_map<Key, Val> store;
    };


    ///////////////////////////////////////////////////////////////////////////////

    template <typename Val>
    void KVServer<Val>::Process(const Message& msg) {//接收端的handle函数，先处理msg信息，分出meta和data两部分，送入到默认处理函数中
        if (msg.meta.simple_app) {
            SimpleApp::Process(msg); return;
        }
        KVMeta meta;
        meta.cmd = msg.meta.head;
        meta.push = msg.meta.push;
        meta.pull = msg.meta.pull;
        meta.sender = msg.meta.sender;
        meta.timestamp = msg.meta.timestamp;
        meta.customer_id = msg.meta.customer_id;
        KVPairs<Val> data;
        int n = msg.data.size();
        if (n) {
            CHECK_GE(n, 2);
            data.keys = msg.data[0];
            data.vals = msg.data[1];
            if (n > 2) {
                CHECK_EQ(n, 3);
                data.lens = msg.data[2];
                CHECK_EQ(data.lens.size(), data.keys.size());
            }
        }
        CHECK(request_handle_);
        request_handle_(meta, data, this);
    }

    template <typename Val>
    void KVServer<Val>::Response(const KVMeta& req, const KVPairs<Val>& res) {//第三步：在默认handle中处理好msg中的push或者pull操作以及对应的数据后，开始组织回应的msg
        Message msg;
        //包头信息meta来自req
        msg.meta.app_id = obj_->app_id();
        msg.meta.customer_id = req.customer_id;
        msg.meta.request = false;
        msg.meta.push = req.push;
        msg.meta.pull = req.pull;
        msg.meta.head = req.cmd;
        msg.meta.timestamp = req.timestamp;
        msg.meta.recver = req.sender;
        //简单来说，只有来自对端的msg是pull请求时，才会执行下面的语句
        if (res.keys.size()) {//这里从上面的KVServerDefaultHandle可知，只有当msg中是pull数据才会有keys，如果是push的话，res中的数据是空，也就不会执行
            msg.AddData(res.keys);
            msg.AddData(res.vals);
            if (res.lens.size()) {
                msg.AddData(res.lens);
            }
        }
        Postoffice::Get()->van()->Send(msg, nullptr, 0);//这一步是不管何种命令都会发送，如果是pull的话自然是把需要的数据传递回去，如果是push的话，这里的meta
        //中push为真，但是没有数据，从接收端的处理函数，也就是worker的process来看
    }

    template <typename Val>
    void KVWorker<Val>::DefaultSlicer(//默认的切分函数，可考虑在此处对消息切分成QUIC符合的格式
        const KVPairs<Val>& send, const std::vector<Range>& ranges,
        typename KVWorker<Val>::SlicedKVs* sliced) {
        sliced->resize(ranges.size());//这里是重新设置大小，将sliced的容量设置为ranges中个数

        // find the positions in msg.key
        size_t n = ranges.size();//这里ranges的大小即为server的数量
        std::vector<size_t> pos(n + 1);
        const Key* begin = send.keys.begin();//将send这个kv pairs里的成员变量keys的首个元素的地址返回
        const Key* end = send.keys.end();//将send这个kv pairs里的成员变量keys的末尾元素的地址返回
        for (size_t i = 0; i < n; ++i) {//++i 无需创建临时变量，相比于i++效率更高，这里的n表示server的数量
            if (i == 0) {
                pos[0] = std::lower_bound(begin, end, ranges[0].begin()) - begin;//lower_bound()函数用于在指定区域内查找不小于目标值的第一个元素
                //这里要注意一点，begin和end并不是两个数，他是两个指针，因此这里相当于是在keys里寻找第一个≥ranges[0].begin()的数
                // 并返回这个指针，同时做了一个指针的减法操作，那么这里相当于是存储的这个返回的指针的相对位置
                begin += pos[0];//将begin
            }
            else {
                CHECK_EQ(ranges[i - 1].end(), ranges[i].begin());
            }
            size_t len = std::lower_bound(begin, end, ranges[i].end()) - begin;
            begin += len;
            pos[i + 1] = pos[i] + len;

            // don't send it to servers for empty kv
            sliced->at(i).first = (len != 0);
        }
        CHECK_EQ(pos[n], send.keys.size());
        if (send.keys.empty()) return;

        // the length of value
        size_t k = 0, val_begin = 0, val_end = 0;
        if (send.lens.empty()) {
            k = send.vals.size() / send.keys.size();
            CHECK_EQ(k * send.keys.size(), send.vals.size());
        }
        else {
            CHECK_EQ(send.keys.size(), send.lens.size());
        }

        // slice
        for (size_t i = 0; i < n; ++i) {
            if (pos[i + 1] == pos[i]) {
                sliced->at(i).first = false;
                continue;
            }
            sliced->at(i).first = true;
            auto& kv = sliced->at(i).second;
            kv.keys = send.keys.segment(pos[i], pos[i + 1]);
            if (send.lens.size()) {
                kv.lens = send.lens.segment(pos[i], pos[i + 1]);
                for (int l : kv.lens) val_end += l;
                kv.vals = send.vals.segment(val_begin, val_end);
                val_begin = val_end;
            }
            else {
                kv.vals = send.vals.segment(pos[i] * k, pos[i + 1] * k);
            }
        }
    }

    template <typename Val>//这里是模板类，其中Val是占位符名称，主要用于泛型编程，简单来说就是可为任意类型
    void KVWorker<Val>::Send(int timestamp, bool push, bool pull, int cmd, const KVPairs<Val>& kvs) {
        // slice the message
        SlicedKVs sliced;
        slicer_(kvs, Postoffice::Get()->GetServerKeyRanges(), &sliced);//这一步是对传输的kv对作切分来传输，为了使QUIC能传输,
        //便需要将拆分大小符合QUIC中1536数据包除去包头的部分，此时才能将数据进行发送，可以参照democlient.c中140行的demo_client_prepare_to_send()函数

        // need to add response first, since it will not always trigger the callback
        int skipped = 0;
        for (size_t i = 0; i < sliced.size(); ++i) {
            if (!sliced[i].first) ++skipped;
        }
        obj_->AddResponse(timestamp, skipped);
        if ((size_t)skipped == sliced.size()) {
            RunCallback(timestamp);
        }
        //DBG_PRINTF("|Sliced Size:%d|", sliced.size());
        for (size_t i = 0; i < sliced.size(); ++i) {
            const auto& s = sliced[i];
            if (!s.first) continue;
            Message msg;
            msg.meta.app_id = obj_->app_id();
            msg.meta.customer_id = obj_->customer_id();
            msg.meta.request = true;
            msg.meta.push = push;
            msg.meta.pull = pull;
            msg.meta.head = cmd;
            msg.meta.timestamp = timestamp;
            msg.meta.recver = Postoffice::Get()->ServerRankToID(i);
            msg.meta.priority = kvs.priority;
            const auto& kvs = s.second;//这里的kvs是当前切片的kvpairs
            if (kvs.keys.size()) {
                msg.AddData(kvs.keys);//这里是首先存入keys类型数据，此时data中有一个元素
                msg.AddData(kvs.vals);//这里接着推入vals类型数据，此时data中有两个元素
                if (kvs.lens.size()) {
                    msg.AddData(kvs.lens);
                }
            }
            Postoffice::Get()->van()->Send(msg, nullptr, 0);//调用send函数准备发送
        }
    }


    template <typename Val>
    void KVWorker<Val>::Process(const Message& msg) {//worker端的接收处理函数
        if (msg.meta.simple_app) {
            SimpleApp::Process(msg); return;
        }
        // store the data for pulling
        int ts = msg.meta.timestamp;
        if (msg.meta.pull) {//这里的包头的pull在server的传递过程中并不会发生改变，只会保持原有的值，也就是最开始worker的请求，
            //因此这里处理的是来自server端被pull下的数据
            CHECK_GE(msg.data.size(), (size_t)2);
            KVPairs<Val> kvs;
            kvs.keys = msg.data[0];//获取pull数据的keys
            kvs.vals = msg.data[1];//获取pull数据的vals
            if (msg.data.size() > (size_t)2) {
                kvs.lens = msg.data[2];
            }
            mu_.lock();
            recv_kvs_[ts].push_back(kvs);//将该kvs数据存入到recv_kvs_队列中，以供mxnet进行计算
            mu_.unlock();
        }
        //可以看到这里的处理函数并没有处理push的的消息，而是直接跳过，不影响

        // finished, run callbacks
        if (obj_->NumResponse(ts) == Postoffice::Get()->num_servers() - 1) {
            RunCallback(ts);
        }
    }
    template <typename Val>
    void KVWorker<Val>::RunCallback(int timestamp) {
        mu_.lock();
        auto it = callbacks_.find(timestamp);
        if (it != callbacks_.end()) {
            mu_.unlock();

            CHECK(it->second);
            it->second();

            mu_.lock();
            callbacks_.erase(it);
        }
        mu_.unlock();
    }

    template <typename Val>
    template <typename C, typename D>
    int KVWorker<Val>::AddPullCB(
        const SArray<Key>& keys, C* vals, D* lens, int cmd,
        const Callback& cb) {
        int ts = obj_->NewRequest(kServerGroup);
        AddCallback(ts, [this, ts, keys, vals, lens, cb]() mutable {
            mu_.lock();
            auto& kvs = recv_kvs_[ts];
            mu_.unlock();

            // do check
            size_t total_key = 0, total_val = 0;
            for (const auto& s : kvs) {
                Range range = FindRange(keys, s.keys.front(), s.keys.back() + 1);
                CHECK_EQ(range.size(), s.keys.size())
                    << "unmatched keys size from one server";
                if (lens) CHECK_EQ(s.lens.size(), s.keys.size());
                total_key += s.keys.size();
                total_val += s.vals.size();
            }
            CHECK_EQ(total_key, keys.size()) << "lost some servers?";

            // fill vals and lens
            std::sort(kvs.begin(), kvs.end(), [](
                const KVPairs<Val>& a, const KVPairs<Val>& b) {
                    return a.keys.front() < b.keys.front();
                });
            CHECK_NOTNULL(vals);
            if (vals->empty()) {
                vals->resize(total_val);
            }
            else {
                CHECK_EQ(vals->size(), total_val);
            }
            Val* p_vals = vals->data();
            int* p_lens = nullptr;
            if (lens) {
                if (lens->empty()) {
                    lens->resize(keys.size());
                }
                else {
                    CHECK_EQ(lens->size(), keys.size());
                }
                p_lens = lens->data();
            }
            for (const auto& s : kvs) {
                memcpy(p_vals, s.vals.data(), s.vals.size() * sizeof(Val));
                p_vals += s.vals.size();
                if (p_lens) {
                    memcpy(p_lens, s.lens.data(), s.lens.size() * sizeof(int));
                    p_lens += s.lens.size();
                }
            }

            mu_.lock();
            recv_kvs_.erase(ts);
            mu_.unlock();
            if (cb) cb();
            });

        return ts;
    }

}  // namespace ps
#endif  // PS_KV_APP_H_
