#include "lwc/led.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

int main(void)
{
    CHECK(lwc_led_level(LWC_LED_BOOT, 0, 0) && !lwc_led_level(LWC_LED_BOOT, 150, 0));
    CHECK(lwc_led_level(LWC_LED_SETUP, 999, 0) && !lwc_led_level(LWC_LED_SETUP, 1000, 0));
    CHECK(lwc_led_level(LWC_LED_ONLINE, 123456, 0));
    /* double blink: on, off, on, long off */
    CHECK(lwc_led_level(LWC_LED_FAULT, 50, 0) && !lwc_led_level(LWC_LED_FAULT, 150, 0));
    CHECK(lwc_led_level(LWC_LED_FAULT, 250, 0) && !lwc_led_level(LWC_LED_FAULT, 600, 0));
    /* downlink flash inverts until its end, also across the counter wrap */
    CHECK(!lwc_led_level(LWC_LED_ONLINE, 1000, 1150) && lwc_led_level(LWC_LED_ONLINE, 1150, 1150));
    CHECK(!lwc_led_level(LWC_LED_ONLINE, 0xFFFFFFF0U, 100U));
    if (failures != 0) {
        fprintf(stderr, "test_led: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_led: all passed\n");
    return 0;
}
