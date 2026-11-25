/*
 * Payload codecs in JavaScript (ADR-0021), on QuickJS-NG.
 *
 * A codec is compiled once to bytecode (lwc_codec_compile). Every decode
 * then runs in a new runtime and context, so nothing is kept between calls
 * or between devices, under a memory limit, a C stack limit and a time
 * limit. Only the core language is there: no std/os modules, no file,
 * network or timer access.
 *
 * Decoders, tried in this order:
 *   decodeUplink(input)            TTN v3, ChirpStack v4: input {bytes, fPort,
 *                                  recvTime}; returns {data, warnings, errors}
 *   Decoder(bytes, port)           TTN v2: returns the data
 *   Decode(fPort, bytes, variables) ChirpStack v3: returns the data
 * The data must be a plain object; it is returned as JSON text.
 *
 * Encoders (downlinks), tried in this order:
 *   encodeDownlink(input)          TTN v3, ChirpStack v4: input {data, fPort}
 *                                  (fPort when asked); returns {bytes, fPort,
 *                                  warnings, errors}
 *   Encoder(object, port)          TTN v2: returns the bytes
 *   Encode(fPort, object, variables) ChirpStack v3: returns the bytes
 */
#ifndef LWC_CODEC_H
#define LWC_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_CODEC_ERR_MAX 160U

/** Where the engine's memory comes from, and the limits of one call. */
typedef struct {
    void *(*malloc_fn)(size_t size);
    void *(*realloc_fn)(void *ptr, size_t size);
    void (*free_fn)(void *ptr);
    size_t (*usable_fn)(const void *ptr); /* the size of a block from malloc_fn */
    int64_t (*now_us)(void);              /* a monotonic clock */
    size_t mem_limit;   /* bytes, for one runtime */
    size_t stack_limit; /* bytes of C stack the engine may use */
    uint32_t call_ms;   /* one decode, setup included */
    uint32_t compile_ms;
} lwc_codec_env_t;

/** Compiled codec. `bc` comes from env->malloc_fn. */
typedef struct {
    uint8_t *bc;
    size_t len;
    bool has_decode; /* decodeUplink, Decoder or Decode */
    bool has_encode; /* encodeDownlink, Encoder or Encode */
} lwc_codec_bin_t;

/**
 * Compile `src` (`n` bytes, need not be NUL-terminated) and run its top
 * level once to find its functions. False, with `err` set, for a syntax
 * error, an exception, a limit, or neither a decoder nor an encoder (a
 * device's decoder and encoder, published as two files, are stored joined).
 */
bool lwc_codec_compile(const lwc_codec_env_t *env, const char *src, size_t n, lwc_codec_bin_t *out,
                       char err[LWC_CODEC_ERR_MAX]);

void lwc_codec_bin_free(const lwc_codec_env_t *env, lwc_codec_bin_t *b);

/**
 * Decode one uplink. `recv_unix_ms` < 0: no recvTime. On success `out` holds
 * the data as JSON text (an object, NUL-terminated, at most `cap` - 1
 * bytes) and `warn` the decoder's warnings joined with "; " (may be empty).
 * False, with `err` set, otherwise (an exception, errors from the decoder,
 * no data, a limit, or output larger than `cap`).
 */
bool lwc_codec_decode(const lwc_codec_env_t *env, const lwc_codec_bin_t *b, uint8_t port, const uint8_t *data,
                      size_t len, int64_t recv_unix_ms, char *out, size_t cap, char *warn, size_t wcap,
                      char err[LWC_CODEC_ERR_MAX]);

/**
 * Encode one downlink. `object` is JSON text of one object (`n` bytes).
 * `port` 0: not asked (the codec must give one). On success `out` holds
 * *len bytes (at most `cap`), *port_out the FPort (the codec's, else
 * `port`), and `warn` the encoder's warnings. False, with `err` set, for an
 * exception, errors from the encoder, bytes that are not integers 0..255 or
 * too many, no FPort or one outside 1..223, or a limit.
 */
bool lwc_codec_encode(const lwc_codec_env_t *env, const lwc_codec_bin_t *b, const char *object, size_t n,
                      uint8_t port, uint8_t *out, size_t cap, size_t *len, uint8_t *port_out, char *warn, size_t wcap,
                      char err[LWC_CODEC_ERR_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* LWC_CODEC_H */
