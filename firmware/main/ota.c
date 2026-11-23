/*
 * Firmware update and recovery (ADR-0008). See ota.h.
 *
 * Updates are pulled with esp_https_ota from CONFIG_GW_OTA_URL, a GitHub
 * "latest release" link in the public firmware-only repository. TLS checks
 * the server against the ESP-IDF CA bundle. The image itself must carry a
 * Secure Boot v2 RSA signature from the key whose public part is in the
 * running image's signature block; esp_https_ota_finish() -> esp_ota_end()
 * checks it (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT). TLS protects
 * the download; the signature decides what may run.
 *
 * A check reads only the image header (esp_https_ota_get_img_desc) and
 * installs when the remote app's ELF SHA-256 differs from the running one
 * and from the image that was last rolled back.
 *
 * Rollback: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE. A new image boots once as
 * "pending verify". A reset before it is marked valid makes the bootloader
 * boot the previous image. The health task marks it valid after the checks
 * below hold for HEALTH_STABLE_S, or rolls back when the window ends.
 */
#include "ota.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_core_dump.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "eth.h"
#include "fault.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lbsfwd.h"
#include "radio_pkt.h"
#include "sdkconfig.h"
#include "sv.h"
#include "wifi.h"

static const char *TAG = "ota";

/* Kconfig leaves a bool undefined when it is off. */
#ifndef CONFIG_GW_OTA_TEST_NEVER_CONFIRM
#define CONFIG_GW_OTA_TEST_NEVER_CONFIRM 0
#endif
#ifndef CONFIG_GW_OTA_REQUIRE_RADIO
#define CONFIG_GW_OTA_REQUIRE_RADIO 0
#endif
#ifndef CONFIG_GW_OTA_AUTO_INSTALL
#define CONFIG_GW_OTA_AUTO_INSTALL 0
#endif
#ifndef CONFIG_GW_OTA_HEALTH_REQUIRE_SERVER
#define CONFIG_GW_OTA_HEALTH_REQUIRE_SERVER 0
#endif

#define HEALTH_STABLE_S 30
#define HEALTH_TASK_STACK 4096
#define HEALTH_TASK_PRIO 3
#define OTA_TASK_STACK 10240 /* TLS handshake + RSA-3072 check in esp_ota_end */
#define OTA_TASK_PRIO 2      /* below the forwarder and the radio */
#define PENDING_RETRY_MS 30000
#define HTTP_TIMEOUT_MS 15000
#define HTTP_RX_BUF 4096 /* GitHub sends large headers */
#define HTTP_TX_BUF 2048 /* signed redirect URLs are long */
#define RESTART_DELAY_MS 500
#define SHA_LEN 32
#define WAIT_SLICE_MS 10000
/* Watchdog budget: a check is up to three TLS connects (two redirects) with
 * a 15 s timeout each, plus header reads; downloads beat per chunk. */
#define SV_BUDGET_MS 180000

static TaskHandle_t s_ota_task;
static sv_id_t s_sv = -1;
static atomic_bool s_forced; /* OTA_CHECK: install even with auto-install off */
static atomic_bool s_health_ok; /* this boot's image passed or was already valid */
static atomic_bool s_server_ok; /* a check reached the update server this boot */
static uint8_t s_failed_sha[SHA_LEN]; /* image that failed to install this boot */
static bool s_have_failed;
static char s_last[48] = "not checked yet"; /* written by the OTA task only */

/* ---- helpers ------------------------------------------------------------ */

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default: return "unknown";
    }
}

static const char *img_state_str(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined";
    default: return "?";
    }
}

static const char *running_state_str(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) {
        return "factory-flashed"; /* USB flash: no otadata entry yet */
    }
    return img_state_str(st);
}

bool ota_pending_verify(void)
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void ip_of(const char *ifkey, char *buf, size_t len)
{
    esp_netif_ip_info_t ip = {0};
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(ifkey);
    if (n == NULL || esp_netif_get_ip_info(n, &ip) != ESP_OK || ip.ip.addr == 0) {
        snprintf(buf, len, "-");
        return;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
}

/* ---- boot report -------------------------------------------------------- */

static void report_coredump(void)
{
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }
    esp_core_dump_summary_t *sum = heap_caps_calloc(1, sizeof *sum, MALLOC_CAP_DEFAULT);
    if (sum == NULL) {
        return;
    }
    if (esp_core_dump_get_summary(sum) == ESP_OK) {
        ESP_LOGW(TAG, "stored coredump: task '%.*s' pc 0x%08" PRIx32 " mcause 0x%" PRIx32
                      " mtval 0x%" PRIx32 " (decode: idf.py coredump-info)",
                 (int)sizeof sum->exc_task, sum->exc_task, sum->exc_pc, sum->ex_info.mcause,
                 sum->ex_info.mtval);
    } else {
        ESP_LOGW(TAG, "stored coredump present (summary unreadable)");
    }
    heap_caps_free(sum);
}

void ota_boot_report(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI(TAG, "boot: reset reason %s; running %s (%s) at 0x%" PRIx32 ", version %s",
             reset_reason_str(esp_reset_reason()), run->label, running_state_str(),
             run->address, d->version);

    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (bad != NULL) {
        esp_app_desc_t bd;
        const bool have = esp_ota_get_partition_description(bad, &bd) == ESP_OK;
        ESP_LOGW(TAG, "image in %s (%s) was rolled back", bad->label, have ? bd.version : "?");
    }
    report_coredump();
}


/* ---- update check ------------------------------------------------------- */

static void set_last(const char *fmt, const char *arg)
{
    snprintf(s_last, sizeof s_last, fmt, arg);
}

static void sha_hex(const uint8_t *sha, char *out) /* first 8 bytes, 16 hex digits */
{
    for (int i = 0; i < 8; i++) {
        sprintf(out + 2 * i, "%02x", sha[i]);
    }
}

static bool rolled_back_before(const uint8_t *sha)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t bd;
    return bad != NULL && esp_ota_get_partition_description(bad, &bd) == ESP_OK &&
           memcmp(bd.app_elf_sha256, sha, SHA_LEN) == 0;
}

/* One check. Installs and restarts when allowed and the image is new. */
static void check_once(bool may_install)
{
    const esp_http_client_config_t http = {
        .url = CONFIG_GW_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_RX_BUF,
        .buffer_size_tx = HTTP_TX_BUF,
        .max_redirection_count = 5,
        .keep_alive_enable = true,
    };
    const esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;

    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "check: cannot open %s (%s)", CONFIG_GW_OTA_URL, esp_err_to_name(err));
        set_last("error: %s", esp_err_to_name(err));
        return;
    }
    esp_app_desc_t nd;
    err = esp_https_ota_get_img_desc(h, &nd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "check: no image header (%s)", esp_err_to_name(err));
        set_last("error: header %s", esp_err_to_name(err));
        (void)esp_https_ota_abort(h);
        return;
    }
    atomic_store(&s_server_ok, true);

    const esp_app_desc_t *cur = esp_app_get_description();
    char sha[17];
    sha_hex(nd.app_elf_sha256, sha);
    const char *skip = NULL;
    if (strncmp(nd.project_name, cur->project_name, sizeof nd.project_name) != 0) {
        skip = "other project";
    } else if (memcmp(nd.app_elf_sha256, cur->app_elf_sha256, SHA_LEN) == 0) {
        skip = "up to date";
    } else if (rolled_back_before(nd.app_elf_sha256)) {
        skip = "rolled back before";
    } else if (s_have_failed && memcmp(nd.app_elf_sha256, s_failed_sha, SHA_LEN) == 0) {
        skip = "failed before";
    } else if (ota_pending_verify()) {
        skip = "running image pending verify";
    } else if (!may_install) {
        skip = "update available (OTA_CHECK installs)";
    }
    if (skip != NULL) {
        ESP_LOGI(TAG, "check: remote %.32s (%s): %s", nd.version, sha, skip);
        set_last("%s", skip);
        (void)esp_https_ota_abort(h);
        return;
    }

    ESP_LOGW(TAG, "installing %.32s (%s) from %s", nd.version, sha, CONFIG_GW_OTA_URL);
    const int64_t t0 = esp_timer_get_time();
    do {
        sv_beat(s_sv);
        err = esp_https_ota_perform(h);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(h)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        (void)esp_https_ota_abort(h);
    } else {
        err = esp_https_ota_finish(h); /* image check incl. signature, sets boot slot */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "install of %.32s failed: %s%s", nd.version, esp_err_to_name(err),
                 err == ESP_ERR_OTA_VALIDATE_FAILED ? " (corrupt, wrong chip, or bad signature)"
                                                    : "");
        memcpy(s_failed_sha, nd.app_elf_sha256, SHA_LEN);
        s_have_failed = true;
        set_last("install failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "installed %.32s in %" PRId64 " ms; restarting (pending verify)", nd.version,
             (esp_timer_get_time() - t0) / 1000);
    char why[48];
    snprintf(why, sizeof why, "OTA installed %.24s", nd.version);
    fault_restart(why);
}

/* Wait up to `ms` for OTA_CHECK, beating the watchdog. True when notified. */
static bool wait_notify(uint32_t ms)
{
    while (ms > 0U) {
        const uint32_t step = ms > WAIT_SLICE_MS ? WAIT_SLICE_MS : ms;
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(step)) != 0U) {
            sv_beat(s_sv);
            return true;
        }
        ms -= step;
        sv_beat(s_sv);
    }
    return false;
}

static void ota_task(void *arg)
{
    (void)arg;
    s_sv = sv_register("ota", SV_BUDGET_MS);
    for (;;) {
        while (!(wifi_has_ip() || eth_has_ip())) {
            (void)wait_notify(1000);
        }
        const bool forced = atomic_exchange(&s_forced, false);
        check_once(!ota_pending_verify() && (CONFIG_GW_OTA_AUTO_INSTALL || forced));
        /* While pending verify, retry until the server has been reached once:
         * the health check needs it. */
        const bool fast = ota_pending_verify() && !atomic_load(&s_server_ok);
        (void)wait_notify(fast ? PENDING_RETRY_MS : CONFIG_GW_OTA_POLL_MIN * 60000U);
    }
}

void ota_check_now(void)
{
    if (s_ota_task != NULL) {
        atomic_store(&s_forced, true);
        xTaskNotifyGive(s_ota_task);
    }
}

/* ---- health check --------------------------------------------------------- */

static void health_task(void *arg)
{
    (void)arg;
    const int64_t deadline_s = esp_timer_get_time() / 1000000 + CONFIG_GW_OTA_HEALTH_TIMEOUT_S;
    int stable_s = 0;
    for (;;) {
        const bool net = wifi_has_ip() || eth_has_ip();
        const bool server = !CONFIG_GW_OTA_HEALTH_REQUIRE_SERVER || atomic_load(&s_server_ok);
#if CONFIG_GW_OTA_REQUIRE_RADIO
        const bool radio = atomic_load(&g_radio_stats.starts) > 0U;
#else
        const bool radio = true;
#endif
        stable_s = (net && server && radio) ? stable_s + 1 : 0;
#if CONFIG_GW_OTA_TEST_NEVER_CONFIRM
        stable_s = 0;
#endif
        if (stable_s >= HEALTH_STABLE_S) {
            const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                atomic_store(&s_health_ok, true);
                ESP_LOGW(TAG, "health check passed: image marked valid (upstream %s)",
                         lbsfwd_link_up() ? "up" : "down");
                vTaskDelete(NULL);
            }
            ESP_LOGE(TAG, "mark valid: %s", esp_err_to_name(err));
        }
        if (esp_timer_get_time() / 1000000 >= deadline_s) {
            ESP_LOGE(TAG, "health check failed (net %d, update server %d, radio %d): rolling back",
                     net, server, radio);
            fault_note("OTA health check failed: rollback");
            vTaskDelay(pdMS_TO_TICKS(200)); /* let the log out */
            (void)esp_ota_mark_app_invalid_rollback_and_reboot();
            ESP_LOGE(TAG, "rollback not possible: restarting");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ota_health_start(void)
{
    if (!ota_pending_verify()) {
        atomic_store(&s_health_ok, true);
        return;
    }
    ESP_LOGW(TAG, "new image: health check for up to %d s (needs %d s stable)%s",
             CONFIG_GW_OTA_HEALTH_TIMEOUT_S, HEALTH_STABLE_S,
             CONFIG_GW_OTA_TEST_NEVER_CONFIRM ? " - TEST BUILD, will roll back" : "");
    const BaseType_t ok = xTaskCreatePinnedToCore(health_task, "ota_health", HEALTH_TASK_STACK,
                                                  NULL, HEALTH_TASK_PRIO, NULL, 0);
    if (ok != pdPASS) {
        /* Nothing would watch this image: do not trust it. */
        ESP_LOGE(TAG, "health task not created: rolling back");
        fault_note("OTA health task not created: rollback");
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();
    }
}

/* ---- start ------------------------------------------------------------------ */

esp_err_t ota_start(void)
{
    ESP_LOGI(TAG, "update URL %s, every %d min", CONFIG_GW_OTA_URL, CONFIG_GW_OTA_POLL_MIN);
    const BaseType_t ok = xTaskCreatePinnedToCore(ota_task, "ota", OTA_TASK_STACK, NULL,
                                                  OTA_TASK_PRIO, &s_ota_task, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void ota_status_line(char *buf, size_t len)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    char wip[16], eip[16];
    ip_of("WIFI_STA_DEF", wip, sizeof wip);
    ip_of("ETH_DEF", eip, sizeof eip);
    snprintf(buf, len, "version=%s slot=%s state=%s wifi=%s eth=%s upstream=%s last_check=\"%s\"",
             esp_app_get_description()->version, run->label, running_state_str(), wip, eip,
             lbsfwd_link_up() ? "up" : "down", s_last);
}
