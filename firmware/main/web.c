/* Management web UI and REST API (ADR-0010). See web.h. */
#include "web.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
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
#include "freertos/FreeRTOS.h"
#include "led.h"
#include "logbuf.h"
#include "lwc/form.h"
#include "lwc/netaddr.h"
#include "lwc/session.h"
#include "lwip/sockets.h"
#include "ota.h"
#include "pktfwd.h"
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
    char *out = heap_caps_malloc(2048, MALLOC_CAP_DEFAULT);
    if (out == NULL) {
        return send_status(req, "500 Internal Server Error", "no memory\n");
    }
    size_t o = 0;
    out[o++] = '{';
    out[o] = '\0';
    ota_status_line(line, sizeof line);
    json_str(out, 2048, &o, "ota", line);
    fault_status_line(line, sizeof line);
    json_str(out, 2048, &o, "faults", line);
    sv_status_line(line, sizeof line);
    json_str(out, 2048, &o, "stack_free", line);
    snprintf(line, sizeof line,
             "rx=%u crc_ok=%u dropped=%u starts=%u restarts=%u upstream=%s led=%s uptime_s=%" PRIu32
             " heap_internal=%u heap_internal_min=%u heap_psram=%u",
             atomic_load(&g_radio_stats.rxnb), atomic_load(&g_radio_stats.rxok),
             atomic_load(&g_radio_stats.dropped), atomic_load(&g_radio_stats.starts),
             atomic_load(&g_radio_stats.restarts), pktfwd_link_up() ? "up" : "down", led_state(), now_s(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    json_str(out, 2048, &o, "radio", line);
    char eui[17] = "";
    (void)pktfwd_eui(eui);
    snprintf(line, sizeof line, "eui=%s report=%s server=%s port=%d", eui,
             pktfwd_ttn_enabled() ? "on" : "off", CONFIG_GW_PKTFWD_SERVER, CONFIG_GW_PKTFWD_PORT);
    json_str(out, 2048, &o, "ttn", line);
    if (o + 2U < 2048U) {
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
    if (v.buf == NULL || pktfwd_eui(eui) != ESP_OK) {
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

static esp_err_t api_settings(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_ADMIN, &r)) {
        return r;
    }
    char body[64] = "", v[8];
    const int n = req->content_len > 0 && req->content_len < sizeof body
                      ? httpd_req_recv(req, body, req->content_len)
                      : -1;
    if (n <= 0 || lwc_form_get(body, (size_t)n, "ttn", v, sizeof v) < 0 ||
        (strcmp(v, "on") != 0 && strcmp(v, "off") != 0)) {
        return send_status(req, "400 Bad Request", "ttn=on|off\n");
    }
    const bool on = strcmp(v, "on") == 0;
    if (on == pktfwd_ttn_enabled()) {
        return send_status(req, "200 OK", "unchanged\n");
    }
    r = pktfwd_set_ttn(on);
    if (r != ESP_OK) {
        return send_status(req, "500 Internal Server Error", esp_err_to_name(r));
    }
    return reboot_soon(req, "web settings changed", on ? "TTN reporting on: restarting\n"
                                                         : "TTN reporting off: restarting\n");
}

/* ---- first-time setup (ADR-0011) -----------------------------------------------
 * Only while the setup access point is open (no Wi-Fi credentials). With no
 * admin password yet, the form also sets it (claim); otherwise it needs an
 * admin session. */

static esp_err_t api_setup_get(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_NONE, &r)) {
        return r;
    }
    if (!wifi_setup_active()) {
        return send_status(req, "404 Not Found", "setup is closed\n");
    }
    char names[WIFI_SETUP_MAX_NETS][33];
    const size_t n = wifi_setup_networks(names, WIFI_SETUP_MAX_NETS);
    char out[WIFI_SETUP_MAX_NETS * 72 + 64];
    char host[WEBAUTH_HOST_LEN];
    webauth_hostname(host);
    size_t o = (size_t)snprintf(out, sizeof out, "{\"host\":\"%s.local\",\"admin_set\":%s,\"networks\":[",
                                host, webauth_has_password(LWC_ROLE_ADMIN) ? "true" : "false");
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && o + 1U < sizeof out) {
            out[o++] = ',';
        }
        json_quote(out, sizeof out, &o, names[i]);
    }
    o += (size_t)snprintf(out + o, sizeof out - o, "]}\n");
    common_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, (ssize_t)o);
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

/* Validate and store a setup form; NULL on success, else the reason. Used by
 * the HTTPS page and the plain-HTTP fallback on the captive portal. */
static const char *setup_save(char *body, size_t got, bool claim, bool want_repeat, esp_err_t *err)
{
    char ssid[33], pass[64], admin[64], admin2[64], ttn[8];
    const bool ttn_on = lwc_form_get(body, got, "ttn", ttn, sizeof ttn) > 0; /* checkbox */
    int sl = lwc_form_get(body, got, "ssid", ssid, sizeof ssid);
    if (sl < 1) { /* "Other network" in the list: the typed name */
        sl = lwc_form_get(body, got, "ssid_other", ssid, sizeof ssid);
    }
    const int pl = lwc_form_get(body, got, "password", pass, sizeof pass);
    const int al = claim ? lwc_form_get(body, got, "admin", admin, sizeof admin) : 8;
    const bool same = !claim || !want_repeat ||
                      (lwc_form_get(body, got, "admin2", admin2, sizeof admin2) == al &&
                       strcmp(admin, admin2) == 0);
    memset(body, 0, got);
    const char *why = NULL;
    *err = ESP_OK;
    if (sl < 1) {
        why = "Choose a network.";
    } else if (pl < 8) {
        why = "Wi-Fi password: 8 to 63 characters.";
    } else if (al < 8) {
        why = "Admin password: 8 to 63 characters.";
    } else if (!same) {
        why = "The admin passwords differ.";
    }
    if (why == NULL && claim) {
        *err = webauth_set_password(LWC_ROLE_ADMIN, admin, (size_t)al);
    }
    if (why == NULL && *err == ESP_OK) {
        *err = wifi_store_credentials(ssid, pass);
    }
    if (why == NULL && *err == ESP_OK) {
        *err = pktfwd_set_ttn(ttn_on);
    }
    memset(pass, 0, sizeof pass);
    memset(admin, 0, sizeof admin);
    memset(admin2, 0, sizeof admin2);
    if (why == NULL && *err != ESP_OK) {
        why = esp_err_to_name(*err);
    }
    if (why == NULL) {
        ESP_LOGW(TAG, "setup: Wi-Fi stored%s; restarting", claim ? " and admin password set" : "");
    }
    return why;
}

static esp_err_t api_setup_post(httpd_req_t *req)
{
    esp_err_t r;
    if (!gate(req, LWC_ROLE_NONE, &r)) {
        return r;
    }
    if (!wifi_setup_active()) {
        return send_status(req, "404 Not Found", "setup is closed\n");
    }
    const bool claim = !webauth_has_password(LWC_ROLE_ADMIN);
    if (!claim && role_of(req) != LWC_ROLE_ADMIN) {
        return send_status(req, "401 Unauthorized", "log in as admin to change Wi-Fi\n");
    }
    char body[BODY_MAX + 1];
    size_t got;
    if (!read_body(req, body, &got)) {
        return send_status(req, "400 Bad Request", "bad body\n");
    }
    esp_err_t err;
    const char *why = setup_save(body, got, claim, false, &err); /* the page checks the repeat */
    if (why != NULL) {
        return send_status(req, err != ESP_OK ? "500 Internal Server Error" : "400 Bad Request", why);
    }
    return reboot_soon(req, "web setup complete", "saved: the gateway restarts and joins the network\n");
}

/* ---- captive portal (ADR-0013) -----------------------------------------------
 * Plain HTTP on port 80, only in setup mode. Every request for another host
 * or path (the phones' connectivity probes) is sent to the portal page; that
 * page offers the HTTPS setup first and, for an unclaimed gateway only, a
 * plain-HTTP fallback form with a warning. */

#define PORTAL_PAGE_MAX 8192

static httpd_handle_t s_portal;

/* Append `v` HTML-escaped (network names come from the air). */
static void html_text(char *out, size_t cap, size_t *o, const char *v)
{
    for (; *v != '\0' && *o + 7U < cap; v++) {
        const char *e = *v == '<' ? "&lt;" : *v == '>' ? "&gt;" : *v == '&' ? "&amp;"
                        : *v == '"' ? "&quot;" : *v == '\'' ? "&#39;" : NULL;
        if (e != NULL) {
            *o += (size_t)snprintf(out + *o, cap - *o, "%s", e);
        } else {
            out[(*o)++] = *v;
        }
    }
    out[*o] = '\0';
}

static void html_raw(char *out, size_t cap, size_t *o, const char *v)
{
    const int n = snprintf(out + *o, cap - *o, "%s", v);
    *o = n < 0 ? *o : (*o + (size_t)n < cap ? *o + (size_t)n : cap - 1U);
}

static const char k_portal_head[] =
    "<!doctype html><html lang=en><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Gateway setup</title><style>"
    "body{margin:0;font:16px/1.5 system-ui,sans-serif;background:#121416;color:#e3e6e8}"
    "header{padding:12px 16px;background:#0b0d0e;border-bottom:3px solid #e8a100;"
    "font:600 14px ui-monospace,monospace;letter-spacing:.14em;text-transform:uppercase}"
    "main{max-width:400px;margin:0 auto;padding:16px}"
    ".btn{display:block;text-align:center;padding:12px;border-radius:4px;background:#e8a100;color:#1b1f22;"
    "font:600 13px ui-monospace,monospace;letter-spacing:.08em;text-transform:uppercase;text-decoration:none}"
    "p{color:#8b949a;font-size:14px}code{color:#e3e6e8}details{margin-top:24px;border-top:1px solid #3d454b;padding-top:8px}"
    "summary{cursor:pointer;color:#8b949a}.warn{border-left:6px solid #e8a100;padding:6px 10px;color:#e3e6e8}"
    "label{display:grid;gap:4px;margin:12px 0;font-size:13px;color:#8b949a}"
    "input,select{font:inherit;padding:8px;border-radius:4px;border:1px solid #3d454b;background:#1a1d20;color:#e3e6e8}"
    "button{width:100%;padding:10px;border-radius:4px;border:1px solid #3d454b;background:#1a1d20;color:#e3e6e8;font:inherit}"
    "</style><header>&#9632; LoRaWAN Gateway setup</header><main>";

static esp_err_t portal_send(httpd_req_t *req, const char *status, const char *html, size_t len)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, (ssize_t)len);
}

static esp_err_t portal_get(httpd_req_t *req)
{
    if (!wifi_setup_active()) {
        return send_status(req, "404 Not Found", "setup is closed\n");
    }
    char host[64] = "";
    (void)httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    ESP_LOGI(TAG, "portal: GET %.40s (host %.40s)", req->uri, host);
    /* Everything goes straight to the secure page; the plain fallback stays
     * at /plain for phones whose sign-in sheet cannot accept the certificate. */
    if (strcmp(host, "192.168.4.1") != 0 || strcmp(req->uri, "/plain") != 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "https://192.168.4.1/");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }
    char *out = heap_caps_malloc(PORTAL_PAGE_MAX, MALLOC_CAP_SPIRAM);
    if (out == NULL) {
        return send_status(req, "500 Internal Server Error", "no memory\n");
    }
    size_t o = 0;
    out[0] = '\0';
    html_raw(out, PORTAL_PAGE_MAX, &o, k_portal_head);
    html_raw(out, PORTAL_PAGE_MAX, &o,
             "<a class=btn href='https://192.168.4.1/'>Open secure setup</a>"
             "<p>The secure page uses this gateway's own certificate, so your phone warns once: "
             "check the fingerprint on the console if you can, then continue. If this sign-in window "
             "cannot open it, open Safari or Chrome and go to <code>https://192.168.4.1/</code> "
             "while you stay on this Wi-Fi.</p>");
    if (webauth_has_password(LWC_ROLE_ADMIN)) {
        html_raw(out, PORTAL_PAGE_MAX, &o,
                 "<p>This gateway already has an admin: changing its Wi-Fi needs the secure page.</p>");
    } else {
        html_raw(out, PORTAL_PAGE_MAX, &o,
                 "<details><summary>Set up without encryption</summary>"
                 "<p class=warn>Your Wi-Fi password and the new admin password cross this open setup "
                 "network unencrypted. Anyone in radio range now could capture them. Use this only if "
                 "the secure page will not open.</p>"
                 "<form method=post action='/setup'>"
                 "<label>Wi-Fi network<select name=ssid>");
        char names[WIFI_SETUP_MAX_NETS][33];
        const size_t n = wifi_setup_networks(names, WIFI_SETUP_MAX_NETS);
        for (size_t i = 0; i < n; i++) {
            html_raw(out, PORTAL_PAGE_MAX, &o, "<option value=\"");
            html_text(out, PORTAL_PAGE_MAX, &o, names[i]);
            html_raw(out, PORTAL_PAGE_MAX, &o, "\">");
            html_text(out, PORTAL_PAGE_MAX, &o, names[i]);
            html_raw(out, PORTAL_PAGE_MAX, &o, "</option>");
        }
        html_raw(out, PORTAL_PAGE_MAX, &o,
                 "<option value=''>Other network (type its name below)</option></select></label>"
                 "<label>Other network name<input name=ssid_other maxlength=32 autocomplete=off></label>"
                 "<label>Wi-Fi password<input type=password name=password minlength=8 maxlength=63 required></label>"
                 "<label>New admin password<input type=password name=admin minlength=8 maxlength=63 required></label>"
                 "<label>Repeat admin password<input type=password name=admin2 minlength=8 maxlength=63 required></label>"
                 "<label style='display:flex;gap:8px;align-items:center'><input type=checkbox name=ttn checked>"
                 "Report to The Things Network (eu1)</label>"
                 "<button type=submit>Save without encryption</button></form></details>");
    }
    html_raw(out, PORTAL_PAGE_MAX, &o, "</main>");
    const esp_err_t r = portal_send(req, "200 OK", out, o);
    heap_caps_free(out);
    return r;
}

static esp_err_t portal_result(httpd_req_t *req, const char *status, const char *msg)
{
    char page[sizeof k_portal_head + 512];
    size_t o = 0;
    page[0] = '\0';
    html_raw(page, sizeof page, &o, k_portal_head);
    html_raw(page, sizeof page, &o, "<p>");
    html_text(page, sizeof page, &o, msg);
    html_raw(page, sizeof page, &o, "</p><a class=btn href='/plain'>Back</a></main>");
    return portal_send(req, status, page, o);
}

/* Plain-HTTP fallback: first-time claim only (an existing admin's session
 * cookie is Secure and never comes over HTTP). */
static esp_err_t portal_post(httpd_req_t *req)
{
    if (!wifi_setup_active()) {
        return send_status(req, "404 Not Found", "setup is closed\n");
    }
    if (!peer_is_lan(req)) {
        return send_status(req, "403 Forbidden", "LAN only\n");
    }
    char origin[48] = "";
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) == ESP_OK &&
        strcmp(origin, "http://192.168.4.1") != 0 && strcmp(origin, "null") != 0) {
        return send_status(req, "403 Forbidden", "wrong origin\n");
    }
    if (webauth_has_password(LWC_ROLE_ADMIN)) {
        return portal_result(req, "403 Forbidden", "This gateway already has an admin: use the secure setup page.");
    }
    char body[BODY_MAX + 1];
    size_t got;
    if (!read_body(req, body, &got)) {
        return portal_result(req, "400 Bad Request", "The form could not be read.");
    }
    esp_err_t err;
    const char *why = setup_save(body, got, true, true, &err);
    if (why != NULL) {
        return portal_result(req, err != ESP_OK ? "500 Internal Server Error" : "400 Bad Request", why);
    }
    (void)schedule_reboot("portal setup complete");
    char host[WEBAUTH_HOST_LEN], msg[256];
    webauth_hostname(host);
    snprintf(msg, sizeof msg,
             "Saved. The gateway restarts and joins your network. Switch this phone back to your own "
             "Wi-Fi, then open https://%s.local/ (or its address from your router).", host);
    return portal_result(req, "200 OK", msg);
}

static esp_err_t portal_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32790; /* clear of the HTTPS server and other httpd instances */
    cfg.stack_size = 10240; /* PBKDF2 in setup_save */
    cfg.core_id = 0;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_portal, &cfg), TAG, "portal");
    static const httpd_uri_t post = {.uri = "/setup", .method = HTTP_POST, .handler = portal_post};
    static const httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = portal_get};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_portal, &post), TAG, "portal post");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_portal, &get), TAG, "portal get");
    ESP_LOGI(TAG, "captive portal on port 80 (setup mode)");
    /* Phone apps whose DNS now points here fail TLS against our certificate
     * all the time during setup: keep that out of the log. */
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_NONE);
    esp_log_level_set("esp_https_server", ESP_LOG_NONE);
    esp_log_level_set("httpd", ESP_LOG_ERROR);
    return ESP_OK;
}

/* ---- start -------------------------------------------------------------------------- */

esp_err_t web_start(void)
{
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
    cfg.httpd.max_uri_handlers = 16;
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
        {.uri = "/api/setup", .method = HTTP_GET, .handler = api_setup_get},
        {.uri = "/api/ttn/qr.svg", .method = HTTP_GET, .handler = api_ttn_qr},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = api_settings},
        {.uri = "/api/setup", .method = HTTP_POST, .handler = api_setup_post},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_srv, &uris[i]), TAG, "uri %s", uris[i].uri);
    }
    char fp[100];
    webauth_fingerprint(fp, sizeof fp);
    ESP_LOGI(TAG, "HTTPS on port 443 (LAN only); certificate SHA-256 %s", fp);
    if (wifi_setup_active()) {
        (void)portal_start();
    }
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
        ESP_LOGW(TAG, "no admin password: set one with tools/web_password.py (console WEB_PASS)");
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
