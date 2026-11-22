/* Log copy in PSRAM (ADR-0009). See logbuf.h. */
#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwc/logring.h"

static const char *TAG = "logbuf";

#define RING_BYTES (64 * 1024)
#define PREV_BYTES (8 * 1024)
#define PRINT_BYTES 3072 /* printed at boot after an abnormal reset */
#define LINE_MAX_LEN 256

/* Measured on the bench board (P4 v3.2, 2026-11-22): after a panic reset the
 * first bytes at the start of PSRAM (0x48000000, where .ext_ram_noinit
 * begins) were overwritten during boot (likely MSPI PSRAM timing tuning,
 * which writes a test pattern there); the rest was kept. So the ring starts
 * GUARD_BYTES in. */
#define GUARD_BYTES 4096
static EXT_RAM_NOINIT_ATTR _Alignas(64) uint8_t s_mem[GUARD_BYTES + RING_BYTES];
static lwc_logring_t s_ring;
static char *s_prev = (char *)"";
static size_t s_prev_len;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_orig;
static char s_line[LINE_MAX_LEN];

/* Push the written bytes from cache to PSRAM, so a reset does not lose them. */
static void sync_range(const void *p, size_t n)
{
    if (n > 0U) {
        (void)esp_cache_msync((void *)p, n,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
}

static void ring_append(const char *s, size_t n)
{
    const uint32_t before = s_ring.hdr->head;
    lwc_logring_write(&s_ring, s, n);
    const uint32_t after = s_ring.hdr->head;
    if (n >= s_ring.size) {
        sync_range(s_ring.data, s_ring.size);
    } else if (after >= before) {
        sync_range(&s_ring.data[before], after - before);
    } else {
        sync_range(&s_ring.data[before], s_ring.size - before);
        sync_range(s_ring.data, after);
    }
    sync_range(s_ring.hdr, sizeof *s_ring.hdr);
}

/* Runs on the calling task's stack, in any task. Never blocks, never logs. */
static int hook(const char *fmt, va_list ap)
{
    if (!xPortInIsrContext() && s_lock != NULL && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        va_list cp;
        va_copy(cp, ap);
        const int n = vsnprintf(s_line, sizeof s_line, fmt, cp);
        va_end(cp);
        if (n > 0) {
            ring_append(s_line, (size_t)n < sizeof s_line ? (size_t)n : sizeof s_line - 1U);
        }
        xSemaphoreGive(s_lock);
    }
    return s_orig(fmt, ap);
}

void logbuf_init(void)
{
    const bool kept = lwc_logring_attach(&s_ring, &s_mem[GUARD_BYTES], RING_BYTES);
    if (kept) {
        char *p = heap_caps_malloc(PREV_BYTES, MALLOC_CAP_SPIRAM);
        if (p != NULL) {
            s_prev_len = lwc_logring_tail(&s_ring, p, PREV_BYTES);
            s_prev = p;
        }
    }
    lwc_logring_reset(&s_ring);
    sync_range(s_ring.hdr, sizeof *s_ring.hdr);
    s_lock = xSemaphoreCreateMutex();
    if (s_lock != NULL) {
        s_orig = esp_log_set_vprintf(hook);
    }
    ESP_LOGI(TAG, "log ring %u KB in PSRAM; previous boot: %u bytes kept", RING_BYTES / 1024,
             (unsigned)s_prev_len);
}

void logbuf_print_previous(void)
{
    if (s_prev_len == 0U) {
        return;
    }
    const char *start = s_prev;
    if (s_prev_len > PRINT_BYTES) {
        start = s_prev + s_prev_len - PRINT_BYTES;
        const char *nl = strchr(start, '\n');
        start = nl != NULL ? nl + 1 : start;
    }
    /* Plain printf: these lines must not go back into the ring. */
    printf("---- previous boot, last lines ----\n%s---- end of previous boot ----\n", start);
}

const char *logbuf_previous(void)
{
    return s_prev;
}
