#include "lwc/ha.h"

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
    CHECK(lwc_ha_id_ok("a"));
    CHECK(lwc_ha_id_ok("node_01"));
    CHECK(lwc_ha_id_ok("70b3d57ed0001234"));
    CHECK(!lwc_ha_id_ok(""));
    CHECK(!lwc_ha_id_ok(NULL));
    CHECK(!lwc_ha_id_ok("Node"));
    CHECK(!lwc_ha_id_ok("a/b"));
    CHECK(!lwc_ha_id_ok("a+"));
    CHECK(!lwc_ha_id_ok("#"));
    CHECK(!lwc_ha_id_ok("a-b"));
    CHECK(!lwc_ha_id_ok("a b"));
    CHECK(lwc_ha_id_ok("0123456789012345678901234567890"));   /* 31 */
    CHECK(!lwc_ha_id_ok("01234567890123456789012345678901")); /* 32 */
    char id[17];
    lwc_ha_node_id(0x70B3D57ED0001234ULL, id);
    CHECK(strcmp(id, "70b3d57ed0001234") == 0);
    lwc_ha_node_id(1, id);
    CHECK(strcmp(id, "0000000000000001") == 0 && lwc_ha_id_ok(id));
}

static void test_time(void)
{
    char t[21];
    lwc_ha_iso8601(0, t);
    CHECK(strcmp(t, "1970-01-01T00:00:00Z") == 0);
    lwc_ha_iso8601(951782400, t); /* leap day */
    CHECK(strcmp(t, "2000-02-29T00:00:00Z") == 0);
    lwc_ha_iso8601(1795515300, t);
    CHECK(strcmp(t, "2026-11-24T10:15:00Z") == 0);
    lwc_ha_iso8601(4107542399LL, t);
    CHECK(strcmp(t, "2100-02-28T23:59:59Z") == 0);
}

static void test_topics(void)
{
    char t[LWC_HA_TOPIC_MAX];
    CHECK(lwc_ha_topic(t, sizeof t, "gw1", NULL, "status") && strcmp(t, "lora/gw1/status") == 0);
    CHECK(lwc_ha_topic(t, sizeof t, "gw1", "70b3d57ed0001234", "up") && strcmp(t, "lora/gw1/70b3d57ed0001234/up") == 0);
    CHECK(!lwc_ha_topic(t, sizeof t, "gw/1", NULL, "status"));
    CHECK(!lwc_ha_topic(t, sizeof t, "gw1", "#", "up"));
    CHECK(!lwc_ha_topic(t, 10, "gw1", "70b3d57ed0001234", "up"));
}

static void test_up(void)
{
    const uint8_t pl[] = {0x05, 0x6c, 0x6f};
    lwc_ha_up_t u = {.node = "70b3d57ed0001234", .name = "desk \"fipy\"\\\n", .dev_eui = 0x70B3D57ED0001234ULL,
                     .dev_addr = 1, .fcnt = 1234, .port = 2, .confirmed = true, .data = pl, .len = sizeof pl,
                     .rssi = -97, .snr = 7.5f, .sf = 7, .bw_khz = 125, .freq_hz = 868100000, .time_unix_s = 1795515300};
    char out[512];
    size_t n = lwc_ha_up_json(&u, out, sizeof out);
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
    n = lwc_ha_up_json(&u, out, sizeof out);
    CHECK(n > 0 && valid_json(out, n));
    CHECK(strstr(out, "\"port\"") == NULL && strstr(out, "\"ts\"") == NULL && strstr(out, "\"payload_hex\":\"\"") != NULL);
    /* too small: 0, never a cut message */
    CHECK(lwc_ha_up_json(&u, out, 40) == 0);
    /* bad node id */
    u.node = "a/b";
    CHECK(lwc_ha_up_json(&u, out, sizeof out) == 0);
    /* largest payload fits the firmware's buffer */
    static uint8_t big[242];
    u.node = "70b3d57ed0001234";
    u.name = "abcdefghijklmnopqrstuvw";
    u.data = big;
    u.len = sizeof big;
    u.port = 223;
    u.time_unix_s = 1795515300;
    n = lwc_ha_up_json(&u, out, sizeof out);
    CHECK(n == 0); /* 512 is too small for 242 bytes of hex */
    static char big_out[1024];
    n = lwc_ha_up_json(&u, big_out, sizeof big_out);
    CHECK(n > 0 && valid_json(big_out, n));
    /* join */
    n = lwc_ha_join_json(&u, out, sizeof out);
    CHECK(n > 0 && valid_json(out, n) && strstr(out, "fcnt") == NULL && strstr(out, "\"rssi\":-97") != NULL);
}

static void test_discovery(void)
{
    char topic[LWC_HA_TOPIC_MAX], out[1024];
    size_t n = lwc_ha_sensor_config("gw1", "70b3d57ed0001234", "fipy", LWC_HA_RSSI, 3600, topic, sizeof topic, out,
                                    sizeof out);
    CHECK(n > 0 && valid_json(out, n));
    CHECK(strcmp(topic, "homeassistant/sensor/70b3d57ed0001234_rssi/config") == 0);
    CHECK(strcmp(out, "{\"name\":\"RSSI\",\"unique_id\":\"lora_70b3d57ed0001234_rssi\","
                      "\"state_topic\":\"lora/gw1/70b3d57ed0001234/up\",\"value_template\":\"{{ value_json.rssi }}\","
                      "\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\",\"state_class\":\"measurement\","
                      "\"entity_category\":\"diagnostic\",\"availability_topic\":\"lora/gw1/status\","
                      "\"payload_available\":\"online\",\"payload_not_available\":\"offline\",\"expire_after\":3600,"
                      "\"device\":{\"identifiers\":[\"lora_70b3d57ed0001234\"],\"name\":\"LoRa fipy\","
                      "\"via_device\":\"lora_gw_gw1\",\"manufacturer\":\"Experimental\",\"model\":\"LoRa node\"}}") == 0);
    n = lwc_ha_sensor_config("gw1", "70b3d57ed0001234", "", LWC_HA_SNR, 0, topic, sizeof topic, out, sizeof out);
    CHECK(n > 0 && valid_json(out, n));
    CHECK(strcmp(topic, "homeassistant/sensor/70b3d57ed0001234_snr/config") == 0);
    CHECK(strstr(out, "device_class") == NULL && strstr(out, "expire_after") == NULL);
    CHECK(strstr(out, "\"name\":\"LoRa 70b3d57ed0001234\"") != NULL);
    /* name with quotes is escaped */
    n = lwc_ha_sensor_config("gw1", "70b3d57ed0001234", "a\"b", LWC_HA_SNR, 0, topic, sizeof topic, out, sizeof out);
    CHECK(n > 0 && valid_json(out, n) && strstr(out, "LoRa a\\\"b") != NULL);
    /* topic only (to clear it) */
    CHECK(lwc_ha_sensor_config("gw1", "70b3d57ed0001234", NULL, LWC_HA_SNR, 0, topic, sizeof topic, NULL, 0) == 1);
    CHECK(strcmp(topic, "homeassistant/sensor/70b3d57ed0001234_snr/config") == 0);
    /* refused */
    CHECK(lwc_ha_sensor_config("gw1", "a+b", "x", LWC_HA_RSSI, 0, topic, sizeof topic, out, sizeof out) == 0);
    CHECK(lwc_ha_sensor_config("gw1", "abc", "x", LWC_HA_FIELDS, 0, topic, sizeof topic, out, sizeof out) == 0);
    CHECK(lwc_ha_sensor_config("gw1", "abc", "x", LWC_HA_RSSI, 0, topic, sizeof topic, out, 100) == 0);
    /* gateway */
    n = lwc_ha_gateway_config("gw1", topic, sizeof topic, out, sizeof out);
    CHECK(n > 0 && valid_json(out, n));
    CHECK(strcmp(topic, "homeassistant/binary_sensor/lora_gw_gw1_status/config") == 0);
    CHECK(strstr(out, "\"device_class\":\"connectivity\"") != NULL && strstr(out, "\"identifiers\":[\"lora_gw_gw1\"]") != NULL);
    CHECK(lwc_ha_gateway_config("GW", topic, sizeof topic, out, sizeof out) == 0);
    /* longest ids fit the firmware's topic buffer */
    const char *id31 = "0123456789012345678901234567890";
    CHECK(lwc_ha_gateway_config(id31, topic, sizeof topic, out, sizeof out) > 0);
    CHECK(lwc_ha_sensor_config(id31, id31, "x", LWC_HA_RSSI, 1, topic, sizeof topic, out, sizeof out) > 0);
    CHECK(lwc_ha_topic(topic, sizeof topic, id31, id31, "join"));
}

int main(void)
{
    test_ids();
    test_time();
    test_topics();
    test_up();
    test_discovery();
    if (failures != 0) {
        fprintf(stderr, "test_ha: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_ha: all passed\n");
    return 0;
}
