/*
 * Downlinks to the radio: one frame to send (lwc_txpk_t, from the Basics
 * Station client or the local network server), when to send it (just-in-time
 * queue). The duty-cycle budget is in lwc/txrule.h. OS-independent, no
 * allocation.
 */
#ifndef LWC_TXPK_H
#define LWC_TXPK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- txpk ---------------------------------------------------------------- */

typedef enum {
    LWC_TXPK_TIMED = 0, /**< at a concentrator counter value */
    LWC_TXPK_IMMEDIATE, /**< as soon as possible */
    LWC_TXPK_GPS,       /**< at a GPS time (not supported here) */
} lwc_txpk_mode_t;

typedef struct {
    lwc_txpk_mode_t mode;
    uint32_t tmst;
    uint32_t freq_hz;
    uint8_t rfch;
    int8_t powe;      /**< dBm, rounded down */
    uint8_t sf;       /**< 7..12 */
    uint16_t bw_khz;  /**< 125, 250, 500 */
    uint8_t cr_denom; /**< 5..8 */
    bool ipol;
    bool ncrc;        /**< true: no payload CRC */
    uint16_t prea;    /**< preamble symbols 6..64, 0 = default */
    uint16_t size;
    uint8_t payload[255];
} lwc_txpk_t;

/** LoRa time on air in microseconds (Semtech AN1200.13), rounded up.
 *  Low-data-rate optimisation on when a symbol lasts 16 ms or more
 *  (SF11/SF12 at 125 kHz, SF12 at 250 kHz). `preamble` in symbols, without
 *  the 4.25 sync symbols. Returns 0 for invalid parameters. */
uint32_t lwc_lora_airtime_us(uint8_t sf, uint16_t bw_khz, uint8_t cr_denom, uint16_t preamble,
                             uint16_t size, bool crc, bool implicit_header);

/* ---- just-in-time queue ---------------------------------------------------
 * Times are concentrator counter microseconds (wrapping uint32). Comparisons
 * use the signed 32-bit difference, valid within +/- 35 minutes. */

#define LWC_JIT_SLOTS 8U

typedef enum {
    LWC_JIT_OK = 0,
    LWC_JIT_TOO_LATE,
    LWC_JIT_TOO_EARLY,
    LWC_JIT_COLLISION,
    LWC_JIT_FULL,
} lwc_jit_res_t;

typedef struct {
    bool used;
    bool sent; /**< handed to the radio: still blocks others until it ends */
    uint32_t start;
    uint32_t airtime_us;
    uint32_t id;
} lwc_jit_slot_t;

typedef struct {
    lwc_jit_slot_t slot[LWC_JIT_SLOTS];
    uint32_t min_lead_us;    /**< reject when start - now is below this */
    uint32_t max_advance_us; /**< reject when start - now is above this */
    uint32_t guard_us;       /**< spacing required between two frames */
} lwc_jit_t;

void lwc_jit_init(lwc_jit_t *q, uint32_t min_lead_us, uint32_t max_advance_us, uint32_t guard_us);

/** Admit a frame starting at `start` lasting `airtime_us`. On OK, `*slot` is its index. */
lwc_jit_res_t lwc_jit_admit(lwc_jit_t *q, uint32_t now, uint32_t start, uint32_t airtime_us,
                            uint32_t id, size_t *slot);

/** The earliest frame due within `lead_us` of `now`: returns its slot index,
 *  or -1. A frame already past `now + min_send_us` is reported through
 *  `*expired` (slot index, else -1) so the caller can drop it. */
int lwc_jit_due(const lwc_jit_t *q, uint32_t now, uint32_t lead_us, uint32_t min_send_us,
                int *expired);

/** The frame in `slot` is on its way to the air: never due again, but it
 *  still collides with new frames until lwc_jit_ended reports it. */
void lwc_jit_sent(lwc_jit_t *q, size_t slot);

/** A sent frame that ended more than `margin_us` before `now`, or -1. The
 *  caller checks the radio is idle, then releases the slot. */
int lwc_jit_ended(const lwc_jit_t *q, uint32_t now, uint32_t margin_us);

void lwc_jit_release(lwc_jit_t *q, size_t slot);

#ifdef __cplusplus
}
#endif

#endif /* LWC_TXPK_H */
