/* libFuzzer target: MQTT downlink topics and messages are untrusted. */
#include "lwc/mqtt.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    lwc_mqtt_down_t d;
    const char *why = lwc_mqtt_down_parse((const char *)data, size, &d);
    if (why == NULL && d.object == NULL && (d.port < 1U || d.port > 223U || d.len > sizeof d.data)) {
        __builtin_trap();
    }
    /* an object is one span inside the input, starting with '{', at most 1 KB, port 0 or 1..223 */
    if (why == NULL && d.object != NULL &&
        (d.object < (const char *)data || d.object + d.object_len > (const char *)data + size || d.object_len < 2U ||
         d.object_len > LWC_MQTT_OBJECT_MAX || d.object[0] != '{' || d.port > 223U || d.len != 0U)) {
        __builtin_trap();
    }
    /* whatever id was read goes into a result: it must always make valid output */
    char out[512];
    const lwc_mqtt_result_t r = {.node = "70b3d57ed0001234", .dev_eui = 1, .id = d.id,
                                 .state = why != NULL ? LWC_MQTT_DL_REFUSED : LWC_MQTT_DL_QUEUED, .reason = why,
                                 .port = why != NULL ? -1 : d.port, .len = why != NULL ? -1 : (int)d.len,
                                 .time_unix_s = -1};
    if (d.id[LWC_DLQ_ID_MAX] != '\0' || (d.id[0] != '\0' && !lwc_dlq_id_ok(d.id, strlen(d.id))) ||
        lwc_mqtt_result_json(&r, out, sizeof out) == 0) {
        __builtin_trap();
    }
    uint64_t eui;
    (void)lwc_mqtt_down_topic((const char *)data, size, "gw_1", &eui);
    return 0;
}
