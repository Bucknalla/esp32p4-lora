/*
 * The radio's channel plan (ADR-0017): what the SX1301 listens on. One plan
 * feeds the concentrator set-up, the local server's join-accept CFList and the
 * check of the network server's router_config. Dynamic-channel regions
 * (ADR-0024): EU868, AS923-1..4, IN865, KR920, RU864, EU433; fixed-channel
 * regions (ADR-0025): one sub-band of US915, AU915, CN470. OS-independent.
 */
#ifndef LWC_PLAN_H
#define LWC_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwc/region.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_PLAN_MULTISF 8U

typedef struct {
    bool enable;
    uint8_t radio;  /**< 0 or 1 */
    int32_t if_hz;  /**< offset from that radio's centre frequency */
} lwc_plan_chan_t;

typedef struct {
    uint32_t radio_hz[2];                      /**< radio (SX1257, SX1255) centre frequencies */
    lwc_plan_chan_t multisf[LWC_PLAN_MULTISF]; /**< 125 kHz, SF7-12 */
    lwc_plan_chan_t std;                       /**< one LoRa channel at a fixed SF and bandwidth */
    uint8_t std_sf;                            /**< 7..12 */
    uint16_t std_bw_khz;                       /**< 125, 250 or 500 */
    lwc_plan_chan_t fsk;                       /**< FSK 50 kbit/s */
} lwc_plan_t;

typedef enum {
    LWC_PLAN_OK = 0,
    LWC_PLAN_ERR_ARG,
    LWC_PLAN_ERR_RADIO,   /**< EU868: a radio centre frequency outside 863-870 MHz */
    LWC_PLAN_ERR_IF,      /**< a channel outside its radio's receive band */
    LWC_PLAN_ERR_BAND,    /**< EU868: a channel outside 863-870 MHz */
    LWC_PLAN_ERR_DEFAULT, /**< a default channel of the region missing (EU868: 868.1, 868.3, 868.5 MHz) */
    LWC_PLAN_ERR_STD,     /**< the std channel's SF or bandwidth */
    LWC_PLAN_ERR_REGION,  /**< the card cannot serve the region (lwc_card_region_ok) */
    LWC_PLAN_ERR_CHANNEL, /**< fixed plans: a channel not on the region's uplink grid, or FSK on */
    LWC_PLAN_ERR_CARD,    /**< a channel outside the card's band */
} lwc_plan_err_t;

/** The region's default plan. EU868 as on TTN and in Nebra's
 *  global_conf.json.EU868: radios 867.5 and 868.5 MHz, multi-SF 868.1/.3/.5
 *  and 867.1-867.9 MHz, std 868.3 MHz SF7 250 kHz, FSK off. The other
 *  dynamic regions: their default channels only (RP002), the radios centred
 *  on them, std and FSK off. Fixed regions: lwc_plan_subband() with
 *  lwc_plan_default_subband(). False (plan zeroed) for a NULL region. */
bool lwc_plan_default(lwc_plan_t *p, const lwc_region_t *r);

/** Fixed regions: the default sub-band (0-based), as in the TTN frequency
 *  plans: US915 and AU915 FSB2 (1), CN470 FSB11 (10). 0 for the others. */
uint8_t lwc_plan_default_subband(const lwc_region_t *r);

/** Fixed regions: the plan for sub-band `sb` (0-based): its 8 125 kHz
 *  channels, 4 on each radio, and (US915, AU915) its 500 kHz channel on the
 *  std chain at the region's 500 kHz uplink DR; FSK off. As the TTN frequency
 *  plans lay them out. False (plan zeroed) for a dynamic region or a bad
 *  sub-band. */
bool lwc_plan_subband(lwc_plan_t *p, const lwc_region_t *r, uint8_t sb);

/** Fixed regions: the sub-band whose 8 channels are all in the plan, or -1. */
int lwc_plan_subband_of(const lwc_plan_t *p, const lwc_region_t *r);

/** Checks a plan for region `r` on card `c`, in this order: the card can
 *  serve the region; EU868: radio centres in 863-870 MHz; the SX1301's
 *  receive limits (HAL: |IF| + bandwidth/2 <= radio bandwidth/2: 925 kHz for
 *  125 kHz channels, 1000 kHz for 250, 1100 kHz for 500; FSK as 125 kHz);
 *  EU868: channels in 863-870 MHz; dynamic regions: the default channels;
 *  fixed regions: every channel on the region's uplink grid, std at an
 *  uplink DR, FSK off; last, every channel inside the card's band. Band edges
 *  of the other regions: R4. */
lwc_plan_err_t lwc_plan_check(const lwc_plan_t *p, const lwc_region_t *r, const lwc_card_t *c);

/** Absolute frequency of a channel (0 if it is disabled). */
uint32_t lwc_plan_hz(const lwc_plan_t *p, const lwc_plan_chan_t *c);

/** CFList for a join-accept: the enabled multi-SF channels other than the
 *  region's defaults, lowest first, at most 5; unused entries 0. Returns the
 *  count. 0 for the fixed regions (no frequency CFList). */
size_t lwc_plan_cflist(const lwc_plan_t *p, const lwc_region_t *r, uint32_t out[5]);

bool lwc_plan_equal(const lwc_plan_t *a, const lwc_plan_t *b);

/* Stored form (NVS): a version byte, then the fields little-endian, so the
 * record does not depend on the struct layout. */
#define LWC_PLAN_PACKED 72U
#define LWC_PLAN_VERSION 1U

/** Writes LWC_PLAN_PACKED bytes; returns false if `cap` is smaller. */
bool lwc_plan_pack(const lwc_plan_t *p, uint8_t *out, size_t cap);
/** Reads a stored plan; false for another version, a wrong length or a plan
 *  that fails lwc_plan_check for `r` and `c`. */
bool lwc_plan_unpack(const uint8_t *in, size_t n, const lwc_region_t *r, const lwc_card_t *c, lwc_plan_t *p);

#ifdef __cplusplus
}
#endif

#endif /* LWC_PLAN_H */
