/*
* Author: Christian Huitema
* Copyright (c) 2019, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "picoquic_internal.h"
#include <stdlib.h>
#include <string.h>
#include "cc_common.h"
#include "picoquic.h"

/*
Implementation of the IACC algorithm, tuned for Picoquic.

The main idea of IACC is to track the "bottleneck bandwidth", and to tune the
transport stack to send exactly at that speed. This ensures good network
utilisation while avoiding the building of queues. To do that the stack
needs to constantly estimate the available data rate. It does that by
measuring the rate at which acknowledgements come back, providing what it
calls the delivery rate.

That approach includes an implicit feedback loop. The delivery rate can never
exceed the sending rate. That will effectively detects a transmission slow
down due to partial congestion, but if the algorithm just did that the
sending rate will remain constant when the network is lightly loaded
and ratchet down during time of congestion, leading to very low efficiency.
The available bandwidth can only be tested by occasionally sending faster
than the measured delivery rate.

IACC does that by following a cycle of "send, test and drain". During the
sending period, the stack sends at the measured rate. During the testing
period, it sends faster, 25% faster with recommended parameters. This
risk creating a queue if the bandwidth had not increased, so the test
period is followed by a drain period during which the stack sends 25%
slower than the measured rate. If the test is successful, the new bandwidth
measurement will be available at the end of the draining period, and
the increased bandwidth will be used in the next cycle.

Tuning the sending rate does not guarantee a short queue, it only
guarantees a stable queue. IACC controls the queue by limiting the
amount of data "in flight" (congestion window, CWIN) to the product
of the bandwidth estimate by the RTT estimate, plus a safety marging to ensure
continuous transmission. Using the average RTT there would lead to a runaway
loop in which oversized windows lead to increased queues and then increased
average RTT. Instead of average RTT, IACC uses a minimum RTT. Since the
mimimum RTT might vary with routing changes, the minimum RTT is measured
on a sliding window of 10 seconds.

The bandwidth estimation needs to be robust against short term variations
common in wireless networks. IACC retains the maximum
delivery rate observed over a series of probing intervals. Each interval
starts with a specific packet transmission and ends when that packet
or a later transmission is acknowledged. IACC does that by tracking
the delivered counter associated with packets and comparing it to
the delivered counter at start of period.

During start-up, IACC performs its own equivalent of Reno's slow-start.
It does that by using a pacing gain of 2.89, i.e. sending 2.89 times
faster than the measured maximum. It exits slow start when it found
a bandwidth sufficient to fill the pipe.

The bandwidth measurements can be wrong if the application is not sending
enough data to fill the pipe. IACC tracks that, and does not reduce bandwidth
or exit slow start if the application is limiting transmission.

This implementation follows draft-cardwell-iccrg-iacc-congestion-control,
with a couple of changes for handling the multipath nature of quic.
There is a IACC control state per path.
Most of IACC the variables defined in the draft are implemented
in the "IACC state" structure, with a few exceptions:

* IACC.delivered is represented by path_x.delivered, and is maintained
  as part of ACK processing

* Instead of "bytes_in_transit", we use "bytes_in_transit", which is
  already maintained by the stack.

* Compute bytes_delivered by summing all calls to ACK(bytes) before
  the call to RTT update.

* In the Probe BW mode, the draft suggests cwnd_gain = 2. We observed
  that this results in queue sizes of 2, which is too high, so we
  reset that to 1.125.

The "packet" variables are defined in the picoquic_packet_t.

Early testing showed that IACC startup phase requires several more RTT
than the Hystart process used in modern versions of Reno or Cubic. IACC
only ramps up the data rate after the first bandwidth measurement is
available, 2*RTT after start, while Reno or Cubic start ramping up
after just 1 RTT. IACC only exits startup if three consecutive RTT
pass without significant BW measurement increase, which not only
adds delay but also creates big queues as data is sent at 2.89 times
the bottleneck rate. This is a tradeoff: longer search for bandwidth in
slow start is less likely to stop too early because of transient
issues, but one high bandwidth and long delay links this translates
to long delays and a big batch of packet losses.

This IACC implementation addresses these issues by switching to
Hystart instead of startup if the RTT is above the Reno target of
100 ms.

*/

typedef enum {
    picoquic_iacc_alg_startup = 0,
    picoquic_iacc_alg_drain,
    picoquic_iacc_alg_probe_oversent,
    picoquic_iacc_alg_probe_control,
    picoquic_iacc_alg_probe_steady,
    picoquic_iacc_alg_probe_rtt,
    picoquic_iacc_alg_startup_long_rtt
} picoquic_iacc_alg_state_t;

#define IACC_BTL_BW_FILTER_LENGTH 10
#define IACC_RT_PROP_FILTER_LENGTH 10
#define IACC_HIGH_GAIN 2.8853900817779 /* 2/ln(2) */
#define IACC_MIN_PIPE_CWND(mss) (4*mss)
#define IACC_GAIN_CYCLE_LEN 8
#define IACC_PROBE_RTT_INTERVAL 10000000 /* 10 sec, 10000000 microsecs */
#define IACC_PROBE_RTT_PROP_INTERVAL 2500000 /* 10 sec, 10000000 microsecs */
#define IACC_PROBE_RTT_DURATION 200000 /* 200msec, 200000 microsecs */
#define IACC_PACING_RATE_LOW 150000.0 /* 150000 B/s = 1.2 Mbps */
#define IACC_PACING_RATE_MEDIUM 3000000.0 /* 3000000 B/s = 24 Mbps */
#define IACC_GAIN_CYCLE_LEN 8
#define IACC_GAIN_CYCLE_MAX_START 5

#define iacc_max(a,b) ((a) > (b) ? (a) : (b))
#define iacc_min(a,b) ((a) < (b) ? (a) : (b))

static const double iacc_pacing_gain_cycle[IACC_GAIN_CYCLE_LEN] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.25, 0.75 };

typedef struct st_picoquic_iacc_state_t {
    picoquic_iacc_alg_state_t state;
    uint64_t btl_bw;
    uint64_t next_round_delivered;
    uint64_t round_start_time;
    uint64_t btl_bw_filter[IACC_BTL_BW_FILTER_LENGTH];
    uint64_t full_bw;
    uint64_t rt_prop;
    uint64_t rt_prop_stamp;
    uint64_t probe_rt_prop;
    uint64_t probe_rt_prop_stamp;
    uint64_t cycle_stamp;
    uint64_t probe_rtt_done_stamp;
    uint64_t prior_cwnd;
    uint64_t prior_in_flight;
    uint64_t bytes_delivered;
    uint64_t send_quantum;
    picoquic_min_max_rtt_t rtt_filter;
    uint64_t target_cwnd;
    uint64_t last_sequence_blocked;

    uint32_t delta_increase_cnt;
    uint32_t delta_reduce_cnt;

    /*inflight  delta record*/
    uint32_t inflight_current;
    uint32_t over_sent;
    uint32_t inflight_prior;
    int enter_from_drain;

    /*iacc optimize parameters*/
    uint32_t iacc_optimization;
    uint32_t oversent_percent;
    uint32_t pacing_gain_percent;
    uint32_t smooth_iacc_percent;
    uint32_t steady_thresh;
    uint32_t full_bw_reach_max;
    uint64_t bytes_sent_in_round_start;
    uint64_t bytes_delivered_in_round_start;
    uint64_t time_push;
    uint64_t time_pull;
    uint64_t time_calculate;

    /*for graph*/
    uint32_t current_bandwidth;
    uint32_t cur_inflight;

    

    double pacing_gain;
    double cwnd_gain;
    double pacing_rate;
    size_t cycle_index;
    int round_count;
    int full_bw_count;
    int filled_pipe : 1;
    int round_start : 1;
    int rt_prop_expired : 1;
    int probe_rt_prop_expired : 1;
    int probe_rtt_round_done : 1;
    int idle_restart : 1;
    int packet_conservation : 1;
} picoquic_iacc_state_t;

void IACCEnterStartupLongRTT(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x)
{
    uint64_t cwnd = PICOQUIC_CWIN_INITIAL;
    iacc_state->state = picoquic_iacc_alg_startup_long_rtt;

    if (path_x->smoothed_rtt > PICOQUIC_TARGET_RENO_RTT) {
        cwnd = (uint64_t)((double)cwnd * (double)path_x->smoothed_rtt / (double)PICOQUIC_TARGET_RENO_RTT);
    }
    if (cwnd > path_x->cwin) {
        path_x->cwin = cwnd;
    }
}

/*�����������׶Σ�pacing_gain��cwnd_gain������Ϊ2.885*/
void IACCEnterStartup(picoquic_iacc_state_t* iacc_state)
{
    iacc_state->state = picoquic_iacc_alg_startup;
    iacc_state->pacing_gain = IACC_HIGH_GAIN;
    iacc_state->cwnd_gain = IACC_HIGH_GAIN;
}

/*���ݵ�ǰiacc��pacing_rate�Լ���ǰ·��patg_x�����õķ������Ԫsend_mtu������iacc�е�send_quantumֵ�����ֵ�������������ѧϰ*/
void IACCSetSendQuantum(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x)
{
    if (iacc_state->pacing_rate < IACC_PACING_RATE_LOW) {
        iacc_state->send_quantum = 1ull * path_x->send_mtu;
    }
    else if (iacc_state->pacing_rate < IACC_PACING_RATE_MEDIUM) {
        iacc_state->send_quantum = 2ull * path_x->send_mtu;
    }
    else {
        iacc_state->send_quantum = (uint64_t)(iacc_state->pacing_rate * 0.001);
        if (iacc_state->send_quantum > 64000) {
            iacc_state->send_quantum = 64000;
        }
    }
}

/*��������·bdp*/
double IACCBDP(picoquic_iacc_state_t* iacc_state) {
    return (((double)iacc_state->btl_bw * (double)iacc_state->rt_prop) / 1000000.0);
}

/*���㵱ǰ��·bdp�����㴫��gainֵ�µ�cwndֵ�������cwnd������һ��������sendQuantum��������*/
uint64_t IACCInflight(picoquic_iacc_state_t* iacc_state, double gain)
{
    uint64_t cwnd = PICOQUIC_CWIN_INITIAL;
    if (iacc_state->rt_prop != UINT64_MAX) {
        /* Bandwidth is estimated in bytes per second, rtt in microseconds*/
        double estimated_bdp = (((double)iacc_state->btl_bw * (double)iacc_state->rt_prop) / 1000000.0);//����bdp
        uint64_t quanta = 3 * iacc_state->send_quantum;
        cwnd = (uint64_t)(gain * estimated_bdp) + quanta;
    }
    return cwnd;
}

/*���µ�ǰ��target_cwndֵΪIACCInflight���ص�ֵ�������gainֵΪ��ǰiacc_state�е�cwnd_gain*/
void IACCUpdateTargetCwnd(picoquic_iacc_state_t* iacc_state)
{
    iacc_state->target_cwnd = IACCInflight(iacc_state, iacc_state->cwnd_gain);
}

/*��ʼ��iacc�ṹ�壬�����丳ֵ�������path_x����congestion_alg_state��*/
static void picoquic_iacc_init(picoquic_cnx_t* cnx, picoquic_path_t* path_x)
{
    /* Initialize the state of the congestion control algorithm */
    picoquic_iacc_state_t* iacc_state = (picoquic_iacc_state_t*)malloc(sizeof(picoquic_iacc_state_t));
    path_x->congestion_alg_state = (void*)iacc_state;
    if (iacc_state != NULL) {
        memset(iacc_state, 0, sizeof(picoquic_iacc_state_t));
        path_x->cwin = PICOQUIC_CWIN_INITIAL;
        iacc_state->rt_prop = UINT64_MAX;
        uint64_t current_time = picoquic_current_time();
        iacc_state->rt_prop_stamp = current_time;//��ʼ��rtpropʱ���Ϊ��ǰʱ�䣨���ڼ���ʱ�䣩
        iacc_state->cycle_stamp = current_time;//��ʼ��cycleʱ���Ϊ��ǰʱ�䣬���ڴ���̽��׶ε�cycle����

        /*for graph*/
        iacc_state->cur_inflight = 0;
        iacc_state->current_bandwidth = 0;
        iacc_state->bytes_sent_in_round_start = 0;
        iacc_state->bytes_delivered_in_round_start = 0;

        /*for oversent*/
        iacc_state->oversent_percent = 150;
        iacc_state->pacing_gain_percent = IACC_HIGH_GAIN * 100;
        iacc_state->smooth_iacc_percent = 100;
        iacc_state->steady_thresh = 50;
        iacc_state->full_bw_reach_max = 3;

        IACCEnterStartup(iacc_state);//�����������׶�
        IACCSetSendQuantum(iacc_state, path_x);//����send_quantumֵ����ʼ���׶�pacing_rateΪ0���ʴ�ʱquantumֵΪһ��sendmtu
        IACCUpdateTargetCwnd(iacc_state);//��ʼ��targetcwnd������btlbw��ʼֵΪ0���ʼ����bdpֵΪ0����˳�ʼcwndΪ���ϵ�send_quantum��Ҳ����һ��mtu
    }
}

/* Release the state of the congestion control algorithm */
static void picoquic_iacc_delete(picoquic_cnx_t* cnx, picoquic_path_t* path_x)
{
    if (path_x->congestion_alg_state != NULL) {
        free(path_x->congestion_alg_state);
        path_x->congestion_alg_state = NULL;
    }
}

/* Track the round count using the "delivered" counter. The value carried per
 * packet is the delivered count when this packet was sent. If it is greater
 * than next_round_delivered, it means that the packet was sent at or after
 * the beginning of the round, and thus that at least one RTT has elapsed
 * for this round. */

void IACCUpdateBtlBw(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x)
{
    uint64_t bandwidth_estimate = path_x->bandwidth_estimate;
    if (iacc_state->last_sequence_blocked == 0 || !picoquic_cc_was_cwin_blocked(path_x, iacc_state->last_sequence_blocked)) {
        // the estimation is not reliable because the CWIN was not probed entirely
        return;
    }
    /*������Ի�һ���������ͣ����ϱ������ָÿ�����ʾiacc��һ�ֿ�ʼǰ�Ѿ����͵���������Ҳ����iacc_state->next_round_delivered
    ��path_x->delivered_last_packet���ʾ��һ�����ݰ�����֮������·�����Ѿ����͵����������������ͺܺý����ˣ������ֵ�������ϵ�ĳһ����
    ��ͱ�ʾ����һ��֮ǰ�����ݶ��������ˣ����������ˣ���˿��Կ�ʼ��һ�ֵ����ݷ��ͣ�����round_start������Ϊ0����Ҳ��round����������
    */
    DBG_PRINTF("|UPDATE BW|");
    if (path_x->delivered_last_packet >= iacc_state->next_round_delivered)
    {
        DBG_PRINTF("|ROUND START|");
        iacc_state->next_round_delivered = path_x->delivered;//������һ�ֿ�ʼ����Ҫ�����ݷ�������
        iacc_state->round_count++;
        iacc_state->round_start = 1;
    }
    else {
        iacc_state->round_start = 0;
    }

    if (iacc_state->round_start) {
        /* Forget the oldest BW round, shift by 1, compute the max BTL_BW for
         * the remaining rounds, set current round max to current value */

        iacc_state->btl_bw = 0;

        /*��һ������׷�����ƿ������*/
        for (int i = IACC_BTL_BW_FILTER_LENGTH - 2; i >= 0; i--) {
            uint64_t b = iacc_state->btl_bw_filter[i];
            iacc_state->btl_bw_filter[i + 1] = b;
            if (b > iacc_state->btl_bw) {
                iacc_state->btl_bw = b;
            }
        }

        iacc_state->btl_bw_filter[0] = 0;
    }

    if (bandwidth_estimate > iacc_state->btl_bw_filter[0]) {
        iacc_state->btl_bw_filter[0] = bandwidth_estimate;
        if (bandwidth_estimate > iacc_state->btl_bw) {
            iacc_state->btl_bw = bandwidth_estimate;
        }
    }
}

/* This will use one way samples if available */
/* Should augment that with common RTT filter to suppress jitter */
/*�ж��Ƿ���Ҫ���¼�¼��RTprop*/
void IACCUpdateRTprop(picoquic_iacc_state_t* iacc_state, uint64_t rtt_sample, uint64_t current_time, picoquic_path_t* path_x)
{
    iacc_state->probe_rt_prop_expired = current_time > (iacc_state->probe_rt_prop_stamp + IACC_PROBE_RTT_PROP_INTERVAL);
    if (rtt_sample <= iacc_state->probe_rt_prop || iacc_state->probe_rt_prop_expired) {
        iacc_state->probe_rt_prop = rtt_sample;
        iacc_state->probe_rt_prop_stamp = current_time;
    }

    //�����ʾ�����ǰʱ������ϴ�rtprop��¼ʱ���ѳ���10�룬���ʾ�ϴμ�¼��rtpropʧЧ����Ҫ���²�������¼
    iacc_state->rt_prop_expired =
        current_time > iacc_state->rt_prop_stamp + IACC_PROBE_RTT_INTERVAL;//������ܵ�����Ϊ2.5��
    if (iacc_state->probe_rt_prop <= iacc_state->rt_prop || iacc_state->rt_prop_expired) {
        //���¼�¼rtprop������Ϊ��ѡһ��Ҫô������rttС�����ֵ��Ҫôrtprop��ʱ��δ���¶�ʧЧ
        iacc_state->rt_prop = iacc_state->probe_rt_prop;//���������������֮һ�������rtpropΪ��ǰrtt����ֵ�������п��ܳ��ֱ������
        iacc_state->rt_prop_stamp = iacc_state->probe_rt_prop_stamp;//ͬʱ���¼�¼��ʱ���
    }

    if (iacc_state->state == picoquic_iacc_alg_probe_steady && !iacc_state->idle_restart) {
        if (iacc_state->round_start) {
            iacc_state->inflight_current = path_x->bytes_in_transit;
            IACCEnterProbeRound2(iacc_state, path_x);
            iacc_state->round_start = 0;
        }
    }

    if (iacc_state->state == picoquic_iacc_alg_probe_oversent && !iacc_state->idle_restart) {
        DBG_PRINTF("in round2");
        double bdp = IACCBDP(iacc_state);
        if (iacc_state->round_start) {
            double delta_bw;
            uint64_t delta_sent = path_x->bytes_send - iacc_state->bytes_sent_in_round_start;
            uint64_t delta_acked = iacc_state->bytes_delivered - iacc_state->bytes_delivered_in_round_start;

            if (bdp == 0) {
                iacc_state->cwnd_gain = 0.2;
            }
            else
            {
                delta_bw = ((double)delta_sent - (double)delta_acked) / bdp;
                double tmp = iacc_state->cwnd_gain - delta_bw;
                DBG_PRINTF("|IACC-Delta_bw:%.3f|cwndgain_gap:%.3f|", delta_bw, tmp);
                if (delta_bw > 0) {
                    iacc_state->cwnd_gain = iacc_max(tmp, 0.2) * iacc_state->smooth_iacc_percent / 100.0;
                }
                else if (delta_bw <= 0) {
                    iacc_state->cwnd_gain = tmp * iacc_state->smooth_iacc_percent / 100.0;
                }
            }

            IACCEnterProbeRound3(iacc_state);
            iacc_state->inflight_current = 0;
            iacc_state->round_start = 0;
        }
    }

    if (iacc_state->state == picoquic_iacc_alg_probe_control && !iacc_state->idle_restart) {
        if (iacc_state->round_start) {
            IACCEnterProbeRound1(iacc_state);
        }
    }

    if (iacc_state->bytes_delivered > 0) {
        iacc_state->idle_restart = 0;
    }
}

/*��һ�����ж��Ƿ���Խ���cycle�е���һ���׶�*/
int IACCIsNextCyclePhase(picoquic_iacc_state_t* iacc_state, uint64_t prior_in_flight, uint64_t packets_lost, uint64_t current_time)
{
    int is_full_length = (current_time - iacc_state->cycle_stamp) > iacc_state->rt_prop;
    //��������ֵ��ʾ��ǰʱ�����һ�μ�¼��cycleʱ����ļ���Ƿ����һ��rtprop�����ǵ�һ������

    if (iacc_state->pacing_gain != 1.0) {
        if (iacc_state->pacing_gain > 1.0) {
            is_full_length &=
                (packets_lost > 0 ||
                    prior_in_flight >= IACCInflight(iacc_state, iacc_state->pacing_gain));
        }
        else {  /*  (IACC.pacing_gain < 1) */
            is_full_length |= prior_in_flight <= IACCInflight(iacc_state, 1.0);
        }
    }
    return is_full_length;
}

/*����cycle����pacinggain*/
void IACCAdvanceCyclePhase(picoquic_iacc_state_t* iacc_state, uint64_t current_time)
{
    iacc_state->cycle_stamp = current_time;//���µ�ǰcycleʱ���
    iacc_state->cycle_index++;//����index
    if (iacc_state->cycle_index >= IACC_GAIN_CYCLE_LEN) {
        int start = (int)(iacc_state->rt_prop / PICOQUIC_TARGET_RENO_RTT);
        if (start > IACC_GAIN_CYCLE_MAX_START) {
            start = IACC_GAIN_CYCLE_MAX_START;
        }
        iacc_state->cycle_index = start;
    }

    iacc_state->pacing_gain = iacc_pacing_gain_cycle[iacc_state->cycle_index];//�����趨��ֵ����pacing_gain
}

/*����Ƿ���Ҫͨ������̽��׶ε�cycle������pacing_gain*/
/*
void IACCCheckCyclePhase(picoquic_iacc_state_t* iacc_state, uint64_t packets_lost, uint64_t current_time)
{
    if (iacc_state->state == picoquic_iacc_alg_probe_bw &&
        IACCIsNextCyclePhase(iacc_state, iacc_state->prior_in_flight, packets_lost, current_time)) {
        IACCAdvanceCyclePhase(iacc_state, current_time);//�������������������1�����ڴ���̽��׶Σ�2�����Խ�����һ���������ڣ������pacing_gain
    }
}
*/

/*����Ƿ�ﵽƿ������û������²����¼�������������������������������ʾ�ѵ���*/
void IACCCheckFullPipe(picoquic_iacc_state_t* iacc_state, int rs_is_app_limited)
{
    if (!iacc_state->filled_pipe && iacc_state->round_start && !rs_is_app_limited) {
        if (iacc_state->btl_bw >= iacc_state->full_bw * 1.25) {  // IACC.BtlBw still growing?
            iacc_state->full_bw = iacc_state->btl_bw;   // record new baseline level
            iacc_state->full_bw_count = 0;
        }
        else {
            iacc_state->full_bw_count++; // another round w/o much growth
            if (iacc_state->full_bw_count >= 3) {
                iacc_state->filled_pipe = 1;
            }
        }
    }
}

/*�������̽��׶Σ���ʱpacing_gain����Ϊ1��cwnd_gain����Ϊ1.5*/
/*void IACCEnterProbeBW(picoquic_iacc_state_t* iacc_state, uint64_t current_time)
{
    iacc_state->state = picoquic_iacc_alg_probe_bw;
    iacc_state->pacing_gain = 1.0;
    iacc_state->cwnd_gain = 1.5;
    iacc_state->cycle_index = 4;  // TODO: random_int_in_range(0, 5); 
    IACCAdvanceCyclePhase(iacc_state, current_time);
}
*/

/*�����ſս׶Σ�pacing_gain����Ϊ1/2.885��cwnd_gain����Ϊ2.885*/
void IACCEnterDrain(picoquic_iacc_state_t* iacc_state)
{
    iacc_state->state = picoquic_iacc_alg_drain;
    iacc_state->pacing_gain = 1.0 / IACC_HIGH_GAIN;  /* pace slowly */
    iacc_state->cwnd_gain = IACC_HIGH_GAIN;   /* maintain cwnd */
    iacc_state->enter_from_drain = 1;
}

/*����Ƿ���Ҫ�������������ſգ�����ſս������̽��׶�*/
void IACCCheckDrain(picoquic_iacc_state_t* iacc_state, uint64_t bytes_in_transit, uint64_t current_time)
{
    if (iacc_state->state == picoquic_iacc_alg_startup && iacc_state->filled_pipe) {
        IACCEnterDrain(iacc_state);//��������������׶����Ѵﵽƿ������������ſս׶�
    }

    if (iacc_state->state == picoquic_iacc_alg_drain && bytes_in_transit <= IACCInflight(iacc_state, 1.0)) {
        //��������ſս׶Σ�����;�ֽ���С�ڼ���Ŀ��������������ʾ�ſ���ϣ��ɽ������̽��׶�
        //IACCEnterProbeBW(iacc_state, current_time);  /* we estimate queue is drained */
        IACCEnterProbeRound1(iacc_state);
    }
}

/*����round1�׶�*/
void IACCEnterProbeRound1(picoquic_iacc_state_t* iacc_state) {
    iacc_state->state = picoquic_iacc_alg_probe_steady;
    if (iacc_state->enter_from_drain) {
        iacc_state->cwnd_gain = 2.5;
        iacc_state->enter_from_drain = 2.5;
    }
    iacc_state->pacing_gain = iacc_state->pacing_gain_percent / 100.0;
}

/*����round2�׶�*/
void IACCEnterProbeRound2(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x) {
    iacc_state->state = picoquic_iacc_alg_probe_oversent;
    iacc_state->pacing_gain = iacc_state->pacing_gain_percent / 100.0;
    iacc_state->cwnd_gain = iacc_state->oversent_percent / 100.0;
    iacc_state->over_sent = 0;
    iacc_state->bytes_sent_in_round_start = path_x->bytes_send;
    iacc_state->bytes_delivered_in_round_start = iacc_state->bytes_delivered;
}

/*����round3�׶�*/
void IACCEnterProbeRound3(picoquic_iacc_state_t* iacc_state) {
    iacc_state->state = picoquic_iacc_alg_probe_control;
    iacc_state->pacing_gain = iacc_state->pacing_gain_percent / 100.0;
}

void IACCExitStartupLongRtt(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x, uint64_t current_time)
{
    /* Reset the round filter so it will start at current time */
    iacc_state->next_round_delivered = path_x->delivered;
    iacc_state->round_count++;
    iacc_state->round_start = 1;
    /* Set the filled pipe indicator */
    iacc_state->full_bw = iacc_state->btl_bw;
    iacc_state->full_bw_count = 3;
    iacc_state->filled_pipe = 1;
    /* Enter drain */
    IACCEnterDrain(iacc_state);
    /* If there were just few bytes in transit, enter probe */
    if (path_x->bytes_in_transit <= IACCInflight(iacc_state, 1.0)) {
        //IACCEnter(iacc_state, current_time);
        IACCEnterProbeRound1(iacc_state);
    }
}

/*
void IACCEnterProbeRTT(picoquic_iacc_state_t* iacc_state)
{
    iacc_state->state = picoquic_iacc_alg_probe_rtt;
    iacc_state->pacing_gain = 1.0;
    iacc_state->cwnd_gain = 1.0;
}
*/

/*�����ʱ̽�⵽�Ĵ�����ƿ��������ô���˳�rtt̽��׶�ֱ�ӵ�����̽��׶Σ���������������׶�*/
/*
void IACCExitProbeRTT(picoquic_iacc_state_t* iacc_state, uint64_t current_time)
{
    if (iacc_state->filled_pipe) {
        IACCEnterProbeBW(iacc_state, current_time);
    }
    else {
        IACCEnterStartup(iacc_state);
    }
}
*/

int InLossRecovery(picoquic_iacc_state_t* iacc_state)
{
    return iacc_state->packet_conservation;
}

/*�ݴ�path�ṹ���е�cwnd�����ǳ��ڻָ��������ߴ��ڴ���̽��׶Σ���Ҫ�Ƚϼ�¼��ǰһ��cwndֵ���ȽϺ󱣴������Ǹ�*/
uint64_t IACCSaveCwnd(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x) {
    uint64_t w = path_x->cwin;

    if ((InLossRecovery(iacc_state)) &&
        (path_x->cwin < iacc_state->prior_cwnd)) {
        w = iacc_state->prior_cwnd;
    }

    return w;
}

/*�ָ�path�е�cwin���Ʋ������ָ���ֵΪ֮ǰ�洢��ֵ*/
void IACCRestoreCwnd(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x)
{
    if (path_x->cwin < iacc_state->prior_cwnd) {
        path_x->cwin = iacc_state->prior_cwnd;
    }
}

/*
void IACCHandleProbeRTT(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t current_time)
{
#if 0
    // Ignore low rate samples during ProbeRTT: 
    C.app_limited =
        (BW.delivered + bytes_in_transit) ? 0 : 1;
#endif
    //ע�⣬���´����ǰ���ǵ�ǰ�Ѵ���rtt̽��׶Σ�cwnd����û�б���Ϊ4��mtu���մ������׶�ת�룩�����Ѿ�����Ϊ4��mtu
    if (iacc_state->probe_rtt_done_stamp == 0 &&
        bytes_in_transit <= IACC_MIN_PIPE_CWND(path_x->send_mtu)) {
        //��̽����δ��ʼ��ͬʱ��⵽��;�ֽ����Ѿ�С��4��mtu�ˣ����ʾ�Ѿ�����Ӧ�����޽׶Σ����Կ�ʼ����rtprop��
        iacc_state->probe_rtt_done_stamp =
            current_time + IACC_PROBE_RTT_DURATION;//��¼ʱ���Ϊ��ǰʱ�����200ms��200msΪ̽���ά��ʱ��
        iacc_state->probe_rtt_round_done = 0;//���±�־Ϊ���������ڻ�û������������0
        iacc_state->next_round_delivered = path_x->delivered;
    }
    else if (iacc_state->probe_rtt_done_stamp != 0) {
        if (iacc_state->round_start) {
            iacc_state->probe_rtt_round_done = 1;
        }

        if (iacc_state->probe_rtt_round_done &&
            current_time > iacc_state->probe_rtt_done_stamp) {
            iacc_state->rt_prop_stamp = current_time;
            IACCRestoreCwnd(iacc_state, path_x);
            IACCExitProbeRTT(iacc_state, current_time);
        }
    }
}
*/

/*����Ƿ���Ҫ����rtt̽��׶Σ����ߴ�rtt̽��׶��˳�������̽������������׶�*/
/*
void IACCCheckProbeRTT(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t current_time)
{
    if (iacc_state->state != picoquic_iacc_alg_probe_rtt &&
        iacc_state->rt_prop_expired &&
        !iacc_state->idle_restart) {
        //��һ�����ǵ�rtprop��ʱ��δ����ʱ��ǿ�ƽ���rtt̽��׶Σ��ý׶�˫gainֵ��������Ϊ1��
        //���Ǿ�����ص�pacing_rate��cwnd��û�з����仯����һ����ֻ�Ƿ���gainֵ��״̬�ĸı�
        IACCEnterProbeRTT(iacc_state);//����rtt̽��׶�
        iacc_state->prior_cwnd = IACCSaveCwnd(iacc_state, path_x);//����cwnd��ֵ
        iacc_state->probe_rtt_done_stamp = 0;
        //��һ������ʾ����rtt̽�������ʱ�������Ϊ0��ʾ̽����δ��ʼ����Ϊrtprop�Ĳ�������Ҫ��Ӧ�����޽׶εģ�������cwnd��Ϊ4��mtu������Ҫ�ȴ�һ��ʱ��
    }

    if (iacc_state->state == picoquic_iacc_alg_probe_rtt) {
        //�����ǰ�Ѿ�����RTT̽��׶�
        IACCHandleProbeRTT(iacc_state, path_x, bytes_in_transit, current_time);
        iacc_state->idle_restart = 0;
    }
}
*/

/*����iacc״̬��Ϣ��ע������ÿ���յ�ack��ִ�еģ�������Щ������ִ����һ�����ڵı仯*/
void IACCUpdateModelAndState(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x,
    uint64_t rtt_sample, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t current_time)
{
    IACCUpdateBtlBw(iacc_state, path_x);//����̽���ƿ������
    //IACCCheckCyclePhase(iacc_state, packets_lost, current_time);
    IACCCheckFullPipe(iacc_state, path_x->last_bw_estimate_path_limited);
    IACCCheckDrain(iacc_state, bytes_in_transit, current_time);
    IACCUpdateRTprop(iacc_state, rtt_sample, current_time, path_x);
    //IACCCheckProbeRTT(iacc_state, path_x, bytes_in_transit, current_time);
}

/*����pacingrate��ע��������µ�ֻ��iacc�е�pacingrate����û���漰path����*/
void IACCSetPacingRateWithGain(picoquic_iacc_state_t* iacc_state, double pacing_gain)
{
    double rate = pacing_gain * (double)iacc_state->btl_bw;

    if (iacc_state->filled_pipe || rate > iacc_state->pacing_rate) {
        iacc_state->pacing_rate = rate;
    }
}

void IACCSetPacingRate(picoquic_iacc_state_t* iacc_state)
{
    IACCSetPacingRateWithGain(iacc_state, iacc_state->pacing_gain);
}

/* TODO: clarity on bytes vs packets  */
void IACCModulateCwndForRecovery(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x,
    uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered)
{
    if (packets_lost > 0) {
        if (path_x->cwin > packets_lost) {
            path_x->cwin -= packets_lost;
        }
        else {
            path_x->cwin = path_x->send_mtu;
        }
    }
    if (iacc_state->packet_conservation) {
        if (path_x->cwin < bytes_in_transit + bytes_delivered) {
            path_x->cwin = bytes_in_transit + bytes_delivered;
        }
    }
}

/*����ǰ����RTT̽��׶���cwnd*/
void IACCModulateCwndForProbeRTT(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x)
{
    //if (iacc_state->state == picoquic_iacc_alg_probe_rtt)
    if (iacc_state->state == picoquic_iacc_alg_probe_control)
    {
        if (path_x->cwin > IACC_MIN_PIPE_CWND(path_x->send_mtu)) {
            path_x->cwin = IACC_MIN_PIPE_CWND(path_x->send_mtu);
        }
    }
}

void IACCSetCwnd(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered)
{
    IACCUpdateTargetCwnd(iacc_state);
    IACCModulateCwndForRecovery(iacc_state, path_x, bytes_in_transit, packets_lost, bytes_delivered);
    if (!iacc_state->packet_conservation) {
        if (iacc_state->filled_pipe) {
            path_x->cwin += bytes_delivered;
            if (path_x->cwin > iacc_state->target_cwnd) {
                path_x->cwin = iacc_state->target_cwnd;
            }
        }
        else if (path_x->cwin < iacc_state->target_cwnd || path_x->delivered < PICOQUIC_CWIN_INITIAL)
        {
            path_x->cwin += bytes_delivered;
            if (path_x->cwin < IACC_MIN_PIPE_CWND(path_x->send_mtu))
            {
                path_x->cwin = IACC_MIN_PIPE_CWND(path_x->send_mtu);
            }
        }
    }

    //IACCModulateCwndForProbeRTT(iacc_state, path_x);//�ж��Ƿ����probertt�׶Σ��Ӷ�
}


void IACCUpdateControlParameters(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered)
{
    IACCSetPacingRate(iacc_state);
    IACCSetSendQuantum(iacc_state, path_x);
    IACCSetCwnd(iacc_state, path_x, bytes_in_transit, packets_lost, bytes_delivered);
}

void IACCHandleRestartFromIdle(picoquic_iacc_state_t* iacc_state, uint64_t bytes_in_transit, int is_app_limited)
{
    if (bytes_in_transit == 0 && is_app_limited)
    {
        iacc_state->idle_restart = 1;
        //if (iacc_state->state == picoquic_iacc_alg_probe_bw) {
        if (iacc_state->state == picoquic_iacc_alg_probe_steady) {
            IACCSetPacingRateWithGain(iacc_state, 1.0);
        }
    }
}


/* This is the per ACK processing, activated upon receiving an ACK.
 * At that point, we expect the following:
 *  - delivered has been updated to reflect all the data acked on the path.
 *  - the delivery rate sample has been computed.
 */

void  IACCUpdateOnACK(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x,
    uint64_t rtt_sample, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered,
    uint64_t current_time)
{
    DBG_PRINTF("|IACC-OnAck|");
    IACCUpdateModelAndState(iacc_state, path_x, rtt_sample, bytes_in_transit,
        packets_lost, current_time);
    IACCUpdateControlParameters(iacc_state, path_x, bytes_in_transit, packets_lost, bytes_delivered);
}

void IACCOnTransmit(picoquic_iacc_state_t* iacc_state, uint64_t bytes_in_transit, int is_app_limited)
{
    IACCHandleRestartFromIdle(iacc_state, bytes_in_transit, is_app_limited);
}

/* Dealing with recovery. What happens when all
 * the packets are lost, when all packets have been retransmitted.. */

void IACCOnAllPacketsLost(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x)
{
    iacc_state->prior_cwnd = IACCSaveCwnd(iacc_state, path_x);
    path_x->cwin = path_x->send_mtu;
}

void IACCOnEnterFastRecovery(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t bytes_delivered)
{
    if (bytes_delivered < path_x->send_mtu) {
        bytes_delivered = path_x->send_mtu;
    }
    iacc_state->prior_cwnd = IACCSaveCwnd(iacc_state, path_x);
    path_x->cwin = bytes_in_transit + bytes_delivered;
    iacc_state->packet_conservation = 1;
}

void IACCAfterOneRoundtripInFastRecovery(picoquic_iacc_state_t* iacc_state)
{
    iacc_state->packet_conservation = 0;
}

void IACCExitFastRecovery(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x)
{
    iacc_state->packet_conservation = 0;
    IACCRestoreCwnd(iacc_state, path_x);
}

/*
 * In order to implement IACC, we map generic congestion notification
 * signals to the corresponding IACC actions.
 */
static void picoquic_iacc_notify(
    picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    uint64_t rtt_measurement,
    uint64_t nb_bytes_acknowledged,
    uint64_t lost_packet_number,
    uint64_t current_time)
{
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(lost_packet_number);
#endif
    /*cc�㷨�ĳ�ʼ���������ڴ���path���ߴ���connectionʱ���еģ���������Ǵ�path��ȡ������㷨�Ľṹ��*/
    picoquic_iacc_state_t* iacc_state = (picoquic_iacc_state_t*)path_x->congestion_alg_state;

    if (iacc_state != NULL) {
        /*
        if (iacc_state->state == picoquic_iacc_alg_probe_bw) {//�����ǰ���ڴ���̽��׶Σ����ӡ�����Ϣ
            printf("IACC BW probing\n");
        }
        */
        switch (notification) {//���ݴ����notification������Ҫ��ɵĲ���
        case picoquic_congestion_notification_acknowledgement://�����acknowledgement�źţ�����ݴ����ack��������iacc���ѷ���������
            /* sum the amount of data acked per packet */
            iacc_state->bytes_delivered += nb_bytes_acknowledged;
            break;
        case picoquic_congestion_notification_repeat://�����repeat�źţ�������ڴ���̽��׶δ�ӡ��Ϣ�ⲻ���κδ���
            break;
        case picoquic_congestion_notification_timeout://�����timeout�źţ�������ڴ���̽��׶δ�ӡ��Ϣ�ⲻ���κδ���
            /* enter recovery */
            break;
        case picoquic_congestion_notification_spurious_repeat://�����spurious_repeat�źţ�������ڴ���̽��׶δ�ӡ��Ϣ�ⲻ���κδ���
            break;
        case picoquic_congestion_notification_rtt_measurement://�����RTT�����ź�
            if (iacc_state->state == picoquic_iacc_alg_startup && path_x->smoothed_rtt > PICOQUIC_TARGET_RENO_RTT) {
                IACCEnterStartupLongRTT(iacc_state, path_x);//��������������������׶�ʱ
            }
            if (iacc_state->state == picoquic_iacc_alg_startup_long_rtt) {
                if (picoquic_hystart_test(&iacc_state->rtt_filter, rtt_measurement, path_x->pacing_packet_time_microsec, current_time, false)) {
                    IACCExitStartupLongRtt(iacc_state, path_x, current_time);
                }
            }
            break;
        case picoquic_congestion_notification_bw_measurement://�����Bw�����ź�
            /* RTT measurements will happen after the bandwidth is estimated */
            if (iacc_state->state == picoquic_iacc_alg_startup_long_rtt) {
                IACCUpdateBtlBw(iacc_state, path_x);
                if (rtt_measurement <= iacc_state->rt_prop) {
                    iacc_state->rt_prop = rtt_measurement;
                    iacc_state->rt_prop_stamp = current_time;
                }
                if (picoquic_cc_was_cwin_blocked(path_x, iacc_state->last_sequence_blocked)) {
                    picoquic_hystart_increase(path_x, &iacc_state->rtt_filter, iacc_state->bytes_delivered);
                }
                iacc_state->bytes_delivered = 0;

                picoquic_update_pacing_data(path_x);
            }
            else {
                IACCUpdateOnACK(iacc_state, path_x,
                    rtt_measurement, path_x->bytes_in_transit, 0 /* packets_lost */, iacc_state->bytes_delivered,
                    current_time);
                /* Remember the number in flight before the next ACK -- TODO: update after send instead. */
                iacc_state->prior_in_flight = path_x->bytes_in_transit;
                /* Reset the number of bytes delivered */
                iacc_state->bytes_delivered = 0;

                if (iacc_state->pacing_rate > 0) {
                    /* Set the pacing rate in picoquic sender */
                    picoquic_update_pacing_rate(path_x, iacc_state->pacing_rate, iacc_state->send_quantum);
                }
            }
            break;
        case picoquic_congestion_notification_cwin_blocked:
            iacc_state->last_sequence_blocked = picoquic_cc_get_sequence_number(path_x);
        default:
            /* ignore */
            break;
        }
    }
}

#define picoquic_iacc_ID 0x49414343 /* IACC */

picoquic_congestion_algorithm_t picoquic_iacc_algorithm_struct = {
        picoquic_iacc_ID,
        picoquic_iacc_init,
        picoquic_iacc_notify,
        picoquic_iacc_delete
};

/*��һ����ָ��cc�㷨���������������ӿڣ����ϲ�ѡ�������*/
picoquic_congestion_algorithm_t* picoquic_iacc_algorithm = &picoquic_iacc_algorithm_struct;