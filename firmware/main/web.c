/* Management web UI and REST API (ADR-0010). See web.h. */
#include "web.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_check.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "fault.h"
#include "gwmode.h"
#include "lns.h"
#include "lwc/hex.h"
#include "freertos/FreeRTOS.h"
#include "led.h"
#include "logbuf.h"
#include "lwc/form.h"
#include "lwc/netaddr.h"
#include "lwc/session.h"
#include "lwip/sockets.h"
#include "ota.h"
#include "lbsfwd.h"
#include "radioplan.h"
#include "conc_hal.h"
#include "radio_pkt.h"
#include "sdkconfig.h"
#include "sv.h"
#include "psa/crypto.h"
#include "webauth.h"
#include "wifi.h"
#include "qrcode.h"
#include "mdns.h"

static const char *TAG = "web";

#define SESSION_IDLE_S (30U * 60U)
#define SESSION_ABS_S (12U * 3600U)
#define BODY_MAX 512
#define CHUNK 2048
#define STATUS_MAX 3072U
#define HTTPD_STACK 10240 /* TLS handshake (ECDSA) + PBKDF2 */

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz_start[] asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[] asm("_binary_app_css_gz_end");

static httpd_handle_t s_srv;
static lwc_sess_table_t s_sess; /* httpd task only */
static lwc_throttle_t s_throttle;

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* ---- request guards ---------------------------------------------------------- */

/* LAN only (core/src/netaddr.c, host-tested). */
static bool peer_is_lan(httpd_req_t *req)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &sl) != 0) {
        return false;
    }
    if (ss.ss_family == AF_INET) {
        return lwc_ip_is_lan((const uint8_t *)&((struct sockaddr_in *)&ss)->sin_addr.s_addr, 4);
    }
#if CONFIG_LWIP_IPV6
    if (ss.ss_family == AF_INET6) {
        return lwc_ip_is_lan(((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr, 16);
    }
#endif
    return false;
}

static void common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; frame-ancestors 'none'; form-action 'self'");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Strict-Transport-Security", "max-age=31536000");
}

static esp_err_t send_status(httpd_req_t *req, const char *status, const char *msg)
{
    common_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, msg);
}

static bool cookie_token(httpd_req_t *req, uint8_t tok[LWC_SESS_TOKEN_LEN])
{
    char cookie[256];
    const size_t n = httpd_req_get_hdr_value_len(req, "Cookie");
    return n > 0 && n < sizeof cookie &&
           httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) == ESP_OK &&
           lwc_sess_parse_cookie(cookie, n, tok);
}

/* Role of the request's session, refreshing it. Every handler runs in the
 * one httpd task, so the session table needs no lock. */
static lwc_role_t role_of(httpd_req_t *req)
{
    uint8_t tok[LWC_SESS_TOKEN_LEN];
    return cookie_token(req, tok) ? lwc_sess_check(&s_sess, tok, now_s()) : LWC_ROLE_NONE;
}

/* State-changing requests must carry a header a cross-site form cannot set. */
static bool csrf_ok(httpd_req_t *req)
{
    char v[8];
    return httpd_req_get_hdr_value_str(req, "X-Requested-With", v, sizeof v) == ESP_OK &&
           strcmp(v, "gw") == 0;
}

/* Common gate: LAN, CSRF for writes, role. Sends the refusal itself. */
static bool gate(httpd_req_t *req, lwc_role_t need, esp_err_t *ret)
{
    if (!peer_is_lan(req)) {
        *ret = send_status(req, "403 Forbidden", "LAN only\n");
        return false;
    }
    if (req->method != HTTP_GET && !csrf_ok(req)) {
        *ret = send_status(req, "403 Forbidden", "missing X-Requested-With\n");
        return false;
    }
    if (need != LWC_ROLE_NONE) {
        const lwc_role_t have = role_of(req);
        if (have == LWC_ROLE_NONE) {
            *ret = send_status(req, "401 Unauthorized", "login required\n");
            return false;
        }
        if (have < need) {
            *ret = send_status(req, "403 Forbidden", "admin only\n");
            return false;
        }
    }
    return true;
}

/* ---- static UI ------------------------------------------------------------------ */

static esp_err_t send_gz(httpd_req_t *req, const char *type, const uint8_t *a, const uint8_t *b)
{
    common_headers(req);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)a, b - a);
}

static esp_err_t ui_index(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_NONE, &r)) {
        return r;
    }
    return send_gz(req, "text/html; charset=utf-8", index_html_gz_start, index_html_gz_end);
}

static esp_err_t ui_js(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_NONE, &r)) {
        return r;
    }
    return send_gz(req, "text/javascript", app_js_gz_start, app_js_gz_end);
}

static esp_err_t ui_css(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_NONE, &r)) {
        return r;
    }
    return send_gz(req, "text/css", app_css_gz_start, app_css_gz_end);
}

/* ---- login ------------------------------------------------------------------------ */

static esp_err_t api_login(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_NONE, &r)) {
        return r;
    }
    const uint32_t wait = lwc_throttle_wait(&s_throttle, now_s());
    if (wait > 0U) {
        char msg[64];
        snprintf(msg, sizeof msg, "too many failed logins: wait %" PRIu32 " s\n", wait);
        return send_status(req, "429 Too Many Requests", msg);
    }
    if (req->content_len == 0 || req->content_len > BODY_MAX) {
        return send_status(req, "400 Bad Request", "bad body\n");
    }
    char body[BODY_MAX + 1];
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    char user[16], pass[65];
    lwc_role_t role = LWC_ROLE_NONE;
    const int ul = lwc_form_get(body, got, "user", user, sizeof user);
    const int pl = lwc_form_get(body, got, "password", pass, sizeof pass);
    memset(body, 0, sizeof body);
    if (ul > 0 && pl >= 0) {
        if (strcmp(user, "admin") == 0) {
            role = LWC_ROLE_ADMIN;
        } else if (strcmp(user, "viewer") == 0) {
            role = LWC_ROLE_VIEWER;
        }
    }
    const bool ok = role != LWC_ROLE_NONE && webauth_verify(role, pass, (size_t)(pl > 0 ? pl : 0));
    memset(pass, 0, sizeof pass);

    uint8_t tok[LWC_SESS_TOKEN_LEN];
    lwc_throttle_result(&s_throttle, ok, now_s());
    if (ok) {
        esp_fill_random(tok, sizeof tok);
        lwc_sess_create(&s_sess, tok, role, now_s());
    }
    if (!ok) {
        /* Untrusted text never reaches the log: only the two known names. */
        ESP_LOGW(TAG, "login failed (user %s)", role == LWC_ROLE_ADMIN    ? "admin"
                                                : role == LWC_ROLE_VIEWER ? "viewer"
                                                                          : "(other)");
        return send_status(req, "401 Unauthorized", "wrong user or password\n");
    }
    char cookie[128];
    int o = snprintf(cookie, sizeof cookie, LWC_SESS_COOKIE "=");
    for (size_t i = 0; i < sizeof tok; i++) {
        o += snprintf(cookie + o, sizeof cookie - (size_t)o, "%02x", tok[i]);
    }
    snprintf(cookie + o, sizeof cookie - (size_t)o,
             "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=%u", SESSION_ABS_S);
    memset(tok, 0, sizeof tok);
    ESP_LOGI(TAG, "login: %s", role == LWC_ROLE_ADMIN ? "admin" : "viewer");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    common_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, role == LWC_ROLE_ADMIN ? "{\"role\":\"admin\"}"
                                                          : "{\"role\":\"viewer\"}");
}

static esp_err_t api_logout(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_NONE, &r)) {
        return r;
    }
    uint8_t tok[LWC_SESS_TOKEN_LEN];
    if (cookie_token(req, tok)) {
        lwc_sess_end(&s_sess, tok);
    }
    httpd_resp_set_hdr(req, "Set-Cookie",
                       LWC_SESS_COOKIE "=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
    return send_status(req, "200 OK", "logged out\n");
}

/* ---- status, logs ------------------------------------------------------------------ */

/* Append `val` as a JSON string ("..." with escapes); stops at the buffer end. */
static void json_quote(char *out, size_t cap, size_t *o, const char *val)
{
    if (*o + 1U < cap) {
        out[(*o)++] = '"';
    }
    for (const char *p = val; *p != '\0' && *o + 7U < cap; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[(*o)++] = '\\';
            out[(*o)++] = (char)c;
        } else if (c < 0x20U) {
            *o += (size_t)snprintf(out + *o, cap - *o, "\\u%04x", c);
        } else {
            out[(*o)++] = (char)c;
        }
    }
    if (*o + 1U < cap) {
        out[(*o)++] = '"';
    }
    out[*o < cap ? *o : cap - 1U] = '\0';
}

static void json_str(char *out, size_t cap, size_t *o, const char *key, const char *val)
{
    if (*o >= cap) {
        return;
    }
    int n = snprintf(out + *o, cap - *o, "%s\"%s\":", *o > 1 ? "," : "", key);
    *o += n > 0 ? (size_t)n : 0U;
    json_quote(out, cap, o, val);
}

static esp_err_t api_status(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_VIEWER, &r)) {
        return r;
    }
    char line[256];
    char *out = heap_caps_malloc(STATUS_MAX, MALLOC_CAP_DEFAULT);
    if (out == NULL) {
        return send_status(req, "500 Internal Server Error", "no memory\n");
    }
    size_t o = 0;
    out[o++] = '{';
    out[o] = '\0';
    ota_status_line(line, sizeof line);
    json_str(out, STATUS_MAX, &o, "ota", line);
    fault_status_line(line, sizeof line);
    json_str(out, STATUS_MAX, &o, "faults", line);
    sv_status_line(line, sizeof line);
    json_str(out, STATUS_MAX, &o, "stack_free", line);
    snprintf(line, sizeof line,
             "rx=%u crc_ok=%u dropped=%u starts=%u restarts=%u upstream=%s led=%s uptime_s=%" PRIu32
             " heap_internal=%u heap_internal_min=%u heap_psram=%u",
             atomic_load(&g_radio_stats.rxnb), atomic_load(&g_radio_stats.rxok),
             atomic_load(&g_radio_stats.dropped), atomic_load(&g_radio_stats.starts),
             atomic_load(&g_radio_stats.restarts), lbsfwd_link_up() ? "up" : "down", led_state(), now_s(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    json_str(out, STATUS_MAX, &o, "radio", line);
    char eui[17] = "";
    (void)gwmode_eui(eui);
    char lbs_host[GW_LBS_HOST_MAX];
    gwmode_lbs_host(lbs_host);
    snprintf(line, sizeof line, "eui=%s report=%s mode=%s protocol=basicstation server=%s port=%d lns_key=%s", eui,
             gwmode_forwards() ? "on" : "off", gwmode_name(gwmode()), lbs_host, (int)gwmode_lbs_port(),
             gwmode_lbs_key_set() ? "set" : "none");
    json_str(out, STATUS_MAX, &o, "ttn", line);
    lns_status_line(line, sizeof line);
    json_str(out, STATUS_MAX, &o, "lns", line);
    lbsfwd_status(line, sizeof line);
    if (line[0] != '\0') {
        json_str(out, STATUS_MAX, &o, "lbs", strncmp(line, "LBS ", 4) == 0 ? line + 4 : line);
    }
    if (o + 2U < STATUS_MAX) {
        out[o++] = '}';
        out[o++] = '\n';
    }
    common_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    r = httpd_resp_send(req, out, (ssize_t)o);
    heap_caps_free(out);
    return r;
}

static esp_err_t api_logs(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_VIEWER, &r)) {
        return r;
    }
    char q[16] = "";
    const bool prev = httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "prev=1");
    const size_t cap = 16 * 1024;
    char *buf = prev ? NULL : heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    const char *text = prev ? logbuf_previous() : (buf != NULL && logbuf_current(buf, cap) ? buf : "");
    common_headers(req);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    r = httpd_resp_sendstr(req, text);
    heap_caps_free(buf);
    return r;
}

/* ---- admin actions ----------------------------------------------------------------- */

static esp_err_t api_coredump_get(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        return send_status(req, "404 Not Found", "no coredump stored\n");
    }
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DEFAULT);
    if (buf == NULL) {
        return send_status(req, "500 Internal Server Error", "no memory\n");
    }
    common_headers(req);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    r = ESP_OK;
    for (size_t off = 0; off < size && r == ESP_OK; off += CHUNK) {
        const size_t n = size - off < CHUNK ? size - off : CHUNK;
        r = esp_flash_read(NULL, buf, addr + off, n);
        if (r == ESP_OK) {
            r = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n);
        }
    }
    heap_caps_free(buf);
    if (r == ESP_OK) {
        r = httpd_resp_send_chunk(req, NULL, 0);
    }
    return r;
}

static esp_err_t api_coredump_delete(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    r = esp_core_dump_image_erase();
    return r == ESP_OK ? send_status(req, "200 OK", "coredump erased\n")
                       : send_status(req, "500 Internal Server Error", esp_err_to_name(r));
}

static esp_err_t api_ota_check(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    ota_check_now();
    return send_status(req, "202 Accepted", "update check started; see status\n");
}

static void reboot_cb(void *arg)
{
    fault_restart(arg);
}

/* Restart a second later, after the reply has gone out. */
static esp_err_t schedule_reboot(const char *why)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = reboot_cb, .arg = (void *)why, .name = "web_reboot"};
    if (t == NULL && esp_timer_create(&a, &t) != ESP_OK) {
        return ESP_FAIL;
    }
    return esp_timer_start_once(t, 1000000);
}

static esp_err_t reboot_soon(httpd_req_t *req, const char *why, const char *msg)
{
    if (schedule_reboot(why) != ESP_OK) {
        return send_status(req, "500 Internal Server Error", "timer\n");
    }
    return send_status(req, "202 Accepted", msg);
}

static esp_err_t api_reboot(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    return reboot_soon(req, "web UI reboot", "rebooting\n");
}

/* ---- TTN registration help and settings (ADR-0012) ---------------------------- */

typedef struct {
    char *buf;
    size_t cap, len;
} svg_t;

/* QR modules -> one SVG path (a 1x1 square per dark module). */
static void qr_to_svg(esp_qrcode_handle_t qr, void *arg)
{
    svg_t *v = arg;
    const int n = esp_qrcode_get_size(qr);
    v->len = (size_t)snprintf(v->buf, v->cap,
                              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-4 -4 %d %d\" "
                              "shape-rendering=\"crispEdges\"><rect x=\"-4\" y=\"-4\" width=\"%d\" "
                              "height=\"%d\" fill=\"#fff\"/><path fill=\"#000\" d=\"",
                              n + 8, n + 8, n + 8, n + 8);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n && v->len + 24U < v->cap; x++) {
            if (esp_qrcode_get_module(qr, x, y)) {
                v->len += (size_t)snprintf(v->buf + v->len, v->cap - v->len, "M%d %dh1v1h-1z", x, y);
            }
        }
    }
    v->len += (size_t)snprintf(v->buf + v->len, v->cap - v->len, "\"/></svg>");
}

static esp_err_t api_ttn_qr(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_VIEWER, &r)) {
        return r;
    }
    char eui[17];
    svg_t v = {.cap = 12288};
    v.buf = heap_caps_malloc(v.cap, MALLOC_CAP_SPIRAM);
    if (v.buf == NULL || gwmode_eui(eui) != ESP_OK) {
        heap_caps_free(v.buf);
        return send_status(req, "500 Internal Server Error", "qr\n");
    }
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func_with_cb = qr_to_svg;
    cfg.user_data = &v;
    cfg.max_qrcode_version = 4;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    if (esp_qrcode_generate(&cfg, eui) != ESP_OK || v.len == 0U) {
        heap_caps_free(v.buf);
        return send_status(req, "500 Internal Server Error", "qr\n");
    }
    common_headers(req);
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    r = httpd_resp_send(req, v.buf, (ssize_t)v.len);
    heap_caps_free(v.buf);
    return r;
}

static bool read_body(httpd_req_t *req, char *body, size_t *got);

/* Settings (admin): mode=ttn|standalone|hybrid, lns_host=<host>, lns_port=<1-65535>,
 * lns_key=<key> (write-only; empty erases it). Any change applies after a
 * restart. */
static esp_err_t api_settings(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    char body[BODY_MAX + 1];
    size_t got;
    if (!read_body(req, body, &got)) {
        return send_status(req, "400 Bad Request", "bad body\n");
    }
    char mode[16], host[GW_LBS_HOST_MAX], key[GW_LBS_KEY_MAX], port_s[8];
    const int ml = lwc_form_get(body, got, "mode", mode, sizeof mode);
    const int hl = lwc_form_get(body, got, "lns_host", host, sizeof host);
    const int pl = lwc_form_get(body, got, "lns_port", port_s, sizeof port_s);
    const long port = pl > 0 ? strtol(port_s, NULL, 10) : 0;
    const int kl = lwc_form_get(body, got, "lns_key", key, sizeof key);
    memset(body, 0, sizeof body);
    lwc_mode_t m;
    if ((ml < 0 && hl < 0 && pl < 0 && kl < 0) || (ml >= 0 && !gwmode_parse(mode, &m)) ||
        (pl >= 0 && (port < 1 || port > 65535))) {
        memset(key, 0, sizeof key);
        return send_status(req, "400 Bad Request", "mode=ttn|standalone|hybrid, lns_host, lns_port (1-65535), lns_key\n");
    }
    bool changed = false;
    if (ml >= 0 && m != gwmode()) {
        r = gwmode_set(m);
        if (r != ESP_OK) {
            memset(key, 0, sizeof key);
            return send_status(req, "500 Internal Server Error", esp_err_to_name(r));
        }
        changed = true;
    }
    if (hl >= 0 || pl >= 0 || kl >= 0) {
        r = gwmode_set_lbs(hl >= 0 ? host : NULL, (uint16_t)port, kl >= 0 ? key : NULL);
        memset(key, 0, sizeof key);
        if (r == ESP_ERR_INVALID_ARG) {
            return send_status(req, "400 Bad Request", "lns_host: a host name; lns_key: no spaces or quotes\n");
        }
        if (r != ESP_OK) {
            return send_status(req, "500 Internal Server Error", esp_err_to_name(r));
        }
        changed = true;
        ESP_LOGW(TAG, "Basics Station settings changed%s", kl >= 0 ? " (LNS key replaced)" : "");
    }
    if (!changed) {
        return send_status(req, "200 OK", "unchanged\n");
    }
    return reboot_soon(req, "web settings changed", "saved: restarting\n");
}

/* ---- devices (ADR-0015) -----------------------------------------------------------
 * GET lists the local network server's devices (viewer; no keys). POST adds
 * one, POST /api/devices/delete removes one (admin). AppKeys go in, never out. */

#define DEVICES_JSON_MAX 20480 /* 32 devices at ~500 B each, worst case */

/* ---- radio plan (ADR-0017) --------------------------------------------------------- */

static esp_err_t api_radio_get(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_VIEWER, &r)) {
        return r;
    }
    static char out[1024];
    const size_t n = radioplan_json(out, sizeof out);
    if (n == 0U) {
        return send_status(req, "500 Internal Server Error", "plan\n");
    }
    common_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, (ssize_t)n);
}

/* gain=<dBi, one decimal>, tx=on|off, lns_tx=on|off (admin). A gain change
 * restarts the radio (the gain table is cut at start). */
static bool parse_db10(const char *s, int16_t *out)
{
    const bool neg = s[0] == '-';
    const char *p = neg ? s + 1 : s;
    int v = 0, digits = 0, frac = -1;
    for (; *p != '\0'; p++) {
        if (*p == '.' && frac < 0) {
            frac = 0;
            continue;
        }
        if (*p < '0' || *p > '9' || frac > 0 || digits > 3) {
            return false;
        }
        v = v * 10 + (*p - '0');
        digits++;
        if (frac == 0) {
            frac = 1;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (frac != 1) {
        v *= 10; /* no decimal given */
    }
    *out = (int16_t)(neg ? -v : v);
    return true;
}

static esp_err_t api_radio_post(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    char body[BODY_MAX + 1];
    size_t got;
    if (!read_body(req, body, &got)) {
        return send_status(req, "400 Bad Request", "bad body\n");
    }
    char g[12], tx[8], ltx[8], v[16];
    /* receive windows (rx1_delay, rx2_hz, rx2_dr): their own save */
    const int rl = lwc_form_get(body, got, "rx1_delay", v, sizeof v);
    if (rl >= 0) {
        radio_rx_cfg_t w;
        radioplan_rx(&w);
        char hz[16], dr[8];
        w.rx1_delay_s = (uint8_t)atoi(v);
        if (lwc_form_get(body, got, "rx2_hz", hz, sizeof hz) >= 0) {
            w.rx2_hz = (uint32_t)strtoul(hz, NULL, 10);
        }
        if (lwc_form_get(body, got, "rx2_dr", dr, sizeof dr) >= 0) {
            w.rx2_dr = (uint8_t)atoi(dr);
        }
        r = radioplan_set_rx(&w);
        if (r == ESP_ERR_INVALID_ARG) {
            return send_status(req, "400 Bad Request",
                               "RX1 delay 1-15 s; RX2 a transmit frequency in 863-870 MHz; RX2 DR0-DR5\n");
        }
        return r == ESP_OK ? send_status(req, "200 OK", "saved: applies to devices that join from now on\n")
                           : send_status(req, "500 Internal Server Error", esp_err_to_name(r));
    }
    radio_tx_cfg_t c;
    radioplan_tx(&c);
    const radio_tx_cfg_t old = c;
    if (lwc_form_get(body, got, "gain", g, sizeof g) >= 0 && !parse_db10(g, &c.gain_db10)) {
        return send_status(req, "400 Bad Request", "gain: dBi, e.g. 3.0\n");
    }
    if (lwc_form_get(body, got, "tx", tx, sizeof tx) >= 0) {
        c.tx_on = strcmp(tx, "on") == 0;
    }
    if (lwc_form_get(body, got, "lns_tx", ltx, sizeof ltx) >= 0) {
        c.lns_tx = strcmp(ltx, "on") == 0;
    }
    r = radioplan_set_tx(&c);
    if (r == ESP_ERR_INVALID_ARG) {
        return send_status(req, "400 Bad Request", "gain: -5.0 to 15.0 dBi\n");
    }
    if (r == ESP_ERR_NOT_SUPPORTED) {
        return send_status(req, "400 Bad Request", "transmit is not built into this firmware\n");
    }
    if (r != ESP_OK) {
        return send_status(req, "500 Internal Server Error", esp_err_to_name(r));
    }
    if (c.gain_db10 != old.gain_db10) {
        conc_hal_restart();
        return send_status(req, "200 OK", "saved: the radio restarts with the new power cap\n");
    }
    return send_status(req, "200 OK", "saved\n");
}

/* Channel plan editor (ADR-0017, admin): r0, r1 = radio centres in Hz;
 * m0..m7 = "enable:radio:if_hz"; std = "enable:radio:if_hz:sf:bw_khz";
 * fsk = "enable:radio:if_hz"; or reset=1 for the built-in plan. Stored as
 * "local"; a connected network server replaces it with its own. */
static bool parse_chan(const char *s, lwc_plan_chan_t *c, int *sf, int *bw)
{
    int en = 0, radio = 0;
    long ifhz = 0;
    int n = 0;
    if (sf != NULL) {
        n = sscanf(s, "%d:%d:%ld:%d:%d", &en, &radio, &ifhz, sf, bw);
        if (n != 5) {
            return false;
        }
    } else if (sscanf(s, "%d:%d:%ld", &en, &radio, &ifhz) != 3) {
        return false;
    }
    if ((en != 0 && en != 1) || (radio != 0 && radio != 1) || ifhz < -1000000L || ifhz > 1000000L) {
        return false;
    }
    *c = (lwc_plan_chan_t){en == 1, (uint8_t)radio, (int32_t)ifhz};
    return true;
}

static const char *plan_err_text(lwc_plan_err_t e)
{
    return e == LWC_PLAN_ERR_RADIO     ? "a radio centre is outside 863-870 MHz\n"
           : e == LWC_PLAN_ERR_IF      ? "a channel is outside its radio's band (|IF| <= 400 kHz at 125 kHz)\n"
           : e == LWC_PLAN_ERR_BAND    ? "a channel reaches outside 863-870 MHz\n"
           : e == LWC_PLAN_ERR_DEFAULT ? "868.1, 868.3 and 868.5 MHz must stay (EU868 default channels)\n"
           : e == LWC_PLAN_ERR_STD     ? "std channel: SF7-12, 125/250/500 kHz\n"
                                       : "plan not valid\n";
}

static esp_err_t api_radio_plan(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    char body[BODY_MAX + 1];
    size_t got;
    if (!read_body(req, body, &got)) {
        return send_status(req, "400 Bad Request", "bad body\n");
    }
    char v[48];
    lwc_plan_t p;
    radioplan_src_t src = RADIOPLAN_LOCAL;
    if (lwc_form_get(body, got, "reset", v, sizeof v) >= 0 && strcmp(v, "1") == 0) {
        lwc_plan_default_eu868(&p);
        src = RADIOPLAN_DEFAULT;
    } else {
        radioplan_get(&p);
        for (int i = 0; i < 2; i++) {
            const char key[3] = {'r', (char)('0' + i), '\0'};
            if (lwc_form_get(body, got, key, v, sizeof v) >= 0) {
                p.radio_hz[i] = (uint32_t)strtoul(v, NULL, 10);
            }
        }
        for (unsigned i = 0; i < LWC_PLAN_MULTISF; i++) {
            const char key[3] = {'m', (char)('0' + i), '\0'};
            if (lwc_form_get(body, got, key, v, sizeof v) >= 0 && !parse_chan(v, &p.multisf[i], NULL, NULL)) {
                return send_status(req, "400 Bad Request", "m0..m7: enable:radio:if_hz\n");
            }
        }
        int sf = p.std_sf, bw = p.std_bw_khz;
        if (lwc_form_get(body, got, "std", v, sizeof v) >= 0) {
            if (!parse_chan(v, &p.std, &sf, &bw) || sf < 0 || sf > 12 || bw < 0 || bw > 500) {
                return send_status(req, "400 Bad Request", "std: enable:radio:if_hz:sf:bw_khz\n");
            }
            p.std_sf = (uint8_t)sf;
            p.std_bw_khz = (uint16_t)bw;
        }
        if (lwc_form_get(body, got, "fsk", v, sizeof v) >= 0 && !parse_chan(v, &p.fsk, NULL, NULL)) {
            return send_status(req, "400 Bad Request", "fsk: enable:radio:if_hz\n");
        }
    }
    const lwc_plan_err_t pe = lwc_plan_check(&p);
    if (pe != LWC_PLAN_OK) {
        return send_status(req, "400 Bad Request", plan_err_text(pe));
    }
    bool changed = false;
    r = radioplan_store(&p, src, &changed);
    if (r == ESP_ERR_INVALID_STATE) {
        return send_status(req, "409 Conflict", "the radio could not start with this plan earlier\n");
    }
    if (r != ESP_OK) {
        return send_status(req, "500 Internal Server Error", esp_err_to_name(r));
    }
    if (changed) {
        ESP_LOGW(TAG, "channel plan changed on the Config tab (%s)", radioplan_source_name(src));
        conc_hal_restart();
        return send_status(req, "200 OK", "saved: the radio restarts with the new plan\n");
    }
    return send_status(req, "200 OK", "unchanged\n");
}

static esp_err_t api_devices_get(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_VIEWER, &r)) {
        return r;
    }
    char *out = heap_caps_malloc(DEVICES_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        return send_status(req, "503 Service Unavailable", "no memory\n");
    }
    const size_t n = lns_devices_json(out, DEVICES_JSON_MAX);
    common_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    r = httpd_resp_send(req, out, (ssize_t)n);
    free(out);
    return r;
}

static const char *ns_err_text(lwc_ns_err_t e)
{
    return e == LWC_NS_ERR_EXISTS ? "A device with this DevEUI exists.\n"
           : e == LWC_NS_ERR_FULL ? "No room: the table is full or storage failed.\n"
           : e == LWC_NS_ERR_NOT_FOUND ? "No such device.\n"
                                       : "Rejected.\n";
}


static esp_err_t api_devices_post(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    char body[BODY_MAX + 1];
    size_t got;
    if (!read_body(req, body, &got)) {
        return send_status(req, "400 Bad Request", "bad body\n");
    }
    char dev[24], join[24], key[40], mac[8], name[LWC_NS_NAME_MAX + 1U];
    const int dl = lwc_form_get(body, got, "dev_eui", dev, sizeof dev);
    const int jl = lwc_form_get(body, got, "join_eui", join, sizeof join);
    const int kl = lwc_form_get(body, got, "app_key", key, sizeof key);
    const int ml = lwc_form_get(body, got, "mac", mac, sizeof mac);
    const int nl = lwc_form_get(body, got, "name", name, sizeof name);
    memset(body, 0, sizeof body);
    uint64_t dev_eui, join_eui;
    uint8_t app_key[16];
    const bool ok_dev = dl > 0 && lwc_hex_eui(dev, (size_t)dl, &dev_eui);
    const bool ok_join = jl > 0 && lwc_hex_eui(join, (size_t)jl, &join_eui);
    const bool ok_key = kl > 0 && lwc_hex_decode(key, (size_t)kl, app_key, sizeof app_key);
    memset(key, 0, sizeof key);
    const char *why = !ok_dev ? "DevEUI: 16 hex digits.\n"
                      : !ok_join ? "JoinEUI (AppEUI): 16 hex digits.\n"
                      : !ok_key ? "AppKey: 32 hex digits.\n"
                      : (ml > 0 && strcmp(mac, "1.0.4") != 0 && strcmp(mac, "1.0.2") != 0)
                          ? "LoRaWAN version: 1.0.2 or 1.0.4.\n"
                          : NULL;
    if (why != NULL) {
        memset(app_key, 0, sizeof app_key);
        return send_status(req, "400 Bad Request", why);
    }
    const lwc_ns_mac_t v = (ml > 0 && strcmp(mac, "1.0.4") == 0) ? LWC_NS_MAC_1_0_4 : LWC_NS_MAC_1_0_2;
    const lwc_ns_err_t e = lns_add(dev_eui, join_eui, app_key, v, nl > 0 ? name : NULL);
    memset(app_key, 0, sizeof app_key);
    if (e != LWC_NS_OK) {
        return send_status(req, e == LWC_NS_ERR_EXISTS ? "409 Conflict" : "507 Insufficient Storage",
                           ns_err_text(e));
    }
    return send_status(req, "200 OK", "added\n");
}

static esp_err_t api_devices_delete(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    char body[BODY_MAX + 1];
    size_t got;
    char dev[24];
    uint64_t dev_eui;
    if (!read_body(req, body, &got)) {
        return send_status(req, "400 Bad Request", "bad body\n");
    }
    const int dl = lwc_form_get(body, got, "dev_eui", dev, sizeof dev);
    if (dl <= 0 || !lwc_hex_eui(dev, (size_t)dl, &dev_eui)) {
        return send_status(req, "400 Bad Request", "DevEUI: 16 hex digits.\n");
    }
    const lwc_ns_err_t e = lns_remove(dev_eui);
    return e == LWC_NS_OK ? send_status(req, "200 OK", "removed\n")
                          : send_status(req, "404 Not Found", ns_err_text(e));
}

/* ---- claim (ADR-0014) ------------------------------------------------------------
 * With no admin password yet, the first LAN visitor sets it, for CLAIM_MINUTES
 * after start or after BLE setup joined a network. Then the window shuts until
 * the next restart or BLE setup; the console WEB_PASS always works. */

#define CLAIM_MINUTES 15

static _Atomic int64_t s_claim_from_us;

void web_claim_restart(void)
{
    atomic_store(&s_claim_from_us, esp_timer_get_time());
}

static bool claim_open(void)
{
    return !webauth_has_password(LWC_ROLE_ADMIN) &&
           esp_timer_get_time() - atomic_load(&s_claim_from_us) < CLAIM_MINUTES * 60LL * 1000000LL;
}

static esp_err_t api_claim_get(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_NONE, &r)) {
        return r;
    }
    char host[WEBAUTH_HOST_LEN];
    webauth_hostname(host);
    char out[160];
    const int n = snprintf(out, sizeof out, "{\"open\":%s,\"admin_set\":%s,\"host\":\"%s.local\"}\n",
                           claim_open() ? "true" : "false",
                           webauth_has_password(LWC_ROLE_ADMIN) ? "true" : "false", host);
    common_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

/* Read a form body of at most BODY_MAX bytes into `body` (NUL-terminated). */
static bool read_body(httpd_req_t *req, char *body, size_t *got)
{
    *got = 0;
    if (req->content_len == 0 || req->content_len > BODY_MAX) {
        return false;
    }
    while (*got < req->content_len) {
        const int n = httpd_req_recv(req, body + *got, req->content_len - *got);
        if (n <= 0) {
            return false;
        }
        *got += (size_t)n;
    }
    body[*got] = '\0';
    return true;
}

static esp_err_t api_claim_post(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_NONE, &r)) {
        return r;
    }
    if (!claim_open()) {
        return send_status(req, "403 Forbidden", "claim is closed\n");
    }
    char body[BODY_MAX + 1];
    size_t got;
    if (!read_body(req, body, &got)) {
        return send_status(req, "400 Bad Request", "bad body\n");
    }
    char admin[64];
    const int al = lwc_form_get(body, got, "admin", admin, sizeof admin);
    memset(body, 0, sizeof body);
    if (al < 8) {
        memset(admin, 0, sizeof admin);
        return send_status(req, "400 Bad Request", "Admin password: 8 to 63 characters.\n");
    }
    esp_err_t err = webauth_set_password(LWC_ROLE_ADMIN, admin, (size_t)al);
    memset(admin, 0, sizeof admin);
    if (err != ESP_OK) {
        return send_status(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "claim: admin password set from the LAN");
    return send_status(req, "200 OK", "claimed: log in as admin\n");
}

/* ---- start -------------------------------------------------------------------------- */

esp_err_t web_start(void)
{
    web_claim_restart();
    lwc_sess_init(&s_sess, SESSION_IDLE_S, SESSION_ABS_S);
    lwc_throttle_init(&s_throttle, 5, 60, 900);

    ESP_RETURN_ON_FALSE(psa_crypto_init() == PSA_SUCCESS, ESP_FAIL, TAG, "psa init");
    const uint8_t *crt, *key;
    size_t crt_len, key_len;
    ESP_RETURN_ON_ERROR(webauth_cert(&crt, &crt_len, &key, &key_len), TAG, "certificate");

    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert = crt;
    cfg.servercert_len = crt_len;
    cfg.prvtkey_pem = key;
    cfg.prvtkey_len = key_len;
    cfg.httpd.stack_size = HTTPD_STACK;
    cfg.httpd.core_id = 0;
    cfg.httpd.max_open_sockets = 3;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.max_uri_handlers = 24;
    cfg.httpd.uri_match_fn = NULL;
    ESP_RETURN_ON_ERROR(httpd_ssl_start(&s_srv, &cfg), TAG, "httpd_ssl_start");

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = ui_index},
        {.uri = "/app.js", .method = HTTP_GET, .handler = ui_js},
        {.uri = "/app.css", .method = HTTP_GET, .handler = ui_css},
        {.uri = "/api/login", .method = HTTP_POST, .handler = api_login},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = api_logout},
        {.uri = "/api/status", .method = HTTP_GET, .handler = api_status},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = api_logs},
        {.uri = "/api/coredump", .method = HTTP_GET, .handler = api_coredump_get},
        {.uri = "/api/coredump", .method = HTTP_DELETE, .handler = api_coredump_delete},
        {.uri = "/api/ota/check", .method = HTTP_POST, .handler = api_ota_check},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = api_reboot},
        {.uri = "/api/claim", .method = HTTP_GET, .handler = api_claim_get},
        {.uri = "/api/ttn/qr.svg", .method = HTTP_GET, .handler = api_ttn_qr},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = api_settings},
        {.uri = "/api/claim", .method = HTTP_POST, .handler = api_claim_post},
        {.uri = "/api/devices", .method = HTTP_GET, .handler = api_devices_get},
        {.uri = "/api/radio", .method = HTTP_GET, .handler = api_radio_get},
        {.uri = "/api/radio", .method = HTTP_POST, .handler = api_radio_post},
        {.uri = "/api/radio/plan", .method = HTTP_POST, .handler = api_radio_plan},
        {.uri = "/api/devices", .method = HTTP_POST, .handler = api_devices_post},
        {.uri = "/api/devices/delete", .method = HTTP_POST, .handler = api_devices_delete},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_srv, &uris[i]), TAG, "uri %s", uris[i].uri);
    }
    char fp[100];
    webauth_fingerprint(fp, sizeof fp);
    ESP_LOGI(TAG, "HTTPS on port 443 (LAN only); certificate SHA-256 %s", fp);
    /* mDNS: https://lorawan-gateway-xxxxxx.local/ on every interface. */
    char host[WEBAUTH_HOST_LEN];
    webauth_hostname(host);
    if (mdns_init() == ESP_OK && mdns_hostname_set(host) == ESP_OK) {
        (void)mdns_instance_name_set("LoRaWAN Gateway");
        (void)mdns_service_add(NULL, "_https", "_tcp", 443, NULL, 0);
        ESP_LOGI(TAG, "mDNS: https://%s.local/", host);
    } else {
        ESP_LOGW(TAG, "mDNS did not start");
    }
    if (!webauth_has_password(LWC_ROLE_ADMIN)) {
        ESP_LOGW(TAG, "no admin password: the first LAN visit to https://%s.local/ sets it (%d min), "
                      "or tools/web_password.py (console WEB_PASS)", host, CLAIM_MINUTES);
    }
    return ESP_OK;
}

static void end_all_work(void *arg)
{
    lwc_sess_end_all(arg);
}

void web_sessions_end_all(void)
{
    /* Called from the console task: run it on the httpd task, which owns the table. */
    if (s_srv != NULL) {
        (void)httpd_queue_work(s_srv, end_all_work, &s_sess);
    }
}

void web_status_line(char *buf, size_t len)
{
    char fp[100];
    webauth_fingerprint(fp, sizeof fp);
    char host[WEBAUTH_HOST_LEN];
    webauth_hostname(host);
    snprintf(buf, len, "https=%s host=%s.local admin=%s viewer=%s cert_sha256=%s", s_srv ? "on" : "off", host,
             webauth_has_password(LWC_ROLE_ADMIN) ? "set" : "none",
             webauth_has_password(LWC_ROLE_VIEWER) ? "set" : "none", fp);
}
