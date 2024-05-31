#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ccpara.h"

cc_info_timestamp_t*
cc_init_timestamp()
{
    cc_info_timestamp_t* cc_timestamp;
    cc_timestamp = (cc_info_timestamp_t*)malloc(sizeof(cc_info_timestamp_t));
    if (cc_timestamp == NULL) {
        return NULL;
    }
    cc_timestamp->timestamp = (uint64_t*)malloc(6561 * sizeof(uint64_t));
    memset(cc_timestamp->timestamp, 0, 6561 * sizeof(uint64_t));
    if (cc_timestamp->timestamp == NULL) {
        return NULL;
    }
    cc_timestamp->len = 0;
    cc_timestamp->cap = 6561;
    return cc_timestamp;
}

cc_info_pacingrate_t*
cc_init_pacingrate()
{
    cc_info_pacingrate_t* cc_pacingrate;
    cc_pacingrate = (cc_info_pacingrate_t*)malloc(sizeof(cc_info_pacingrate_t));
    if (cc_pacingrate == NULL) {
        return NULL;
    }
    cc_pacingrate->pacingrate = (uint32_t*)malloc(6561 * sizeof(uint32_t));
    memset(cc_pacingrate->pacingrate, 0, 6561 * sizeof(uint32_t));
    if (cc_pacingrate->pacingrate == NULL) {
        return NULL;
    }
    cc_pacingrate->len = 0;
    cc_pacingrate->cap = 6561;
    return cc_pacingrate;
}

cc_info_cwndgain_t*
cc_init_cwndgain()
{
    cc_info_cwndgain_t* cc_cwndgain;
    cc_cwndgain = (cc_info_cwndgain_t*)malloc(sizeof(cc_info_cwndgain_t));
    if (cc_cwndgain == NULL) {
        return NULL;
    }
    cc_cwndgain->cwndgain = (float*)malloc(6561 * sizeof(float));
    memset(cc_cwndgain->cwndgain, 0, 6561 * sizeof(float));
    if (cc_cwndgain->cwndgain == NULL) {
        return NULL;
    }
    cc_cwndgain->len = 0;
    cc_cwndgain->cap = 6561;
    return cc_cwndgain;
}

cc_info_cwnd_t*
cc_init_cwnd()
{
    cc_info_cwnd_t* cc_cwnd;
    cc_cwnd = (cc_info_cwnd_t*)malloc(sizeof(cc_info_cwnd_t));
    if (cc_cwnd == NULL) {
        return NULL;
    }
    cc_cwnd->cwnd = (uint32_t*)malloc(6561 * sizeof(uint32_t));
    memset(cc_cwnd->cwnd, 0, 6561 * sizeof(uint32_t));
    if (cc_cwnd->cwnd == NULL) {
        return NULL;
    }
    cc_cwnd->len = 0;
    cc_cwnd->cap = 6561;
    return cc_cwnd;
}

cc_info_inflight_t*
cc_init_inflight()
{
    cc_info_inflight_t* cc_inflight;
    cc_inflight = (cc_info_inflight_t*)malloc(sizeof(cc_info_inflight_t));
    if (cc_inflight == NULL) {
        return NULL;
    }
    cc_inflight->inflight = (uint32_t*)malloc(6561 * sizeof(uint32_t));
    memset(cc_inflight->inflight, 0, 6561 * sizeof(uint32_t));
    if (cc_inflight->inflight == NULL) {
        return NULL;
    }
    cc_inflight->len = 0;
    cc_inflight->cap = 6561;
    return cc_inflight;
}

cc_info_minrtt_t*
cc_init_minrtt()
{
    cc_info_minrtt_t* cc_minrtt;
    cc_minrtt = (cc_info_minrtt_t*)malloc(sizeof(cc_info_minrtt_t));
    if (cc_minrtt == NULL) {
        return NULL;
    }
    cc_minrtt->minrtt = (uint64_t*)malloc(6561 * sizeof(uint64_t));
    memset(cc_minrtt->minrtt, 0, 6561 * sizeof(uint64_t));
    if (cc_minrtt->minrtt == NULL) {
        return NULL;
    }
    cc_minrtt->len = 0;
    cc_minrtt->cap = 6561;
    return cc_minrtt;
}

cc_info_bandwidth_t*
cc_init_bandwidth()
{
    cc_info_bandwidth_t* cc_bandwidth;
    cc_bandwidth = (cc_info_bandwidth_t*)malloc(sizeof(cc_info_bandwidth_t));
    if (cc_bandwidth == NULL) {
        return NULL;
    }
    cc_bandwidth->bandwidth = (uint32_t*)malloc(6561 * sizeof(uint32_t));
    memset(cc_bandwidth->bandwidth, 0, 6561 * sizeof(uint32_t));
    if (cc_bandwidth->bandwidth == NULL) {
        return NULL;
    }
    cc_bandwidth->len = 0;
    cc_bandwidth->cap = 6561;
    return cc_bandwidth;
}

cc_info_loss_t*
cc_init_loss()
{
    cc_info_loss_t* cc_loss;
    cc_loss = (cc_info_loss_t*)malloc(sizeof(cc_info_loss_t));
    if (cc_loss == NULL) {
        return NULL;
    }
    cc_loss->loss = (uint32_t*)malloc(6561 * sizeof(uint32_t));
    memset(cc_loss->loss, 0, 6561 * sizeof(uint32_t));
    if (cc_loss->loss == NULL) {
        return NULL;
    }
    cc_loss->len = 0;
    cc_loss->cap = 6561;
    return cc_loss;
}

cc_info_oversent_t*
cc_init_oversent()
{
    cc_info_oversent_t* cc_oversent;
    cc_oversent = (cc_info_oversent_t*)malloc(sizeof(cc_info_oversent_t));
    if (cc_oversent == NULL) {
        return NULL;
    }
    cc_oversent->oversent = (uint32_t*)malloc(6561 * sizeof(uint32_t));
    memset(cc_oversent->oversent, 0, 6561 * sizeof(uint32_t));
    if (cc_oversent->oversent == NULL) {
        return NULL;
    }
    cc_oversent->len = 0;
    cc_oversent->cap = 6561;
    return cc_oversent;
}

void
cc_add_timestamp(cc_info_timestamp_t* cc_timestamp, uint64_t value)
{
    if (cc_timestamp->len >= cc_timestamp->cap) {
        int cap = cc_timestamp->cap;
        uint64_t* timestamp_tmp = (uint64_t*)realloc(cc_timestamp->timestamp, (2 * cap) * sizeof(uint64_t));
        if (timestamp_tmp == NULL) {
            free(timestamp_tmp);
            printf("realloc timestamp failed!");
            exit(EXIT_FAILURE);
        }
        cc_timestamp->timestamp = timestamp_tmp;
        memset(cc_timestamp->timestamp + cc_timestamp->len, 0, (2 * cap - cc_timestamp->len) * sizeof(uint64_t));//新部分初始化
        cc_timestamp->cap = 2 * cap;
        cc_timestamp->timestamp[cc_timestamp->len] = value;
        cc_timestamp->len++;
    }
    cc_timestamp->timestamp[cc_timestamp->len] = value;
    cc_timestamp->len++;
}

void
cc_add_pacingrate(cc_info_pacingrate_t* cc_pacingrate, uint32_t value)
{
    if (cc_pacingrate->len >= cc_pacingrate->cap) {
        int cap = cc_pacingrate->cap;
        uint32_t* pacingrate_tmp = (uint32_t*)realloc(cc_pacingrate->pacingrate, (2 * cap) * sizeof(uint32_t));
        if (pacingrate_tmp == NULL) {
            free(pacingrate_tmp);
            printf("realloc pacingrate failed!");
            exit(EXIT_FAILURE);
        }
        cc_pacingrate->pacingrate = pacingrate_tmp;
        memset(cc_pacingrate->pacingrate + cc_pacingrate->len, 0, (2 * cap - cc_pacingrate->len) * sizeof(uint32_t));
        cc_pacingrate->cap = 2 * cap;
        cc_pacingrate->pacingrate[cc_pacingrate->len] = value;
        cc_pacingrate->len++;
    }
    cc_pacingrate->pacingrate[cc_pacingrate->len] = value;
    cc_pacingrate->len++;
}

void
cc_add_cwndgain(cc_info_cwndgain_t* cc_cwndgain, float value)
{
    if (cc_cwndgain->len >= cc_cwndgain->cap) {
        int cap = cc_cwndgain->cap;
        float* cwndgain_tmp = (float*)realloc(cc_cwndgain->cwndgain, (2 * cap) * sizeof(float));
        if (cwndgain_tmp == NULL) {
            free(cwndgain_tmp);
            printf("realloc cwndgain failed!");
            exit(EXIT_FAILURE);
        }
        cc_cwndgain->cwndgain = cwndgain_tmp;
        memset(cc_cwndgain->cwndgain + cc_cwndgain->len, 0, (2 * cap - cc_cwndgain->len) * sizeof(float));
        cc_cwndgain->cap = 2 * cap;
        cc_cwndgain->cwndgain[cc_cwndgain->len] = value;
        cc_cwndgain->len++;
    }
    cc_cwndgain->cwndgain[cc_cwndgain->len] = value;
    cc_cwndgain->len++;
}

void
cc_add_cwnd(cc_info_cwnd_t* cc_cwnd, uint32_t value)
{
    if (cc_cwnd->len >= cc_cwnd->cap) {
        int cap = cc_cwnd->cap;
        uint32_t* cwnd_tmp = (uint32_t*)realloc(cc_cwnd->cwnd, (2 * cap) * sizeof(uint32_t));
        if (cwnd_tmp == NULL) {
            free(cwnd_tmp);
            printf("realloc cwnd failed!");
            exit(EXIT_FAILURE);
        }
        cc_cwnd->cwnd = cwnd_tmp;
        memset(cc_cwnd->cwnd + cc_cwnd->len, 0, (2 * cap - cc_cwnd->len) * sizeof(uint32_t));
        cc_cwnd->cap = 2 * cap;
        cc_cwnd->cwnd[cc_cwnd->len] = value;
        cc_cwnd->len++;
    }
    cc_cwnd->cwnd[cc_cwnd->len] = value;
    cc_cwnd->len++;
}

void
cc_add_inflight(cc_info_inflight_t* cc_inflight, uint32_t value)
{
    if (cc_inflight->len >= cc_inflight->cap) {
        int cap = cc_inflight->cap;
        uint32_t* inflight_tmp = (uint32_t*)realloc(cc_inflight->inflight, (2 * cap) * sizeof(uint32_t));
        if (inflight_tmp == NULL) {
            free(inflight_tmp);
            printf("realloc inflight failed!");
            exit(EXIT_FAILURE);
        }
        cc_inflight->inflight = inflight_tmp;
        memset(cc_inflight->inflight + cc_inflight->len, 0, (2 * cap - cc_inflight->len) * sizeof(uint32_t));
        cc_inflight->cap = 2 * cap;
        cc_inflight->inflight[cc_inflight->len] = value;
        cc_inflight->len++;
    }
    cc_inflight->inflight[cc_inflight->len] = value;
    cc_inflight->len++;
}

void
cc_add_minrtt(cc_info_minrtt_t* cc_minrtt, uint64_t value)
{
    if (cc_minrtt->len >= cc_minrtt->cap) {
        int cap = cc_minrtt->cap;
        uint64_t* minrtt_tmp = (uint64_t*)realloc(cc_minrtt->minrtt, (2 * cap) * sizeof(uint64_t));
        if (minrtt_tmp == NULL) {
            free(minrtt_tmp);
            printf("realloc minrtt failed!");
            exit(EXIT_FAILURE);
        }
        cc_minrtt->minrtt = minrtt_tmp;
        memset(cc_minrtt->minrtt + cc_minrtt->len, 0, (2 * cap - cc_minrtt->len) * sizeof(uint64_t));
        cc_minrtt->cap = 2 * cap;
        cc_minrtt->minrtt[cc_minrtt->len] = value;
        cc_minrtt->len++;
    }
    cc_minrtt->minrtt[cc_minrtt->len] = value;
    cc_minrtt->len++;
}

void
cc_add_bandwidth(cc_info_bandwidth_t* cc_bandwidth, uint32_t value)
{
    if (cc_bandwidth->len >= cc_bandwidth->cap) {
        int cap = cc_bandwidth->cap;
        uint32_t* bandwidth_tmp = (uint32_t*)realloc(cc_bandwidth->bandwidth, (2 * cap) * sizeof(uint32_t));
        if (bandwidth_tmp == NULL) {
            free(bandwidth_tmp);
            printf("realloc bandwidth failed!");
            exit(EXIT_FAILURE);
        }
        cc_bandwidth->bandwidth = bandwidth_tmp;
        memset(cc_bandwidth->bandwidth + cc_bandwidth->len, 0, (2 * cap - cc_bandwidth->len) * sizeof(uint32_t));
        cc_bandwidth->cap = 2 * cap;
        cc_bandwidth->bandwidth[cc_bandwidth->len] = value;
        cc_bandwidth->len++;
    }
    cc_bandwidth->bandwidth[cc_bandwidth->len] = value;
    cc_bandwidth->len++;
}

void
cc_add_loss(cc_info_loss_t* cc_loss, uint32_t value)
{
    if (cc_loss->len >= cc_loss->cap) {
        int cap = cc_loss->cap;
        uint32_t* loss_tmp = (uint32_t*)realloc(cc_loss->loss, (2 * cap) * sizeof(uint32_t));
        if (loss_tmp == NULL) {
            free(loss_tmp);
            printf("realloc loss failed!");
            exit(EXIT_FAILURE);
        }
        cc_loss->loss = loss_tmp;
        memset(cc_loss->loss + cc_loss->len, 0, (2 * cap - cc_loss->len) * sizeof(uint32_t));
        cc_loss->cap = 2 * cap;
        cc_loss->loss[cc_loss->len] = value;
        cc_loss->len++;
    }
    cc_loss->loss[cc_loss->len] = value;
    cc_loss->len++;
}

void
cc_add_oversent(cc_info_oversent_t* cc_oversent, uint32_t value)
{
    if (cc_oversent->len >= cc_oversent->cap) {
        int cap = cc_oversent->cap;
        uint32_t* oversent_tmp = (uint32_t*)realloc(cc_oversent->oversent, (2 * cap) * sizeof(uint32_t));
        if (oversent_tmp == NULL) {
            free(oversent_tmp);
            printf("realloc oversent failed!");
            exit(EXIT_FAILURE);
        }
        cc_oversent->oversent = oversent_tmp;
        memset(cc_oversent->oversent + cc_oversent->len, 0, (2 * cap - cc_oversent->len) * sizeof(uint32_t));
        cc_oversent->cap = 2 * cap;
        cc_oversent->oversent[cc_oversent->len] = value;
        cc_oversent->len++;
    }
    cc_oversent->oversent[cc_oversent->len] = value;
    cc_oversent->len++;
}

uint64_t*
cc_get_timestamp(cc_info_timestamp_t* cc_timestamp)
{
    if (cc_timestamp == NULL && cc_timestamp->timestamp == NULL) {
        return NULL;
    }
    return cc_timestamp->timestamp;
}

int
cc_get_lenoftimestamp(cc_info_timestamp_t* cc_timestamp)
{
    if (cc_timestamp == NULL && cc_timestamp->len == 0) {
        return 0;
    }
    return cc_timestamp->len;
}

uint32_t*
cc_get_pacingrate(cc_info_pacingrate_t* cc_pacingrate)
{
    if (cc_pacingrate == NULL && cc_pacingrate->pacingrate == NULL) {
        return NULL;
    }
    return cc_pacingrate->pacingrate;
}

int
cc_get_lenofpacingrate(cc_info_pacingrate_t* cc_pacingrate)
{
    if (cc_pacingrate == NULL && cc_pacingrate->len == 0) {
        return 0;
    }
    return cc_pacingrate->len;
}

float*
cc_get_cwndgain(cc_info_cwndgain_t* cc_cwndgain)
{
    if (cc_cwndgain == NULL && cc_cwndgain->cwndgain == NULL) {
        return NULL;
    }
    return cc_cwndgain->cwndgain;
}

int
cc_get_lenofcwndgain(cc_info_cwndgain_t* cc_cwndgain)
{
    if (cc_cwndgain == NULL && cc_cwndgain->len == 0) {
        return 0;
    }
    return cc_cwndgain->len;
}

uint32_t*
cc_get_cwnd(cc_info_cwnd_t* cc_cwnd)
{
    if (cc_cwnd == NULL && cc_cwnd->cwnd == NULL) {
        return NULL;
    }
    return cc_cwnd->cwnd;
}

int
cc_get_lenofcwnd(cc_info_cwnd_t* cc_cwnd)
{
    if (cc_cwnd == NULL && cc_cwnd->len == 0) {
        return 0;
    }
    return cc_cwnd->len;
}

uint32_t*
cc_get_inflight(cc_info_inflight_t* cc_inflight)
{
    if (cc_inflight == NULL && cc_inflight->inflight == NULL) {
        return NULL;
    }
    return cc_inflight->inflight;
}

int
cc_get_lenofinflight(cc_info_inflight_t* cc_inflight)
{
    if (cc_inflight == NULL && cc_inflight->len == 0) {
        return 0;
    }
    return cc_inflight->len;
}

uint64_t*
cc_get_minrtt(cc_info_minrtt_t* cc_minrtt)
{
    if (cc_minrtt == NULL && cc_minrtt->minrtt == NULL) {
        return NULL;
    }
    return cc_minrtt->minrtt;
}

int
cc_get_lenofminrtt(cc_info_minrtt_t* cc_minrtt)
{
    if (cc_minrtt == NULL && cc_minrtt->len == 0) {
        return 0;
    }
    return cc_minrtt->len;
}

uint32_t*
cc_get_bandwidth(cc_info_bandwidth_t* cc_bandwidth)
{
    if (cc_bandwidth == NULL && cc_bandwidth->bandwidth == NULL) {
        return NULL;
    }
    return cc_bandwidth->bandwidth;
}

int
cc_get_lenofbandwidth(cc_info_bandwidth_t* cc_bandwidth)
{
    if (cc_bandwidth == NULL && cc_bandwidth->len == 0) {
        return 0;
    }
    return cc_bandwidth->len;
}

uint32_t*
cc_get_loss(cc_info_loss_t* cc_loss)
{
    if (cc_loss == NULL && cc_loss->loss == NULL) {
        return NULL;
    }
    return cc_loss->loss;
}

int
cc_get_lenofloss(cc_info_loss_t* cc_loss)
{
    if (cc_loss == NULL && cc_loss->len == 0) {
        return 0;
    }
    return cc_loss->len;
}

uint32_t*
cc_get_oversent(cc_info_oversent_t* cc_oversent)
{
    if (cc_oversent == NULL && cc_oversent->oversent == NULL) {
        return NULL;
    }
    return cc_oversent->oversent;
}

int
cc_get_lenofoversent(cc_info_oversent_t* cc_oversent)
{
    if (cc_oversent == NULL && cc_oversent->len == 0) {
        return 0;
    }
    return cc_oversent->len;
}

void
cc_free_timestamp(cc_info_timestamp_t* cc_timestamp)
{
    free(cc_timestamp->timestamp);
    cc_timestamp->timestamp = NULL;
    cc_timestamp->len = 0;
    cc_timestamp->cap = 0;
}

void
cc_free_pacingrate(cc_info_pacingrate_t* cc_pacingrate)
{
    free(cc_pacingrate->pacingrate);
    cc_pacingrate->pacingrate = NULL;
    cc_pacingrate->len = 0;
    cc_pacingrate->cap = 0;
}

void
cc_free_cwndgain(cc_info_cwndgain_t* cc_cwndgain)
{
    free(cc_cwndgain->cwndgain);
    cc_cwndgain->cwndgain = NULL;
    cc_cwndgain->len = 0;
    cc_cwndgain->cap = 0;
}

void
cc_free_cwnd(cc_info_cwnd_t* cc_cwnd)
{
    free(cc_cwnd->cwnd);
    cc_cwnd->cwnd = NULL;
    cc_cwnd->len = 0;
    cc_cwnd->cap = 0;
}

void
cc_free_inflight(cc_info_inflight_t* cc_inflight)
{
    free(cc_inflight->inflight);
    cc_inflight->inflight = NULL;
    cc_inflight->len = 0;
    cc_inflight->cap = 0;
}

void
cc_free_minrtt(cc_info_minrtt_t* cc_minrtt)
{
    memset(cc_minrtt->minrtt, 0, cc_minrtt->len * sizeof(uint64_t));
    cc_minrtt->len = 0;
}


void
cc_free_bandwidth(cc_info_bandwidth_t* cc_bandwidth)
{
    free(cc_bandwidth->bandwidth);
    cc_bandwidth->bandwidth = NULL;
    cc_bandwidth->len = 0;
    cc_bandwidth->cap = 0;
}

void
cc_free_loss(cc_info_loss_t* cc_loss)
{
    free(cc_loss->loss);
    cc_loss->loss = NULL;
    cc_loss->len = 0;
    cc_loss->cap = 0;
}

void
cc_free_oversent(cc_info_oversent_t* cc_oversent)
{
    free(cc_oversent->oversent);
    cc_oversent->oversent = NULL;
    cc_oversent->len = 0;
    cc_oversent->cap = 0;
}