/*
 * Line protocol for the service console (USB serial).
 *
 *   WIFI_SET <ssid-hex> <password-hex>
 *   WIFI_CLEAR
 *   TX_TEST          one low-power test frame (bring-up)
 *   OTA_CHECK        check the update URL now (install if newer)
 *   STATUS           firmware version, slot, OTA state, IP, faults
 *   WEB_PASS <admin|viewer> <password-hex>   set a web UI password (8..63 bytes)
 *   TEST_ABORT       test builds only: abort (coredump, reset)
 *   TEST_STALL       test builds only: console task stops beating (watchdog)
 *
 * Values are hex-encoded so any byte (spaces, quotes) survives the line format
 * and the console never needs an escaping rule. Input is untrusted: every
 * length is checked, nothing is allocated, output buffers are caller-owned.
 */
#ifndef LWC_CONSOLE_CMD_H
#define LWC_CONSOLE_CMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_WIFI_SSID_MAX 32U /* bytes, IEEE 802.11 */
#define LWC_WIFI_PASS_MIN 8U  /* WPA2-PSK passphrase */
#define LWC_WIFI_PASS_MAX 63U

typedef enum {
    LWC_CMD_NONE = 0, /* empty line */
    LWC_CMD_WIFI_SET,
    LWC_CMD_WIFI_CLEAR,
    LWC_CMD_TX_TEST,
    LWC_CMD_OTA_CHECK,
    LWC_CMD_STATUS,
    LWC_CMD_TEST_ABORT,
    LWC_CMD_TEST_STALL,
    LWC_CMD_WEB_PASS,
} lwc_cmd_kind_t;

typedef enum {
    LWC_CMD_OK = 0,
    LWC_CMD_ERR_ARG,     /* NULL pointer */
    LWC_CMD_ERR_UNKNOWN, /* unknown command word */
    LWC_CMD_ERR_SYNTAX,  /* wrong number of fields, bad hex */
    LWC_CMD_ERR_LENGTH,  /* value too short or too long */
} lwc_cmd_err_t;

typedef struct {
    lwc_cmd_kind_t kind;
    char ssid[LWC_WIFI_SSID_MAX + 1U]; /* NUL-terminated */
    char pass[LWC_WIFI_PASS_MAX + 1U]; /* NUL-terminated; WIFI_SET and WEB_PASS */
    uint8_t role;                      /* WEB_PASS: 1 viewer, 2 admin (lwc_role_t) */
} lwc_cmd_t;

/** Parse one line (without or with trailing CR/LF). */
lwc_cmd_err_t lwc_cmd_parse(const char *line, size_t len, lwc_cmd_t *out);

/** Overwrite the secrets in a parsed command. */
void lwc_cmd_wipe(lwc_cmd_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* LWC_CONSOLE_CMD_H */
