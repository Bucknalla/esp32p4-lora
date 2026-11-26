#include "lwc/mac.h"

#include <string.h>

/* Payload bytes after the CID for each uplink command (LoRaWAN 1.0.4,
 * class A and B); -1: not an uplink command in 1.0.x. */
static int up_len(uint8_t cid)
{
    switch (cid) {
    case LWC_MAC_LINK_CHECK: return 0;
    case LWC_MAC_LINK_ADR: return 1;
    case LWC_MAC_DUTY_CYCLE: return 0;
    case LWC_MAC_RX_PARAM_SETUP: return 1;
    case LWC_MAC_DEV_STATUS: return 2;
    case LWC_MAC_NEW_CHANNEL: return 1;
    case LWC_MAC_RX_TIMING_SETUP: return 0;
    case LWC_MAC_TX_PARAM_SETUP: return 0;
    case LWC_MAC_DL_CHANNEL: return 1;
    case LWC_MAC_DEVICE_TIME: return 0;
    case 0x10: return 1; /* PingSlotInfoReq */
    case 0x11: return 1; /* PingSlotChannelAns */
    case 0x12: return 0; /* BeaconTimingReq (deprecated) */
    case 0x13: return 1; /* BeaconFreqAns */
    default: return -1;
    }
}

size_t lwc_mac_parse_up(const uint8_t *p, size_t n, lwc_mac_up_t *out)
{
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    out->link_adr_status = LWC_MAC_ADR_ALL_ACK;
    if (p == NULL) {
        return 0;
    }
    size_t count = 0, i = 0;
    while (i < n) {
        const uint8_t cid = p[i];
        const int l = up_len(cid);
        if (l < 0 || i + 1U + (size_t)l > n) {
            out->stopped = true;
            break;
        }
        const uint8_t *a = &p[i + 1U];
        switch (cid) {
        case LWC_MAC_LINK_CHECK:
            out->link_check_req = true;
            break;
        case LWC_MAC_DEVICE_TIME:
            out->device_time_req = true;
            break;
        case LWC_MAC_LINK_ADR:
            if (out->link_adr_ans < UINT8_MAX) {
                out->link_adr_ans++;
            }
            out->link_adr_status &= (uint8_t)(a[0] & LWC_MAC_ADR_ALL_ACK);
            break;
        case LWC_MAC_TX_PARAM_SETUP:
            out->tx_param_setup_ans = true;
            break;
        case LWC_MAC_DEV_STATUS: {
            out->dev_status_ans = true;
            out->battery = a[0];
            const uint8_t m = (uint8_t)(a[1] & 0x3FU); /* 6-bit two's complement */
            out->margin = (int8_t)(m >= 32U ? (int)m - 64 : (int)m);
            break;
        }
        default:
            if (out->other < UINT8_MAX) {
                out->other++;
            }
            break;
        }
        i += 1U + (size_t)l;
        count++;
    }
    if (out->link_adr_ans == 0U) {
        out->link_adr_status = 0;
    }
    return count;
}

static bool put(uint8_t *buf, size_t cap, size_t *len, const uint8_t *cmd, size_t n)
{
    if (buf == NULL || len == NULL || *len > cap || cap - *len < n) {
        return false;
    }
    memcpy(&buf[*len], cmd, n);
    *len += n;
    return true;
}

bool lwc_mac_link_check_ans(uint8_t *buf, size_t cap, size_t *len, uint8_t margin_db, uint8_t gw_cnt)
{
    const uint8_t c[3] = {LWC_MAC_LINK_CHECK, margin_db > 254U ? 254U : margin_db, gw_cnt};
    return put(buf, cap, len, c, sizeof c);
}

bool lwc_mac_device_time_ans(uint8_t *buf, size_t cap, size_t *len, uint32_t gps_s, uint8_t frac)
{
    const uint8_t c[6] = {LWC_MAC_DEVICE_TIME, (uint8_t)gps_s, (uint8_t)(gps_s >> 8), (uint8_t)(gps_s >> 16),
                          (uint8_t)(gps_s >> 24), frac};
    return put(buf, cap, len, c, sizeof c);
}

bool lwc_mac_link_adr_req(uint8_t *buf, size_t cap, size_t *len, uint8_t dr, uint8_t tx_power, uint16_t ch_mask,
                          uint8_t ch_mask_cntl, uint8_t nb_trans)
{
    if (dr > 15U || tx_power > 15U || ch_mask_cntl > 7U || nb_trans > 15U) {
        return false;
    }
    const uint8_t c[5] = {LWC_MAC_LINK_ADR, (uint8_t)((dr << 4) | tx_power), (uint8_t)ch_mask,
                          (uint8_t)(ch_mask >> 8), (uint8_t)((ch_mask_cntl << 4) | nb_trans)};
    return put(buf, cap, len, c, sizeof c);
}

bool lwc_mac_dev_status_req(uint8_t *buf, size_t cap, size_t *len)
{
    const uint8_t c[1] = {LWC_MAC_DEV_STATUS};
    return put(buf, cap, len, c, sizeof c);
}

static const uint8_t k_max_eirp[16] = {8, 10, 12, 13, 14, 16, 18, 20, 21, 24, 26, 27, 29, 30, 33, 36};

int lwc_mac_max_eirp_dbm(uint8_t idx)
{
    return idx < 16U ? k_max_eirp[idx] : -1;
}

int lwc_mac_max_eirp_idx(int dbm)
{
    int best = -1;
    for (int i = 0; i < 16; i++) {
        if (k_max_eirp[i] <= dbm) {
            best = i;
        }
    }
    return best;
}

bool lwc_mac_tx_param_setup_req(uint8_t *buf, size_t cap, size_t *len, bool dl_dwell, bool ul_dwell,
                                uint8_t max_eirp_idx)
{
    if (max_eirp_idx > 15U) {
        return false;
    }
    const uint8_t c[2] = {LWC_MAC_TX_PARAM_SETUP,
                          (uint8_t)((dl_dwell ? 0x20U : 0U) | (ul_dwell ? 0x10U : 0U) | max_eirp_idx)};
    return put(buf, cap, len, c, sizeof c);
}

bool lwc_gps_from_unix_us(int64_t unix_us, uint32_t *gps_s, uint8_t *frac)
{
    if (gps_s == NULL || frac == NULL || unix_us < 1483228800LL * 1000000LL) { /* 2017-01-01 */
        return false;
    }
    const int64_t s = unix_us / 1000000LL;
    const int64_t us = unix_us % 1000000LL;
    *gps_s = (uint32_t)(s - LWC_GPS_EPOCH_UNIX + LWC_GPS_LEAP_S);
    *frac = (uint8_t)((us * 256LL) / 1000000LL);
    return true;
}
