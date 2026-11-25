#include "lwc/mqtt.h"

#include <stdio.h>
#include <string.h>

#include "lwc/json.h"

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

/* One complete JSON value and nothing after it. */
static int valid_json(const char *s, size_t n)
{
    lwc_jr_t r = {.p = s, .end = s + n};
    if (!lwc_jr_skip(&r)) {
        return 0;
    }
    lwc_jr_ws(&r);
    return r.p == r.end;
}

static void test_ids(void)
{
    CHECK(lwc_mqtt_id_ok("a"));
    CHECK(lwc_mqtt_id_ok("node_01"));
    CHECK(lwc_mqtt_id_ok("70b3d57ed0001234"));
    CHECK(!lwc_mqtt_id_ok(""));
    CHECK(!lwc_mqtt_id_ok(NULL));
    CHECK(!lwc_mqtt_id_ok("Node"));
    CHECK(!lwc_mqtt_id_ok("a/b"));
    CHECK(!lwc_mqtt_id_ok("a+"));
    CHECK(!lwc_mqtt_id_ok("#"));
    CHECK(!lwc_mqtt_id_ok("a-b"));
    CHECK(!lwc_mqtt_id_ok("a b"));
    CHECK(lwc_mqtt_id_ok("0123456789012345678901234567890"));   /* 31 */
    CHECK(!lwc_mqtt_id_ok("01234567890123456789012345678901")); /* 32 */
    char id[17];
    lwc_mqtt_node_id(0x70B3D57ED0001234ULL, id);
    CHECK(strcmp(id, "70b3d57ed0001234") == 0);
    lwc_mqtt_node_id(1, id);
    CHECK(strcmp(id, "0000000000000001") == 0 && lwc_mqtt_id_ok(id));
}

static void test_time(void)
{
    char t[21];
    lwc_mqtt_iso8601(0, t);
    CHECK(strcmp(t, "1970-01-01T00:00:00Z") == 0);
    lwc_mqtt_iso8601(951782400, t); /* leap day */
    CHECK(strcmp(t, "2000-02-29T00:00:00Z") == 0);
    lwc_mqtt_iso8601(1795515300, t);
    CHECK(strcmp(t, "2026-11-24T10:15:00Z") == 0);
    lwc_mqtt_iso8601(4107542399LL, t);
    CHECK(strcmp(t, "2100-02-28T23:59:59Z") == 0);
}

static void test_topics(void)
{
    char t[LWC_MQTT_TOPIC_MAX];
    CHECK(lwc_mqtt_topic(t, sizeof t, "gw1", NULL, "status") && strcmp(t, "lora/gw1/status") == 0);
    CHECK(lwc_mqtt_topic(t, sizeof t, "gw1", "70b3d57ed0001234", "up") && strcmp(t, "lora/gw1/70b3d57ed0001234/up") == 0);
    CHECK(!lwc_mqtt_topic(t, sizeof t, "gw/1", NULL, "status"));
    CHECK(!lwc_mqtt_topic(t, sizeof t, "gw1", "#", "up"));
    CHECK(!lwc_mqtt_topic(t, 10, "gw1", "70b3d57ed0001234", "up"));
}

static void test_up(void)
{
    const uint8_t pl[] = {0x05, 0x6c, 0x6f};
    lwc_mqtt_up_t u = {.node = "70b3d57ed0001234", .name = "desk \"fipy\"\\\n", .dev_eui = 0x70B3D57ED0001234ULL,
                     .dev_addr = 1, .fcnt = 1234, .port = 2, .confirmed = true, .data = pl, .len = sizeof pl,
                     .rssi = -97, .snr = 7.5f, .sf = 7, .bw_khz = 125, .freq_hz = 868100000, .time_unix_s = 1795515300};
    char out[512];
    size_t n = lwc_mqtt_up_json(&u, out, sizeof out);
    CHECK(n > 0 && n == strlen(out));
    CHECK(valid_json(out, n));
    CHECK(strcmp(out, "{\"node_id\":\"70b3d57ed0001234\",\"name\":\"desk \\\"fipy\\\"\\\\\\u000a\","
                      "\"dev_eui\":\"70B3D57ED0001234\",\"dev_addr\":\"00000001\",\"fcnt\":1234,\"port\":2,"
                      "\"confirmed\":true,\"payload_hex\":\"056C6F\",\"rssi\":-97,\"snr\":7.5,\"sf\":7,"
                      "\"bw_khz\":125,\"freq_hz\":868100000,\"ts\":\"2026-11-24T10:15:00Z\"}") == 0);
    /* no port, no payload, no clock */
    u.port = -1;
    u.len = 0;
    u.time_unix_s = -1;
    u.name = "";
    n = lwc_mqtt_up_json(&u, out, sizeof out);
    CHECK(n > 0 && valid_json(out, n));
    CHECK(strstr(out, "\"port\"") == NULL && strstr(out, "\"ts\"") == NULL && strstr(out, "\"payload_hex\":\"\"") != NULL);
    /* too small: 0, never a cut message */
    CHECK(lwc_mqtt_up_json(&u, out, 40) == 0);
    /* bad node id */
    u.node = "a/b";
    CHECK(lwc_mqtt_up_json(&u, out, sizeof out) == 0);
    /* largest payload fits the firmware's buffer */
    static uint8_t big[242];
    u.node = "70b3d57ed0001234";
    u.name = "abcdefghijklmnopqrstuvw";
    u.data = big;
    u.len = sizeof big;
    u.port = 223;
    u.time_unix_s = 1795515300;
    n = lwc_mqtt_up_json(&u, out, sizeof out);
    CHECK(n == 0); /* 512 is too small for 242 bytes of hex */
    static char big_out[1024];
    n = lwc_mqtt_up_json(&u, big_out, sizeof big_out);
    CHECK(n > 0 && valid_json(big_out, n));
    /* join */
    n = lwc_mqtt_join_json(&u, out, sizeof out);
    CHECK(n > 0 && valid_json(out, n) && strstr(out, "fcnt") == NULL && strstr(out, "\"rssi\":-97") != NULL);
}

static void test_long_ids(void)
{
    /* longest ids fit the firmware's topic buffer */
    char topic[LWC_MQTT_TOPIC_MAX];
    const char *id31 = "0123456789012345678901234567890";
    CHECK(lwc_mqtt_topic(topic, sizeof topic, id31, id31, "join"));
    CHECK(lwc_mqtt_topic(topic, sizeof topic, id31, NULL, "status"));
}

static const char *down(const char *json, lwc_mqtt_down_t *d)
{
    return lwc_mqtt_down_parse(json, strlen(json), d);
}

static void test_down(void)
{
    uint64_t eui = 0;
    const char *t = "lora/gw1/70b3d57ed0001234/down";
    CHECK(lwc_mqtt_down_topic(t, strlen(t), "gw1", &eui) && eui == 0x70B3D57ED0001234ULL);
    t = "lora/gw1/70B3D57ED0001234/down";
    CHECK(lwc_mqtt_down_topic(t, strlen(t), "gw1", &eui));
    CHECK(!lwc_mqtt_down_topic(t, strlen(t), "gw2", &eui));
    CHECK(!lwc_mqtt_down_topic(t, strlen(t) - 1U, "gw1", &eui));
    t = "lora/gw1/70b3d57ed000123/down";
    CHECK(!lwc_mqtt_down_topic(t, strlen(t), "gw1", &eui));
    t = "lora/gw1/70b3d57ed000123x/down";
    CHECK(!lwc_mqtt_down_topic(t, strlen(t), "gw1", &eui));
    t = "lora/gw1/70b3d57ed0001234/up";
    CHECK(!lwc_mqtt_down_topic(t, strlen(t), "gw1", &eui));
    t = "lora/gw10/70b3d57ed0001234/down";
    CHECK(!lwc_mqtt_down_topic(t, strlen(t), "gw1", &eui));
    CHECK(!lwc_mqtt_down_topic(t, strlen(t), "GW10", &eui));

    lwc_mqtt_down_t d;
    CHECK(down("{\"port\":10,\"payload_hex\":\"01aB\"}", &d) == NULL && d.port == 10 && d.len == 2 &&
          d.data[0] == 0x01 && d.data[1] == 0xAB && !d.confirmed);
    CHECK(down(" { \"confirmed\" : false , \"x\":[1,{\"y\":null}], \"payload_hex\":\"\", \"port\":223 } ", &d) ==
              NULL && d.port == 223 && d.len == 0);
    CHECK(down("{\"port\":1,\"payload_hex\":\"00\",\"confirmed\":true}", &d) == NULL && d.confirmed);
    char big[600];
    int n = snprintf(big, sizeof big, "{\"port\":2,\"payload_hex\":\"");
    for (int i = 0; i < 242; i++) {
        n += snprintf(big + n, sizeof big - (size_t)n, "%02x", i);
    }
    (void)snprintf(big + n, sizeof big - (size_t)n, "\"}");
    CHECK(down(big, &d) == NULL && d.len == 242 && d.data[241] == 241);
    big[n] = '0'; /* 243 bytes, quote moved */
    (void)snprintf(big + n, sizeof big - (size_t)n, "00\"}");
    CHECK(down(big, &d) != NULL);

    CHECK(down("{\"port\":0,\"payload_hex\":\"00\"}", &d) != NULL);
    CHECK(down("{\"port\":224,\"payload_hex\":\"00\"}", &d) != NULL);
    CHECK(down("{\"port\":1.5,\"payload_hex\":\"00\"}", &d) != NULL);
    CHECK(down("{\"port\":\"1\",\"payload_hex\":\"00\"}", &d) != NULL);
    CHECK(down("{\"port\":1,\"payload_hex\":\"0\"}", &d) != NULL);
    CHECK(down("{\"port\":1,\"payload_hex\":\"zz\"}", &d) != NULL);
    CHECK(down("{\"port\":1,\"payload_hex\":\"00\",\"confirmed\":1}", &d) != NULL);
    CHECK(down("{\"payload_hex\":\"00\"}", &d) != NULL);
    CHECK(down("{\"port\":1}", &d) != NULL);
    CHECK(down("{\"port\":1,\"payload_hex\":\"00\"} x", &d) != NULL);
    CHECK(down("{\"port\":1,\"payload_hex\":\"00\",}", &d) != NULL);
    CHECK(down("[1]", &d) != NULL && down("", &d) != NULL && lwc_mqtt_down_parse(NULL, 0, &d) != NULL);
    CHECK(lwc_mqtt_down_parse("{\"port\":1,\"payload_hex\":\"00\"}", 10, &d) != NULL); /* cut short */

    /* id: optional, echoed in results; kept when a later field is wrong */
    CHECK(down("{\"id\":\"3f2504e0-4f89-11d3-9a0c-0305e82c3301\",\"port\":4,\"payload_hex\":\"01\",\"confirmed\":true}",
               &d) == NULL &&
          strcmp(d.id, "3f2504e0-4f89-11d3-9a0c-0305e82c3301") == 0 && d.confirmed && d.port == 4U);
    CHECK(down("{\"port\":4,\"payload_hex\":\"01\"}", &d) == NULL && d.id[0] == '\0');
    CHECK(down("{\"id\":\"job.7\",\"port\":0,\"payload_hex\":\"01\"}", &d) != NULL && strcmp(d.id, "job.7") == 0);
    /* id after the wrong field is still read; the first wrong field is reported */
    CHECK(down("{\"port\":0,\"payload_hex\":\"0\",\"id\":\"late\"}", &d) != NULL && strcmp(d.id, "late") == 0);
    CHECK(strcmp(down("{\"port\":0,\"payload_hex\":\"0\",\"id\":\"late\"}", &d), "port: 1 to 223") == 0);
    CHECK(down("{\"port\":\"x\",\"confirmed\":\"no\",\"payload_hex\":7,\"id\":\"k\"}", &d) != NULL &&
          strcmp(d.id, "k") == 0);
    /* broken JSON still stops at once */
    CHECK(down("{\"port\":0,\"id\":\"late\"", &d) != NULL);
    CHECK(down("{\"id\":\"\",\"port\":4,\"payload_hex\":\"01\"}", &d) != NULL);
    CHECK(down("{\"id\":\"a b\",\"port\":4,\"payload_hex\":\"01\"}", &d) != NULL && d.id[0] == '\0');
    CHECK(down("{\"id\":\"a\\\"b\",\"port\":4,\"payload_hex\":\"01\"}", &d) != NULL);
    CHECK(down("{\"id\":7,\"port\":4,\"payload_hex\":\"01\"}", &d) != NULL);
    CHECK(down("{\"id\":\"0123456789012345678901234567890123456\",\"port\":4,\"payload_hex\":\"01\"}", &d) != NULL);
}

static void test_result(void)
{
    char out[512], t[LWC_MQTT_TOPIC_MAX];
    CHECK(lwc_mqtt_topic(t, sizeof t, "gw1", "70b3d57ed0001234", "down/result") &&
          strcmp(t, "lora/gw1/70b3d57ed0001234/down/result") == 0);
    uint64_t eui;
    CHECK(!lwc_mqtt_down_topic(t, strlen(t), "gw1", &eui)); /* never read back as a downlink */
    lwc_mqtt_result_t r = {.node = "70b3d57ed0001234", .dev_eui = 0x70B3D57ED0001234ULL, .id = "valve-1",
                           .state = LWC_MQTT_DL_ACKED, .port = 3, .len = 2, .confirmed = true, .has_fcnt = true,
                           .fcnt_down = 103, .tx = 2, .time_unix_s = 0};
    size_t n = lwc_mqtt_result_json(&r, out, sizeof out);
    CHECK(n > 0 && valid_json(out, n));
    CHECK(strcmp(out, "{\"node_id\":\"70b3d57ed0001234\",\"dev_eui\":\"70B3D57ED0001234\",\"id\":\"valve-1\","
                      "\"status\":\"acked\",\"port\":3,\"len\":2,\"confirmed\":true,\"fcnt_down\":103,\"tx\":2,"
                      "\"ts\":\"1970-01-01T00:00:00Z\"}") == 0);
    /* a refusal of a message that did not parse: no port, a reason with quotes escaped */
    r = (lwc_mqtt_result_t){.node = "70b3d57ed0001234", .dev_eui = 1, .state = LWC_MQTT_DL_REFUSED,
                            .reason = "bad \"JSON\"", .port = -1, .len = -1, .time_unix_s = -1};
    n = lwc_mqtt_result_json(&r, out, sizeof out);
    CHECK(n > 0 && valid_json(out, n) && strstr(out, "\"port\"") == NULL && strstr(out, "\"id\"") == NULL);
    CHECK(strstr(out, "\"status\":\"refused\",\"reason\":\"bad \\\"JSON\\\"\"") != NULL);
    static const char *const names[] = {"queued", "refused", "sent", "acked", "nacked", "dropped"};
    for (int s = 0; s < (int)LWC_MQTT_DL_STATES; s++) {
        r.state = (lwc_mqtt_dl_state_t)s;
        char want[32];
        (void)snprintf(want, sizeof want, "\"status\":\"%s\"", names[s]);
        n = lwc_mqtt_result_json(&r, out, sizeof out);
        CHECK(n > 0 && strstr(out, want) != NULL);
    }
    /* refused */
    r.state = LWC_MQTT_DL_STATES;
    CHECK(lwc_mqtt_result_json(&r, out, sizeof out) == 0);
    r.state = LWC_MQTT_DL_SENT;
    r.id = "a b";
    CHECK(lwc_mqtt_result_json(&r, out, sizeof out) == 0);
    r.id = NULL;
    r.node = "A";
    CHECK(lwc_mqtt_result_json(&r, out, sizeof out) == 0);
    r.node = "70b3d57ed0001234";
    CHECK(lwc_mqtt_result_json(&r, out, 20) == 0);
}

int main(void)
{
    test_ids();
    test_time();
    test_topics();
    test_up();
    test_long_ids();
    test_down();
    test_result();
    if (failures != 0) {
        fprintf(stderr, "test_mqtt: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_mqtt: all passed\n");
    return 0;
}
