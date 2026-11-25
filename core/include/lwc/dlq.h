/*
 * Application downlink queue of one local device (ADR-0018). Pure state, no
 * I/O, so it runs in host tests; the firmware keeps one per device slot and
 * guards it with the local server's lock.
 *
 * The head is offered with each class A answer. Once it went out (marked with
 * lwc_dlq_sent), the device's next uplink decides:
 *   - a repeated uplink (the device missed the answer), or the radio refused
 *     the frame: offered again, with a new downlink counter;
 *   - a new uplink: released. An unconfirmed downlink is done. A confirmed
 *     one is acked if that uplink has FCtrl.ACK set, otherwise nacked: it is
 *     never sent again on its own (if only the ACK uplink was lost, a resend
 *     would run the command twice); the application decides about a retry.
 *
 * Class C (ADR-0020): lwc_dlq_tick() says when the head may go out at once
 * (no uplink needed), at most one every LWC_DLQ_C_GAP_MS per device and not
 * while the device's class A windows are open (lwc_dlq_hold). Then:
 *   - the radio's answer for it (lwc_dlq_tx_result): handed over is the last
 *     word for an unconfirmed downlink; refused is tried again after a
 *     growing pause, dropped after LWC_DLQ_C_REFUSALS; no answer at all
 *     within LWC_DLQ_C_RESULT_MS is dropped, never sent again (it may have
 *     been on air);
 *   - a confirmed one waits for the ACK in a new uplink received after the
 *     downlink ended (an uplink sent before the device heard it settles
 *     nothing); none within the caller's timeout: LWC_DLQ_TIMEOUT.
 */
#ifndef LWC_DLQ_H
#define LWC_DLQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_DLQ_DEPTH 4U
#define LWC_DLQ_DATA_MAX 222U /* the largest EU868 FRMPayload */
#define LWC_DLQ_ID_MAX 36U    /* the caller's id for results (a UUID fits) */
#define LWC_DLQ_C_GAP_MS 5000      /* class C: least time between two sends to one device */
#define LWC_DLQ_C_REFUSALS 5U      /* class C: radio refusals before the head is dropped */
#define LWC_DLQ_C_RESULT_MS 10000  /* class C: longest wait for the radio's answer */

typedef enum {
    LWC_DLQ_TX_PENDING = 0, /* no answer from the radio yet */
    LWC_DLQ_TX_OK,          /* handed to the concentrator */
    LWC_DLQ_TX_FAILED,      /* refused or not handed over: never on air */
} lwc_dlq_txr_t;

typedef struct {
    uint64_t dev_eui; /* the owner: a reused device slot never gets an old payload */
    uint8_t port;     /* 1..223 */
    uint8_t len;
    bool confirmed;
    bool sent;       /* went out, waiting for the device's next uplink */
    uint8_t tx;      /* transmissions so far */
    uint16_t token;  /* the radio request it last went out in */
    uint32_t fcnt;   /* the downlink counter it last went out with */
    bool c_mode;     /* last sent as class C (not as a class A answer) */
    uint8_t txr;     /* lwc_dlq_txr_t of the last send */
    uint8_t refusals; /* class C: radio refusals so far */
    int64_t sent_ms; /* class C: when it was last sent */
    uint32_t end_tmst; /* class C: concentrator time its transmission ends */
    char id[LWC_DLQ_ID_MAX + 1U];
    uint8_t data[LWC_DLQ_DATA_MAX];
} lwc_dlq_item_t;

typedef struct {
    uint8_t n;
    int64_t next_ms; /* class C: not before (gap, refusal pause) */
    int64_t hold_ms; /* class C: not before (the device's class A windows) */
    lwc_dlq_item_t item[LWC_DLQ_DEPTH];
} lwc_dlq_t;

typedef enum {
    LWC_DLQ_KEEP = 0,   /* nothing released */
    LWC_DLQ_DELIVERED,  /* an unconfirmed downlink: the device moved past it */
    LWC_DLQ_ACKED,      /* a confirmed downlink, acknowledged */
    LWC_DLQ_NACKED,     /* a confirmed downlink, not acknowledged */
    LWC_DLQ_TIMEOUT,    /* class C, confirmed: no uplink with the ACK in time */
    LWC_DLQ_DROPPED,    /* class C: given up (the reason says why) */
    LWC_DLQ_SEND,       /* class C: send the head now */
} lwc_dlq_ev_t;

/** True for 1..LWC_DLQ_ID_MAX characters of [A-Za-z0-9_.:-]. `s` has `n` bytes. */
bool lwc_dlq_id_ok(const char *s, size_t n);

/** Empty the queue. Returns how many were waiting. */
size_t lwc_dlq_clear(lwc_dlq_t *q);

/** Empty it if it belongs to another device (the slot was reused). Returns
 *  how many were dropped. */
size_t lwc_dlq_own(lwc_dlq_t *q, uint64_t dev_eui);

/** Append (`it->sent`, `tx`, `token`, `fcnt` are cleared). False if full or
 *  `it` is not valid (port, length). */
bool lwc_dlq_push(lwc_dlq_t *q, const lwc_dlq_item_t *it);

/** The head, or NULL. */
const lwc_dlq_item_t *lwc_dlq_head(const lwc_dlq_t *q);

/** Remove the head into *out (may be NULL). False if empty. */
bool lwc_dlq_pop(lwc_dlq_t *q, lwc_dlq_item_t *out);

/** The head went out in a class A answer, radio request `token`, downlink counter `fcnt`. */
void lwc_dlq_sent(lwc_dlq_t *q, uint16_t token, uint32_t fcnt);

/** The head went out as class C at `now_ms`; its transmission ends at
 *  concentrator time `end_tmst`. The next send waits LWC_DLQ_C_GAP_MS. */
void lwc_dlq_sent_c(lwc_dlq_t *q, uint16_t token, uint32_t fcnt, int64_t now_ms, uint32_t end_tmst);

/** The radio's answer for request `token` (ignored unless it is the head's). */
void lwc_dlq_tx_result(lwc_dlq_t *q, uint16_t token, bool ok);

/** Class C: no send before `until_ms` (the device's class A windows). */
void lwc_dlq_hold(lwc_dlq_t *q, int64_t until_ms);

/** Class C: no send before `until_ms` (the send could not be made). */
void lwc_dlq_defer(lwc_dlq_t *q, int64_t until_ms);

/**
 * An uplink from the owner, received at concentrator time `up_tmst`, before
 * its answer is built. `new_uplink`: false for a repeated one. `ack`: its
 * FCtrl.ACK. A head the radio refused is offered again. A released head is
 * copied to *done (may be NULL). A class C head is left alone (KEEP, still
 * sent: do not offer it) when the uplink came before its transmission ended.
 */
lwc_dlq_ev_t lwc_dlq_uplink(lwc_dlq_t *q, bool new_uplink, bool ack, uint32_t up_tmst, lwc_dlq_item_t *done);

/**
 * Class C, time-driven: what to do with the head at `now_ms`. SEND: send it
 * (then lwc_dlq_sent_c, or lwc_dlq_defer). DELIVERED, TIMEOUT, DROPPED: the
 * head was released into *done; *reason (may be NULL) is set for TIMEOUT and
 * DROPPED. A confirmed head waits up to `ack_timeout_ms` after it was sent.
 */
lwc_dlq_ev_t lwc_dlq_tick(lwc_dlq_t *q, int64_t now_ms, int64_t ack_timeout_ms, lwc_dlq_item_t *done,
                          const char **reason);

#ifdef __cplusplus
}
#endif

#endif /* LWC_DLQ_H */
