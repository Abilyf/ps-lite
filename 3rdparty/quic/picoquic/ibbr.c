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
Implementation of the IBBR algorithm, tuned for Picoquic.

The main idea of IBBR is to track the "bottleneck bandwidth", and to tune the
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

IBBR does that by following a cycle of "send, test and drain". During the
sending period, the stack sends at the measured rate. During the testing
period, it sends faster, 25% faster with recommended parameters. This
risk creating a queue if the bandwidth had not increased, so the test
period is followed by a drain period during which the stack sends 25%
slower than the measured rate. If the test is successful, the new bandwidth
measurement will be available at the end of the draining period, and
the increased bandwidth will be used in the next cycle.

Tuning the sending rate does not guarantee a short queue, it only
guarantees a stable queue. IBBR controls the queue by limiting the
amount of data "in flight" (congestion window, CWIN) to the product
of the bandwidth estimate by the RTT estimate, plus a safety marging to ensure
continuous transmission. Using the average RTT there would lead to a runaway
loop in which oversized windows lead to increased queues and then increased
average RTT. Instead of average RTT, IBBR uses a minimum RTT. Since the
mimimum RTT might vary with routing changes, the minimum RTT is measured
on a sliding window of 10 seconds.

The bandwidth estimation needs to be robust against short term variations
common in wireless networks. IBBR retains the maximum
delivery rate observed over a series of probing intervals. Each interval
starts with a specific packet transmission and ends when that packet
or a later transmission is acknowledged. IBBR does that by tracking
the delivered counter associated with packets and comparing it to
the delivered counter at start of period.

During start-up, IBBR performs its own equivalent of Reno's slow-start.
It does that by using a pacing gain of 2.89, i.e. sending 2.89 times
faster than the measured maximum. It exits slow start when it found
a bandwidth sufficient to fill the pipe.

The bandwidth measurements can be wrong if the application is not sending
enough data to fill the pipe. IBBR tracks that, and does not reduce bandwidth
or exit slow start if the application is limiting transmission.

This implementation follows draft-cardwell-iccrg-ibbr-congestion-control,
with a couple of changes for handling the multipath nature of quic.
There is a IBBR control state per path.
Most of IBBR the variables defined in the draft are implemented
in the "IBBR state" structure, with a few exceptions:

* IBBR.delivered is represented by path_x.delivered, and is maintained
  as part of ACK processing

* Instead of "bytes_in_transit", we use "bytes_in_transit", which is
  already maintained by the stack.

* Compute bytes_delivered by summing all calls to ACK(bytes) before
  the call to RTT update.

* In the Probe BW mode, the draft suggests cwnd_gain = 2. We observed
  that this results in queue sizes of 2, which is too high, so we
  reset that to 1.125.

The "packet" variables are defined in the picoquic_packet_t.

Early testing showed that IBBR startup phase requires several more RTT
than the Hystart process used in modern versions of Reno or Cubic. IBBR
only ramps up the data rate after the first bandwidth measurement is
available, 2*RTT after start, while Reno or Cubic start ramping up
after just 1 RTT. IBBR only exits startup if three consecutive RTT
pass without significant BW measurement increase, which not only
adds delay but also creates big queues as data is sent at 2.89 times
the bottleneck rate. This is a tradeoff: longer search for bandwidth in
slow start is less likely to stop too early because of transient
issues, but one high bandwidth and long delay links this translates
to long delays and a big batch of packet losses.

This IBBR implementation addresses these issues by switching to
Hystart instead of startup if the RTT is above the Reno target of
100 ms.

*/

typedef enum {
    picoquic_ibbr_alg_startup = 0,
    picoquic_ibbr_alg_drain,
    picoquic_ibbr_alg_probe_oversent,
    picoquic_ibbr_alg_probe_control,
    picoquic_ibbr_alg_probe_steady,
    picoquic_ibbr_alg_probe_rtt,
    picoquic_ibbr_alg_startup_long_rtt
} picoquic_ibbr_alg_state_t;

#define IBBR_BTL_BW_FILTER_LENGTH 10
#define IBBR_RT_PROP_FILTER_LENGTH 10
#define IBBR_HIGH_GAIN 2.8853900817779 /* 2/ln(2) */
#define IBBR_MIN_PIPE_CWND(mss) (4*mss)
#define IBBR_GAIN_CYCLE_LEN 8
#define IBBR_PROBE_RTT_INTERVAL 10000000 /* 10 sec, 10000000 microsecs */
#define IBBR_PROBE_RTT_PROP_INTERVAL 2500000 /* 10 sec, 10000000 microsecs */
#define IBBR_PROBE_RTT_DURATION 200000 /* 200msec, 200000 microsecs */
#define IBBR_PACING_RATE_LOW 150000.0 /* 150000 B/s = 1.2 Mbps */
#define IBBR_PACING_RATE_MEDIUM 3000000.0 /* 3000000 B/s = 24 Mbps */
#define IBBR_GAIN_CYCLE_LEN 8
#define IBBR_GAIN_CYCLE_MAX_START 5

#define ibbr_max(a,b) ((a) > (b) ? (a) : (b))
#define ibbr_min(a,b) ((a) < (b) ? (a) : (b))

static const double ibbr_pacing_gain_cycle[IBBR_GAIN_CYCLE_LEN] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.25, 0.75 };

typedef struct st_picoquic_ibbr_state_t {
    picoquic_ibbr_alg_state_t state;
    uint64_t btl_bw;
    uint64_t next_round_delivered;
    uint64_t round_start_time;
    uint64_t btl_bw_filter[IBBR_BTL_BW_FILTER_LENGTH];
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

    /*ibbr optimize parameters*/
    uint32_t ibbr_optimization;
    uint32_t oversent_percent;
    uint32_t pacing_gain_percent;
    uint32_t smooth_ibbr_percent;
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
} picoquic_ibbr_state_t;

void IBBREnterStartupLongRTT(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x)
{
    uint64_t cwnd = PICOQUIC_CWIN_INITIAL;
    ibbr_state->state = picoquic_ibbr_alg_startup_long_rtt;

    if (path_x->smoothed_rtt > PICOQUIC_TARGET_RENO_RTT) {
        cwnd = (uint64_t)((double)cwnd * (double)path_x->smoothed_rtt / (double)PICOQUIC_TARGET_RENO_RTT);
    }
    if (cwnd > path_x->cwin) {
        path_x->cwin = cwnd;
    }
}

/*�����������׶Σ�pacing_gain��cwnd_gain������Ϊ2.885*/
void IBBREnterStartup(picoquic_ibbr_state_t* ibbr_state)
{
    ibbr_state->state = picoquic_ibbr_alg_startup;
    ibbr_state->pacing_gain = IBBR_HIGH_GAIN;
    ibbr_state->cwnd_gain = IBBR_HIGH_GAIN;
}

/*���ݵ�ǰibbr��pacing_rate�Լ���ǰ·��patg_x�����õķ������Ԫsend_mtu������ibbr�е�send_quantumֵ�����ֵ�������������ѧϰ*/
void IBBRSetSendQuantum(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x)
{
    if (ibbr_state->pacing_rate < IBBR_PACING_RATE_LOW) {
        ibbr_state->send_quantum = 1ull * path_x->send_mtu;
    }
    else if (ibbr_state->pacing_rate < IBBR_PACING_RATE_MEDIUM) {
        ibbr_state->send_quantum = 2ull * path_x->send_mtu;
    }
    else {
        ibbr_state->send_quantum = (uint64_t)(ibbr_state->pacing_rate * 0.001);
        if (ibbr_state->send_quantum > 64000) {
            ibbr_state->send_quantum = 64000;
        }
    }
}

/*��������·bdp*/
double IBBRBDP(picoquic_ibbr_state_t* ibbr_state) {
    return (((double)ibbr_state->btl_bw * (double)ibbr_state->rt_prop) / 1000000.0);
}

/*���㵱ǰ��·bdp�����㴫��gainֵ�µ�cwndֵ�������cwnd������һ��������sendQuantum��������*/
uint64_t IBBRInflight(picoquic_ibbr_state_t* ibbr_state, double gain)
{
    uint64_t cwnd = PICOQUIC_CWIN_INITIAL;
    if (ibbr_state->rt_prop != UINT64_MAX) {
        /* Bandwidth is estimated in bytes per second, rtt in microseconds*/
        double estimated_bdp = (((double)ibbr_state->btl_bw * (double)ibbr_state->rt_prop) / 1000000.0);//����bdp
        uint64_t quanta = 3 * ibbr_state->send_quantum;
        cwnd = (uint64_t)(gain * estimated_bdp) + quanta;
    }
    return cwnd;
}

/*���µ�ǰ��target_cwndֵΪIBBRInflight���ص�ֵ�������gainֵΪ��ǰibbr_state�е�cwnd_gain*/
void IBBRUpdateTargetCwnd(picoquic_ibbr_state_t* ibbr_state)
{
    ibbr_state->target_cwnd = IBBRInflight(ibbr_state, ibbr_state->cwnd_gain);
}

/*��ʼ��ibbr�ṹ�壬�����丳ֵ�������path_x����congestion_alg_state��*/
static void picoquic_ibbr_init(picoquic_cnx_t* cnx, picoquic_path_t* path_x)
{
    /* Initialize the state of the congestion control algorithm */
    picoquic_ibbr_state_t* ibbr_state = (picoquic_ibbr_state_t*)malloc(sizeof(picoquic_ibbr_state_t));
    path_x->congestion_alg_state = (void*)ibbr_state;
    if (ibbr_state != NULL) {
        memset(ibbr_state, 0, sizeof(picoquic_ibbr_state_t));
        path_x->cwin = PICOQUIC_CWIN_INITIAL;
        ibbr_state->rt_prop = UINT64_MAX;
        uint64_t current_time = picoquic_current_time();
        ibbr_state->rt_prop_stamp = current_time;//��ʼ��rtpropʱ���Ϊ��ǰʱ�䣨���ڼ���ʱ�䣩
        ibbr_state->cycle_stamp = current_time;//��ʼ��cycleʱ���Ϊ��ǰʱ�䣬���ڴ���̽��׶ε�cycle����

        /*for graph*/
        ibbr_state->cur_inflight = 0;
        ibbr_state->current_bandwidth = 0;
        ibbr_state->bytes_sent_in_round_start = 0;
        ibbr_state->bytes_delivered_in_round_start = 0;

        /*for oversent*/
        ibbr_state->oversent_percent = 150;
        ibbr_state->pacing_gain_percent = IBBR_HIGH_GAIN * 100;
        ibbr_state->smooth_ibbr_percent = 100;
        ibbr_state->steady_thresh = 50;
        ibbr_state->full_bw_reach_max = 3;

        IBBREnterStartup(ibbr_state);//�����������׶�
        IBBRSetSendQuantum(ibbr_state, path_x);//����send_quantumֵ����ʼ���׶�pacing_rateΪ0���ʴ�ʱquantumֵΪһ��sendmtu
        IBBRUpdateTargetCwnd(ibbr_state);//��ʼ��targetcwnd������btlbw��ʼֵΪ0���ʼ����bdpֵΪ0����˳�ʼcwndΪ���ϵ�send_quantum��Ҳ����һ��mtu
    }
}

/* Release the state of the congestion control algorithm */
static void picoquic_ibbr_delete(picoquic_cnx_t* cnx, picoquic_path_t* path_x)
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

void IBBRUpdateBtlBw(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x)
{
    ibbr_state->round_start = 0;

    uint64_t bandwidth_estimate = path_x->bandwidth_estimate;
    if (ibbr_state->last_sequence_blocked == 0 || !picoquic_cc_was_cwin_blocked(path_x, ibbr_state->last_sequence_blocked)) {
        // the estimation is not reliable because the CWIN was not probed entirely
        return;
    }
    /*������Ի�һ���������ͣ����ϱ������ָÿ�����ʾibbr��һ�ֿ�ʼǰ�Ѿ����͵���������Ҳ����ibbr_state->next_round_delivered
    ��path_x->delivered_last_packet���ʾ��һ�����ݰ�����֮������·�����Ѿ����͵����������������ͺܺý����ˣ������ֵ�������ϵ�ĳһ����
    ��ͱ�ʾ����һ��֮ǰ�����ݶ��������ˣ����������ˣ���˿��Կ�ʼ��һ�ֵ����ݷ��ͣ�����round_start������Ϊ0����Ҳ��round����������
    */
    //DBG_PRINTF("|UPDATE BW|");
    if (path_x->delivered_last_packet >= ibbr_state->next_round_delivered)
    {
        //DBG_PRINTF("|ROUND START|");
        ibbr_state->next_round_delivered = path_x->delivered;//������һ�ֿ�ʼ����Ҫ�����ݷ�������
        ibbr_state->round_count++;
        ibbr_state->round_start = 1;
    }

    if (ibbr_state->round_start) {
        /* Forget the oldest BW round, shift by 1, compute the max BTL_BW for
         * the remaining rounds, set current round max to current value */

        ibbr_state->btl_bw = 0;

        /*��һ������׷�����ƿ������*/
        for (int i = IBBR_BTL_BW_FILTER_LENGTH - 2; i >= 0; i--) {
            uint64_t b = ibbr_state->btl_bw_filter[i];
            ibbr_state->btl_bw_filter[i + 1] = b;
            if (b > ibbr_state->btl_bw) {
                ibbr_state->btl_bw = b;
            }
        }

        ibbr_state->btl_bw_filter[0] = 0;
    }

    if (bandwidth_estimate > ibbr_state->btl_bw_filter[0]) {
        ibbr_state->btl_bw_filter[0] = bandwidth_estimate;
        if (bandwidth_estimate > ibbr_state->btl_bw) {
            ibbr_state->btl_bw = bandwidth_estimate;
        }
    }
}

/* This will use one way samples if available */
/* Should augment that with common RTT filter to suppress jitter */
/*�ж��Ƿ���Ҫ���¼�¼��RTprop*/
void IBBRUpdateRTprop(picoquic_ibbr_state_t* ibbr_state, uint64_t rtt_sample, uint64_t current_time, picoquic_path_t* path_x)
{
    ibbr_state->probe_rt_prop_expired = current_time > (ibbr_state->probe_rt_prop_stamp + IBBR_PROBE_RTT_PROP_INTERVAL);
    if (rtt_sample <= ibbr_state->probe_rt_prop || ibbr_state->probe_rt_prop_expired) {
        ibbr_state->probe_rt_prop = rtt_sample;
        ibbr_state->probe_rt_prop_stamp = current_time;
    }

    //�����ʾ�����ǰʱ������ϴ�rtprop��¼ʱ���ѳ���10�룬���ʾ�ϴμ�¼��rtpropʧЧ����Ҫ���²�������¼
    ibbr_state->rt_prop_expired =
        current_time > ibbr_state->rt_prop_stamp + IBBR_PROBE_RTT_INTERVAL;//������ܵ�����Ϊ2.5��
    if (ibbr_state->probe_rt_prop <= ibbr_state->rt_prop || ibbr_state->rt_prop_expired) {
        //���¼�¼rtprop������Ϊ��ѡһ��Ҫô������rttС�����ֵ��Ҫôrtprop��ʱ��δ���¶�ʧЧ
        ibbr_state->rt_prop = ibbr_state->probe_rt_prop;//���������������֮һ�������rtpropΪ��ǰrtt����ֵ�������п��ܳ��ֱ������
        ibbr_state->rt_prop_stamp = ibbr_state->probe_rt_prop_stamp;//ͬʱ���¼�¼��ʱ���
    }

    if (ibbr_state->state == picoquic_ibbr_alg_probe_oversent && !ibbr_state->idle_restart) {
        //DBG_PRINTF("in round2");
        double bdp = IBBRBDP(ibbr_state);
        if (ibbr_state->round_start) {
            double delta_bw;
            uint64_t delta_sent = path_x->bytes_send - ibbr_state->bytes_sent_in_round_start;
            uint64_t delta_acked = ibbr_state->bytes_delivered - ibbr_state->bytes_delivered_in_round_start;

            if (bdp == 0) {
                ibbr_state->cwnd_gain = 0.2;
            }
            else
            {
                //delta_bw = ((double)delta_sent - (double)delta_acked) / bdp;
                delta_bw = ((double)delta_sent - (double)delta_acked) / ((double)ibbr_state->bytes_sent_in_round_start - (double)ibbr_state->bytes_delivered_in_round_start);
                double tmp = ibbr_state->cwnd_gain - delta_bw;
                DBG_PRINTF("|IBBR-Delta_bw:%.3f|cwndgain_gap:%.3f|", delta_bw, tmp);
                if (delta_bw > 0) {
                    ibbr_state->cwnd_gain = ibbr_max(tmp, 0.2) * ibbr_state->smooth_ibbr_percent / 100.0;
                }
                else if (delta_bw <= 0) {
                    ibbr_state->cwnd_gain = tmp * ibbr_state->smooth_ibbr_percent / 100.0;
                }
            }

            IBBREnterProbeControl(ibbr_state);
            ibbr_state->inflight_current = 0;
            ibbr_state->round_start = 0;
        }
    }

    if (ibbr_state->state == picoquic_ibbr_alg_probe_control && !ibbr_state->idle_restart) {
        if (ibbr_state->round_start) {
            IBBREnterProbeSteady(ibbr_state);
        }
    }

    if (ibbr_state->state == picoquic_ibbr_alg_probe_steady && !ibbr_state->idle_restart) {
        if (ibbr_state->round_start) {
            ibbr_state->inflight_current = path_x->bytes_in_transit;
            IBBREnterProbeOversent(ibbr_state, path_x);
            ibbr_state->round_start = 0;
        }
    }

    if (ibbr_state->bytes_delivered > 0) {
        ibbr_state->idle_restart = 0;
    }
}

/*��һ�����ж��Ƿ���Խ���cycle�е���һ���׶�*/
int IBBRIsNextCyclePhase(picoquic_ibbr_state_t* ibbr_state, uint64_t prior_in_flight, uint64_t packets_lost, uint64_t current_time)
{
    int is_full_length = (current_time - ibbr_state->cycle_stamp) > ibbr_state->rt_prop;
    //��������ֵ��ʾ��ǰʱ�����һ�μ�¼��cycleʱ����ļ���Ƿ����һ��rtprop�����ǵ�һ������

    if (ibbr_state->pacing_gain != 1.0) {
        if (ibbr_state->pacing_gain > 1.0) {
            is_full_length &=
                (packets_lost > 0 ||
                    prior_in_flight >= IBBRInflight(ibbr_state, ibbr_state->pacing_gain));
        }
        else {  /*  (IBBR.pacing_gain < 1) */
            is_full_length |= prior_in_flight <= IBBRInflight(ibbr_state, 1.0);
        }
    }
    return is_full_length;
}

/*����cycle����pacinggain*/
void IBBRAdvanceCyclePhase(picoquic_ibbr_state_t* ibbr_state, uint64_t current_time)
{
    ibbr_state->cycle_stamp = current_time;//���µ�ǰcycleʱ���
    ibbr_state->cycle_index++;//����index
    if (ibbr_state->cycle_index >= IBBR_GAIN_CYCLE_LEN) {
        int start = (int)(ibbr_state->rt_prop / PICOQUIC_TARGET_RENO_RTT);
        if (start > IBBR_GAIN_CYCLE_MAX_START) {
            start = IBBR_GAIN_CYCLE_MAX_START;
        }
        ibbr_state->cycle_index = start;
    }

    ibbr_state->pacing_gain = ibbr_pacing_gain_cycle[ibbr_state->cycle_index];//�����趨��ֵ����pacing_gain
}

/*����Ƿ���Ҫͨ������̽��׶ε�cycle������pacing_gain*/
/*
void IBBRCheckCyclePhase(picoquic_ibbr_state_t* ibbr_state, uint64_t packets_lost, uint64_t current_time)
{
    if (ibbr_state->state == picoquic_ibbr_alg_probe_bw &&
        IBBRIsNextCyclePhase(ibbr_state, ibbr_state->prior_in_flight, packets_lost, current_time)) {
        IBBRAdvanceCyclePhase(ibbr_state, current_time);//�������������������1�����ڴ���̽��׶Σ�2�����Խ�����һ���������ڣ������pacing_gain
    }
}
*/

/*����Ƿ�ﵽƿ������û������²����¼�������������������������������ʾ�ѵ���*/
void IBBRCheckFullPipe(picoquic_ibbr_state_t* ibbr_state, int rs_is_app_limited)
{
    if (!ibbr_state->filled_pipe && ibbr_state->round_start && !rs_is_app_limited) {
        if (ibbr_state->btl_bw >= ibbr_state->full_bw * 1.25) {  // IBBR.BtlBw still growing?
            ibbr_state->full_bw = ibbr_state->btl_bw;   // record new baseline level
            ibbr_state->full_bw_count = 0;
        }
        else {
            ibbr_state->full_bw_count++; // another round w/o much growth
            if (ibbr_state->full_bw_count >= 3) {
                ibbr_state->filled_pipe = 1;
            }
        }
    }
}

/*�������̽��׶Σ���ʱpacing_gain����Ϊ1��cwnd_gain����Ϊ1.5*/
/*void IBBREnterProbeBW(picoquic_ibbr_state_t* ibbr_state, uint64_t current_time)
{
    ibbr_state->state = picoquic_ibbr_alg_probe_bw;
    ibbr_state->pacing_gain = 1.0;
    ibbr_state->cwnd_gain = 1.5;
    ibbr_state->cycle_index = 4;  // TODO: random_int_in_range(0, 5); 
    IBBRAdvanceCyclePhase(ibbr_state, current_time);
}
*/

/*�����ſս׶Σ�pacing_gain����Ϊ1/2.885��cwnd_gain����Ϊ2.885*/
void IBBREnterDrain(picoquic_ibbr_state_t* ibbr_state)
{
    ibbr_state->state = picoquic_ibbr_alg_drain;
    ibbr_state->pacing_gain = 1.0 / IBBR_HIGH_GAIN;  /* pace slowly */
    ibbr_state->cwnd_gain = IBBR_HIGH_GAIN;   /* maintain cwnd */
    ibbr_state->enter_from_drain = 1;
}

/*����Ƿ���Ҫ�������������ſգ�����ſս������̽��׶�*/
void IBBRCheckDrain(picoquic_ibbr_state_t* ibbr_state, uint64_t bytes_in_transit, uint64_t current_time, picoquic_path_t* path_x)
{
    if (ibbr_state->state == picoquic_ibbr_alg_startup && ibbr_state->filled_pipe) {
        IBBREnterDrain(ibbr_state);//��������������׶����Ѵﵽƿ������������ſս׶�
    }

    if (ibbr_state->state == picoquic_ibbr_alg_drain && bytes_in_transit <= IBBRInflight(ibbr_state, 1.0)) {
        //��������ſս׶Σ�����;�ֽ���С�ڼ���Ŀ��������������ʾ�ſ���ϣ��ɽ������̽��׶�
        //IBBREnterProbeBW(ibbr_state, current_time);  /* we estimate queue is drained */
        IBBREnterProbeOversent(ibbr_state, path_x);
        ibbr_state->round_start = 0;
    }
}

/*����Oversent�׶�*/
void IBBREnterProbeOversent(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x) {
    ibbr_state->state = picoquic_ibbr_alg_probe_oversent;
    ibbr_state->pacing_gain = ibbr_state->pacing_gain_percent / 100.0;
    ibbr_state->cwnd_gain = ibbr_state->oversent_percent / 100.0;
    ibbr_state->over_sent = 0;
    ibbr_state->bytes_sent_in_round_start = path_x->bytes_send;
    ibbr_state->bytes_delivered_in_round_start = ibbr_state->bytes_delivered;
}

/*����Control�׶�*/
void IBBREnterProbeControl(picoquic_ibbr_state_t* ibbr_state) {
    ibbr_state->state = picoquic_ibbr_alg_probe_control;
    ibbr_state->pacing_gain = ibbr_state->pacing_gain_percent / 100.0;
}

/*����Steady�׶�*/
void IBBREnterProbeSteady(picoquic_ibbr_state_t* ibbr_state) {
    ibbr_state->state = picoquic_ibbr_alg_probe_steady;
    ibbr_state->cwnd_gain = 1;
    ibbr_state->pacing_gain = ibbr_state->pacing_gain_percent / 100.0;
}

void IBBRExitStartupLongRtt(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    /* Reset the round filter so it will start at current time */
    ibbr_state->next_round_delivered = path_x->delivered;
    ibbr_state->round_count++;
    ibbr_state->round_start = 1;
    /* Set the filled pipe indicator */
    ibbr_state->full_bw = ibbr_state->btl_bw;
    ibbr_state->full_bw_count = 3;
    ibbr_state->filled_pipe = 1;
    /* Enter drain */
    IBBREnterDrain(ibbr_state);
    /* If there were just few bytes in transit, enter probe */
    if (path_x->bytes_in_transit <= IBBRInflight(ibbr_state, 1.0)) {
        //IBBREnter(ibbr_state, current_time);
        IBBREnterProbeOversent(ibbr_state, path_x);
    }
}


void IBBREnterProbeRTT(picoquic_ibbr_state_t* ibbr_state)
{
    ibbr_state->state = picoquic_ibbr_alg_probe_rtt;
    ibbr_state->pacing_gain = 1.0;
    ibbr_state->cwnd_gain = 1.0;
}


/*�����ʱ̽�⵽�Ĵ�����ƿ��������ô���˳�rtt̽��׶�ֱ�ӵ�����̽��׶Σ���������������׶�*/

void IBBRExitProbeRTT(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x)
{
    if (ibbr_state->filled_pipe) {
        IBBREnterProbeOversent(ibbr_state, path_x);
    }
    else {
        IBBREnterStartup(ibbr_state);
    }
}


int InLossRecovery(picoquic_ibbr_state_t* ibbr_state)
{
    return ibbr_state->packet_conservation;
}

/*�ݴ�path�ṹ���е�cwnd�����ǳ��ڻָ��������ߴ��ڴ���̽��׶Σ���Ҫ�Ƚϼ�¼��ǰһ��cwndֵ���ȽϺ󱣴������Ǹ�*/
uint64_t IBBRSaveCwnd(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x) {
    uint64_t w = path_x->cwin;

    if ((InLossRecovery(ibbr_state)) &&
        (path_x->cwin < ibbr_state->prior_cwnd)) {
        w = ibbr_state->prior_cwnd;
    }

    return w;
}

/*�ָ�path�е�cwin���Ʋ������ָ���ֵΪ֮ǰ�洢��ֵ*/
void IBBRRestoreCwnd(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x)
{
    if (path_x->cwin < ibbr_state->prior_cwnd) {
        path_x->cwin = ibbr_state->prior_cwnd;
    }
}


void IBBRHandleProbeRTT(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t current_time)
{
#if 0
    // Ignore low rate samples during ProbeRTT: 
    C.app_limited =
        (BW.delivered + bytes_in_transit) ? 0 : 1;
#endif
    //ע�⣬���´����ǰ���ǵ�ǰ�Ѵ���rtt̽��׶Σ�cwnd����û�б���Ϊ4��mtu���մ������׶�ת�룩�����Ѿ�����Ϊ4��mtu
    if (ibbr_state->probe_rtt_done_stamp == 0 &&
        bytes_in_transit <= IBBR_MIN_PIPE_CWND(path_x->send_mtu)) {
        //��̽����δ��ʼ��ͬʱ��⵽��;�ֽ����Ѿ�С��4��mtu�ˣ����ʾ�Ѿ�����Ӧ�����޽׶Σ����Կ�ʼ����rtprop��
        ibbr_state->probe_rtt_done_stamp =
            current_time + IBBR_PROBE_RTT_DURATION;//��¼ʱ���Ϊ��ǰʱ�����200ms��200msΪ̽���ά��ʱ��
        ibbr_state->probe_rtt_round_done = 0;//���±�־Ϊ���������ڻ�û������������0
        ibbr_state->next_round_delivered = path_x->delivered;
    }
    else if (ibbr_state->probe_rtt_done_stamp != 0) {
        if (ibbr_state->round_start) {
            ibbr_state->probe_rtt_round_done = 1;
        }

        if (ibbr_state->probe_rtt_round_done &&
            current_time > ibbr_state->probe_rtt_done_stamp) {
            ibbr_state->rt_prop_stamp = current_time;
            IBBRRestoreCwnd(ibbr_state, path_x);
            IBBRExitProbeRTT(ibbr_state, path_x);
        }
    }
}


/*����Ƿ���Ҫ����rtt̽��׶Σ����ߴ�rtt̽��׶��˳�������̽������������׶�*/
void IBBRCheckProbeRTT(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t current_time)
{
    if (ibbr_state->state != picoquic_ibbr_alg_probe_rtt &&
        ibbr_state->rt_prop_expired &&
        !ibbr_state->idle_restart) {
        //��һ�����ǵ�rtprop��ʱ��δ����ʱ��ǿ�ƽ���rtt̽��׶Σ��ý׶�˫gainֵ��������Ϊ1��
        //���Ǿ�����ص�pacing_rate��cwnd��û�з����仯����һ����ֻ�Ƿ���gainֵ��״̬�ĸı�
        IBBREnterProbeRTT(ibbr_state);//����rtt̽��׶�
        ibbr_state->prior_cwnd = IBBRSaveCwnd(ibbr_state, path_x);//����cwnd��ֵ
        ibbr_state->probe_rtt_done_stamp = 0;
        //��һ������ʾ����rtt̽�������ʱ�������Ϊ0��ʾ̽����δ��ʼ����Ϊrtprop�Ĳ�������Ҫ��Ӧ�����޽׶εģ�������cwnd��Ϊ4��mtu������Ҫ�ȴ�һ��ʱ��
    }

    if (ibbr_state->state == picoquic_ibbr_alg_probe_rtt) {
        //�����ǰ�Ѿ�����RTT̽��׶�
        IBBRHandleProbeRTT(ibbr_state, path_x, bytes_in_transit, current_time);
        ibbr_state->idle_restart = 0;
    }
}


/*����ibbr״̬��Ϣ��ע������ÿ���յ�ack��ִ�еģ�������Щ������ִ����һ�����ڵı仯*/
void IBBRUpdateModelAndState(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x,
    uint64_t rtt_sample, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t current_time)
{
    IBBRUpdateBtlBw(ibbr_state, path_x);//����̽���ƿ������
    //IBBRCheckCyclePhase(ibbr_state, packets_lost, current_time);
    IBBRCheckFullPipe(ibbr_state, path_x->last_bw_estimate_path_limited);
    IBBRCheckDrain(ibbr_state, bytes_in_transit, current_time, path_x);
    IBBRUpdateRTprop(ibbr_state, rtt_sample, current_time, path_x);
    IBBRCheckProbeRTT(ibbr_state, path_x, bytes_in_transit, current_time);
}

/*����pacingrate��ע��������µ�ֻ��ibbr�е�pacingrate����û���漰path����*/
void IBBRSetPacingRateWithGain(picoquic_ibbr_state_t* ibbr_state, double pacing_gain)
{
    double rate = pacing_gain * (double)ibbr_state->btl_bw;

    if (ibbr_state->filled_pipe || rate > ibbr_state->pacing_rate) {
        ibbr_state->pacing_rate = rate;
    }
}

void IBBRSetPacingRate(picoquic_ibbr_state_t* ibbr_state)
{
    IBBRSetPacingRateWithGain(ibbr_state, ibbr_state->pacing_gain);
}

/* TODO: clarity on bytes vs packets  */
void IBBRModulateCwndForRecovery(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x,
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
    if (ibbr_state->packet_conservation) {
        if (path_x->cwin < bytes_in_transit + bytes_delivered) {
            path_x->cwin = bytes_in_transit + bytes_delivered;
        }
    }
}

/*����ǰ����RTT̽��׶���cwnd*/
void IBBRModulateCwndForProbeRTT(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x)
{
    if (ibbr_state->state == picoquic_ibbr_alg_probe_rtt)
    //if (ibbr_state->state == picoquic_ibbr_alg_probe_control)
    {
        if (path_x->cwin > IBBR_MIN_PIPE_CWND(path_x->send_mtu)) {
            path_x->cwin = IBBR_MIN_PIPE_CWND(path_x->send_mtu);
        }
    }
}

void IBBRSetCwnd(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered)
{
    IBBRUpdateTargetCwnd(ibbr_state);
    IBBRModulateCwndForRecovery(ibbr_state, path_x, bytes_in_transit, packets_lost, bytes_delivered);
    if (!ibbr_state->packet_conservation) {
        if (ibbr_state->filled_pipe) {
            path_x->cwin += bytes_delivered;
            if (path_x->cwin > ibbr_state->target_cwnd) {
                path_x->cwin = ibbr_state->target_cwnd;
            }
        }
        else if (path_x->cwin < ibbr_state->target_cwnd || path_x->delivered < PICOQUIC_CWIN_INITIAL)
        {
            path_x->cwin += bytes_delivered;
            if (path_x->cwin < IBBR_MIN_PIPE_CWND(path_x->send_mtu))
            {
                path_x->cwin = IBBR_MIN_PIPE_CWND(path_x->send_mtu);
            }
        }
    }

    IBBRModulateCwndForProbeRTT(ibbr_state, path_x);//�ж��Ƿ����probertt�׶Σ��Ӷ�
}


void IBBRUpdateControlParameters(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered)
{
    IBBRSetPacingRate(ibbr_state);
    IBBRSetSendQuantum(ibbr_state, path_x);
    IBBRSetCwnd(ibbr_state, path_x, bytes_in_transit, packets_lost, bytes_delivered);
}

void IBBRHandleRestartFromIdle(picoquic_ibbr_state_t* ibbr_state, uint64_t bytes_in_transit, int is_app_limited)
{
    if (bytes_in_transit == 0 && is_app_limited)
    {
        ibbr_state->idle_restart = 1;
        //if (ibbr_state->state == picoquic_ibbr_alg_probe_bw) {
        if (ibbr_state->state == picoquic_ibbr_alg_probe_steady) {
            IBBRSetPacingRateWithGain(ibbr_state, 1.0);
        }
    }
}


/* This is the per ACK processing, activated upon receiving an ACK.
 * At that point, we expect the following:
 *  - delivered has been updated to reflect all the data acked on the path.
 *  - the delivery rate sample has been computed.
 */

void  IBBRUpdateOnACK(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x,
    uint64_t rtt_sample, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered,
    uint64_t current_time)
{
    //DBG_PRINTF("|IBBR-OnAck|");
    IBBRUpdateModelAndState(ibbr_state, path_x, rtt_sample, bytes_in_transit,
        packets_lost, current_time);
    IBBRUpdateControlParameters(ibbr_state, path_x, bytes_in_transit, packets_lost, bytes_delivered);
}

void IBBROnTransmit(picoquic_ibbr_state_t* ibbr_state, uint64_t bytes_in_transit, int is_app_limited)
{
    IBBRHandleRestartFromIdle(ibbr_state, bytes_in_transit, is_app_limited);
}

/* Dealing with recovery. What happens when all
 * the packets are lost, when all packets have been retransmitted.. */

void IBBROnAllPacketsLost(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x)
{
    ibbr_state->prior_cwnd = IBBRSaveCwnd(ibbr_state, path_x);
    path_x->cwin = path_x->send_mtu;
}

void IBBROnEnterFastRecovery(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t bytes_delivered)
{
    if (bytes_delivered < path_x->send_mtu) {
        bytes_delivered = path_x->send_mtu;
    }
    ibbr_state->prior_cwnd = IBBRSaveCwnd(ibbr_state, path_x);
    path_x->cwin = bytes_in_transit + bytes_delivered;
    ibbr_state->packet_conservation = 1;
}

void IBBRAfterOneRoundtripInFastRecovery(picoquic_ibbr_state_t* ibbr_state)
{
    ibbr_state->packet_conservation = 0;
}

void IBBRExitFastRecovery(picoquic_ibbr_state_t* ibbr_state, picoquic_path_t* path_x)
{
    ibbr_state->packet_conservation = 0;
    IBBRRestoreCwnd(ibbr_state, path_x);
}

/*
 * In order to implement IBBR, we map generic congestion notification
 * signals to the corresponding IBBR actions.
 */
static void picoquic_ibbr_notify(
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
    picoquic_ibbr_state_t* ibbr_state = (picoquic_ibbr_state_t*)path_x->congestion_alg_state;

    if (ibbr_state != NULL) {
        /*
        if (ibbr_state->state == picoquic_ibbr_alg_probe_bw) {//�����ǰ���ڴ���̽��׶Σ����ӡ�����Ϣ
            printf("IBBR BW probing\n");
        }
        */
        switch (notification) {//���ݴ����notification������Ҫ��ɵĲ���
        case picoquic_congestion_notification_acknowledgement://�����acknowledgement�źţ�����ݴ����ack��������ibbr���ѷ���������
            /* sum the amount of data acked per packet */
            ibbr_state->bytes_delivered += nb_bytes_acknowledged;
            break;
        case picoquic_congestion_notification_repeat://�����repeat�źţ�������ڴ���̽��׶δ�ӡ��Ϣ�ⲻ���κδ���
            break;
        case picoquic_congestion_notification_timeout://�����timeout�źţ�������ڴ���̽��׶δ�ӡ��Ϣ�ⲻ���κδ���
            /* enter recovery */
            break;
        case picoquic_congestion_notification_spurious_repeat://�����spurious_repeat�źţ�������ڴ���̽��׶δ�ӡ��Ϣ�ⲻ���κδ���
            break;
        case picoquic_congestion_notification_rtt_measurement://�����RTT�����ź�
            if (ibbr_state->state == picoquic_ibbr_alg_startup && path_x->smoothed_rtt > PICOQUIC_TARGET_RENO_RTT) {
                IBBREnterStartupLongRTT(ibbr_state, path_x);//��������������������׶�ʱ
            }
            if (ibbr_state->state == picoquic_ibbr_alg_startup_long_rtt) {
                if (picoquic_hystart_test(&ibbr_state->rtt_filter, rtt_measurement, path_x->pacing_packet_time_microsec, current_time, false)) {
                    IBBRExitStartupLongRtt(ibbr_state, path_x, current_time);
                }
            }
            break;
        case picoquic_congestion_notification_bw_measurement://�����Bw�����ź�
            /* RTT measurements will happen after the bandwidth is estimated */
            if (ibbr_state->state == picoquic_ibbr_alg_startup_long_rtt) {
                IBBRUpdateBtlBw(ibbr_state, path_x);
                if (rtt_measurement <= ibbr_state->rt_prop) {
                    ibbr_state->rt_prop = rtt_measurement;
                    ibbr_state->rt_prop_stamp = current_time;
                }
                if (picoquic_cc_was_cwin_blocked(path_x, ibbr_state->last_sequence_blocked)) {
                    picoquic_hystart_increase(path_x, &ibbr_state->rtt_filter, ibbr_state->bytes_delivered);
                }
                ibbr_state->bytes_delivered = 0;

                picoquic_update_pacing_data(path_x);
            }
            else {
                IBBRUpdateOnACK(ibbr_state, path_x,
                    rtt_measurement, path_x->bytes_in_transit, 0 /* packets_lost */, ibbr_state->bytes_delivered,
                    current_time);
                /* Remember the number in flight before the next ACK -- TODO: update after send instead. */
                ibbr_state->prior_in_flight = path_x->bytes_in_transit;
                /* Reset the number of bytes delivered */
                ibbr_state->bytes_delivered = 0;

                if (ibbr_state->pacing_rate > 0) {
                    /* Set the pacing rate in picoquic sender */
                    picoquic_update_pacing_rate(path_x, ibbr_state->pacing_rate, ibbr_state->send_quantum);
                }
            }
            break;
        case picoquic_congestion_notification_cwin_blocked:
            ibbr_state->last_sequence_blocked = picoquic_cc_get_sequence_number(path_x);
        default:
            /* ignore */
            break;
        }
    }
}

#define picoquic_ibbr_ID 0x49414343 /* IBBR */

picoquic_congestion_algorithm_t picoquic_ibbr_algorithm_struct = {
        picoquic_ibbr_ID,
        picoquic_ibbr_init,
        picoquic_ibbr_notify,
        picoquic_ibbr_delete
};

/*��һ����ָ��cc�㷨���������������ӿڣ����ϲ�ѡ�������*/
picoquic_congestion_algorithm_t* picoquic_ibbr_algorithm = &picoquic_ibbr_algorithm_struct;