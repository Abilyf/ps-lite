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
Implementation of the BBR algorithm, tuned for Picoquic.

The main idea of BBR is to track the "bottleneck bandwidth", and to tune the
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

BBR does that by following a cycle of "send, test and drain". During the
sending period, the stack sends at the measured rate. During the testing
period, it sends faster, 25% faster with recommended parameters. This
risk creating a queue if the bandwidth had not increased, so the test
period is followed by a drain period during which the stack sends 25%
slower than the measured rate. If the test is successful, the new bandwidth
measurement will be available at the end of the draining period, and
the increased bandwidth will be used in the next cycle.

Tuning the sending rate does not guarantee a short queue, it only
guarantees a stable queue. BBR controls the queue by limiting the
amount of data "in flight" (congestion window, CWIN) to the product
of the bandwidth estimate by the RTT estimate, plus a safety marging to ensure
continuous transmission. Using the average RTT there would lead to a runaway
loop in which oversized windows lead to increased queues and then increased
average RTT. Instead of average RTT, BBR uses a minimum RTT. Since the
mimimum RTT might vary with routing changes, the minimum RTT is measured
on a sliding window of 10 seconds.

The bandwidth estimation needs to be robust against short term variations
common in wireless networks. BBR retains the maximum
delivery rate observed over a series of probing intervals. Each interval
starts with a specific packet transmission and ends when that packet
or a later transmission is acknowledged. BBR does that by tracking
the delivered counter associated with packets and comparing it to
the delivered counter at start of period.

During start-up, BBR performs its own equivalent of Reno's slow-start.
It does that by using a pacing gain of 2.89, i.e. sending 2.89 times
faster than the measured maximum. It exits slow start when it found
a bandwidth sufficient to fill the pipe.

The bandwidth measurements can be wrong if the application is not sending
enough data to fill the pipe. BBR tracks that, and does not reduce bandwidth
or exit slow start if the application is limiting transmission.

This implementation follows draft-cardwell-iccrg-bbr-congestion-control,
with a couple of changes for handling the multipath nature of quic.
There is a BBR control state per path.
Most of BBR the variables defined in the draft are implemented
in the "BBR state" structure, with a few exceptions:

* BBR.delivered is represented by path_x.delivered, and is maintained
  as part of ACK processing

* Instead of "bytes_in_transit", we use "bytes_in_transit", which is
  already maintained by the stack.

* Compute bytes_delivered by summing all calls to ACK(bytes) before
  the call to RTT update.

* In the Probe BW mode, the draft suggests cwnd_gain = 2. We observed
  that this results in queue sizes of 2, which is too high, so we
  reset that to 1.125.

The "packet" variables are defined in the picoquic_packet_t.

Early testing showed that BBR startup phase requires several more RTT
than the Hystart process used in modern versions of Reno or Cubic. BBR
only ramps up the data rate after the first bandwidth measurement is
available, 2*RTT after start, while Reno or Cubic start ramping up
after just 1 RTT. BBR only exits startup if three consecutive RTT
pass without significant BW measurement increase, which not only
adds delay but also creates big queues as data is sent at 2.89 times
the bottleneck rate. This is a tradeoff: longer search for bandwidth in
slow start is less likely to stop too early because of transient
issues, but one high bandwidth and long delay links this translates
to long delays and a big batch of packet losses.

This BBR implementation addresses these issues by switching to
Hystart instead of startup if the RTT is above the Reno target of
100 ms.

*/

typedef enum {
    picoquic_bbr_alg_startup = 0,
    picoquic_bbr_alg_drain,
    picoquic_bbr_alg_probe_bw,
    picoquic_bbr_alg_probe_rtt,
    picoquic_bbr_alg_startup_long_rtt
} picoquic_bbr_alg_state_t;

#define BBR_BTL_BW_FILTER_LENGTH 10
#define BBR_RT_PROP_FILTER_LENGTH 10
#define BBR_HIGH_GAIN 2.8853900817779 /* 2/ln(2) */
#define BBR_MIN_PIPE_CWND(mss) (4*mss)
#define BBR_GAIN_CYCLE_LEN 8
#define BBR_PROBE_RTT_INTERVAL 10000000 /* 10 sec, 10000000 microsecs */
#define BBR_PROBE_RTT_DURATION 200000 /* 200msec, 200000 microsecs */
#define BBR_PACING_RATE_LOW 150000.0 /* 150000 B/s = 1.2 Mbps */
#define BBR_PACING_RATE_MEDIUM 3000000.0 /* 3000000 B/s = 24 Mbps */
#define BBR_GAIN_CYCLE_LEN 8
#define BBR_GAIN_CYCLE_MAX_START 5

static const double bbr_pacing_gain_cycle[BBR_GAIN_CYCLE_LEN] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.25, 0.75};

typedef struct st_picoquic_bbr_state_t {
    picoquic_bbr_alg_state_t state;
    uint64_t btl_bw;
    uint64_t next_round_delivered;
    uint64_t round_start_time;
    uint64_t btl_bw_filter[BBR_BTL_BW_FILTER_LENGTH];
    uint64_t full_bw;
    uint64_t rt_prop;
    uint64_t rt_prop_stamp;
    uint64_t cycle_stamp;
    uint64_t probe_rtt_done_stamp;
    uint64_t prior_cwnd;
    uint64_t prior_in_flight;
    uint64_t bytes_delivered;
    uint64_t send_quantum;
    picoquic_min_max_rtt_t rtt_filter;
    uint64_t target_cwnd;
    uint64_t last_sequence_blocked;
    double pacing_gain;
    double cwnd_gain;
    double pacing_rate;
    size_t cycle_index;
    int round_count;
    int full_bw_count;
    int filled_pipe : 1;
    int round_start : 1;
    int rt_prop_expired : 1;
    int probe_rtt_round_done : 1;
    int idle_restart : 1;
    int packet_conservation : 1;
} picoquic_bbr_state_t;

void BBREnterStartupLongRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t cwnd = PICOQUIC_CWIN_INITIAL;
    bbr_state->state = picoquic_bbr_alg_startup_long_rtt;

    if (path_x->smoothed_rtt > PICOQUIC_TARGET_RENO_RTT) {
        cwnd = (uint64_t)((double)cwnd * (double)path_x->smoothed_rtt / (double)PICOQUIC_TARGET_RENO_RTT);
    }
    if (cwnd > path_x->cwin) {
        path_x->cwin = cwnd;
    }
}

/*进入慢启动阶段，pacing_gain与cwnd_gain均设置为2.885*/
void BBREnterStartup(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->state = picoquic_bbr_alg_startup;
    bbr_state->pacing_gain = BBR_HIGH_GAIN;
    bbr_state->cwnd_gain = BBR_HIGH_GAIN;
}

/*根据当前bbr的pacing_rate以及当前路径patg_x中设置的发送最大单元send_mtu来决定bbr中的send_quantum值，这个值的作用仍需继续学习*/
void BBRSetSendQuantum(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (bbr_state->pacing_rate < BBR_PACING_RATE_LOW) {
        bbr_state->send_quantum = 1ull * path_x->send_mtu;
    }
    else if (bbr_state->pacing_rate < BBR_PACING_RATE_MEDIUM) {
        bbr_state->send_quantum = 2ull * path_x->send_mtu;
    }
    else {
        bbr_state->send_quantum = (uint64_t)(bbr_state->pacing_rate * 0.001);
        if (bbr_state->send_quantum > 64000) {
            bbr_state->send_quantum = 64000;
        }
    }
}

/*估算当前链路bdp，计算传入gain值下的cwnd值（这里的cwnd包含了一个三倍的sendQuantum）并返回*/
uint64_t BBRInflight(picoquic_bbr_state_t* bbr_state, double gain)
{
    uint64_t cwnd = PICOQUIC_CWIN_INITIAL;
    if (bbr_state->rt_prop != UINT64_MAX){
        /* Bandwidth is estimated in bytes per second, rtt in microseconds*/
        double estimated_bdp = (((double)bbr_state->btl_bw * (double)bbr_state->rt_prop) / 1000000.0);//估算bdp
        uint64_t quanta = 3 * bbr_state->send_quantum;
        cwnd = (uint64_t)(gain * estimated_bdp) + quanta;
    }
    return cwnd;
}

/*更新当前的target_cwnd值为BBRInflight返回的值，传入的gain值为当前bbr_state中的cwnd_gain*/
void BBRUpdateTargetCwnd(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->target_cwnd = BBRInflight(bbr_state, bbr_state->cwnd_gain);
}

/*初始化bbr结构体，并将其赋值给传入的path_x变量congestion_alg_state中*/
static void picoquic_bbr_init(picoquic_cnx_t *cnx, picoquic_path_t* path_x)
{
    /* Initialize the state of the congestion control algorithm */
    picoquic_bbr_state_t* bbr_state = (picoquic_bbr_state_t*)malloc(sizeof(picoquic_bbr_state_t));
    path_x->congestion_alg_state = (void*)bbr_state;
    if (bbr_state != NULL) {
        memset(bbr_state, 0, sizeof(picoquic_bbr_state_t));
        path_x->cwin = PICOQUIC_CWIN_INITIAL;
        bbr_state->rt_prop = UINT64_MAX;
        uint64_t current_time = picoquic_current_time();
        bbr_state->rt_prop_stamp = current_time;//初始化rtprop时间戳为当前时间（用于计算时间）
        bbr_state->cycle_stamp = current_time;//初始化cycle时间戳为当前时间，用于带宽探测阶段的cycle调整

        BBREnterStartup(bbr_state);//进入慢启动阶段
        BBRSetSendQuantum(bbr_state, path_x);//设置send_quantum值，初始化阶段pacing_rate为0，故此时quantum值为一个sendmtu
        BBRUpdateTargetCwnd(bbr_state);//初始化targetcwnd，由于btlbw初始值为0，故计算的bdp值为0，因此初始cwnd为加上的send_quantum，也就是一个mtu
    }
}

/* Release the state of the congestion control algorithm */
static void picoquic_bbr_delete(picoquic_cnx_t *cnx, picoquic_path_t* path_x)
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

void BBRUpdateBtlBw(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    uint64_t bandwidth_estimate = path_x->bandwidth_estimate;
    if (bbr_state->last_sequence_blocked == 0 || !picoquic_cc_was_cwin_blocked(path_x, bbr_state->last_sequence_blocked)) {
        // the estimation is not reliable because the CWIN was not probed entirely
        return;
    }
    /*这里可以画一条线来解释，线上被多个点分割，每个点表示bbr下一轮开始前已经发送的数据量，也就是bbr_state->next_round_delivered
    而path_x->delivered_last_packet则表示上一个数据包发送之后这条路径上已经发送的数据量，因此这里就很好解释了，当这个值大于线上的某一个点
    这就表示，这一轮之前的数据都发送完了，甚至超出了，因此可以开始这一轮的数据发送，所以round_start被设置为0，这也是round开启的条件
    */
    if (path_x->delivered_last_packet >= bbr_state->next_round_delivered)
    {
        bbr_state->next_round_delivered = path_x->delivered;//更新下一轮开始所需要的数据发送量，
        bbr_state->round_count++;
        bbr_state->round_start = 1;
    }
    else {
        bbr_state->round_start = 0;
    }

    if (bbr_state->round_start) {
        /* Forget the oldest BW round, shift by 1, compute the max BTL_BW for
         * the remaining rounds, set current round max to current value */

        bbr_state->btl_bw = 0;

        /*这一部分是追踪最大瓶颈带宽*/
        for (int i = BBR_BTL_BW_FILTER_LENGTH - 2; i >= 0; i--) {//这一部分将bw组内所有的bw值往右后移一位，同时记录最大的bw值为btlbw
            uint64_t b = bbr_state->btl_bw_filter[i];
            bbr_state->btl_bw_filter[i + 1] = b;
            if (b > bbr_state->btl_bw) {
                bbr_state->btl_bw = b;
            }
        }

        bbr_state->btl_bw_filter[0] = 0;//这一部分将第一位清空，方便后续添加
    }

    if (bandwidth_estimate > bbr_state->btl_bw_filter[0]) {
        bbr_state->btl_bw_filter[0] =bandwidth_estimate;//将新测量的bw加入到bw组
        if (bandwidth_estimate > bbr_state->btl_bw) {
            bbr_state->btl_bw = bandwidth_estimate;//如果新加入的bw大于记录的bw，则更新记录的bw为该值
        }
    }
    //实际上，从这一段bw更新逻辑也可以看出，并不是每个ack都会开启一个round，round开启的条件和数据量有关，若有时收到ack后并不满足round开启条件
    //那么这部分的带宽bw[0]更新为最新的且大于该值的bw，这会产生一个问题，如果在round开启后测量的第一个bw偏大，那么在下一个round开启时
    //小于第一个bw的值是会被丢弃的，只有当round开启时才会从新一轮的round里选择出最大的bw
}

/* This will use one way samples if available */
/* Should augment that with common RTT filter to suppress jitter */
void BBRUpdateRTprop(picoquic_bbr_state_t* bbr_state, uint64_t rtt_sample, uint64_t current_time)
{
    bbr_state->rt_prop_expired =
            current_time > bbr_state->rt_prop_stamp + BBR_PROBE_RTT_INTERVAL;
    if (rtt_sample <= bbr_state->rt_prop || bbr_state->rt_prop_expired) {
        bbr_state->rt_prop = rtt_sample;
        bbr_state->rt_prop_stamp = current_time;
    }
}

/*这一步是判断是否可以进入cycle中的下一个阶段*/
int BBRIsNextCyclePhase(picoquic_bbr_state_t* bbr_state, uint64_t prior_in_flight, uint64_t packets_lost, uint64_t current_time)
{
    int is_full_length = (current_time - bbr_state->cycle_stamp) > bbr_state->rt_prop;
    //这里的这个值表示当前时间和上一次记录的cycle时间戳的间隔是否大于一个rtprop，这是第一个条件

    if (bbr_state->pacing_gain != 1.0) {
        if (bbr_state->pacing_gain > 1.0) {
            is_full_length &=
                    (packets_lost > 0 ||
                     prior_in_flight >= BBRInflight(bbr_state, bbr_state->pacing_gain));
        }
        else {  /*  (BBR.pacing_gain < 1) */
            is_full_length |= prior_in_flight <= BBRInflight(bbr_state, 1.0);
        }
    }
    return is_full_length;
}


void BBRAdvanceCyclePhase(picoquic_bbr_state_t* bbr_state, uint64_t current_time)
{
    bbr_state->cycle_stamp = current_time;//更新当前cycle时间戳
    bbr_state->cycle_index++;//增加index
    if (bbr_state->cycle_index >= BBR_GAIN_CYCLE_LEN) {
        int start = (int)(bbr_state->rt_prop / PICOQUIC_TARGET_RENO_RTT);
        if (start > BBR_GAIN_CYCLE_MAX_START) {
            start = BBR_GAIN_CYCLE_MAX_START;
        }
        bbr_state->cycle_index = start;
    }

    bbr_state->pacing_gain = bbr_pacing_gain_cycle[bbr_state->cycle_index];//根据设定的值更新pacing_gain
}

void BBRCheckCyclePhase(picoquic_bbr_state_t* bbr_state, uint64_t packets_lost, uint64_t current_time)
{
    if (bbr_state->state == picoquic_bbr_alg_probe_bw &&
        BBRIsNextCyclePhase(bbr_state, bbr_state->prior_in_flight, packets_lost, current_time)) {
        BBRAdvanceCyclePhase(bbr_state, current_time);
    }
}

void BBRCheckFullPipe(picoquic_bbr_state_t* bbr_state, int rs_is_app_limited)
{
    if (!bbr_state->filled_pipe && bbr_state->round_start && !rs_is_app_limited) {
        if (bbr_state->btl_bw >= bbr_state->full_bw * 1.25) {  // BBR.BtlBw still growing?
            bbr_state->full_bw = bbr_state->btl_bw;   // record new baseline level
            bbr_state->full_bw_count = 0;
        }
        else {
            bbr_state->full_bw_count++; // another round w/o much growth
            if (bbr_state->full_bw_count >= 3) {
                bbr_state->filled_pipe = 1;
            }
        }
    }
}

/*进入带宽探测阶段，此时pacing_gain设置为1，cwnd_gain设置为1.5*/
void BBREnterProbeBW(picoquic_bbr_state_t* bbr_state, uint64_t current_time)
{
    bbr_state->state = picoquic_bbr_alg_probe_bw;
    bbr_state->pacing_gain = 1.0;
    bbr_state->cwnd_gain = 1.5;
    bbr_state->cycle_index = 4;  /* TODO: random_int_in_range(0, 5); */
    BBRAdvanceCyclePhase(bbr_state, current_time);
}

/*进入排空阶段，pacing_gain设置为1/2.885，cwnd_gain设置为2.885*/
void BBREnterDrain(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->state = picoquic_bbr_alg_drain;
    bbr_state->pacing_gain = 1.0 / BBR_HIGH_GAIN;  /* pace slowly */
    bbr_state->cwnd_gain = BBR_HIGH_GAIN;   /* maintain cwnd */
}

void BBRCheckDrain(picoquic_bbr_state_t* bbr_state, uint64_t bytes_in_transit, uint64_t current_time)
{
    if (bbr_state->state == picoquic_bbr_alg_startup && bbr_state->filled_pipe) {
        BBREnterDrain(bbr_state);
    }

    if (bbr_state->state == picoquic_bbr_alg_drain && bytes_in_transit <= BBRInflight(bbr_state, 1.0)) {
        BBREnterProbeBW(bbr_state, current_time);  /* we estimate queue is drained */
    }
}

void BBRExitStartupLongRtt(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t current_time)
{
    /* Reset the round filter so it will start at current time */
    bbr_state->next_round_delivered = path_x->delivered;
    bbr_state->round_count++;
    bbr_state->round_start = 1;
    /* Set the filled pipe indicator */
    bbr_state->full_bw = bbr_state->btl_bw;
    bbr_state->full_bw_count = 3;
    bbr_state->filled_pipe = 1;
    /* Enter drain */
    BBREnterDrain(bbr_state);
    /* If there were just few bytes in transit, enter probe */
    if (path_x->bytes_in_transit <= BBRInflight(bbr_state, 1.0)) {
        BBREnterProbeBW(bbr_state, current_time);
    }
}

void BBREnterProbeRTT(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->state = picoquic_bbr_alg_probe_rtt;
    bbr_state->pacing_gain = 1.0;
    bbr_state->cwnd_gain = 1.0;
}

void BBRExitProbeRTT(picoquic_bbr_state_t* bbr_state, uint64_t current_time)
{
    if (bbr_state->filled_pipe) {
        BBREnterProbeBW(bbr_state, current_time);
    }
    else {
        BBREnterStartup(bbr_state);
    }
}

int InLossRecovery(picoquic_bbr_state_t* bbr_state)
{
    return bbr_state->packet_conservation;
}

uint64_t BBRSaveCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x) {
    uint64_t w = path_x->cwin;

    if ((InLossRecovery(bbr_state) || bbr_state->state == picoquic_bbr_alg_probe_bw) &&
        (path_x->cwin < bbr_state->prior_cwnd)){
        w = bbr_state->prior_cwnd;
    }

    return w;
}

void BBRRestoreCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x)
{
    if (path_x->cwin < bbr_state->prior_cwnd) {
        path_x->cwin = bbr_state->prior_cwnd;
    }
}


void BBRHandleProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t * path_x, uint64_t bytes_in_transit, uint64_t current_time)
{
#if 0
    /* Ignore low rate samples during ProbeRTT: */
    C.app_limited =
        (BW.delivered + bytes_in_transit) ? 0 : 1;
#endif

    if (bbr_state->probe_rtt_done_stamp == 0 &&
        bytes_in_transit <= BBR_MIN_PIPE_CWND(path_x->send_mtu)) {
        bbr_state->probe_rtt_done_stamp =
                current_time + BBR_PROBE_RTT_DURATION;
        bbr_state->probe_rtt_round_done = 0;
        bbr_state->next_round_delivered = path_x->delivered;
    }
    else if (bbr_state->probe_rtt_done_stamp != 0) {
        if (bbr_state->round_start) {
            bbr_state->probe_rtt_round_done = 1;
        }

        if (bbr_state->probe_rtt_round_done &&
            current_time > bbr_state->probe_rtt_done_stamp) {
            bbr_state->rt_prop_stamp = current_time;
            BBRRestoreCwnd(bbr_state, path_x);
            BBRExitProbeRTT(bbr_state, current_time);
        }
    }
}

void BBRCheckProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t current_time)
{
    if (bbr_state->state != picoquic_bbr_alg_probe_rtt &&
        bbr_state->rt_prop_expired &&
        !bbr_state->idle_restart) {
        BBREnterProbeRTT(bbr_state);
        bbr_state->prior_cwnd = BBRSaveCwnd(bbr_state, path_x);
        bbr_state->probe_rtt_done_stamp = 0;
    }

    if (bbr_state->state == picoquic_bbr_alg_probe_rtt) {
        BBRHandleProbeRTT(bbr_state, path_x, bytes_in_transit, current_time);
        bbr_state->idle_restart = 0;
    }
}

void BBRUpdateModelAndState(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x,
                            uint64_t rtt_sample, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t current_time)
{
    //DBG_PRINTF("|Current CWND:%d|", bbr_state->target_cwnd);
    BBRUpdateBtlBw(bbr_state, path_x);//更新探测的瓶颈带宽
    BBRCheckCyclePhase(bbr_state, packets_lost, current_time);
    BBRCheckFullPipe(bbr_state, path_x->last_bw_estimate_path_limited);
    BBRCheckDrain(bbr_state, bytes_in_transit, current_time);
    BBRUpdateRTprop(bbr_state, rtt_sample, current_time);
    BBRCheckProbeRTT(bbr_state, path_x, bytes_in_transit, current_time);
}

void BBRSetPacingRateWithGain(picoquic_bbr_state_t* bbr_state, double pacing_gain)
{
    double rate = pacing_gain * (double)bbr_state->btl_bw;

    if (bbr_state->filled_pipe || rate > bbr_state->pacing_rate){
        bbr_state->pacing_rate = rate;
    }
}

void BBRSetPacingRate(picoquic_bbr_state_t* bbr_state)
{
    BBRSetPacingRateWithGain(bbr_state, bbr_state->pacing_gain);
}

/* TODO: clarity on bytes vs packets  */
void BBRModulateCwndForRecovery(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x,
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
    if (bbr_state->packet_conservation) {
        if (path_x->cwin < bytes_in_transit + bytes_delivered) {
            path_x->cwin = bytes_in_transit + bytes_delivered;
        }
    }
}

void BBRModulateCwndForProbeRTT(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    if (bbr_state->state == picoquic_bbr_alg_probe_rtt)
    {
        if (path_x->cwin > BBR_MIN_PIPE_CWND(path_x->send_mtu)) {
            path_x->cwin = BBR_MIN_PIPE_CWND(path_x->send_mtu);
        }
    }
}

void BBRSetCwnd(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered)
{
    BBRUpdateTargetCwnd(bbr_state);
    BBRModulateCwndForRecovery(bbr_state, path_x, bytes_in_transit, packets_lost, bytes_delivered);
    if (!bbr_state->packet_conservation) {
        if (bbr_state->filled_pipe) {
            path_x->cwin += bytes_delivered;
            if (path_x->cwin > bbr_state->target_cwnd) {
                path_x->cwin = bbr_state->target_cwnd;
            }
        }
        else if (path_x->cwin < bbr_state->target_cwnd || path_x->delivered < PICOQUIC_CWIN_INITIAL)
        {
            path_x->cwin += bytes_delivered;
            if (path_x->cwin < BBR_MIN_PIPE_CWND(path_x->send_mtu))
            {
                path_x->cwin = BBR_MIN_PIPE_CWND(path_x->send_mtu);
            }
        }
    }

    BBRModulateCwndForProbeRTT(bbr_state, path_x);
}


void BBRUpdateControlParameters(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered)
{
    BBRSetPacingRate(bbr_state);
    BBRSetSendQuantum(bbr_state, path_x);
    BBRSetCwnd(bbr_state, path_x, bytes_in_transit, packets_lost, bytes_delivered);
}

void BBRHandleRestartFromIdle(picoquic_bbr_state_t* bbr_state, uint64_t bytes_in_transit, int is_app_limited)
{
    if (bytes_in_transit == 0 && is_app_limited)
    {
        bbr_state->idle_restart = 1;
        if (bbr_state->state == picoquic_bbr_alg_probe_bw) {
            BBRSetPacingRateWithGain(bbr_state, 1.0);
        }
    }
}


/* This is the per ACK processing, activated upon receiving an ACK.
 * At that point, we expect the following:
 *  - delivered has been updated to reflect all the data acked on the path.
 *  - the delivery rate sample has been computed.
 */

void  BBRUpdateOnACK(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x,
                     uint64_t rtt_sample, uint64_t bytes_in_transit, uint64_t packets_lost, uint64_t bytes_delivered,
                     uint64_t current_time)
{
    BBRUpdateModelAndState(bbr_state, path_x, rtt_sample, bytes_in_transit,
                           packets_lost, current_time);
    BBRUpdateControlParameters(bbr_state, path_x, bytes_in_transit, packets_lost, bytes_delivered);
}

void BBROnTransmit(picoquic_bbr_state_t* bbr_state, uint64_t bytes_in_transit, int is_app_limited)
{
    BBRHandleRestartFromIdle(bbr_state, bytes_in_transit, is_app_limited);
}

/* Dealing with recovery. What happens when all
 * the packets are lost, when all packets have been retransmitted.. */

void BBROnAllPacketsLost(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    bbr_state->prior_cwnd = BBRSaveCwnd(bbr_state, path_x);
    path_x->cwin = path_x->send_mtu;
}

void BBROnEnterFastRecovery(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x, uint64_t bytes_in_transit, uint64_t bytes_delivered )
{
    if (bytes_delivered < path_x->send_mtu) {
        bytes_delivered = path_x->send_mtu;
    }
    bbr_state->prior_cwnd = BBRSaveCwnd(bbr_state, path_x);
    path_x->cwin = bytes_in_transit + bytes_delivered;
    bbr_state->packet_conservation = 1;
}

void BBRAfterOneRoundtripInFastRecovery(picoquic_bbr_state_t* bbr_state)
{
    bbr_state->packet_conservation = 0;
}

void BBRExitFastRecovery(picoquic_bbr_state_t* bbr_state, picoquic_path_t* path_x)
{
    bbr_state->packet_conservation = 0;
    BBRRestoreCwnd(bbr_state, path_x);
}

/*
 * In order to implement BBR, we map generic congestion notification
 * signals to the corresponding BBR actions.
 */
static void picoquic_bbr_notify(
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
    picoquic_bbr_state_t* bbr_state = (picoquic_bbr_state_t*)path_x->congestion_alg_state;

    if (bbr_state != NULL) {
        if (bbr_state->state == picoquic_bbr_alg_probe_bw) {//如果当前处于带宽探测阶段，则打印相关信息
            printf("BBR BW probing\n");
            //DBG_PRINTF("BBR BW Probing");
        }
        switch (notification) {//根据传入的notification决定需要完成的操作
            case picoquic_congestion_notification_acknowledgement://如果是acknowledgement信号，则根据传入的ack数量更新bbr中已发送数据量
                /* sum the amount of data acked per packet */
                bbr_state->bytes_delivered += nb_bytes_acknowledged;
                break;
            case picoquic_congestion_notification_repeat://如果是repeat信号，则除了在带宽探测阶段打印信息外不作任何处理
                break;
            case picoquic_congestion_notification_timeout://如果是timeout信号，则除了在带宽探测阶段打印信息外不作任何处理
                /* enter recovery */
                break;
            case picoquic_congestion_notification_spurious_repeat://如果是spurious_repeat信号，则除了在带宽探测阶段打印信息外不作任何处理
                break;
            case picoquic_congestion_notification_rtt_measurement://如果是RTT测量信号
                if (bbr_state->state == picoquic_bbr_alg_startup && path_x->smoothed_rtt > PICOQUIC_TARGET_RENO_RTT) {
                    BBREnterStartupLongRTT(bbr_state, path_x);//这种情况适用于慢启动阶段时
                }
                if (bbr_state->state == picoquic_bbr_alg_startup_long_rtt) {
                    if (picoquic_hystart_test(&bbr_state->rtt_filter, rtt_measurement, path_x->pacing_packet_time_microsec, current_time, false)) {
                        BBRExitStartupLongRtt(bbr_state, path_x, current_time);
                    }
                }
                break;
            case picoquic_congestion_notification_bw_measurement://如果是Bw测量信号
                /* RTT measurements will happen after the bandwidth is estimated */
                if (bbr_state->state == picoquic_bbr_alg_startup_long_rtt) {
                    BBRUpdateBtlBw(bbr_state, path_x);
                    if (rtt_measurement <= bbr_state->rt_prop) {
                        bbr_state->rt_prop = rtt_measurement;
                        bbr_state->rt_prop_stamp = current_time;
                    }
                    if (picoquic_cc_was_cwin_blocked(path_x, bbr_state->last_sequence_blocked)) {
                        picoquic_hystart_increase(path_x, &bbr_state->rtt_filter, bbr_state->bytes_delivered);
                    }
                    bbr_state->bytes_delivered = 0;

                    picoquic_update_pacing_data(path_x);
                } else {
                    BBRUpdateOnACK(bbr_state, path_x,
                                   rtt_measurement, path_x->bytes_in_transit, 0 /* packets_lost */, bbr_state->bytes_delivered,
                                   current_time);
                    /* Remember the number in flight before the next ACK -- TODO: update after send instead. */
                    bbr_state->prior_in_flight = path_x->bytes_in_transit;
                    /* Reset the number of bytes delivered */
                    bbr_state->bytes_delivered = 0;

                    if (bbr_state->pacing_rate > 0) {
                        /* Set the pacing rate in picoquic sender */
                        picoquic_update_pacing_rate(path_x, bbr_state->pacing_rate, bbr_state->send_quantum);
                    }
                }
                break;
            case picoquic_congestion_notification_cwin_blocked:
                bbr_state->last_sequence_blocked = picoquic_cc_get_sequence_number(path_x);
            default:
                /* ignore */
                break;
        }
    }
}

#define picoquic_bbr_ID 0x42424F00 /* BBR */

picoquic_congestion_algorithm_t picoquic_bbr_algorithm_struct = {
        picoquic_bbr_ID,
        picoquic_bbr_init,
        picoquic_bbr_notify,
        picoquic_bbr_delete
};

/*这一部分指定cc算法，包含三个函数接口，供上层选择与调用*/
picoquic_congestion_algorithm_t* picoquic_bbr_algorithm = &picoquic_bbr_algorithm_struct;