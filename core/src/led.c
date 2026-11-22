#include "lwc/led.h"

bool lwc_led_level(lwc_led_state_t state, uint32_t t_ms, uint32_t flash_until_ms)
{
    bool on;
    switch (state) {
    case LWC_LED_BOOT:
        on = (t_ms % 200U) < 100U;
        break;
    case LWC_LED_SETUP:
        on = (t_ms % 2000U) < 1000U;
        break;
    case LWC_LED_FAULT: {
        const uint32_t p = t_ms % 1000U; /* on 0-100, off, on 200-300, off */
        on = p < 100U || (p >= 200U && p < 300U);
        break;
    }
    case LWC_LED_ONLINE:
    default:
        on = true;
        break;
    }
    const bool flashing = flash_until_ms != 0U && (int32_t)(flash_until_ms - t_ms) > 0;
    return flashing ? !on : on;
}

bool lwc_hold_update(lwc_hold_t *h, bool pressed, uint32_t now_ms, uint32_t hold_ms)
{
    if (!pressed) {
        h->down = false;
        h->fired = false;
        return false;
    }
    if (!h->down) {
        h->down = true;
        h->since_ms = now_ms;
    }
    if (!h->fired && now_ms - h->since_ms >= hold_ms) {
        h->fired = true;
        return true;
    }
    return false;
}
