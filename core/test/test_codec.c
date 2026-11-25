#include "lwc/codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define USABLE(p) malloc_size(p)
#else
#include <malloc.h>
#define USABLE(p) malloc_usable_size((void *)(p))
#endif

#include "lwc/json.h"

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

/* every block counted: nothing may be left after a call */
static long s_blocks;

static void *t_malloc(size_t n)
{
    void *p = malloc(n == 0U ? 1U : n);
    if (p != NULL) {
        s_blocks++;
    }
    return p;
}

static void *t_realloc(void *p, size_t n)
{
    if (n == 0U) {
        if (p != NULL) {
            s_blocks--;
        }
        free(p);
        return NULL;
    }
    void *q = realloc(p, n);
    if (q != NULL && p == NULL) {
        s_blocks++;
    }
    return q;
}

static void t_free(void *p)
{
    if (p != NULL) {
        s_blocks--;
    }
    free(p);
}

static size_t t_usable(const void *p)
{
    return USABLE(p);
}

static int64_t t_now(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* the firmware's limits (ADR-0021); the C stack limit suits the host's 8 MB stack */
static const lwc_codec_env_t ENV = {.malloc_fn = t_malloc,
                                    .realloc_fn = t_realloc,
                                    .free_fn = t_free,
                                    .usable_fn = t_usable,
                                    .now_us = t_now,
                                    .mem_limit = 1024 * 1024,
                                    .stack_limit = 256 * 1024,
                                    .call_ms = 200,
                                    .compile_ms = 500};

static char out[1024], warn[128], err[LWC_CODEC_ERR_MAX];

/* compile `src`, decode `bytes` on `port`; true if both worked */
static bool run(const char *src, uint8_t port, const uint8_t *bytes, size_t n, int64_t ms)
{
    lwc_codec_bin_t b;
    err[0] = '\0';
    if (!lwc_codec_compile(&ENV, src, strlen(src), &b, err)) {
        return false;
    }
    const bool ok = lwc_codec_decode(&ENV, &b, port, bytes, n, ms, out, sizeof out, warn, sizeof warn, err);
    lwc_codec_bin_free(&ENV, &b);
    return ok;
}

static int valid_object(const char *s)
{
    lwc_jr_t r = {.p = s, .end = s + strlen(s)};
    if (s[0] != '{' || !lwc_jr_skip(&r)) {
        return 0;
    }
    lwc_jr_ws(&r);
    return r.p == r.end;
}

static const uint8_t TH[] = {0x01, 0x75, 0x64, 0x03, 0x67, 0x10, 0x01};

static void test_formats(void)
{
    /* TTN v3 / ChirpStack v4 */
    CHECK(run("function decodeUplink(input) {"
              "  var b = input.bytes;"
              "  return { data: { port: input.fPort, battery: b[2], temperature: ((b[6] << 8) | b[5]) / 10 } };"
              "}",
              85, TH, sizeof TH, -1));
    CHECK(strcmp(out, "{\"port\":85,\"battery\":100,\"temperature\":27.2}") == 0 && warn[0] == '\0');
    /* warnings joined; recvTime is a Date */
    CHECK(run("function decodeUplink(i) { return { data: { t: i.recvTime.toISOString(), n: i.bytes.length },"
              " warnings: ['low battery', 'old firmware'] }; }",
              1, TH, 2, 1795515300000LL));
    CHECK(strcmp(out, "{\"t\":\"2026-11-24T10:15:00.000Z\",\"n\":2}") == 0);
    CHECK(strcmp(warn, "low battery; old firmware") == 0);
    /* no clock: no recvTime */
    CHECK(run("function decodeUplink(i) { return { data: { has: i.recvTime !== undefined } }; }", 1, TH, 1, -1));
    CHECK(strcmp(out, "{\"has\":false}") == 0);
    /* errors from the decoder */
    CHECK(!run("function decodeUplink(i) { return { errors: ['unknown frame type', 'bad length'] }; }", 1, TH, 1, -1));
    CHECK(strcmp(err, "decoder errors: unknown frame type; bad length") == 0 && out[0] == '\0');
    /* TTN v2 and ChirpStack v3 */
    CHECK(run("function Decoder(bytes, port) { return { port: port, first: bytes[0] }; }", 9, TH, sizeof TH, -1));
    CHECK(strcmp(out, "{\"port\":9,\"first\":1}") == 0);
    CHECK(run("function Decode(fPort, bytes, variables) { return { port: fPort, n: bytes.length,"
              " v: typeof variables }; }",
              3, TH, sizeof TH, -1));
    CHECK(strcmp(out, "{\"port\":3,\"n\":7,\"v\":\"object\"}") == 0);
    /* decodeUplink wins over the older ones; empty payload */
    CHECK(run("function Decoder() { return { which: 'v2' }; }"
              "function decodeUplink(i) { return { data: { which: 'v3', n: i.bytes.length } }; }",
              1, NULL, 0, -1));
    CHECK(strcmp(out, "{\"which\":\"v3\",\"n\":0}") == 0);
    /* strings are escaped by JSON.stringify; the result is one valid object */
    CHECK(run("function decodeUplink() { return { data: { s: 'a\"b\\\\c\\n\\u0001\\u00e9' } }; }", 1, TH, 1, -1));
    CHECK(valid_object(out));
}

static void test_refused(void)
{
    lwc_codec_bin_t b;
    /* compile time */
    CHECK(!lwc_codec_compile(&ENV, "function decodeUplink( {", 24, &b, err) && strncmp(err, "syntax: SyntaxError", 19) == 0);
    CHECK(b.bc == NULL);
    CHECK(!lwc_codec_compile(&ENV, "var x = 1;", 10, &b, err) && strstr(err, "no decoder") != NULL);
    CHECK(!lwc_codec_compile(&ENV, "throw new Error('boom')", 23, &b, err) && strcmp(err, "top level: Error: boom") == 0);
    CHECK(!lwc_codec_compile(&ENV, "while (true) {}", 15, &b, err) && strcmp(err, "top level: time limit reached") == 0);
    CHECK(lwc_codec_compile(&ENV, "function Encoder() {}", 21, &b, err) && !b.has_decode && b.has_encode); /* an encoder alone */
    CHECK(!lwc_codec_decode(&ENV, &b, 1, TH, 1, -1, out, sizeof out, warn, sizeof warn, err) &&
          strstr(err, "no decodeUplink") != NULL);
    lwc_codec_bin_free(&ENV, &b);
    const char *both = "function decodeUplink(){} function encodeDownlink(){}";
    CHECK(lwc_codec_compile(&ENV, both, strlen(both), &b, err) && b.has_decode &&
          b.has_encode && b.len > 0U);
    lwc_codec_bin_free(&ENV, &b);
    CHECK(b.bc == NULL && b.len == 0U);
    /* not NUL-terminated input */
    const char src[] = "function Decoder(b){return{n:b.length}}XXXX";
    CHECK(lwc_codec_compile(&ENV, src, sizeof src - 5U, &b, err));
    lwc_codec_bin_free(&ENV, &b);

    /* at run time */
    CHECK(!run("function decodeUplink() { return 42; }", 1, TH, 1, -1) && strstr(err, "must return an object") != NULL);
    CHECK(!run("function decodeUplink() { return { data: 42 }; }", 1, TH, 1, -1) &&
          strstr(err, "the data must be an object") != NULL);
    CHECK(!run("function decodeUplink() { return { data: [1, 2] }; }", 1, TH, 1, -1) &&
          strstr(err, "the data must be an object") != NULL);
    CHECK(!run("function decodeUplink() { return {}; }", 1, TH, 1, -1)); /* no data */
    CHECK(!run("async function decodeUplink() { return { data: {} }; }", 1, TH, 1, -1) &&
          strstr(err, "must not be async") != NULL);
    CHECK(!run("function Decoder() { return Promise.resolve({}); }", 1, TH, 1, -1) &&
          strstr(err, "must not be async") != NULL);
    CHECK(!run("function decodeUplink() { throw new TypeError('bad frame'); }", 1, TH, 1, -1) &&
          strcmp(err, "decodeUplink: TypeError: bad frame") == 0);
    CHECK(!run("function decodeUplink() { throw 42; }", 1, TH, 1, -1) && strcmp(err, "decodeUplink: 42") == 0);
    CHECK(!run("function decodeUplink() { throw { toString: function () { throw 1; } }; }", 1, TH, 1, -1) &&
          strcmp(err, "decodeUplink: exception") == 0);
    CHECK(!run("function decodeUplink() { return { data: { n: 1n } }; }", 1, TH, 1, -1) &&
          strncmp(err, "JSON: TypeError", 15) == 0);
    /* too large for the output buffer */
    CHECK(!run("function decodeUplink() { return { data: { s: 'x'.repeat(2000) } }; }", 1, TH, 1, -1) &&
          strncmp(err, "data too large", 14) == 0);
    /* hostile scripts: the limits stop them, nothing leaks */
    CHECK(!run("function decodeUplink() { while (true) {} }", 1, TH, 1, -1) &&
          strcmp(err, "decodeUplink: time limit reached") == 0);
    CHECK(!run("function decodeUplink() { return { data: { toJSON: function () { while (true) {} } } }; }", 1, TH, 1,
               -1) &&
          strcmp(err, "JSON: time limit reached") == 0);
    CHECK(!run("function decodeUplink() { return /(a+)+$/.test('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!'); }", 1, TH,
               1, -1) &&
          (strcmp(err, "decodeUplink: time limit reached") == 0 || strstr(err, "RangeError") != NULL)); /* either stops it */
    CHECK(!run("function f(n) { return f(n + 1) + 1; } function decodeUplink() { return { data: { r: f(0) } }; }", 1,
               TH, 1, -1) &&
          strstr(err, "RangeError") != NULL);
    CHECK(!run("function decodeUplink() { var a = []; for (;;) a.push(new Array(10000).fill(1)); }", 1, TH, 1, -1) &&
          strstr(err, "decodeUplink") != NULL);
    CHECK(!run("var g = 0; Object.defineProperty(globalThis, 'decodeUplink', { get: function () { throw 1; } });"
               "function Decoder() { return { ok: 1 }; }",
               1, TH, 1, -1) ||
          strcmp(out, "{\"ok\":1}") == 0);
    /* bad arguments */
    CHECK(!lwc_codec_decode(&ENV, NULL, 1, TH, 1, -1, out, sizeof out, warn, sizeof warn, err));
    lwc_codec_bin_t empty = {0};
    CHECK(!lwc_codec_decode(&ENV, &empty, 1, TH, 1, -1, out, sizeof out, warn, sizeof warn, err));
}

/* nothing is kept between calls: a global set by one call is gone in the next */
static void test_isolation(void)
{
    lwc_codec_bin_t b;
    const char *src = "var calls = 0; function decodeUplink() { calls++; return { data: { calls: calls } }; }";
    CHECK(lwc_codec_compile(&ENV, src, strlen(src), &b, err));
    for (int i = 0; i < 3; i++) {
        CHECK(lwc_codec_decode(&ENV, &b, 1, TH, 1, -1, out, sizeof out, warn, sizeof warn, err));
        CHECK(strcmp(out, "{\"calls\":1}") == 0);
    }
    lwc_codec_bin_free(&ENV, &b);
}

static uint8_t eb[242];
static size_t elen;
static uint8_t eport;

/* compile `src`, encode `obj` asking `port`; true if both worked */
static bool enc(const char *src, const char *obj, uint8_t port)
{
    lwc_codec_bin_t b;
    err[0] = '\0';
    elen = 99;
    eport = 0;
    if (!lwc_codec_compile(&ENV, src, strlen(src), &b, err)) {
        return false;
    }
    const bool ok = lwc_codec_encode(&ENV, &b, obj, strlen(obj), port, eb, sizeof eb, &elen, &eport, warn,
                                     sizeof warn, err);
    lwc_codec_bin_free(&ENV, &b);
    return ok;
}

static const char DEC[] = "function decodeUplink() { return { data: {} }; }";

static void test_encode(void)
{
    char src[512];
    /* TTN v3 / ChirpStack v4: fPort from the codec, else the request */
    snprintf(src, sizeof src, "%s function encodeDownlink(i) { return { bytes: [i.data.on ? 1 : 0, i.data.level],"
                              " fPort: 10, warnings: i.fPort ? ['port ' + i.fPort + ' asked'] : [] }; }", DEC);
    CHECK(enc(src, "{\"on\":true,\"level\":200}", 0) && elen == 2U && eb[0] == 1U && eb[1] == 200U && eport == 10U &&
          warn[0] == '\0');
    CHECK(enc(src, "{\"on\":false,\"level\":7}", 5) && eb[0] == 0U && eport == 10U && strcmp(warn, "port 5 asked") == 0);
    snprintf(src, sizeof src, "%s function encodeDownlink(i) { return { bytes: [i.data.x] }; }", DEC);
    CHECK(enc(src, "{\"x\":255}", 3) && elen == 1U && eb[0] == 255U && eport == 3U);
    CHECK(!enc(src, "{\"x\":1}", 0) && strstr(err, "no FPort") != NULL);
    /* a Uint8Array, and an empty payload */
    snprintf(src, sizeof src, "%s function encodeDownlink(i) { return { bytes: new Uint8Array([9, 8, 7]), fPort: 2 }; }", DEC);
    CHECK(enc(src, "{}", 0) && elen == 3U && eb[2] == 7U && eport == 2U);
    snprintf(src, sizeof src, "%s function encodeDownlink() { return { bytes: [], fPort: 2 }; }", DEC);
    CHECK(enc(src, "{}", 0) && elen == 0U);
    /* TTN v2 and ChirpStack v3 */
    snprintf(src, sizeof src, "%s function Encoder(o, port) { return [o.a, port]; }", DEC);
    CHECK(enc(src, "{\"a\":42}", 6) && elen == 2U && eb[0] == 42U && eb[1] == 6U && eport == 6U);
    snprintf(src, sizeof src, "%s function Encode(fPort, o, vars) { return [fPort, o.a, typeof vars === 'object' ? 1 : 0]; }",
             DEC);
    CHECK(enc(src, "{\"a\":43}", 4) && elen == 3U && eb[0] == 4U && eb[1] == 43U && eb[2] == 1U);
    CHECK(!enc(src, "{\"a\":43}", 0) && strstr(err, "no FPort") != NULL);
    /* refused */
    snprintf(src, sizeof src, "%s function encodeDownlink() { return { errors: ['level out of range'] }; }", DEC);
    CHECK(!enc(src, "{}", 1) && strcmp(err, "encoder errors: level out of range") == 0 && elen == 0U);
    static const char *const bad_bytes[] = {"[256]", "[-1]", "[1.5]", "['1']", "[null]", "'0102'", "{}", "[NaN]",
                                            "[Infinity]", "new Array(243).fill(0)", "undefined"};
    for (size_t i = 0; i < sizeof bad_bytes / sizeof bad_bytes[0]; i++) {
        snprintf(src, sizeof src, "%s function encodeDownlink() { return { bytes: %s, fPort: 1 }; }", DEC, bad_bytes[i]);
        CHECK(!enc(src, "{}", 1) && strncmp(err, "encode: ", 8) == 0 && elen == 0U);
    }
    snprintf(src, sizeof src, "%s function encodeDownlink() { return { bytes: new Array(242).fill(1), fPort: 1 }; }", DEC);
    CHECK(enc(src, "{}", 0) && elen == 242U);
    static const char *const bad_port[] = {"0", "224", "1.5", "'2'", "-1"};
    for (size_t i = 0; i < sizeof bad_port / sizeof bad_port[0]; i++) {
        snprintf(src, sizeof src, "%s function encodeDownlink() { return { bytes: [1], fPort: %s }; }", DEC, bad_port[i]);
        CHECK(!enc(src, "{}", 1) && strstr(err, "FPort") != NULL);
    }
    snprintf(src, sizeof src, "%s async function encodeDownlink() { return { bytes: [1] }; }", DEC);
    CHECK(!enc(src, "{}", 1) && strstr(err, "must not be async") != NULL);
    snprintf(src, sizeof src, "%s function Encoder() { return Promise.resolve([1]); }", DEC);
    CHECK(!enc(src, "{}", 1) && strstr(err, "must not be async") != NULL);
    snprintf(src, sizeof src, "%s function encodeDownlink() { while (true) {} }", DEC);
    CHECK(!enc(src, "{}", 1) && strcmp(err, "encodeDownlink: time limit reached") == 0);
    snprintf(src, sizeof src, "%s function encodeDownlink() { throw new RangeError('bad'); }", DEC);
    CHECK(!enc(src, "{}", 1) && strcmp(err, "encodeDownlink: RangeError: bad") == 0);
    /* a getter on the bytes array that loops: stopped by the time limit */
    snprintf(src, sizeof src, "%s function encodeDownlink() { var a = [1]; Object.defineProperty(a, 0, { get: function"
                              " () { while (true) {} } }); return { bytes: a, fPort: 1 }; }", DEC);
    CHECK(!enc(src, "{}", 1) && strstr(err, "time limit") != NULL);
    /* the object must be one JSON object */
    snprintf(src, sizeof src, "%s function encodeDownlink() { return { bytes: [1], fPort: 1 }; }", DEC);
    CHECK(!enc(src, "[1]", 1) && strstr(err, "must be a JSON object") != NULL);
    CHECK(!enc(src, "{\"a\":", 1) && strncmp(err, "load: SyntaxError", 17) == 0);
    /* no encoder */
    CHECK(!enc(DEC, "{}", 1) && strstr(err, "no encodeDownlink") != NULL);
    lwc_codec_bin_t b;
    CHECK(lwc_codec_compile(&ENV, DEC, strlen(DEC), &b, err) && !b.has_encode);
    CHECK(!lwc_codec_encode(&ENV, &b, NULL, 0, 1, eb, sizeof eb, &elen, &eport, warn, sizeof warn, err));
    lwc_codec_bin_free(&ENV, &b);
}

int main(void)
{
    test_formats();
    test_refused();
    test_isolation();
    test_encode();
    CHECK(s_blocks == 0);
    if (s_blocks != 0) {
        fprintf(stderr, "test_codec: %ld block(s) left\n", s_blocks);
    }
    if (failures != 0) {
        fprintf(stderr, "test_codec: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_codec: all passed\n");
    return 0;
}
