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

typedef struct {
    uint64_t dev_eui; /* the owner: a reused device slot never gets an old payload */
    uint8_t port;     /* 1..223 */
    uint8_t len;
    bool confirmed;
    bool sent;       /* went out, waiting for the device's next uplink */
    uint8_t tx;      /* transmissions so far */
    uint16_t token;  /* the radio request it last went out in */
    uint32_t fcnt;   /* the downlink counter it last went out with */
    char id[LWC_DLQ_ID_MAX + 1U];
    uint8_t data[LWC_DLQ_DATA_MAX];
} lwc_dlq_item_t;

typedef struct {
    uint8_t n;
    lwc_dlq_item_t item[LWC_DLQ_DEPTH];
} lwc_dlq_t;

typedef enum {
    LWC_DLQ_KEEP = 0,   /* nothing released */
    LWC_DLQ_DELIVERED,  /* an unconfirmed downlink: the device moved past it */
    LWC_DLQ_ACKED,      /* a confirmed downlink, acknowledged */
    LWC_DLQ_NACKED,     /* a confirmed downlink, not acknowledged */
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

/** The head went out in radio request `token` with downlink counter `fcnt`. */
void lwc_dlq_sent(lwc_dlq_t *q, uint16_t token, uint32_t fcnt);

/**
 * An uplink from the owner, before its answer is built. `new_uplink`: false
 * for a repeated one. `ack`: its FCtrl.ACK. `refused`: the radio refused the
 * head's last request. A released head is copied to *done (may be NULL).
 */
lwc_dlq_ev_t lwc_dlq_uplink(lwc_dlq_t *q, bool new_uplink, bool ack, bool refused, lwc_dlq_item_t *done);

#ifdef __cplusplus
}
#endif

#endif /* LWC_DLQ_H */
