/* Status LED and HAT button (see led.h). One low-priority task owns both pins. */
#include "led.h"
#include "ble_prov.h"

#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "eth.h"
#include "gwmode.h"
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
#define BUTTON_HOLD_MS 5000U /* hold the HAT button this long: BLE setup */

static bool s_fwd; /* read once: the mode applies after a restart anyway */
static _Atomic lwc_led_state_t s_state = LWC_LED_BOOT;

static lwc_led_state_t pick(uint32_t now)
{
    static unsigned last_restarts;
    static uint32_t fault_until;
    if (ble_prov_active()) {
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
    const bool upstream_down = settled && s_fwd && !pktfwd_link_up();
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
    lwc_hold_t hold = {0};
    bool was_down = false;
    for (;;) {
        sv_beat(sv);
        const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const bool down = gpio_get_level(CONFIG_GW_BUTTON_GPIO) == 0; /* to GND, pull-up */
        if (down != was_down) {
            was_down = down;
            ESP_LOGI(TAG, "button %s", down ? "pressed" : "released");
        }
        if (lwc_hold_update(&hold, down, now, BUTTON_HOLD_MS)) {
            ble_prov_open("button held 5 s");
        }
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
    const gpio_config_t btn = {.pin_bit_mask = 1ULL << CONFIG_GW_BUTTON_GPIO,
                               .mode = GPIO_MODE_INPUT,
                               .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "button gpio");
    s_fwd = gwmode_forwards();
    const BaseType_t ok = xTaskCreatePinnedToCore(led_task, "led", 3072, NULL, 1, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
