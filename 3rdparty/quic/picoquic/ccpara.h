#ifndef CCPARA_H
#define CCPARA_H

#include <stdint.h>

typedef struct cc_info_timestamp_s {
    uint64_t* timestamp;
    int             len;
    int             cap;
} cc_info_timestamp_t;

typedef struct cc_info_pacingrate_s {
    uint32_t* pacingrate;
    int             len;
    int             cap;
} cc_info_pacingrate_t;

typedef struct cc_info_cwndgain_s {
    float* cwndgain;
    int             len;
    int             cap;
} cc_info_cwndgain_t;

typedef struct cc_info_cwnd_s {
    uint32_t* cwnd;
    int             len;
    int             cap;
} cc_info_cwnd_t;

typedef struct cc_info_inflight_s {
    uint32_t* inflight;
    int             len;
    int             cap;
} cc_info_inflight_t;

typedef struct cc_info_minrtt_s {
    uint64_t* minrtt;
    int             len;
    int             cap;
} cc_info_minrtt_t;

typedef struct cc_info_bandwidth_s {
    uint32_t* bandwidth;
    int             len;
    int             cap;
} cc_info_bandwidth_t;

typedef struct cc_info_loss_s {
    uint32_t* loss;
    int             len;
    int             cap;
} cc_info_loss_t;

typedef struct cc_info_oversent_s {
    uint32_t* oversent;
    int             len;
    int             cap;
} cc_info_oversent_t;

cc_info_timestamp_t* cc_init_timestamp();

cc_info_pacingrate_t* cc_init_pacingrate();

cc_info_cwndgain_t* cc_init_cwndgain();

cc_info_cwnd_t* cc_init_cwnd();

cc_info_inflight_t* cc_init_inflight();

cc_info_minrtt_t* cc_init_minrtt();

cc_info_bandwidth_t* cc_init_bandwidth();

cc_info_loss_t* cc_init_loss();

cc_info_oversent_t* cc_init_oversent();

void cc_add_timestamp(cc_info_timestamp_t* cc_timestamp, uint64_t value);

void cc_add_pacingrate(cc_info_pacingrate_t* cc_pacingrate, uint32_t value);

void cc_add_cwndgain(cc_info_cwndgain_t* cc_cwndgain, float value);

void cc_add_cwnd(cc_info_cwnd_t* cc_cwnd, uint32_t value);

void cc_add_inflight(cc_info_inflight_t* cc_inflight, uint32_t value);

void cc_add_minrtt(cc_info_minrtt_t* cc_minrtt, uint64_t value);

void cc_add_bandwidth(cc_info_bandwidth_t* cc_bandwidth, uint32_t value);

void cc_add_loss(cc_info_loss_t* cc_loss, uint32_t value);

void cc_add_oversent(cc_info_oversent_t* cc_oversent, uint32_t value);

void cc_free_timestamp(cc_info_timestamp_t* cc_timestamp);

void cc_free_pacingrate(cc_info_pacingrate_t* cc_pacingrate);

void cc_free_cwndgain(cc_info_cwndgain_t* cc_cwndgain);

void cc_free_cwnd(cc_info_cwnd_t* cc_cwnd);

void cc_free_inflight(cc_info_inflight_t* cc_inflight);

void cc_free_minrtt(cc_info_minrtt_t* cc_minrtt);

void cc_reset_minrtt(cc_info_minrtt_t* cc_minrtt);

void cc_free_bandwidth(cc_info_bandwidth_t* cc_bandwidth);

void cc_free_loss(cc_info_loss_t* cc_loss);

void cc_free_oversent(cc_info_oversent_t* cc_oversent);

#endif