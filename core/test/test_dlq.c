#include "lwc/dlq.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define EUI 0x70B3D57ED0001234ULL

static lwc_dlq_item_t item(uint8_t port, uint8_t len, bool confirmed, const char *id)
{
    lwc_dlq_item_t it;
    memset(&it, 0, sizeof it);
    it.dev_eui = EUI;
    it.port = port;
    it.len = len;
    it.confirmed = confirmed;
    for (uint8_t i = 0; i < len; i++) {
        it.data[i] = (uint8_t)(port + i);
    }
    if (id != NULL) {
        (void)snprintf(it.id, sizeof it.id, "%s", id);
    }
    it.sent = true; /* push clears these */
    it.tx = 9;
    it.token = 77;
    return it;
}

static void test_ids(void)
{
    CHECK(lwc_dlq_id_ok("a", 1));
    CHECK(lwc_dlq_id_ok("3f2504e0-4f89-11d3-9a0c-0305e82c3301", 36));
    CHECK(lwc_dlq_id_ok("job_7.valve:open", 16));
    CHECK(!lwc_dlq_id_ok("3f2504e0-4f89-11d3-9a0c-0305e82c33012", 37));
    CHECK(!lwc_dlq_id_ok("", 0));
    CHECK(!lwc_dlq_id_ok(NULL, 1));
    CHECK(!lwc_dlq_id_ok("a b", 3));
    CHECK(!lwc_dlq_id_ok("a\"b", 3));
    CHECK(!lwc_dlq_id_ok("a\\u0041", 7));
    CHECK(!lwc_dlq_id_ok("a/b", 3));
}

static void test_push(void)
{
    lwc_dlq_t q;
    memset(&q, 0, sizeof q);
    CHECK(lwc_dlq_head(&q) == NULL && !lwc_dlq_pop(&q, NULL));
    lwc_dlq_item_t it = item(1, 2, false, "x");
    for (unsigned i = 0; i < LWC_DLQ_DEPTH; i++) {
        it.port = (uint8_t)(1U + i);
        CHECK(lwc_dlq_push(&q, &it));
    }
    CHECK(q.n == LWC_DLQ_DEPTH && !lwc_dlq_push(&q, &it)); /* full */
    const lwc_dlq_item_t *h = lwc_dlq_head(&q);
    CHECK(h != NULL && h->port == 1U && !h->sent && h->tx == 0U && h->token == 0U && strcmp(h->id, "x") == 0);
    /* bad items */
    lwc_dlq_t e;
    memset(&e, 0, sizeof e);
    it.port = 0;
    CHECK(!lwc_dlq_push(&e, &it));
    it.port = 224;
    CHECK(!lwc_dlq_push(&e, &it));
    it.port = 1;
    it.len = LWC_DLQ_DATA_MAX + 1U;
    CHECK(!lwc_dlq_push(&e, &it) && e.n == 0U);
    /* order kept */
    lwc_dlq_item_t out;
    CHECK(lwc_dlq_pop(&q, &out) && out.port == 1U && lwc_dlq_head(&q)->port == 2U && q.n == 3U);
    /* a reused slot: another device's items are dropped */
    CHECK(lwc_dlq_own(&q, EUI) == 0U && q.n == 3U);
    CHECK(lwc_dlq_own(&q, EUI + 1U) == 3U && q.n == 0U);
    CHECK(lwc_dlq_push(&q, &(lwc_dlq_item_t){.dev_eui = EUI, .port = 5}));
    lwc_dlq_item_t other = item(6, 1, false, NULL);
    other.dev_eui = EUI + 1U;
    CHECK(lwc_dlq_push(&q, &other) && q.n == 1U && lwc_dlq_head(&q)->dev_eui == EUI + 1U);
    CHECK(lwc_dlq_clear(&q) == 1U && q.n == 0U);
}

/* The behaviour already proven on the bench (unconfirmed). */
static void test_unconfirmed(void)
{
    lwc_dlq_t q;
    memset(&q, 0, sizeof q);
    lwc_dlq_item_t a = item(9, 1, false, "a"), b = item(7, 5, false, NULL), done;
    CHECK(lwc_dlq_push(&q, &a) && lwc_dlq_push(&q, &b));
    /* not out yet: an uplink changes nothing, an ACK is ignored */
    CHECK(lwc_dlq_uplink(&q, true, true, 0, &done) == LWC_DLQ_KEEP && q.n == 2U);
    lwc_dlq_sent(&q, 11, 40);
    const lwc_dlq_item_t *h = lwc_dlq_head(&q);
    CHECK(h->sent && h->token == 11U && h->fcnt == 40U && h->tx == 1U);
    /* the device repeats its uplink: offered again */
    CHECK(lwc_dlq_uplink(&q, false, false, 0, &done) == LWC_DLQ_KEEP && !lwc_dlq_head(&q)->sent);
    lwc_dlq_sent(&q, 12, 41);
    CHECK(lwc_dlq_head(&q)->tx == 2U && lwc_dlq_head(&q)->fcnt == 41U);
    /* the radio refused it: offered again, not counted (another token's answer is ignored) */
    lwc_dlq_tx_result(&q, 99, false);
    CHECK(lwc_dlq_head(&q)->txr == LWC_DLQ_TX_PENDING);
    lwc_dlq_tx_result(&q, 12, false);
    CHECK(lwc_dlq_uplink(&q, true, false, 0, &done) == LWC_DLQ_KEEP && !lwc_dlq_head(&q)->sent &&
          lwc_dlq_head(&q)->tx == 1U && q.n == 2U);
    lwc_dlq_sent(&q, 13, 42);
    /* a new uplink: released */
    memset(&done, 0, sizeof done);
    CHECK(lwc_dlq_uplink(&q, true, false, 0, &done) == LWC_DLQ_DELIVERED && q.n == 1U);
    CHECK(done.port == 9U && done.fcnt == 42U && done.tx == 2U && strcmp(done.id, "a") == 0);
    CHECK(lwc_dlq_head(&q)->port == 7U && !lwc_dlq_head(&q)->sent);
    /* the next one, released the same way (an ACK bit on it means nothing) */
    lwc_dlq_sent(&q, 14, 43);
    CHECK(lwc_dlq_uplink(&q, true, true, 0, NULL) == LWC_DLQ_DELIVERED && q.n == 0U);
    CHECK(lwc_dlq_uplink(&q, true, true, 0, NULL) == LWC_DLQ_KEEP);
}

static void test_confirmed(void)
{
    lwc_dlq_t q;
    memset(&q, 0, sizeof q);
    lwc_dlq_item_t c = item(3, 2, true, "valve-1"), done;
    CHECK(lwc_dlq_push(&q, &c));
    lwc_dlq_sent(&q, 20, 100);
    /* ACK in the next new uplink: acked */
    CHECK(lwc_dlq_uplink(&q, true, true, 0, &done) == LWC_DLQ_ACKED && q.n == 0U);
    CHECK(strcmp(done.id, "valve-1") == 0 && done.fcnt == 100U && done.tx == 1U);

    /* no ACK in the next new uplink: nacked at once, never sent again on its own */
    CHECK(lwc_dlq_push(&q, &c));
    lwc_dlq_sent(&q, 21, 101);
    CHECK(lwc_dlq_uplink(&q, true, false, 0, &done) == LWC_DLQ_NACKED && q.n == 0U && done.tx == 1U);

    /* a repeated uplink (even with ACK set: that bit belongs to the earlier
     * frame) offers it again with a new counter; then the ACK counts */
    CHECK(lwc_dlq_push(&q, &c));
    lwc_dlq_sent(&q, 22, 102);
    CHECK(lwc_dlq_uplink(&q, false, true, 0, &done) == LWC_DLQ_KEEP && q.n == 1U && !lwc_dlq_head(&q)->sent);
    lwc_dlq_sent(&q, 23, 103);
    CHECK(lwc_dlq_uplink(&q, true, true, 0, &done) == LWC_DLQ_ACKED && done.tx == 2U && done.fcnt == 103U);

    /* a burst of repeats (up to 7 for one confirmed uplink): each offers it again */
    CHECK(lwc_dlq_push(&q, &c));
    for (uint32_t i = 0; i < 7U; i++) {
        lwc_dlq_sent(&q, (uint16_t)(30U + i), 110U + i);
        CHECK(lwc_dlq_uplink(&q, false, false, 0, &done) == LWC_DLQ_KEEP && q.n == 1U);
    }
    lwc_dlq_sent(&q, 40, 120);
    CHECK(lwc_dlq_uplink(&q, true, false, 0, &done) == LWC_DLQ_NACKED && done.tx == 8U);

    /* refused by the radio, then the new uplink: not a NACK (it never went out) */
    CHECK(lwc_dlq_push(&q, &c));
    lwc_dlq_sent(&q, 41, 121);
    lwc_dlq_tx_result(&q, 41, false);
    CHECK(lwc_dlq_uplink(&q, true, false, 0, &done) == LWC_DLQ_KEEP && q.n == 1U && lwc_dlq_head(&q)->tx == 0U);
    lwc_dlq_sent(&q, 42, 122);
    CHECK(lwc_dlq_uplink(&q, true, true, 0, &done) == LWC_DLQ_ACKED);

    /* refused by the radio and an ACK in the same new uplink: the ACK cannot be
     * for a frame that never went out; offered again */
    CHECK(lwc_dlq_push(&q, &c));
    lwc_dlq_sent(&q, 43, 123);
    lwc_dlq_tx_result(&q, 43, false);
    CHECK(lwc_dlq_uplink(&q, true, true, 0, &done) == LWC_DLQ_KEEP && q.n == 1U && !lwc_dlq_head(&q)->sent);
    lwc_dlq_sent(&q, 44, 124);
    CHECK(lwc_dlq_uplink(&q, true, false, 0, &done) == LWC_DLQ_NACKED && done.fcnt == 124U);

    /* an ACK while an unconfirmed one is out does not ack the confirmed one behind it */
    lwc_dlq_item_t u = item(4, 1, false, NULL);
    CHECK(lwc_dlq_push(&q, &u) && lwc_dlq_push(&q, &c));
    lwc_dlq_sent(&q, 50, 130);
    CHECK(lwc_dlq_uplink(&q, true, true, 0, &done) == LWC_DLQ_DELIVERED && q.n == 1U);
    CHECK(lwc_dlq_head(&q)->confirmed && !lwc_dlq_head(&q)->sent);
    CHECK(lwc_dlq_uplink(&q, true, true, 0, &done) == LWC_DLQ_KEEP && q.n == 1U);
}

static void test_class_c(void)
{
    lwc_dlq_t q;
    memset(&q, 0, sizeof q);
    lwc_dlq_item_t u = item(8, 2, false, "c-u"), c = item(9, 1, true, "c-c"), done;
    const char *why;
    const int64_t TO = 60000; /* ack timeout */
    CHECK(lwc_dlq_tick(&q, 1000, TO, &done, &why) == LWC_DLQ_KEEP); /* empty */
    CHECK(lwc_dlq_push(&q, &u) && lwc_dlq_push(&q, &c));
    /* send at once; then not again within the gap */
    CHECK(lwc_dlq_tick(&q, 1000, TO, &done, &why) == LWC_DLQ_SEND);
    lwc_dlq_sent_c(&q, 60, 200, 1000, 5000000U);
    CHECK(lwc_dlq_tick(&q, 1500, TO, &done, &why) == LWC_DLQ_KEEP);
    /* handed over: an unconfirmed one is done */
    lwc_dlq_tx_result(&q, 60, true);
    CHECK(lwc_dlq_tick(&q, 2000, TO, &done, &why) == LWC_DLQ_DELIVERED && done.port == 8U && why == NULL);
    /* the next one waits for the gap */
    CHECK(lwc_dlq_tick(&q, 5999, TO, &done, &why) == LWC_DLQ_KEEP);
    CHECK(lwc_dlq_tick(&q, 6000, TO, &done, &why) == LWC_DLQ_SEND);
    lwc_dlq_sent_c(&q, 61, 201, 6000, 7000000U);
    lwc_dlq_tx_result(&q, 61, true);
    /* confirmed: an uplink that left the device before the downlink ended settles nothing, and it is not offered */
    CHECK(lwc_dlq_uplink(&q, true, false, 6900000U, &done) == LWC_DLQ_KEEP && q.n == 1U && lwc_dlq_head(&q)->sent);
    CHECK(lwc_dlq_tick(&q, 7000, TO, &done, &why) == LWC_DLQ_KEEP);
    /* the wrap of the concentrator counter is handled */
    lwc_dlq_item_t *h = &q.item[0];
    h->end_tmst = 0xFFFFFF00U;
    CHECK(lwc_dlq_uplink(&q, true, false, 0xFFFFFE00U, &done) == LWC_DLQ_KEEP);
    CHECK(lwc_dlq_uplink(&q, true, true, 0x00000100U, &done) == LWC_DLQ_ACKED && strcmp(done.id, "c-c") == 0);

    /* confirmed, no uplink in time: TIMEOUT with its own reason */
    CHECK(lwc_dlq_push(&q, &c));
    CHECK(lwc_dlq_tick(&q, 20000, TO, &done, &why) == LWC_DLQ_SEND);
    lwc_dlq_sent_c(&q, 62, 202, 20000, 100U);
    lwc_dlq_tx_result(&q, 62, true);
    CHECK(lwc_dlq_tick(&q, 20000 + TO - 1, TO, &done, &why) == LWC_DLQ_KEEP);
    CHECK(lwc_dlq_tick(&q, 20000 + TO, TO, &done, &why) == LWC_DLQ_TIMEOUT && why != NULL && q.n == 0U);
    /* a later uplink with an ACK means nothing now */
    CHECK(lwc_dlq_uplink(&q, true, true, 500U, &done) == LWC_DLQ_KEEP);

    /* refused: tried again after a growing pause, dropped after 5 refusals */
    memset(&q, 0, sizeof q);
    CHECK(lwc_dlq_push(&q, &u));
    int64_t t = 100000;
    for (unsigned i = 1; i <= LWC_DLQ_C_REFUSALS; i++) {
        CHECK(lwc_dlq_tick(&q, t, TO, &done, &why) == LWC_DLQ_SEND);
        lwc_dlq_sent_c(&q, (uint16_t)(70U + i), 300U + i, t, 0);
        lwc_dlq_tx_result(&q, (uint16_t)(70U + i), false);
        const lwc_dlq_ev_t ev = lwc_dlq_tick(&q, t + 1, TO, &done, &why);
        if (i < LWC_DLQ_C_REFUSALS) {
            CHECK(ev == LWC_DLQ_KEEP && !lwc_dlq_head(&q)->sent && lwc_dlq_head(&q)->tx == 0U);
            /* the pause counts from the refusal, and grows with each one */
            const int64_t next = t + 1 + (int64_t)i * LWC_DLQ_C_GAP_MS;
            CHECK(lwc_dlq_tick(&q, next - 1, TO, &done, &why) == LWC_DLQ_KEEP);
            t = next;
        } else {
            CHECK(ev == LWC_DLQ_DROPPED && why != NULL && q.n == 0U);
        }
    }

    /* no answer from the radio: dropped, never sent again */
    CHECK(lwc_dlq_push(&q, &c));
    t = 500000;
    CHECK(lwc_dlq_tick(&q, t, TO, &done, &why) == LWC_DLQ_SEND);
    lwc_dlq_sent_c(&q, 80, 400, t, 0);
    CHECK(lwc_dlq_tick(&q, t + LWC_DLQ_C_RESULT_MS, TO, &done, &why) == LWC_DLQ_KEEP);
    CHECK(lwc_dlq_tick(&q, t + LWC_DLQ_C_RESULT_MS + 1, TO, &done, &why) == LWC_DLQ_DROPPED && why != NULL &&
          q.n == 0U);

    /* the device's class A windows hold the next send */
    CHECK(lwc_dlq_push(&q, &u));
    lwc_dlq_hold(&q, 700000);
    lwc_dlq_hold(&q, 600000); /* never shortened */
    CHECK(lwc_dlq_tick(&q, 699999, TO, &done, &why) == LWC_DLQ_KEEP);
    CHECK(lwc_dlq_tick(&q, 700000, TO, &done, &why) == LWC_DLQ_SEND);
    /* a send that could not be made waits */
    lwc_dlq_defer(&q, 701000);
    CHECK(lwc_dlq_tick(&q, 700500, TO, &done, &why) == LWC_DLQ_KEEP);
    CHECK(lwc_dlq_tick(&q, 701000, TO, &done, &why) == LWC_DLQ_SEND);

    /* a head sent in a class A answer is left to the uplinks */
    lwc_dlq_sent(&q, 90, 500);
    CHECK(lwc_dlq_tick(&q, 10000000, TO, &done, &why) == LWC_DLQ_KEEP && q.n == 1U);
    /* and a class C head the radio refused is offered in the class A answer */
    CHECK(lwc_dlq_uplink(&q, true, false, 0, &done) == LWC_DLQ_DELIVERED);
    CHECK(lwc_dlq_push(&q, &u));
    lwc_dlq_sent_c(&q, 91, 501, 800000, 0xF0000000U);
    lwc_dlq_tx_result(&q, 91, false);
    CHECK(lwc_dlq_uplink(&q, true, false, 0xE0000000U, &done) == LWC_DLQ_KEEP && !lwc_dlq_head(&q)->sent);
}

int main(void)
{
    test_ids();
    test_push();
    test_unconfirmed();
    test_confirmed();
    test_class_c();
    if (failures != 0) {
        fprintf(stderr, "test_dlq: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_dlq: all passed\n");
    return 0;
}
