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
    if (h->tx < UINT8_MAX) {
        h->tx++;
    }
}

lwc_dlq_ev_t lwc_dlq_uplink(lwc_dlq_t *q, bool new_uplink, bool ack, bool refused, lwc_dlq_item_t *done)
{
    if (q == NULL || q->n == 0U || !q->item[0].sent) {
        return LWC_DLQ_KEEP; /* an ACK with nothing confirmed out is ignored */
    }
    lwc_dlq_item_t *h = &q->item[0];
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
