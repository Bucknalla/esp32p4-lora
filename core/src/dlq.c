#include "lwc/dlq.h"

#include <string.h>

bool lwc_dlq_id_ok(const char *s, size_t n)
{
    if (s == NULL || n == 0U || n > LWC_DLQ_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == ':' || c == '-')) {
            return false;
        }
    }
    return true;
}

size_t lwc_dlq_clear(lwc_dlq_t *q)
{
    if (q == NULL) {
        return 0;
    }
    const size_t n = q->n;
    memset(q, 0, sizeof *q);
    return n;
}

size_t lwc_dlq_own(lwc_dlq_t *q, uint64_t dev_eui)
{
    if (q == NULL || q->n == 0U || q->item[0].dev_eui == dev_eui) {
        return 0;
    }
    return lwc_dlq_clear(q);
}

bool lwc_dlq_push(lwc_dlq_t *q, const lwc_dlq_item_t *it)
{
    if (q == NULL || it == NULL || q->n >= LWC_DLQ_DEPTH || it->port < 1U || it->port > 223U ||
        it->len > LWC_DLQ_DATA_MAX) {
        return false;
    }
    (void)lwc_dlq_own(q, it->dev_eui);
    lwc_dlq_item_t *d = &q->item[q->n++];
    *d = *it;
    d->sent = false;
    d->tx = 0;
    d->token = 0;
    d->fcnt = 0;
    d->c_mode = false;
    d->txr = LWC_DLQ_TX_PENDING;
    d->refusals = 0;
    d->sent_ms = 0;
    d->end_tmst = 0;
    d->id[LWC_DLQ_ID_MAX] = '\0';
    return true;
}

const lwc_dlq_item_t *lwc_dlq_head(const lwc_dlq_t *q)
{
    return q != NULL && q->n > 0U ? &q->item[0] : NULL;
}

bool lwc_dlq_pop(lwc_dlq_t *q, lwc_dlq_item_t *out)
{
    if (q == NULL || q->n == 0U) {
        return false;
    }
    if (out != NULL) {
        *out = q->item[0];
    }
    memmove(&q->item[0], &q->item[1], sizeof q->item[0] * (size_t)(LWC_DLQ_DEPTH - 1U));
    memset(&q->item[LWC_DLQ_DEPTH - 1U], 0, sizeof q->item[0]);
    q->n--;
    return true;
}

void lwc_dlq_sent(lwc_dlq_t *q, uint16_t token, uint32_t fcnt)
{
    if (q == NULL || q->n == 0U) {
        return;
    }
    lwc_dlq_item_t *h = &q->item[0];
    h->sent = true;
    h->token = token;
    h->fcnt = fcnt;
    h->c_mode = false;
    h->txr = LWC_DLQ_TX_PENDING;
    if (h->tx < UINT8_MAX) {
        h->tx++;
    }
}

void lwc_dlq_sent_c(lwc_dlq_t *q, uint16_t token, uint32_t fcnt, int64_t now_ms, uint32_t end_tmst)
{
    if (q == NULL || q->n == 0U) {
        return;
    }
    lwc_dlq_sent(q, token, fcnt);
    lwc_dlq_item_t *h = &q->item[0];
    h->c_mode = true;
    h->sent_ms = now_ms;
    h->end_tmst = end_tmst;
    q->next_ms = now_ms + LWC_DLQ_C_GAP_MS;
}

void lwc_dlq_tx_result(lwc_dlq_t *q, uint16_t token, bool ok)
{
    if (q == NULL || q->n == 0U || !q->item[0].sent || q->item[0].token != token || token == 0U) {
        return;
    }
    q->item[0].txr = ok ? LWC_DLQ_TX_OK : LWC_DLQ_TX_FAILED;
}

void lwc_dlq_hold(lwc_dlq_t *q, int64_t until_ms)
{
    if (q != NULL && until_ms > q->hold_ms) {
        q->hold_ms = until_ms;
    }
}

void lwc_dlq_defer(lwc_dlq_t *q, int64_t until_ms)
{
    if (q != NULL && until_ms > q->next_ms) {
        q->next_ms = until_ms;
    }
}

lwc_dlq_ev_t lwc_dlq_uplink(lwc_dlq_t *q, bool new_uplink, bool ack, uint32_t up_tmst, lwc_dlq_item_t *done)
{
    if (q == NULL || q->n == 0U || !q->item[0].sent) {
        return LWC_DLQ_KEEP; /* an ACK with nothing confirmed out is ignored */
    }
    lwc_dlq_item_t *h = &q->item[0];
    const bool refused = h->txr == LWC_DLQ_TX_FAILED;
    if (h->c_mode && !refused && (int32_t)(up_tmst - h->end_tmst) < 0) {
        /* sent before the device could have heard it: settles nothing */
        return LWC_DLQ_KEEP;
    }
    if (refused) {
        /* never on air: offered again, not counted */
        h->sent = false;
        if (h->tx > 0U) {
            h->tx--;
        }
        return LWC_DLQ_KEEP;
    }
    if (!new_uplink) {
        /* the device repeats its uplink: it did not get the answer. Its ACK
         * bit belongs to the earlier frame. Offered again, new counter. */
        h->sent = false;
        return LWC_DLQ_KEEP;
    }
    const bool confirmed = h->confirmed;
    (void)lwc_dlq_pop(q, done);
    return !confirmed ? LWC_DLQ_DELIVERED : ack ? LWC_DLQ_ACKED : LWC_DLQ_NACKED;
}

lwc_dlq_ev_t lwc_dlq_tick(lwc_dlq_t *q, int64_t now_ms, int64_t ack_timeout_ms, lwc_dlq_item_t *done,
                          const char **reason)
{
    if (reason != NULL) {
        *reason = NULL;
    }
    if (q == NULL || q->n == 0U) {
        return LWC_DLQ_KEEP;
    }
    lwc_dlq_item_t *h = &q->item[0];
    if (!h->sent) {
        return now_ms >= q->next_ms && now_ms >= q->hold_ms ? LWC_DLQ_SEND : LWC_DLQ_KEEP;
    }
    if (!h->c_mode) {
        return LWC_DLQ_KEEP; /* sent in a class A answer: the next uplink settles it */
    }
    const char *why = NULL;
    lwc_dlq_ev_t ev = LWC_DLQ_KEEP;
    switch ((lwc_dlq_txr_t)h->txr) {
    case LWC_DLQ_TX_FAILED:
        h->sent = false;
        if (h->tx > 0U) {
            h->tx--;
        }
        h->refusals++;
        if (h->refusals >= LWC_DLQ_C_REFUSALS) {
            ev = LWC_DLQ_DROPPED;
            why = "refused by the radio 5 times (duty cycle or busy)";
        } else {
            lwc_dlq_defer(q, now_ms + (int64_t)h->refusals * LWC_DLQ_C_GAP_MS);
        }
        break;
    case LWC_DLQ_TX_PENDING:
        if (now_ms - h->sent_ms > LWC_DLQ_C_RESULT_MS) {
            ev = LWC_DLQ_DROPPED; /* it may have been on air: never sent again */
            why = "no answer from the radio";
        }
        break;
    case LWC_DLQ_TX_OK:
    default:
        if (!h->confirmed) {
            ev = LWC_DLQ_DELIVERED;
        } else if (now_ms - h->sent_ms >= ack_timeout_ms) {
            ev = LWC_DLQ_TIMEOUT;
            why = "no uplink with the ACK in time";
        }
        break;
    }
    if (ev != LWC_DLQ_KEEP) {
        (void)lwc_dlq_pop(q, done);
        if (reason != NULL) {
            *reason = why;
        }
    }
    return ev;
}
