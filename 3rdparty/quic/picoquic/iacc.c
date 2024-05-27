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

/*进入慢启动阶段，pacing_gain与cwnd_gain均设置为2.885*/
void IACCEnterStartup(picoquic_iacc_state_t* iacc_state)
{
    iacc_state->state = picoquic_iacc_alg_startup;
    iacc_state->pacing_gain = IACC_HIGH_GAIN;
    iacc_state->cwnd_gain = IACC_HIGH_GAIN;
}

/*根据当前iacc的pacing_rate以及当前路径patg_x中设置的发送最大单元send_mtu来决定iacc中的send_quantum值，这个值的作用仍需继续学习*/
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

/*仅估算链路bdp*/
double IACCBDP(picoquic_iacc_state_t* iacc_state) {
    return (((double)iacc_state->btl_bw * (double)iacc_state->rt_prop) / 1000000.0);
}

/*估算当前链路bdp，计算传入gain值下的cwnd值（这里的cwnd包含了一个三倍的sendQuantum）并返回*/
uint64_t IACCInflight(picoquic_iacc_state_t* iacc_state, double gain)
{
    uint64_t cwnd = PICOQUIC_CWIN_INITIAL;
    if (iacc_state->rt_prop != UINT64_MAX) {
        /* Bandwidth is estimated in bytes per second, rtt in microseconds*/
        double estimated_bdp = (((double)iacc_state->btl_bw * (double)iacc_state->rt_prop) / 1000000.0);//估算bdp
        uint64_t quanta = 3 * iacc_state->send_quantum;
        cwnd = (uint64_t)(gain * estimated_bdp) + quanta;
    }
    return cwnd;
}

/*更新当前的target_cwnd值为IACCInflight返回的值，传入的gain值为当前iacc_state中的cwnd_gain*/
void IACCUpdateTargetCwnd(picoquic_iacc_state_t* iacc_state)
{
    iacc_state->target_cwnd = IACCInflight(iacc_state, iacc_state->cwnd_gain);
}

/*初始化iacc结构体，并将其赋值给传入的path_x变量congestion_alg_state中*/
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
        iacc_state->rt_prop_stamp = current_time;//初始化rtprop时间戳为当前时间（用于计算时间）
        iacc_state->cycle_stamp = current_time;//初始化cycle时间戳为当前时间，用于带宽探测阶段的cycle调整

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

        IACCEnterStartup(iacc_state);//进入慢启动阶段
        IACCSetSendQuantum(iacc_state, path_x);//设置send_quantum值，初始化阶段pacing_rate为0，故此时quantum值为一个sendmtu
        IACCUpdateTargetCwnd(iacc_state);//初始化targetcwnd，由于btlbw初始值为0，故计算的bdp值为0，因此初始cwnd为加上的send_quantum，也就是一个mtu
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
    /*这里可以画一条线来解释，线上被多个点分割，每个点表示iacc下一轮开始前已经发送的数据量，也就是iacc_state->next_round_delivered
    而path_x->delivered_last_packet则表示上一个数据包发送之后这条路径上已经发送的数据量，因此这里就很好解释了，当这个值大于线上的某一个点
    这就表示，这一轮之前的数据都发送完了，甚至超出了，因此可以开始这一轮的数据发送，所以round_start被设置为0，这也是round开启的条件
    */
    DBG_PRINTF("|UPDATE BW|");
    if (path_x->delivered_last_packet >= iacc_state->next_round_delivered)
    {
        DBG_PRINTF("|ROUND START|");
        iacc_state->next_round_delivered = path_x->delivered;//更新下一轮开始所需要的数据发送量，
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

        /*这一部分是追踪最大瓶颈带宽*/
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
/*判断是否需要更新记录的RTprop*/
void IACCUpdateRTprop(picoquic_iacc_state_t* iacc_state, uint64_t rtt_sample, uint64_t current_time, picoquic_path_t* path_x)
{
    iacc_state->probe_rt_prop_expired = current_time > (iacc_state->probe_rt_prop_stamp + IACC_PROBE_RTT_PROP_INTERVAL);
    if (rtt_sample <= iacc_state->probe_rt_prop || iacc_state->probe_rt_prop_expired) {
        iacc_state->probe_rt_prop = rtt_sample;
        iacc_state->probe_rt_prop_stamp = current_time;
    }

    //这里表示如果当前时间距离上次rtprop记录时间已超过10秒，则表示上次记录的rtprop失效，需要重新测量并记录
    iacc_state->rt_prop_expired =
        current_time > iacc_state->rt_prop_stamp + IACC_PROBE_RTT_INTERVAL;//这里可能得设置为2.5秒
    if (iacc_state->probe_rt_prop <= iacc_state->rt_prop || iacc_state->rt_prop_expired) {
        //重新记录rtprop的条件为二选一，要么采样的rtt小于这个值，要么rtprop因长时间未更新而失效
        iacc_state->rt_prop = iacc_state->probe_rt_prop;//如果满足以上条件之一，则更新rtprop为当前rtt采样值，这里有可能出现变大的情况
        iacc_state->rt_prop_stamp = iacc_state->probe_rt_prop_stamp;//同时更新记录的时间戳
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

/*这一步是判断是否可以进入cycle中的下一个阶段*/
int IACCIsNextCyclePhase(picoquic_iacc_state_t* iacc_state, uint64_t prior_in_flight, uint64_t packets_lost, uint64_t current_time)
{
    int is_full_length = (current_time - iacc_state->cycle_stamp) > iacc_state->rt_prop;
    //这里的这个值表示当前时间和上一次记录的cycle时间戳的间隔是否大于一个rtprop，这是第一个条件

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

/*更具cycle更新pacinggain*/
void IACCAdvanceCyclePhase(picoquic_iacc_state_t* iacc_state, uint64_t current_time)
{
    iacc_state->cycle_stamp = current_time;//更新当前cycle时间戳
    iacc_state->cycle_index++;//增加index
    if (iacc_state->cycle_index >= IACC_GAIN_CYCLE_LEN) {
        int start = (int)(iacc_state->rt_prop / PICOQUIC_TARGET_RENO_RTT);
        if (start > IACC_GAIN_CYCLE_MAX_START) {
            start = IACC_GAIN_CYCLE_MAX_START;
        }
        iacc_state->cycle_index = start;
    }

    iacc_state->pacing_gain = iacc_pacing_gain_cycle[iacc_state->cycle_index];//根据设定的值更新pacing_gain
}

/*检测是否需要通过带宽探测阶段的cycle来更新pacing_gain*/
/*
void IACCCheckCyclePhase(picoquic_iacc_state_t* iacc_state, uint64_t packets_lost, uint64_t current_time)
{
    if (iacc_state->state == picoquic_iacc_alg_probe_bw &&
        IACCIsNextCyclePhase(iacc_state, iacc_state->prior_in_flight, packets_lost, current_time)) {
        IACCAdvanceCyclePhase(iacc_state, current_time);//如果满足上述两个条件1）处于带宽探测阶段；2）可以进入下一个更新周期，则更新pacing_gain
    }
}
*/

/*检测是否达到瓶颈带宽，没有则更新并重新技术，有则计数，若计数超过三次则表示已到达*/
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

/*进入带宽探测阶段，此时pacing_gain设置为1，cwnd_gain设置为1.5*/
/*void IACCEnterProbeBW(picoquic_iacc_state_t* iacc_state, uint64_t current_time)
{
    iacc_state->state = picoquic_iacc_alg_probe_bw;
    iacc_state->pacing_gain = 1.0;
    iacc_state->cwnd_gain = 1.5;
    iacc_state->cycle_index = 4;  // TODO: random_int_in_range(0, 5); 
    IACCAdvanceCyclePhase(iacc_state, current_time);
}
*/

/*进入排空阶段，pacing_gain设置为1/2.885，cwnd_gain设置为2.885*/
void IACCEnterDrain(picoquic_iacc_state_t* iacc_state)
{
    iacc_state->state = picoquic_iacc_alg_drain;
    iacc_state->pacing_gain = 1.0 / IACC_HIGH_GAIN;  /* pace slowly */
    iacc_state->cwnd_gain = IACC_HIGH_GAIN;   /* maintain cwnd */
    iacc_state->enter_from_drain = 1;
}

/*检测是否需要从慢启动进入排空，或从排空进入带宽探测阶段*/
void IACCCheckDrain(picoquic_iacc_state_t* iacc_state, uint64_t bytes_in_transit, uint64_t current_time)
{
    if (iacc_state->state == picoquic_iacc_alg_startup && iacc_state->filled_pipe) {
        IACCEnterDrain(iacc_state);//如果处于慢启动阶段且已达到瓶颈带宽，则进入排空阶段
    }

    if (iacc_state->state == picoquic_iacc_alg_drain && bytes_in_transit <= IACCInflight(iacc_state, 1.0)) {
        //如果处于排空阶段，且在途字节数小于计算的可容纳数量，则表示排空完毕，可进入带宽探测阶段
        //IACCEnterProbeBW(iacc_state, current_time);  /* we estimate queue is drained */
        IACCEnterProbeRound1(iacc_state);
    }
}

/*进入round1阶段*/
void IACCEnterProbeRound1(picoquic_iacc_state_t* iacc_state) {
    iacc_state->state = picoquic_iacc_alg_probe_steady;
    if (iacc_state->enter_from_drain) {
        iacc_state->cwnd_gain = 2.5;
        iacc_state->enter_from_drain = 2.5;
    }
    iacc_state->pacing_gain = iacc_state->pacing_gain_percent / 100.0;
}

/*进入round2阶段*/
void IACCEnterProbeRound2(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x) {
    iacc_state->state = picoquic_iacc_alg_probe_oversent;
    iacc_state->pacing_gain = iacc_state->pacing_gain_percent / 100.0;
    iacc_state->cwnd_gain = iacc_state->oversent_percent / 100.0;
    iacc_state->over_sent = 0;
    iacc_state->bytes_sent_in_round_start = path_x->bytes_send;
    iacc_state->bytes_delivered_in_round_start = iacc_state->bytes_delivered;
}

/*进入round3阶段*/
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

/*如果此时探测到的带宽是瓶颈带宽，那么就退出rtt探测阶段直接到带宽探测阶段，否则进入慢启动阶段*/
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

/*暂存path结构体中的cwnd，若是出于恢复丢包或者处于带宽探测阶段，则还要比较记录的前一个cwnd值，比较后保存更大的那个*/
uint64_t IACCSaveCwnd(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x) {
    uint64_t w = path_x->cwin;

    if ((InLossRecovery(iacc_state)) &&
        (path_x->cwin < iacc_state->prior_cwnd)) {
        w = iacc_state->prior_cwnd;
    }

    return w;
}

/*恢复path中的cwin控制参数，恢复的值为之前存储的值*/
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
    //注意，以下代码的前提是当前已处于rtt探测阶段，cwnd可能没有被置为4个mtu（刚从其他阶段转入）或者已经被置为4个mtu
    if (iacc_state->probe_rtt_done_stamp == 0 &&
        bytes_in_transit <= IACC_MIN_PIPE_CWND(path_x->send_mtu)) {
        //若探测尚未开始，同时检测到在途字节数已经小于4个mtu了，这表示已经处于应用受限阶段，可以开始更新rtprop了
        iacc_state->probe_rtt_done_stamp =
            current_time + IACC_PROBE_RTT_DURATION;//记录时间戳为当前时间加上200ms，200ms为探测的维持时间
        iacc_state->probe_rtt_round_done = 0;//更新标志为参数，由于还没结束，所以是0
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

/*检测是否需要进入rtt探测阶段，或者从rtt探测阶段退出到带宽探测或者慢启动阶段*/
/*
void IACCCheckProbeRTT(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t current_time)
{
    if (iacc_state->state != picoquic_iacc_alg_probe_rtt &&
        iacc_state->rt_prop_expired &&
        !iacc_state->idle_restart) {
        //这一部分是当rtprop长时间未更新时，强制进入rtt探测阶段，该阶段双gain值均被设置为1，
        //但是具体调控的pacing_rate和cwnd并没有发生变化，这一部分只是发生gain值和状态的改变
        IACCEnterProbeRTT(iacc_state);//进入rtt探测阶段
        iacc_state->prior_cwnd = IACCSaveCwnd(iacc_state, path_x);//保存cwnd的值
        iacc_state->probe_rtt_done_stamp = 0;
        //这一变量表示的是rtt探测结束的时间戳，设为0表示探测尚未开始，因为rtprop的测量是需要在应用受限阶段的，做法是cwnd将为4个mtu，这需要等待一段时间
    }

    if (iacc_state->state == picoquic_iacc_alg_probe_rtt) {
        //如果当前已经处于RTT探测阶段
        IACCHandleProbeRTT(iacc_state, path_x, bytes_in_transit, current_time);
        iacc_state->idle_restart = 0;
    }
}
*/

/*更新iacc状态信息，注意这是每次收到ack后执行的，所以有些函数的执行是一个长期的变化*/
void IACCUpdateModelAndState(picoquic_iacc_state_t* iacc_state, picoquic_path_t* path_x,
    uint64_t rtt_sample, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t current_time)
{
    IACCUpdateBtlBw(iacc_state, path_x);//更新探测的瓶颈带宽
    //IACCCheckCyclePhase(iacc_state, packets_lost, current_time);
    IACCCheckFullPipe(iacc_state, path_x->last_bw_estimate_path_limited);
    IACCCheckDrain(iacc_state, bytes_in_transit, current_time);
    IACCUpdateRTprop(iacc_state, rtt_sample, current_time, path_x);
    //IACCCheckProbeRTT(iacc_state, path_x, bytes_in_transit, current_time);
}

/*更新pacingrate，注意这里更新的只是iacc中的pacingrate，并没有涉及path参数*/
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

/*若当前处于RTT探测阶段且cwnd*/
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

    //IACCModulateCwndForProbeRTT(iacc_state, path_x);//判断是否进入probertt阶段，从而
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
    /*cc算法的初始化部分是在创建path或者创建connection时进行的，因此这里是从path中取得相关算法的结构体*/
    picoquic_iacc_state_t* iacc_state = (picoquic_iacc_state_t*)path_x->congestion_alg_state;

    if (iacc_state != NULL) {
        /*
        if (iacc_state->state == picoquic_iacc_alg_probe_bw) {//如果当前处于带宽探测阶段，则打印相关信息
            printf("IACC BW probing\n");
        }
        */
        switch (notification) {//根据传入的notification决定需要完成的操作
        case picoquic_congestion_notification_acknowledgement://如果是acknowledgement信号，则根据传入的ack数量更新iacc中已发送数据量
            /* sum the amount of data acked per packet */
            iacc_state->bytes_delivered += nb_bytes_acknowledged;
            break;
        case picoquic_congestion_notification_repeat://如果是repeat信号，则除了在带宽探测阶段打印信息外不作任何处理
            break;
        case picoquic_congestion_notification_timeout://如果是timeout信号，则除了在带宽探测阶段打印信息外不作任何处理
            /* enter recovery */
            break;
        case picoquic_congestion_notification_spurious_repeat://如果是spurious_repeat信号，则除了在带宽探测阶段打印信息外不作任何处理
            break;
        case picoquic_congestion_notification_rtt_measurement://如果是RTT测量信号
            if (iacc_state->state == picoquic_iacc_alg_startup && path_x->smoothed_rtt > PICOQUIC_TARGET_RENO_RTT) {
                IACCEnterStartupLongRTT(iacc_state, path_x);//这种情况适用于慢启动阶段时
            }
            if (iacc_state->state == picoquic_iacc_alg_startup_long_rtt) {
                if (picoquic_hystart_test(&iacc_state->rtt_filter, rtt_measurement, path_x->pacing_packet_time_microsec, current_time, false)) {
                    IACCExitStartupLongRtt(iacc_state, path_x, current_time);
                }
            }
            break;
        case picoquic_congestion_notification_bw_measurement://如果是Bw测量信号
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

/*这一部分指定cc算法，包含三个函数接口，供上层选择与调用*/
picoquic_congestion_algorithm_t* picoquic_iacc_algorithm = &picoquic_iacc_algorithm_struct;