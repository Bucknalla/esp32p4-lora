/* Software watchdog (ADR-0009). See sv.h. */
#include "sv.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "fault.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwc/supervisor.h"

static const char *TAG = "sv";

#define SV_PERIOD_MS 1000
#define SV_STACK 3072
#define SV_PRIO 6 /* above every core-0 worker, below the radio (core 1) */
#define SV_LOOP_LIMIT 5U
#define SV_STABLE_MS (10U * 60U * 1000U)

static lwc_sv_t s_sv;
static TaskHandle_t s_task[LWC_SV_MAX]; /* for stack reports */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

sv_id_t sv_register(const char *name, uint32_t budget_ms)
{
    taskENTER_CRITICAL(&s_lock);
    const int id = lwc_sv_register(&s_sv, name, budget_ms, now_ms());
    if (id >= 0) {
        s_task[id] = xTaskGetCurrentTaskHandle();
    }
    taskEXIT_CRITICAL(&s_lock);
    if (id < 0) {
        ESP_LOGE(TAG, "cannot watch '%s' (table full)", name);
    } else {
        ESP_LOGI(TAG, "watching '%s' (budget %" PRIu32 " s)", name, budget_ms / 1000U);
    }
    return id;
}

void sv_beat(sv_id_t id)
{
    lwc_sv_beat(&s_sv, id, now_ms());
}

void sv_pause(sv_id_t id)
{
    lwc_sv_pause(&s_sv, id);
}

void sv_delay(sv_id_t id, uint32_t ms)
{
    while (ms > 0U) {
        const uint32_t step = ms > 1000U ? 1000U : ms;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
        sv_beat(id);
    }
}

static void sv_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    bool stable = false;
    for (;;) {
        (void)esp_task_wdt_reset();
        uint32_t late = 0;
        const int id = lwc_sv_expired(&s_sv, now_ms(), &late);
        if (id >= 0) {
            char why[48];
            snprintf(why, sizeof why, "watchdog: %s stalled %" PRIu32 " s", s_sv.e[id].name,
                     late / 1000U);
            if (fault_streak() >= SV_LOOP_LIMIT) {
                /* Rebooting has not helped: keep running, so OTA can fix it. */
                ESP_LOGE(TAG, "%s (abnormal run %" PRIu32 ": not aborting)", why, fault_streak());
                lwc_sv_pause(&s_sv, id);
            } else {
                ESP_LOGE(TAG, "%s: aborting for a coredump", why);
                fault_note(why);
                vTaskDelay(pdMS_TO_TICKS(100)); /* let the log out */
                esp_system_abort(why);
            }
        }
        if (!stable && now_ms() > SV_STABLE_MS) {
            stable = true;
            fault_mark_stable();
        }
        vTaskDelay(pdMS_TO_TICKS(SV_PERIOD_MS));
    }
}

void sv_status_line(char *buf, size_t len)
{
    size_t o = 0;
    buf[0] = '\0';
    for (int i = 0; i < LWC_SV_MAX && o < len; i++) {
        if (s_sv.e[i].name == NULL || s_task[i] == NULL) {
            continue;
        }
        const int n = snprintf(buf + o, len - o, "%s%s=%u", o ? " " : "", s_sv.e[i].name,
                               (unsigned)uxTaskGetStackHighWaterMark(s_task[i]));
        if (n < 0) {
            break;
        }
        o += (size_t)n;
    }
}

esp_err_t sv_start(void)
{
    const BaseType_t ok =
        xTaskCreatePinnedToCore(sv_task, "sv", SV_STACK, NULL, SV_PRIO, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
