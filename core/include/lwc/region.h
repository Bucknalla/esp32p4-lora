/*
 * LoRaWAN regional parameters (ADR-0023): RP002 tables per region, and the
 * rules that differ between regions as functions (the fixed-channel plans of
 * US915 and AU915 differ by rule, not only by value). Values come from
 * core/src/region_tables.inc, generated from a pinned reference
 * (tools/regions/). Pure, no I/O: host-tested.
 *
 * Duty cycle, dwell time and listen-before-talk depend on national rules as
 * well as RP002; they are applied elsewhere (EU868: core/src/txpk.c).
 */
#ifndef LWC_REGION_H
#define LWC_REGION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LWC_REGION_EU868 = 0,
    LWC_REGION_US915,
    LWC_REGION_AU915,
    LWC_REGION_AS923_1,
    LWC_REGION_AS923_2,
    LWC_REGION_AS923_3,
    LWC_REGION_AS923_4,
    LWC_REGION_IN865,
    LWC_REGION_KR920,
    LWC_REGION_RU864,
    LWC_REGIONS
} lwc_region_id_t;

typedef enum {
    LWC_DR_NONE = 0, /* RFU in this region */
    LWC_DR_LORA,
    LWC_DR_FSK,
    LWC_DR_LRFHSS,
} lwc_dr_kind_t;

typedef struct {
    uint8_t kind; /* lwc_dr_kind_t */
    uint8_t sf;   /* LoRa: 7..12 */
    uint16_t bw_khz;
    uint8_t n;       /* largest FRMPayload, repeater-compatible, no dwell limit */
    uint8_t n_dwell; /* the same with the 400 ms dwell limit */
} lwc_region_dr_t;

/* A regular channel grid of a fixed plan: channel i at base + i * step. */
typedef struct {
    uint32_t base_hz;
    uint32_t step_hz;
    uint8_t min_dr;
    uint8_t max_dr;
} lwc_region_grid_t;

typedef struct {
    uint32_t hz;
    uint8_t min_dr;
    uint8_t max_dr;
} lwc_region_ch_t;

typedef struct {
    lwc_region_id_t id;
    const char *name;
    int8_t max_eirp_dbm;   /* TXPower 0 */
    uint8_t tx_powers;     /* TXPower 0..tx_powers-1, 2 dB apart */
    uint32_t rx2_hz;       /* default RX2 */
    uint8_t rx2_dr;
    bool tx_param_setup;   /* TxParamSetupReq (dwell time) is defined */
    lwc_region_dr_t dr[16];
    int8_t rx1_dr[16][8];  /* [uplink DR][RX1DROffset]: the RX1 DR, -1 none */
    uint8_t rx1_dwell_min_dr; /* with the downlink dwell limit, RX1 DR is at least this (AS923: 2) */
    bool fixed;            /* 64 + 8 uplink channels, 8 downlink (US915, AU915) */
    lwc_region_grid_t up125, up500, down; /* fixed plans */
    uint8_t n_default;     /* the others: default channels */
    lwc_region_ch_t default_ch[3];
} lwc_region_t;

/** The region, or NULL for an unknown id. */
const lwc_region_t *lwc_region(lwc_region_id_t id);

/** "EU868", "US915", "AS923-1", ... (case-sensitive). */
bool lwc_region_parse(const char *name, lwc_region_id_t *out);

/** The LoRa DR of an SF and bandwidth, or -1. `downlink` matters in the fixed
 *  plans, where the downlink DRs (DR8-13) repeat uplink SF/BW pairs. */
int lwc_region_dr_of(const lwc_region_t *r, uint8_t sf, uint16_t bw_khz, bool downlink);

/** Largest FRMPayload at `dr` (no FOpts), 0 for an RFU or bad DR. */
size_t lwc_region_max_n(const lwc_region_t *r, int dr, bool dwell);

/** RX1 DR for an uplink DR and RX1DROffset, -1 if not defined. `dwell`: the
 *  device has the 400 ms downlink dwell limit (TxParamSetupReq). */
int lwc_region_rx1_dr(const lwc_region_t *r, int up_dr, uint8_t offset, bool dwell);

/** Fixed plans: the uplink channel index (0-63 at 125 kHz, 64-71 at
 *  500 kHz) of a frequency and bandwidth, or -1. Always -1 for the others. */
int lwc_region_channel(const lwc_region_t *r, uint32_t hz, uint16_t bw_khz);

/** RX1 frequency for an uplink: the same frequency, or in the fixed plans
 *  downlink channel (uplink channel % 8). 0 if the uplink is not on a
 *  channel of the plan. */
uint32_t lwc_region_rx1_hz(const lwc_region_t *r, uint32_t up_hz, uint16_t up_bw_khz);

/** EIRP of a TXPower index (MaxEIRP - 2 * idx), or INT8_MIN if not defined. */
int8_t lwc_region_tx_power_dbm(const lwc_region_t *r, uint8_t idx);

/** Fixed plans, LoRaWAN 1.0.3+: the join-accept CFList (type 1, channel
 *  masks) that enables sub-band `sb` (0-7: channels 8sb..8sb+7 and 64+sb).
 *  False for the other regions or a bad sub-band. */
bool lwc_region_cflist_subband(const lwc_region_t *r, uint8_t sb, uint8_t out[16]);

/** Fixed plans: the two LinkADRReq masks that move a device from all
 *  channels to sub-band `sb`: ChMaskCntl 7 (125 kHz off) with the 500 kHz
 *  channel, then the 16-channel block holding the sub-band. False for the
 *  other regions or a bad sub-band. */
bool lwc_region_link_adr_subband(const lwc_region_t *r, uint8_t sb, uint16_t mask[2], uint8_t cntl[2]);

#ifdef __cplusplus
}
#endif

#endif /* LWC_REGION_H */
