/* Status LED (see led.h). One low-priority task owns the pin. */
#include "led.h"

#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwc/led.h"
#include "ota.h"
#include "pktfwd.h"
#include "radio_pkt.h"
#include "radio_tx.h"
#include "sdkconfig.h"
#include "sv.h"
#include "wifi.h"

static const char *TAG = "led";

#define TICK_MS 20
#define GRACE_MS 60000U /* radio and upstream get this long after boot */
#define FAULT_HOLD_MS 60000U /* a radio restart shows this long */

static bool s_ttn; /* read once: the setting applies after a restart anyway */
static _Atomic lwc_led_state_t s_state = LWC_LED_BOOT;

static lwc_led_state_t pick(uint32_t now)
{
    static unsigned last_restarts;
    static uint32_t fault_until;
    if (wifi_setup_active()) {
        return LWC_LED_SETUP;
    }
    if (!(wifi_has_ip() || eth_has_ip())) {
        return LWC_LED_BOOT;
    }
    const unsigned restarts = atomic_load(&g_radio_stats.restarts);
    if (restarts != last_restarts) {
        last_restarts = restarts;
        fault_until = now + FAULT_HOLD_MS;
    }
    const bool settled = now > GRACE_MS;
    const bool radio_missing = settled && atomic_load(&g_radio_stats.starts) == 0U;
    const bool upstream_down = settled && s_ttn && !pktfwd_link_up();
    if (ota_pending_verify() || radio_missing || upstream_down ||
        (int32_t)(fault_until - now) > 0) {
        return LWC_LED_FAULT;
    }
    return LWC_LED_ONLINE;
}

static void led_task(void *arg)
{
    (void)arg;
    const sv_id_t sv = sv_register("led", 30000);
    unsigned last_tx = atomic_load(&g_radio_tx_stats.txnb);
    uint32_t flash_until = 0;
    for (;;) {
        sv_beat(sv);
        const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const unsigned tx = atomic_load(&g_radio_tx_stats.txnb);
        if (tx != last_tx) {
            last_tx = tx;
            flash_until = now + LWC_LED_FLASH_MS;
        }
        const lwc_led_state_t st = pick(now);
        atomic_store(&s_state, st);
        (void)gpio_set_level(CONFIG_GW_LED_GPIO, lwc_led_level(st, now, flash_until) ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

const char *led_state(void)
{
    static const char *const names[] = {"boot", "setup", "online", "fault"};
    const lwc_led_state_t st = atomic_load(&s_state);
    return (unsigned)st < 4U ? names[st] : "?";
}

esp_err_t led_start(void)
{
    const gpio_config_t io = {.pin_bit_mask = 1ULL << CONFIG_GW_LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    s_ttn = pktfwd_ttn_enabled();
    const BaseType_t ok = xTaskCreatePinnedToCore(led_task, "led", 3072, NULL, 1, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
