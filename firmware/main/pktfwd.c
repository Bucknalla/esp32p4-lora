/*
 * Semtech UDP packet forwarder (GWMP v2) to TTN / ChirpStack.
 *
 * Uplinks: CRC-valid packets from the radio queue, up to RX_PER_PUSH per
 * PUSH_DATA. Keepalive: PULL_DATA every CONFIG_GW_PKTFWD_KEEPALIVE_S, so the
 * server can reach us through NAT. Status: `stat` every CONFIG_GW_PKTFWD_STAT_S.
 * Downlinks: PULL_RESP `txpk` is parsed (core/src/txpk.c) and handed to the
 * radio task, which admits it (JIT queue, duty cycle, power cap) and answers;
 * the forwarder sends TX_ACK with that result.
 * Recovery: no PULL_ACK for LINK_DEAD_S, or a socket error, closes the socket,
 * resolves the server again and reconnects with backoff.
 * Protocol encoding and parsing: core/src/gwmp.c (host-tested).
 */
#include "pktfwd.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "eth.h"
#include "freertos/task.h"
#include "lwc/gwmp.h"
#include "lwc/txpk.h"
#include "radio_tx.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "radio_pkt.h"
#include "sdkconfig.h"
#include "wifi.h"

static const char *TAG = "pktfwd";

#define TASK_STACK 6144
#define TASK_PRIO 5
#define TASK_CORE 0

#define RX_PER_PUSH 4U
#define DGRAM_MAX 2400U
#define RECV_TIMEOUT_MS 50
#define LINK_DEAD_S 60
#define BACKOFF_MIN_MS 1000U
#define BACKOFF_MAX_MS 60000U
#define WALLCLOCK_VALID_S 1700000000LL
#define TX_RESULT_WAIT_MS 100

static QueueHandle_t s_queue;
static uint8_t s_eui[8];
static int s_sock = -1;

/* Static buffers: nothing allocated per datagram. */
static uint8_t s_tx[DGRAM_MAX];
static uint8_t s_rx[DGRAM_MAX];
static radio_pkt_t s_items[RX_PER_PUSH];
static lwc_rxpk_t s_pk[RX_PER_PUSH];

/* Upstream accounting for `stat` (reset each report). */
static uint32_t s_push_sent, s_push_acked, s_rxfw, s_dwnb;
static uint16_t s_push_token, s_pull_token;
static int64_t s_last_pull_ack_s;

static int64_t now_s(void)
{
    return (int64_t)(esp_timer_get_time() / 1000000LL);
}

static int64_t wallclock_s(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < WALLCLOCK_VALID_S) {
        return -1;
    }
    return tv.tv_sec;
}

static int parse_hex_eui(const char *s, uint8_t out[8])
{
    if (strlen(s) != 16U) {
        return -1;
    }
    for (int i = 0; i < 8; i++) {
        char byte[3] = {s[2 * i], s[2 * i + 1], '\0'};
        char *end = NULL;
        const unsigned long v = strtoul(byte, &end, 16);
        if (end != &byte[2]) {
            return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static esp_err_t init_eui(void)
{
    if (CONFIG_GW_PKTFWD_EUI[0] != '\0') {
        ESP_RETURN_ON_FALSE(parse_hex_eui(CONFIG_GW_PKTFWD_EUI, s_eui) == 0, ESP_ERR_INVALID_ARG,
                            TAG, "GW_PKTFWD_EUI must be 16 hex digits");
        return ESP_OK;
    }
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BASE), TAG, "base MAC");
    lwc_eui64_from_mac(mac, s_eui);
    return ESP_OK;
}

static bool have_ip(void)
{
    return wifi_has_ip() || eth_has_ip();
}

static void close_socket(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

static esp_err_t open_socket(void)
{
    char port[8];
    snprintf(port, sizeof port, "%d", CONFIG_GW_PKTFWD_PORT);
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *res = NULL;
    const int gai = getaddrinfo(CONFIG_GW_PKTFWD_SERVER, port, &hints, &res);
    if (gai != 0 || res == NULL) {
        ESP_LOGW(TAG, "DNS %s failed (%d)", CONFIG_GW_PKTFWD_SERVER, gai);
        return ESP_FAIL;
    }
    char ip[16];
    inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip);

    s_sock = socket(res->ai_family, res->ai_socktype, 0);
    if (s_sock < 0) {
        freeaddrinfo(res);
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return ESP_FAIL;
    }
    /* connect(): send() goes to the server, recv() accepts only its replies. */
    if (connect(s_sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect: errno %d", errno);
        freeaddrinfo(res);
        close_socket();
        return ESP_FAIL;
    }
    freeaddrinfo(res);
    const struct timeval tv = {.tv_sec = 0, .tv_usec = RECV_TIMEOUT_MS * 1000};
    (void)setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ESP_LOGI(TAG, "server %s (%s) port %d", CONFIG_GW_PKTFWD_SERVER, ip, CONFIG_GW_PKTFWD_PORT);
    return ESP_OK;
}

static bool send_dgram(size_t len)
{
    const int n = send(s_sock, s_tx, len, 0);
    if (n != (int)len) {
        ESP_LOGW(TAG, "send: errno %d", errno);
        return false;
    }
    return true;
}

static bool send_pull_data(void)
{
    s_pull_token = (uint16_t)esp_random();
    const size_t h = lwc_gwmp_header(s_tx, sizeof s_tx, LWC_GWMP_PULL_DATA, s_pull_token, s_eui);
    return send_dgram(h);
}

static bool send_push(const char *what, size_t json_len)
{
    s_push_sent++;
    if (!send_dgram(LWC_GWMP_HDR_UP + json_len)) {
        return false;
    }
    ESP_LOGD(TAG, "PUSH_DATA %s token %04x (%u B)", what, s_push_token,
             (unsigned)(LWC_GWMP_HDR_UP + json_len));
    return true;
}

static bool send_rxpk(size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const radio_pkt_t *it = &s_items[i];
        s_pk[i] = (lwc_rxpk_t){
            .time_unix_us = it->time_unix_us, .tmst = it->tmst, .freq_hz = it->freq_hz,
            .chan = it->chan, .rfch = it->rfch, .stat = it->stat, .sf = it->sf,
            .bw_khz = it->bw_khz, .cr_denom = it->cr_denom, .rssi = it->rssi,
            .lsnr = it->lsnr, .size = it->size, .payload = it->payload,
        };
    }
    s_push_token = (uint16_t)esp_random();
    (void)lwc_gwmp_header(s_tx, sizeof s_tx, LWC_GWMP_PUSH_DATA, s_push_token, s_eui);
    size_t len = 0;
    const lwc_gwmp_err_t e = lwc_gwmp_rxpk_json((char *)&s_tx[LWC_GWMP_HDR_UP],
                                                sizeof s_tx - LWC_GWMP_HDR_UP, s_pk, n, &len);
    if (e != LWC_GWMP_OK) {
        ESP_LOGE(TAG, "rxpk json: %d", (int)e);
        return true; /* drop this batch, keep the link */
    }
    if (!send_push("rxpk", len)) {
        return false;
    }
    s_rxfw += (uint32_t)n;
    ESP_LOGI(TAG, "forwarded %u uplink(s)", (unsigned)n);
    return true;
}

static bool send_stat(void)
{
    const lwc_stat_t st = {
        .time_unix_s = wallclock_s(),
        .rxnb = atomic_exchange(&g_radio_stats.rxnb, 0U),
        .rxok = atomic_exchange(&g_radio_stats.rxok, 0U),
        .rxfw = s_rxfw,
        .ackr_pct = s_push_sent ? (s_push_acked * 100U) / s_push_sent : 0U,
        .dwnb = s_dwnb,
        .txnb = atomic_exchange(&g_radio_tx_stats.txnb, 0U),
    };
    s_rxfw = 0;
    s_dwnb = 0;
    const uint32_t sent = s_push_sent, acked = s_push_acked;
    s_push_sent = 0;
    s_push_acked = 0;

    s_push_token = (uint16_t)esp_random();
    (void)lwc_gwmp_header(s_tx, sizeof s_tx, LWC_GWMP_PUSH_DATA, s_push_token, s_eui);
    size_t len = 0;
    if (lwc_gwmp_stat_json((char *)&s_tx[LWC_GWMP_HDR_UP], sizeof s_tx - LWC_GWMP_HDR_UP, &st,
                           &len) != LWC_GWMP_OK) {
        return true;
    }
    ESP_LOGI(TAG, "stat: rx %" PRIu32 " ok %" PRIu32 " fw %" PRIu32 ", acked %" PRIu32 "/%" PRIu32
                  ", tx %" PRIu32 ", dropped %u, radio restarts %u",
             st.rxnb, st.rxok, st.rxfw, acked, sent, st.txnb, atomic_load(&g_radio_stats.dropped),
             atomic_load(&g_radio_stats.restarts));
    return send_push("stat", len);
}

static radio_tx_req_t s_txreq;

static void send_tx_ack(uint16_t token, const radio_tx_res_t *res)
{
    char json[96];
    int n;
    if (res->result == RADIO_TX_OK && res->power_capped) {
        n = snprintf(json, sizeof json, "{\"txpk_ack\":{\"warn\":\"TX_POWER\",\"value\":%d}}",
                     res->power_dbm);
    } else {
        n = snprintf(json, sizeof json, "{\"txpk_ack\":{\"error\":\"%s\"}}",
                     radio_tx_result_str(res->result));
    }
    if (n <= 0 || (size_t)n >= sizeof json) {
        return;
    }
    (void)lwc_gwmp_header(s_tx, sizeof s_tx, LWC_GWMP_TX_ACK, token, s_eui);
    memcpy(&s_tx[LWC_GWMP_HDR_UP], json, (size_t)n);
    (void)send_dgram(LWC_GWMP_HDR_UP + (size_t)n);
}

static void handle_pull_resp(uint16_t token, const char *json, size_t len)
{
    const lwc_txpk_err_t e = lwc_txpk_parse(json, len, &s_txreq.pk);
    if (e != LWC_TXPK_OK) {
        /* Semtech forwarder behaviour: log, no TX_ACK for unparsable txpk. */
        ESP_LOGW(TAG, "PULL_RESP token %04x: bad txpk (%d)", token, (int)e);
        return;
    }
    s_txreq.token = token;
    s_txreq.test = false;
    radio_tx_res_t res = {token, RADIO_TX_BUSY, 0, false};
    /* Drop results of earlier requests that timed out. */
    radio_tx_res_t stale;
    while (xQueueReceive(g_radio_tx_res, &stale, 0) == pdTRUE) {
    }
    if (xQueueSend(g_radio_tx_req, &s_txreq, pdMS_TO_TICKS(10)) == pdTRUE) {
        const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(TX_RESULT_WAIT_MS);
        radio_tx_res_t r;
        while ((int32_t)(end - xTaskGetTickCount()) > 0) {
            if (xQueueReceive(g_radio_tx_res, &r, pdMS_TO_TICKS(10)) == pdTRUE && r.token == token) {
                res = r;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "downlink token %04x: %" PRIu32 " Hz SF%u %d dBm, %u B -> %s%s", token,
             s_txreq.pk.freq_hz, s_txreq.pk.sf, s_txreq.pk.powe, s_txreq.pk.size,
             radio_tx_result_str(res.result), res.power_capped ? " (power capped)" : "");
    send_tx_ack(token, &res);
}

static void handle_downstream(void)
{
    for (;;) {
        const int n = recv(s_sock, s_rx, sizeof s_rx, 0);
        if (n <= 0) {
            return; /* timeout or error: the link check catches a dead socket */
        }
        lwc_gwmp_down_t d;
        if (lwc_gwmp_parse_down(s_rx, (size_t)n, &d) != LWC_GWMP_OK) {
            ESP_LOGW(TAG, "ignored %d-byte datagram", n);
            continue;
        }
        switch (d.type) {
        case LWC_GWMP_PUSH_ACK:
            if (d.token == s_push_token) {
                s_push_acked++;
            }
            break;
        case LWC_GWMP_PULL_ACK:
            if (d.token == s_pull_token) {
                if (s_last_pull_ack_s == 0) {
                    ESP_LOGI(TAG, "server answered PULL_DATA: link up");
                }
                s_last_pull_ack_s = now_s();
            }
            break;
        case LWC_GWMP_PULL_RESP:
            s_dwnb++;
            handle_pull_resp(d.token, d.json, d.json_len);
            break;
        default:
            break;
        }
    }
}

static void pktfwd_task(void *arg)
{
    (void)arg;
    uint32_t backoff = BACKOFF_MIN_MS;

    while (!have_ip()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_GW_PKTFWD_NTP_SERVER);
    if (esp_netif_sntp_init(&sntp) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed: packets go out without \"time\"");
    }

    for (;;) {
        while (!have_ip()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (open_socket() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff = (backoff * 2U > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : backoff * 2U;
            continue;
        }
        backoff = BACKOFF_MIN_MS;
        s_last_pull_ack_s = 0;
        const int64_t opened = now_s();
        int64_t next_pull = 0;
        int64_t next_stat = now_s() + CONFIG_GW_PKTFWD_STAT_S;
        bool ok = true;

        while (ok) {
            /* Uplinks: wait briefly for the first, then take what is queued. */
            size_t n = 0;
            if (xQueueReceive(s_queue, &s_items[0], pdMS_TO_TICKS(RECV_TIMEOUT_MS)) == pdTRUE) {
                n = 1;
                while (n < RX_PER_PUSH && xQueueReceive(s_queue, &s_items[n], 0) == pdTRUE) {
                    n++;
                }
            }
            /* Forward CRC-valid packets only (Semtech default). */
            size_t keep = 0;
            for (size_t i = 0; i < n; i++) {
                if (s_items[i].stat == 1) {
                    if (keep != i) {
                        s_items[keep] = s_items[i];
                    }
                    keep++;
                }
            }
            if (keep > 0) {
                ok = send_rxpk(keep);
            }

            const int64_t t = now_s();
            if (ok && t >= next_pull) {
                ok = send_pull_data();
                next_pull = t + CONFIG_GW_PKTFWD_KEEPALIVE_S;
            }
            if (ok && t >= next_stat) {
                ok = send_stat();
                next_stat = t + CONFIG_GW_PKTFWD_STAT_S;
            }
            if (ok) {
                handle_downstream();
            }
            const int64_t last = s_last_pull_ack_s ? s_last_pull_ack_s : opened;
            if (now_s() - last > LINK_DEAD_S) {
                ESP_LOGW(TAG, "no PULL_ACK for %d s: reconnecting", LINK_DEAD_S);
                ok = false;
            }
            if (!have_ip()) {
                ok = false;
            }
        }
        close_socket();
        vTaskDelay(pdMS_TO_TICKS(backoff));
    }
}

esp_err_t pktfwd_start(QueueHandle_t rx_queue)
{
    ESP_RETURN_ON_FALSE(rx_queue != NULL, ESP_ERR_INVALID_ARG, TAG, "queue");
    ESP_RETURN_ON_ERROR(init_eui(), TAG, "EUI");
    s_queue = rx_queue;
    ESP_LOGI(TAG, "gateway EUI %02X%02X%02X%02X%02X%02X%02X%02X -> %s:%d", s_eui[0], s_eui[1],
             s_eui[2], s_eui[3], s_eui[4], s_eui[5], s_eui[6], s_eui[7],
             CONFIG_GW_PKTFWD_SERVER, CONFIG_GW_PKTFWD_PORT);
    const BaseType_t ok =
        xTaskCreatePinnedToCore(pktfwd_task, "pktfwd", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
