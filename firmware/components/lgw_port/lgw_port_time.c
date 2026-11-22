/* Timing and temperature fallback for the libloragw platform layer. */
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lgw_port.h"
#include "lgw_port_esp.h"

/* ---- timing -------------------------------------------------------------- */

#define US_PER_TICK (1000000UL / configTICK_RATE_HZ)

void lgw_port_wait_us(unsigned long delay_us)
{
    if (delay_us == 0) {
        return;
    }
    if (delay_us < US_PER_TICK) {
        esp_rom_delay_us((uint32_t)delay_us);
        return;
    }
    /* Round up, plus one tick: vTaskDelay(n) can return up to a tick early. */
    const TickType_t ticks = (TickType_t)((delay_us + US_PER_TICK - 1) / US_PER_TICK) + 1;
    vTaskDelay(ticks);
}

void lgw_port_wait_ms(unsigned long delay_ms)
{
    if (delay_ms == 0) {
        return;
    }
    const unsigned long ms_per_tick = 1000UL / configTICK_RATE_HZ;
    if (ms_per_tick == 0 || delay_ms < ms_per_tick) {
        lgw_port_wait_us(delay_ms * 1000UL);
        return;
    }
    const TickType_t ticks = (TickType_t)((delay_ms + ms_per_tick - 1) / ms_per_tick) + 1;
    vTaskDelay(ticks);
}

/* ---- temperature fallback ------------------------------------------------ */

static float s_fixed_temperature = 30.0f;

void lgw_port_set_fixed_temperature(float celsius)
{
    s_fixed_temperature = celsius;
}

float lgw_port_fixed_temperature(void)
{
    return s_fixed_temperature;
}
