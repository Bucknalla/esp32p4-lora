#include "lwc/codec.h"

#include <stdio.h>
#include <string.h>

#include "quickjs.h"

/* ---- one runtime per call ------------------------------------------------------
 * Every JSValue taken here is freed on every path: JS_FreeRuntime asserts that
 * no object is left, and a leak would stop the gateway. */

typedef struct {
    const lwc_codec_env_t *env;
    int64_t deadline_us;
} run_t;

/* the signatures of JSMallocFunctions and JSInterruptHandler are fixed */
// cppcheck-suppress constParameterCallback
static void *m_calloc(void *o, size_t count, size_t size)
{
    const run_t *r = o;
    if (size != 0U && count > SIZE_MAX / size) {
        return NULL;
    }
    void *p = r->env->malloc_fn(count * size);
    if (p != NULL) {
        memset(p, 0, count * size);
    }
    return p;
}

static void *m_malloc(void *o, size_t size)
{
    return ((const run_t *)o)->env->malloc_fn(size);
}

static void m_free(void *o, void *ptr)
{
    if (ptr != NULL) {
        ((const run_t *)o)->env->free_fn(ptr);
    }
}

static void *m_realloc(void *o, void *ptr, size_t size)
{
    return ((const run_t *)o)->env->realloc_fn(ptr, size);
}

/* JSMallocFunctions has no opaque for this one: the environment is global
 * enough (one allocator for the whole firmware). */
static size_t (*s_usable)(const void *ptr);

static size_t m_usable(const void *ptr)
{
    return s_usable != NULL && ptr != NULL ? s_usable(ptr) : 0U;
}

static const JSMallocFunctions MF = {m_calloc, m_malloc, m_free, m_realloc, m_usable};

// cppcheck-suppress constParameterCallback
static int interrupt(JSRuntime *rt, void *o)
{
    (void)rt;
    const run_t *r = o;
    return r->env->now_us() > r->deadline_us ? 1 : 0;
}

static JSRuntime *new_runtime(run_t *r, uint32_t ms)
{
    s_usable = r->env->usable_fn;
    r->deadline_us = r->env->now_us() + (int64_t)ms * 1000;
    JSRuntime *rt = JS_NewRuntime2(&MF, r);
    if (rt == NULL) {
        return NULL;
    }
    JS_SetMemoryLimit(rt, r->env->mem_limit);
    JS_SetMaxStackSize(rt, r->env->stack_limit);
    JS_SetInterruptHandler(rt, interrupt, r);
    return rt;
}

static void set_err(char err[LWC_CODEC_ERR_MAX], const char *what, const char *detail)
{
    if (err != NULL) {
        (void)snprintf(err, LWC_CODEC_ERR_MAX, "%s%s%s", what, detail != NULL && detail[0] != '\0' ? ": " : "",
                       detail != NULL ? detail : "");
    }
}

/* The pending exception as text ("TypeError: x is not a function"), then
 * cleared. A time-out reads as one. */
static void take_exception(JSContext *ctx, const run_t *r, const char *what, char err[LWC_CODEC_ERR_MAX])
{
    JSValue e = JS_GetException(ctx);
    if (r->env->now_us() > r->deadline_us) {
        JS_FreeValue(ctx, e);
        set_err(err, what, "time limit reached");
        return;
    }
    const char *s = JS_IsUncatchableError(e) ? NULL : JS_ToCString(ctx, e);
    if (s == NULL) {
        /* toString threw, or an uncatchable error (out of memory, stack) */
        JS_FreeValue(ctx, JS_GetException(ctx));
        set_err(err, what, JS_IsUncatchableError(e) ? "out of memory or stack" : "exception");
    } else {
        set_err(err, what, s);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, e);
}

static bool is_fn(JSContext *ctx, JSValueConst global, const char *name)
{
    JSValue f = JS_GetPropertyStr(ctx, global, name);
    const bool yes = JS_IsFunction(ctx, f);
    JS_FreeValue(ctx, f);
    if (JS_HasException(ctx)) { /* a getter threw */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    return yes;
}

/* ---- compile ------------------------------------------------------------------- */

bool lwc_codec_compile(const lwc_codec_env_t *env, const char *src, size_t n, lwc_codec_bin_t *out,
                       char err[LWC_CODEC_ERR_MAX])
{
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (env == NULL || src == NULL || out == NULL) {
        set_err(err, "compile", "bad arguments");
        return false;
    }
    /* JS_Eval wants a NUL after the text */
    char *text = env->malloc_fn(n + 1U);
    if (text == NULL) {
        set_err(err, "compile", "out of memory");
        return false;
    }
    memcpy(text, src, n);
    text[n] = '\0';
    run_t r = {.env = env};
    JSRuntime *rt = new_runtime(&r, env->compile_ms);
    JSContext *ctx = rt != NULL ? JS_NewContext(rt) : NULL;
    bool ok = false;
    if (ctx == NULL) {
        set_err(err, "compile", "out of memory");
    } else {
        JSValue fn = JS_Eval(ctx, text, n, "codec.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(fn)) {
            take_exception(ctx, &r, "syntax", err);
        } else {
            size_t blen = 0;
            uint8_t *buf = JS_WriteObject(ctx, &blen, fn, JS_WRITE_OBJ_BYTECODE);
            JSValue top = JS_EvalFunction(ctx, fn); /* takes fn */
            if (buf == NULL) {
                set_err(err, "compile", "out of memory");
            } else if (JS_IsException(top)) {
                take_exception(ctx, &r, "top level", err);
            } else {
                JSValue g = JS_GetGlobalObject(ctx);
                out->has_decode = is_fn(ctx, g, "decodeUplink") || is_fn(ctx, g, "Decoder") || is_fn(ctx, g, "Decode");
                out->has_encode =
                    is_fn(ctx, g, "encodeDownlink") || is_fn(ctx, g, "Encoder") || is_fn(ctx, g, "Encode");
                JS_FreeValue(ctx, g);
                out->bc = env->malloc_fn(blen);
                if (out->bc == NULL) {
                    set_err(err, "compile", "out of memory");
                } else if (!out->has_decode && !out->has_encode) {
                    set_err(err, "compile", "no decoder (decodeUplink, Decoder, Decode) or encoder (encodeDownlink, "
                                            "Encoder, Encode)");
                } else {
                    memcpy(out->bc, buf, blen);
                    out->len = blen;
                    ok = true;
                }
            }
            JS_FreeValue(ctx, top);
            if (buf != NULL) {
                js_free_rt(rt, buf);
            }
        }
        JS_FreeContext(ctx);
    }
    if (rt != NULL) {
        JS_FreeRuntime(rt);
    }
    env->free_fn(text);
    if (!ok) {
        lwc_codec_bin_free(env, out);
    }
    return ok;
}

void lwc_codec_bin_free(const lwc_codec_env_t *env, lwc_codec_bin_t *b)
{
    if (b == NULL) {
        return;
    }
    if (b->bc != NULL && env != NULL) {
        env->free_fn(b->bc);
    }
    memset(b, 0, sizeof *b);
}

/* ---- decode -------------------------------------------------------------------- */

/* Array of strings -> "a; b"; false (nothing written) if `v` is not a non-empty array. */
static bool join(JSContext *ctx, JSValueConst v, char *out, size_t cap)
{
    if (out == NULL || cap == 0U || !JS_IsArray(v)) {
        return false;
    }
    int64_t n = 0;
    if (JS_GetLength(ctx, v, &n) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    size_t o = 0;
    out[0] = '\0';
    for (int64_t i = 0; i < n && i < 8 && o + 1U < cap; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        const char *s = JS_ToCString(ctx, e);
        if (s != NULL) {
            const int w = snprintf(out + o, cap - o, "%s%s", o > 0U ? "; " : "", s);
            o = w < 0 ? o : (o + (size_t)w < cap ? o + (size_t)w : cap - 1U);
            JS_FreeCString(ctx, s);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, e);
    }
    return n > 0;
}

static bool plain_object(JSValueConst v)
{
    return JS_IsObject(v) && !JS_IsArray(v) && !JS_IsPromise(v);
}

bool lwc_codec_decode(const lwc_codec_env_t *env, const lwc_codec_bin_t *b, uint8_t port, const uint8_t *data,
                      size_t len, int64_t recv_unix_ms, char *out, size_t cap, char *warn, size_t wcap,
                      char err[LWC_CODEC_ERR_MAX])
{
    if (warn != NULL && wcap > 0U) {
        warn[0] = '\0';
    }
    if (out != NULL && cap > 0U) {
        out[0] = '\0';
    }
    if (env == NULL || b == NULL || b->bc == NULL || out == NULL || cap < 3U || (len > 0U && data == NULL)) {
        set_err(err, "decode", "bad arguments");
        return false;
    }
    run_t r = {.env = env};
    JSRuntime *rt = new_runtime(&r, env->call_ms);
    JSContext *ctx = rt != NULL ? JS_NewContext(rt) : NULL;
    bool ok = false;
    if (ctx == NULL) {
        set_err(err, "decode", "out of memory");
        if (rt != NULL) {
            JS_FreeRuntime(rt);
        }
        return false;
    }
    JSValue result = JS_UNDEFINED, payload = JS_UNDEFINED;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue obj = JS_ReadObject(ctx, b->bc, b->len, JS_READ_OBJ_BYTECODE);
    JSValue top = JS_IsException(obj) ? JS_EXCEPTION : JS_EvalFunction(ctx, obj); /* takes obj */
    JSValue bytes = JS_NewArray(ctx);
    for (size_t i = 0; i < len && !JS_IsException(bytes); i++) {
        if (JS_SetPropertyUint32(ctx, bytes, (uint32_t)i, JS_NewInt32(ctx, data[i])) < 0) {
            break;
        }
    }
    if (JS_IsException(top) || JS_IsException(bytes) || JS_HasException(ctx)) {
        take_exception(ctx, &r, "load", err);
    } else if (is_fn(ctx, g, "decodeUplink")) {
        JSValue in = JS_NewObject(ctx);
        (void)JS_SetPropertyStr(ctx, in, "bytes", JS_DupValue(ctx, bytes));
        (void)JS_SetPropertyStr(ctx, in, "fPort", JS_NewInt32(ctx, port));
        if (recv_unix_ms >= 0) {
            (void)JS_SetPropertyStr(ctx, in, "recvTime", JS_NewDate(ctx, (double)recv_unix_ms));
        }
        JSValue fn = JS_GetPropertyStr(ctx, g, "decodeUplink");
        result = JS_HasException(ctx) ? JS_EXCEPTION : JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&in);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, in);
        if (JS_IsException(result)) {
            take_exception(ctx, &r, "decodeUplink", err);
        } else if (!plain_object(result)) {
            set_err(err, "decodeUplink", JS_IsPromise(result) ? "must not be async" : "must return an object");
        } else {
            JSValue errors = JS_GetPropertyStr(ctx, result, "errors");
            JSValue warnings = JS_GetPropertyStr(ctx, result, "warnings");
            char e[LWC_CODEC_ERR_MAX];
            if (JS_HasException(ctx)) {
                take_exception(ctx, &r, "decodeUplink", err);
            } else if (join(ctx, errors, e, sizeof e)) {
                set_err(err, "decoder errors", e);
            } else {
                (void)join(ctx, warnings, warn, wcap);
                payload = JS_GetPropertyStr(ctx, result, "data");
                ok = !JS_HasException(ctx);
                if (!ok) {
                    take_exception(ctx, &r, "decodeUplink", err);
                }
            }
            JS_FreeValue(ctx, errors);
            JS_FreeValue(ctx, warnings);
        }
    } else if (is_fn(ctx, g, "Decoder") || is_fn(ctx, g, "Decode")) {
        const bool v2 = is_fn(ctx, g, "Decoder");
        JSValue fn = JS_GetPropertyStr(ctx, g, v2 ? "Decoder" : "Decode");
        JSValue vars = JS_NewObject(ctx);
        JSValue args[3];
        if (v2) {
            args[0] = bytes;
            args[1] = JS_NewInt32(ctx, port);
        } else {
            args[0] = JS_NewInt32(ctx, port);
            args[1] = bytes;
            args[2] = vars;
        }
        payload = JS_HasException(ctx) ? JS_EXCEPTION : JS_Call(ctx, fn, JS_UNDEFINED, v2 ? 2 : 3, (JSValueConst *)args);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, vars);
        ok = !JS_IsException(payload);
        if (!ok) {
            payload = JS_UNDEFINED;
            take_exception(ctx, &r, v2 ? "Decoder" : "Decode", err);
        }
    } else {
        set_err(err, "decode", "no decodeUplink, Decoder or Decode function");
    }
    if (ok && !plain_object(payload)) {
        ok = false;
        set_err(err, "decode", JS_IsPromise(payload) ? "must not be async" : "the data must be an object");
    }
    if (ok) {
        JSValue json = JS_JSONStringify(ctx, payload, JS_UNDEFINED, JS_UNDEFINED);
        size_t n = 0;
        const char *s = JS_IsException(json) ? NULL : JS_ToCStringLen(ctx, &n, json);
        if (s == NULL) {
            ok = false;
            take_exception(ctx, &r, "JSON", err);
        } else if (n + 1U > cap) {
            ok = false;
            char t[48];
            (void)snprintf(t, sizeof t, "%u bytes, at most %u", (unsigned)n, (unsigned)(cap - 1U));
            set_err(err, "data too large", t);
        } else {
            memcpy(out, s, n + 1U);
        }
        if (s != NULL) {
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, json);
    }
    JS_FreeValue(ctx, payload);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, bytes);
    JS_FreeValue(ctx, top);
    JS_FreeValue(ctx, g);
    if (JS_HasException(ctx)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (!ok) {
        out[0] = '\0';
        if (warn != NULL && wcap > 0U) {
            warn[0] = '\0';
        }
    }
    return ok;
}

/* ---- encode -------------------------------------------------------------------- */

/* d as an integer in lo..hi, else -1 (NaN and infinities too: the range is
 * checked before the cast, which would be undefined otherwise). */
static int int_in(double d, int lo, int hi)
{
    if (!(d >= (double)lo && d <= (double)hi)) {
        return -1;
    }
    const int i = (int)d;
    return (double)i == d ? i : -1;
}

/* A JS array of integers 0..255 -> bytes. NULL, or what is wrong. */
static const char *to_bytes(JSContext *ctx, JSValueConst v, uint8_t *out, size_t cap, size_t *len)
{
    if (!JS_IsArray(v)) {
        /* a Uint8Array is accepted too */
        size_t un = 0;
        const uint8_t *u = JS_IsObject(v) ? JS_GetUint8Array(ctx, &un, v) : NULL;
        if (u == NULL) {
            if (JS_HasException(ctx)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            return "the bytes must be an array";
        }
        if (un > cap) {
            return "too many bytes";
        }
        memcpy(out, u, un);
        *len = un;
        return NULL;
    }
    int64_t n = 0;
    if (JS_GetLength(ctx, v, &n) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return "the bytes have no length";
    }
    if (n < 0 || (uint64_t)n > cap) {
        return "too many bytes";
    }
    for (int64_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        double d = -1;
        const bool num = JS_IsNumber(e) && JS_ToFloat64(ctx, &d, e) == 0;
        JS_FreeValue(ctx, e);
        if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return "the bytes cannot be read";
        }
        const int v8 = num ? int_in(d, 0, 255) : -1;
        if (v8 < 0) {
            return "the bytes must be integers 0 to 255";
        }
        out[i] = (uint8_t)v8;
    }
    *len = (size_t)n;
    return NULL;
}

bool lwc_codec_encode(const lwc_codec_env_t *env, const lwc_codec_bin_t *b, const char *object, size_t n,
                      uint8_t port, uint8_t *out, size_t cap, size_t *len, uint8_t *port_out, char *warn, size_t wcap,
                      char err[LWC_CODEC_ERR_MAX])
{
    if (warn != NULL && wcap > 0U) {
        warn[0] = '\0';
    }
    if (len != NULL) {
        *len = 0;
    }
    if (env == NULL || b == NULL || b->bc == NULL || object == NULL || out == NULL || len == NULL ||
        port_out == NULL) {
        set_err(err, "encode", "bad arguments");
        return false;
    }
    /* JS_ParseJSON wants a NUL after the text */
    char *text = env->malloc_fn(n + 1U);
    if (text == NULL) {
        set_err(err, "encode", "out of memory");
        return false;
    }
    memcpy(text, object, n);
    text[n] = '\0';
    run_t r = {.env = env};
    JSRuntime *rt = new_runtime(&r, env->call_ms);
    JSContext *ctx = rt != NULL ? JS_NewContext(rt) : NULL;
    if (ctx == NULL) {
        set_err(err, "encode", "out of memory");
        if (rt != NULL) {
            JS_FreeRuntime(rt);
        }
        env->free_fn(text);
        return false;
    }
    bool ok = false;
    int out_port = port;
    JSValue result = JS_UNDEFINED, bytes = JS_UNDEFINED;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue obj = JS_ReadObject(ctx, b->bc, b->len, JS_READ_OBJ_BYTECODE);
    JSValue top = JS_IsException(obj) ? JS_EXCEPTION : JS_EvalFunction(ctx, obj); /* takes obj */
    JSValue data = JS_IsException(top) ? JS_UNDEFINED : JS_ParseJSON(ctx, text, n, "object");
    if (JS_IsException(top) || JS_IsException(data) || JS_HasException(ctx)) {
        take_exception(ctx, &r, "load", err);
    } else if (!plain_object(data)) {
        set_err(err, "encode", "the object must be a JSON object");
    } else if (is_fn(ctx, g, "encodeDownlink")) {
        JSValue in = JS_NewObject(ctx);
        (void)JS_SetPropertyStr(ctx, in, "data", JS_DupValue(ctx, data));
        if (port != 0U) {
            (void)JS_SetPropertyStr(ctx, in, "fPort", JS_NewInt32(ctx, port));
        }
        (void)JS_SetPropertyStr(ctx, in, "variables", JS_NewObject(ctx));
        JSValue fn = JS_GetPropertyStr(ctx, g, "encodeDownlink");
        result = JS_HasException(ctx) ? JS_EXCEPTION : JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&in);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, in);
        if (JS_IsException(result)) {
            take_exception(ctx, &r, "encodeDownlink", err);
        } else if (!plain_object(result)) {
            set_err(err, "encodeDownlink", JS_IsPromise(result) ? "must not be async" : "must return an object");
        } else {
            JSValue errors = JS_GetPropertyStr(ctx, result, "errors");
            JSValue warnings = JS_GetPropertyStr(ctx, result, "warnings");
            JSValue fp = JS_GetPropertyStr(ctx, result, "fPort");
            char e[LWC_CODEC_ERR_MAX];
            if (JS_HasException(ctx)) {
                take_exception(ctx, &r, "encodeDownlink", err);
            } else if (join(ctx, errors, e, sizeof e)) {
                set_err(err, "encoder errors", e);
            } else {
                (void)join(ctx, warnings, warn, wcap);
                if (!JS_IsUndefined(fp) && !JS_IsNull(fp)) {
                    double d = -1;
                    out_port = JS_IsNumber(fp) && JS_ToFloat64(ctx, &d, fp) == 0 ? int_in(d, 1, 223) : -1;
                }
                bytes = JS_GetPropertyStr(ctx, result, "bytes");
                ok = !JS_HasException(ctx);
                if (!ok) {
                    take_exception(ctx, &r, "encodeDownlink", err);
                }
            }
            JS_FreeValue(ctx, errors);
            JS_FreeValue(ctx, warnings);
            JS_FreeValue(ctx, fp);
        }
    } else if (is_fn(ctx, g, "Encoder") || is_fn(ctx, g, "Encode")) {
        const bool v2 = is_fn(ctx, g, "Encoder");
        JSValue fn = JS_GetPropertyStr(ctx, g, v2 ? "Encoder" : "Encode");
        JSValue vars = JS_NewObject(ctx);
        JSValue args[3];
        if (v2) {
            args[0] = data;
            args[1] = JS_NewInt32(ctx, port);
        } else {
            args[0] = JS_NewInt32(ctx, port);
            args[1] = data;
            args[2] = vars;
        }
        bytes = JS_HasException(ctx) ? JS_EXCEPTION : JS_Call(ctx, fn, JS_UNDEFINED, v2 ? 2 : 3, (JSValueConst *)args);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, vars);
        ok = !JS_IsException(bytes);
        if (!ok) {
            bytes = JS_UNDEFINED;
            take_exception(ctx, &r, v2 ? "Encoder" : "Encode", err);
        }
    } else {
        set_err(err, "encode", "no encodeDownlink, Encoder or Encode function");
    }
    if (ok) {
        const char *why = JS_IsPromise(bytes) ? "must not be async" : to_bytes(ctx, bytes, out, cap, len);
        if (why != NULL) {
            ok = false;
            *len = 0;
            set_err(err, "encode", r.env->now_us() > r.deadline_us ? "time limit reached" : why);
        } else if (out_port < 1 || out_port > 223) {
            ok = false;
            *len = 0;
            set_err(err, "encode", out_port == 0 ? "no FPort: give port, or the codec must set fPort"
                                                 : "the FPort must be 1 to 223");
        } else {
            *port_out = (uint8_t)out_port;
        }
    }
    JS_FreeValue(ctx, bytes);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, top);
    JS_FreeValue(ctx, g);
    if (JS_HasException(ctx)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    env->free_fn(text);
    if (!ok && warn != NULL && wcap > 0U) {
        warn[0] = '\0';
    }
    return ok;
}
