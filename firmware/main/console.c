#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwc/console_cmd.h"
#include "nvs.h"
#include "ota.h"
#include "fault.h"
#include "sv.h"
#include "sdkconfig.h"
#include "radio_tx.h"
#include "wifi.h"

static const char *TAG = "console";

#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM
#define RX_BUF_SIZE 512
#define TX_BUF_SIZE 4096 /* logs never block the radio task on the UART */
#define CONSOLE_LINE_MAX 256 /* WIFI_SET + 64 + 126 hex chars fits */
#define READ_TIMEOUT_MS 1000
#define RESTART_DELAY_MS 500
#define TASK_STACK 4096
#define TASK_PRIO 2
#define TASK_CORE 0
#define SV_BUDGET_MS 30000 /* reads time out every second */

#ifndef CONFIG_GW_TEST_COMMANDS
#define CONFIG_GW_TEST_COMMANDS 0
#endif

static sv_id_t s_sv = -1;

/* Replies start with a fixed prefix so tools can find them among log lines. */
#define REPLY(fmt, ...) printf("CONSOLE: " fmt "\n", ##__VA_ARGS__)

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- > 0U) {
        *v++ = 0;
    }
}

static esp_err_t store_wifi(const lwc_cmd_t *c)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(h, WIFI_NVS_KEY_SSID, c->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, WIFI_NVS_KEY_PASS, c->pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t clear_wifi(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void handle_line(char *line, size_t len)
{
    lwc_cmd_t cmd;
    const lwc_cmd_err_t perr = lwc_cmd_parse(line, len, &cmd);
    wipe(line, len); /* the line holds the password in hex */
    if (perr != LWC_CMD_OK) {
        REPLY("ERR parse %d", (int)perr);
        return;
    }

    esp_err_t err = ESP_OK;
    switch (cmd.kind) {
    case LWC_CMD_NONE:
        return;
    case LWC_CMD_WIFI_SET:
        err = store_wifi(&cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi credentials stored for \"%s\"", cmd.ssid);
            REPLY("OK WIFI_SET");
        }
        break;
    case LWC_CMD_TX_TEST: {
        /* One proprietary frame, downlink polarity, lowest power, RX2 channel. */
        static radio_tx_req_t req;
        static const uint8_t pl[] = {0xE0, 'L', 'W', 'G', 'W', '-', 'T', 'E', 'S', 'T'};
        memset(&req, 0, sizeof req);
        req.test = true;
        req.pk.mode = LWC_TXPK_IMMEDIATE;
        req.pk.freq_hz = 869525000U;
        req.pk.powe = -6;
        req.pk.sf = 9;
        req.pk.bw_khz = 125;
        req.pk.cr_denom = 5;
        req.pk.ipol = true;
        req.pk.size = sizeof pl;
        memcpy(req.pk.payload, pl, sizeof pl);
        if (g_radio_tx_req == NULL || xQueueSend(g_radio_tx_req, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
            REPLY("ERR radio not running");
        } else {
            REPLY("OK TX_TEST queued");
        }
        lwc_cmd_wipe(&cmd);
        return; /* no restart */
    }
    case LWC_CMD_OTA_CHECK:
        ota_check_now();
        REPLY("OK OTA_CHECK (result in the log)");
        return;
    case LWC_CMD_STATUS: {
        char line_out[256];
        ota_status_line(line_out, sizeof line_out);
        REPLY("STATUS %s", line_out);
        fault_status_line(line_out, sizeof line_out);
        REPLY("FAULTS %s", line_out);
        sv_status_line(line_out, sizeof line_out);
        REPLY("STACK_FREE %s", line_out);
        return;
    }
    case LWC_CMD_TEST_ABORT:
    case LWC_CMD_TEST_STALL:
#if CONFIG_GW_TEST_COMMANDS
        if (cmd.kind == LWC_CMD_TEST_ABORT) {
            REPLY("OK TEST_ABORT");
            fflush(stdout);
            fault_note("console TEST_ABORT");
            abort();
        }
        REPLY("OK TEST_STALL: console stops beating the watchdog");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000)); /* no sv_beat: the supervisor must fire */
        }
#else
        REPLY("ERR test commands not in this build");
        return;
#endif
    case LWC_CMD_WIFI_CLEAR:
        err = clear_wifi();
        if (err == ESP_OK) {
            REPLY("OK WIFI_CLEAR");
        }
        break;
    default:
        err = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    lwc_cmd_wipe(&cmd);

    if (err != ESP_OK) {
        REPLY("ERR %s", esp_err_to_name(err));
        return;
    }
    if (ota_pending_verify()) {
        ESP_LOGW(TAG, "restart while the image is pending verify: the bootloader rolls back");
    }
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    fault_restart("console Wi-Fi change"); /* simplest way to apply new credentials */
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[CONSOLE_LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    s_sv = sv_register("console", SV_BUDGET_MS);

    for (;;) {
        sv_beat(s_sv);
        uint8_t ch;
        const int n = uart_read_bytes(CONSOLE_UART, &ch, 1, pdMS_TO_TICKS(READ_TIMEOUT_MS));
        if (n <= 0) {
            continue;
        }
        if (ch == '\n') {
            if (overflow) {
                REPLY("ERR line too long");
            } else {
                handle_line(line, len);
            }
            wipe(line, sizeof line);
            len = 0;
            overflow = false;
        } else if (len < sizeof line) {
            line[len++] = (char)ch;
        } else {
            overflow = true;
        }
    }
}

esp_err_t console_start(void)
{
    ESP_RETURN_ON_ERROR(uart_driver_install(CONSOLE_UART, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0), TAG,
                        "uart driver");
    /* stdout through the driver too, so logs and replies do not interleave
     * with direct FIFO writes. */
    uart_vfs_dev_use_driver(CONSOLE_UART);
    const BaseType_t ok = xTaskCreatePinnedToCore(console_task, "console", TASK_STACK, NULL,
                                                  TASK_PRIO, NULL, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    REPLY("READY"); /* tools wait for this after the port-open reset */
    return ESP_OK;
}
