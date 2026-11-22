/*
 * Status LED patterns (one LED, on/off). Pure timing: the firmware picks the
 * state; this says whether the LED is lit at time `t_ms`.
 */
#ifndef LWC_LED_H
#define LWC_LED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LWC_LED_BOOT = 0, /* no network yet: fast blink (5 Hz) */
    LWC_LED_SETUP,    /* setup access point open: slow blink (0.5 Hz) */
    LWC_LED_ONLINE,   /* all good: steady on */
    LWC_LED_FAULT,    /* needs attention: double blink every second */
} lwc_led_state_t;

#define LWC_LED_FLASH_MS 150U /* a downlink inverts the LED this long */

/** LED level for `state` at `t_ms`; `flash_until_ms` (0: none) inverts it
 *  while `t_ms` is before it. */
bool lwc_led_level(lwc_led_state_t state, uint32_t t_ms, uint32_t flash_until_ms);

#ifdef __cplusplus
}
#endif

#endif /* LWC_LED_H */
