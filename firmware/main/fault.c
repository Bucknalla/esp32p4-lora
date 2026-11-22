/* Fault record (ADR-0009). See fault.h. */
#include "fault.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "fault";

#define NOTE_MAGIC 0x464E5431U /* "FNT1" */
#define NVS_NS "fault"
#define NVS_KEY "counts"
#define NOTE_LEN 48

/* RTC no-init: kept across software resets, panics and watchdog resets;
 * lost on power-on (then the magic is wrong). */
typedef struct {
    uint32_t magic;
    uint32_t streak; /* abnormal resets in a row */
    char note[NOTE_LEN];
    uint32_t check; /* magic ^ streak ^ sum of note bytes */
} rtc_rec_t;

static RTC_NOINIT_ATTR rtc_rec_t s_rec;

typedef struct {
    uint32_t boots;
    uint32_t panic;    /* panic, incl. supervisor aborts */
    uint32_t watchdog; /* interrupt / task / other watchdog, CPU lockup */
    uint32_t brownout;
    uint32_t software; /* esp_restart */
    uint32_t other;    /* power-on, external pin, ... */
} counts_t;

static counts_t s_counts;
static bool s_abnormal;

static uint32_t rec_check(const rtc_rec_t *r)
{
    uint32_t sum = r->magic ^ r->streak;
    for (size_t i = 0; i < NOTE_LEN; i++) {
        sum += (uint8_t)r->note[i];
    }
    return sum;
}

static void rec_seal(void)
{
    s_rec.magic = NOTE_MAGIC;
    s_rec.check = rec_check(&s_rec);
}

static bool rec_valid(void)
{
    return s_rec.magic == NOTE_MAGIC && s_rec.check == rec_check(&s_rec) &&
           memchr(s_rec.note, '\0', NOTE_LEN) != NULL;
}

void fault_note(const char *why)
{
    if (!rec_valid()) {
        memset(&s_rec, 0, sizeof s_rec);
    }
    memset(s_rec.note, 0, NOTE_LEN);
    strncpy(s_rec.note, why != NULL ? why : "", NOTE_LEN - 1U);
    rec_seal();
}

void fault_restart(const char *why)
{
    fault_note(why);
    ESP_LOGW(TAG, "restart: %s", s_rec.note);
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the log out */
    esp_restart();
}

bool fault_last_reset_abnormal(void)
{
    return s_abnormal;
}

uint32_t fault_streak(void)
{
    return rec_valid() ? s_rec.streak : 0U;
}

void fault_mark_stable(void)
{
    if (rec_valid() && s_rec.streak != 0U) {
        ESP_LOGI(TAG, "stable: abnormal-reset run (%" PRIu32 ") cleared", s_rec.streak);
        s_rec.streak = 0;
        rec_seal();
    }
}

void fault_boot(void)
{
    const esp_reset_reason_t r = esp_reset_reason();
    uint32_t *bucket;
    switch (r) {
    case ESP_RST_PANIC:
        bucket = &s_counts.panic;
        s_abnormal = true;
        break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_CPU_LOCKUP:
        bucket = &s_counts.watchdog;
        s_abnormal = true;
        break;
    case ESP_RST_BROWNOUT:
        bucket = &s_counts.brownout;
        s_abnormal = true;
        break;
    case ESP_RST_SW:
        bucket = &s_counts.software;
        break;
    default:
        bucket = &s_counts.other;
        break;
    }

    const bool kept = rec_valid();
    if (kept && s_rec.note[0] != '\0') {
        ESP_LOGW(TAG, "previous boot ended: %s", s_rec.note);
    }
    if (!kept) {
        memset(&s_rec, 0, sizeof s_rec);
    }
    s_rec.note[0] = '\0';
    memset(s_rec.note, 0, NOTE_LEN);
    s_rec.streak = s_abnormal ? s_rec.streak + 1U : 0U;
    rec_seal();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        size_t n = sizeof s_counts;
        counts_t c = {0};
        if (nvs_get_blob(h, NVS_KEY, &c, &n) == ESP_OK && n == sizeof c) {
            const uint32_t keep_bucket_offset = (uint32_t)((uint8_t *)bucket - (uint8_t *)&s_counts);
            s_counts = c;
            bucket = (uint32_t *)((uint8_t *)&s_counts + keep_bucket_offset);
        }
        s_counts.boots++;
        (*bucket)++;
        if (nvs_set_blob(h, NVS_KEY, &s_counts, sizeof s_counts) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG,
             "boots %" PRIu32 ": panic %" PRIu32 ", watchdog %" PRIu32 ", brownout %" PRIu32
             ", software %" PRIu32 ", other %" PRIu32 "; abnormal run %" PRIu32,
             s_counts.boots, s_counts.panic, s_counts.watchdog, s_counts.brownout,
             s_counts.software, s_counts.other, s_rec.streak);
}

void fault_status_line(char *buf, size_t len)
{
    snprintf(buf, len,
             "boots=%" PRIu32 " panic=%" PRIu32 " wdt=%" PRIu32 " brownout=%" PRIu32
             " sw=%" PRIu32 " run=%" PRIu32,
             s_counts.boots, s_counts.panic, s_counts.watchdog, s_counts.brownout,
             s_counts.software, fault_streak());
}
