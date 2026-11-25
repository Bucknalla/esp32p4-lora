/* libFuzzer target: MQTT downlink topics and messages are untrusted. */
#include "lwc/mqtt.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    lwc_mqtt_down_t d;
    if (lwc_mqtt_down_parse((const char *)data, size, &d) == NULL &&
        (d.port < 1U || d.port > 223U || d.len > sizeof d.data)) {
        __builtin_trap();
    }
    uint64_t eui;
    (void)lwc_mqtt_down_topic((const char *)data, size, "gw_1", &eui);
    return 0;
}
