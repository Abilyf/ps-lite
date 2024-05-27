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
        SArray<Val> vals; //�����val��ʾ���ͣ���������KVPairs<Val>��val���ʾvalue������
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
            int ts = obj_->NewRequest(kServerGroup);//���ﷵ�ص�Ӧ���Ǳ��������ڵ�ǰ�ڵ��Ѽ�¼�����е��±�
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
    struct KVServerDefaultHandle {//�ڶ�����server��Ĭ�ϵ�msg����������һ����push��pull�����ִ���process�����������meta��data����
        void operator()(
            const KVMeta& req_meta, const KVPairs<Val>& req_data, KVServer<Val>* server) {
            size_t n = req_data.keys.size();//��ȡkeys����
            KVPairs<Val> res;
            if (!req_meta.pull) {
                CHECK_EQ(n, req_data.vals.size());//�����push������keys������values�����Ƿ��Ӧ
            }
            else {
                res.keys = req_data.keys; res.vals.resize(n);//�����pull����keys�����res��������res��values�Ĵ�С��res����Ӧ������
            }
            for (size_t i = 0; i < n; ++i) {//����keys��������ѭ��
                Key key = req_data.keys[i];//ȡ�õ�һ��key
                if (req_meta.push) {
                    //DBG_PRINTF("|Msg-Type:Push|");
                    store[key] += req_data.vals[i];//�����pushָ����ϴ������ݾۺϵ���store��Ӧ��key��
                }
                if (req_meta.pull) {
                    //DBG_PRINTF("|Msg-Type:Pull|");
                    res.vals[i] = store[key];//�����pullָ����store��ȡ����key��Ӧ��value�浽res���Ա㷢��
                }
            }
            server->Response(req_meta, res);//�������meta�Ͱ�װ���res��data������������Ӧ����Response��
        }
        std::unordered_map<Key, Val> store;
    };


    ///////////////////////////////////////////////////////////////////////////////

    template <typename Val>
    void KVServer<Val>::Process(const Message& msg) {//���ն˵�handle�������ȴ���msg��Ϣ���ֳ�meta��data�����֣����뵽Ĭ�ϴ�������
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
    void KVServer<Val>::Response(const KVMeta& req, const KVPairs<Val>& res) {//����������Ĭ��handle�д����msg�е�push����pull�����Լ���Ӧ�����ݺ󣬿�ʼ��֯��Ӧ��msg
        Message msg;
        //��ͷ��Ϣmeta����req
        msg.meta.app_id = obj_->app_id();
        msg.meta.customer_id = req.customer_id;
        msg.meta.request = false;
        msg.meta.push = req.push;
        msg.meta.pull = req.pull;
        msg.meta.head = req.cmd;
        msg.meta.timestamp = req.timestamp;
        msg.meta.recver = req.sender;
        //����˵��ֻ�����ԶԶ˵�msg��pull����ʱ���Ż�ִ����������
        if (res.keys.size()) {//����������KVServerDefaultHandle��֪��ֻ�е�msg����pull���ݲŻ���keys�������push�Ļ���res�е������ǿգ�Ҳ�Ͳ���ִ��
            msg.AddData(res.keys);
            msg.AddData(res.vals);
            if (res.lens.size()) {
                msg.AddData(res.lens);
            }
        }
        Postoffice::Get()->van()->Send(msg, nullptr, 0);//��һ���ǲ��ܺ�������ᷢ�ͣ������pull�Ļ���Ȼ�ǰ���Ҫ�����ݴ��ݻ�ȥ�������push�Ļ��������meta
        //��pushΪ�棬����û�����ݣ��ӽ��ն˵Ĵ�������Ҳ����worker��process����
    }

    template <typename Val>
    void KVWorker<Val>::DefaultSlicer(//Ĭ�ϵ��зֺ������ɿ����ڴ˴�����Ϣ�зֳ�QUIC���ϵĸ�ʽ
        const KVPairs<Val>& send, const std::vector<Range>& ranges,
        typename KVWorker<Val>::SlicedKVs* sliced) {
        sliced->resize(ranges.size());//�������������ô�С����sliced����������Ϊranges�и���

        // find the positions in msg.key
        size_t n = ranges.size();//����ranges�Ĵ�С��Ϊserver������
        std::vector<size_t> pos(n + 1);
        const Key* begin = send.keys.begin();//��send���kv pairs��ĳ�Ա����keys���׸�Ԫ�صĵ�ַ����
        const Key* end = send.keys.end();//��send���kv pairs��ĳ�Ա����keys��ĩβԪ�صĵ�ַ����
        for (size_t i = 0; i < n; ++i) {//++i ���贴����ʱ�����������i++Ч�ʸ��ߣ������n��ʾserver������
            if (i == 0) {
                pos[0] = std::lower_bound(begin, end, ranges[0].begin()) - begin;//lower_bound()����������ָ�������ڲ��Ҳ�С��Ŀ��ֵ�ĵ�һ��Ԫ��
                //����Ҫע��һ�㣬begin��end����������������������ָ�룬��������൱������keys��Ѱ�ҵ�һ����ranges[0].begin()����
                // ���������ָ�룬ͬʱ����һ��ָ��ļ�����������ô�����൱���Ǵ洢��������ص�ָ������λ��
                begin += pos[0];//��begin
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

    template <typename Val>//������ģ���࣬����Val��ռλ�����ƣ���Ҫ���ڷ��ͱ�̣�����˵���ǿ�Ϊ��������
    void KVWorker<Val>::Send(int timestamp, bool push, bool pull, int cmd, const KVPairs<Val>& kvs) {
        // slice the message
        SlicedKVs sliced;
        slicer_(kvs, Postoffice::Get()->GetServerKeyRanges(), &sliced);//��һ���ǶԴ����kv�����з������䣬Ϊ��ʹQUIC�ܴ���,
        //����Ҫ����ִ�С����QUIC��1536���ݰ���ȥ��ͷ�Ĳ��֣���ʱ���ܽ����ݽ��з��ͣ����Բ���democlient.c��140�е�demo_client_prepare_to_send()����

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
            const auto& kvs = s.second;//�����kvs�ǵ�ǰ��Ƭ��kvpairs
            if (kvs.keys.size()) {
                msg.AddData(kvs.keys);//���������ȴ���keys�������ݣ���ʱdata����һ��Ԫ��
                msg.AddData(kvs.vals);//�����������vals�������ݣ���ʱdata��������Ԫ��
                if (kvs.lens.size()) {
                    msg.AddData(kvs.lens);
                }
            }
            Postoffice::Get()->van()->Send(msg, nullptr, 0);//����send����׼������
        }
    }


    template <typename Val>
    void KVWorker<Val>::Process(const Message& msg) {//worker�˵Ľ��մ�����
        if (msg.meta.simple_app) {
            SimpleApp::Process(msg); return;
        }
        // store the data for pulling
        int ts = msg.meta.timestamp;
        if (msg.meta.pull) {//����İ�ͷ��pull��server�Ĵ��ݹ����в����ᷢ���ı䣬ֻ�ᱣ��ԭ�е�ֵ��Ҳ�����ʼworker������
            //������ﴦ���������server�˱�pull�µ�����
            CHECK_GE(msg.data.size(), (size_t)2);
            KVPairs<Val> kvs;
            kvs.keys = msg.data[0];//��ȡpull���ݵ�keys
            kvs.vals = msg.data[1];//��ȡpull���ݵ�vals
            if (msg.data.size() > (size_t)2) {
                kvs.lens = msg.data[2];
            }
            mu_.lock();
            recv_kvs_[ts].push_back(kvs);//����kvs���ݴ��뵽recv_kvs_�����У��Թ�mxnet���м���
            mu_.unlock();
        }
        //���Կ�������Ĵ�������û�д���push�ĵ���Ϣ������ֱ����������Ӱ��

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
