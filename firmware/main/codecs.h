/* Payload codecs (ADR-0021): JavaScript decoders stored on the "storage"
 * partition (littlefs, /storage/codecs/<name>.js), compiled once to bytecode
 * and run by one task with limits (core/src/codec.c). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lwc/codec.h"

#define CODEC_NAME_MAX 31U            /* [a-z0-9_-] */
#define CODEC_SRC_MAX (128U * 1024U)  /* one script */
#define CODECS_MAX 16U

/** Mount the storage, start the codec task, compile the stored codecs. A
 *  storage that cannot be used leaves codecs off; the gateway runs on. */
esp_err_t codecs_start(void);

bool codecs_name_ok(const char *name);

/** True if a codec of that name is stored (compiled or not). */
bool codecs_exists(const char *name);

/**
 * Decode with codec `name` (see lwc_codec_decode). Waits for the codec task;
 * the limits bound the wait. False, with `err` set, if it failed.
 */
bool codecs_decode(const char *name, uint8_t port, const uint8_t *data, size_t len, int64_t recv_unix_ms, char *out,
                   size_t cap, char *warn, size_t wcap, char err[LWC_CODEC_ERR_MAX]);

/** Encode a downlink with codec `name` (see lwc_codec_encode). */
bool codecs_encode(const char *name, const char *object, size_t n, uint8_t port, uint8_t *out, size_t cap, size_t *len,
                   uint8_t *port_out, char *warn, size_t wcap, char err[LWC_CODEC_ERR_MAX]);

/** Compile `src`; if it compiles, store it as `name` (replacing one).
 *  ESP_ERR_INVALID_ARG: bad name or size, or it does not compile (`err`);
 *  ESP_ERR_NO_MEM: CODECS_MAX stored; ESP_ERR_INVALID_STATE: storage off. */
esp_err_t codecs_put(const char *name, const char *src, size_t n, char err[LWC_CODEC_ERR_MAX]);

/** ESP_ERR_NOT_FOUND if there is none. */
esp_err_t codecs_delete(const char *name);

/** The stored source, in a buffer from malloc the caller frees. */
esp_err_t codecs_source(const char *name, char **buf, size_t *len);

/** [{"name":..,"size":..,"ok":true|false,"error":..,"encode":..}] */
size_t codecs_json(char *out, size_t cap);

/** codecs=N storage=on|off decoded=N encoded=N failed=N */
void codecs_status_line(char *buf, size_t len);
