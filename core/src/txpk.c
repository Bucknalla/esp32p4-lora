#include "lwc/txpk.h"

#include <string.h>

/* ==== minimal JSON reader =================================================
 * Enough for txpk: objects, strings without escapes in values we use (escapes
 * are skipped correctly elsewhere), numbers, true/false/null, and arrays or
 * nested objects in keys we skip. Depth-limited; never reads past `end`. */

#define JSON_MAX_DEPTH 8

typedef struct {
    const char *p;
    const char *end;
} jr_t;

static void ws(jr_t *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

static int lit(jr_t *r, char c)
{
    ws(r);
    if (r->p < r->end && *r->p == c) {
        r->p++;
        return 1;
    }
    return 0;
}

/* String: on success sets [*s, *n) to the raw contents (escapes not decoded). */
static int str(jr_t *r, const char **s, size_t *n)
{
    ws(r);
    if (r->p >= r->end || *r->p != '"') {
        return 0;
    }
    r->p++;
    const char *start = r->p;
    while (r->p < r->end && *r->p != '"') {
        if ((unsigned char)*r->p < 0x20U) {
            return 0;
        }
        if (*r->p == '\\') {
            r->p++;
            if (r->p >= r->end) {
                return 0;
            }
        }
        r->p++;
    }
    if (r->p >= r->end) {
        return 0;
    }
    *s = start;
    *n = (size_t)(r->p - start);
    r->p++;
    return 1;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Number token: sets [*s, *n). Validates JSON number syntax. */
static int num(jr_t *r, const char **s, size_t *n)
{
    ws(r);
    const char *start = r->p;
    if (r->p < r->end && *r->p == '-') {
        r->p++;
    }
    if (r->p >= r->end || !is_digit(*r->p)) {
        return 0;
    }
    while (r->p < r->end && is_digit(*r->p)) {
        r->p++;
    }
    if (r->p < r->end && *r->p == '.') {
        r->p++;
        if (r->p >= r->end || !is_digit(*r->p)) {
            return 0;
        }
        while (r->p < r->end && is_digit(*r->p)) {
            r->p++;
        }
    }
    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
        r->p++;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-')) {
            r->p++;
        }
        if (r->p >= r->end || !is_digit(*r->p)) {
            return 0;
        }
        while (r->p < r->end && is_digit(*r->p)) {
            r->p++;
        }
    }
    *s = start;
    *n = (size_t)(r->p - start);
    return 1;
}

static int word(jr_t *r, const char *w)
{
    ws(r);
    const size_t n = strlen(w);
    if ((size_t)(r->end - r->p) >= n && memcmp(r->p, w, n) == 0) {
        r->p += n;
        return 1;
    }
    return 0;
}

static int skip_value(jr_t *r, int depth);

static int skip_container(jr_t *r, int depth, char close, int is_obj)
{
    if (depth > JSON_MAX_DEPTH) {
        return 0;
    }
    if (lit(r, close)) {
        return 1;
    }
    for (;;) {
        if (is_obj) {
            const char *k;
            size_t kn;
            if (!str(r, &k, &kn) || !lit(r, ':')) {
                return 0;
            }
        }
        if (!skip_value(r, depth + 1)) {
            return 0;
        }
        if (lit(r, ',')) {
            continue;
        }
        return lit(r, close);
    }
}

static int skip_value(jr_t *r, int depth)
{
    const char *s;
    size_t n;
    ws(r);
    if (r->p >= r->end) {
        return 0;
    }
    switch (*r->p) {
    case '{': r->p++; return skip_container(r, depth, '}', 1);
    case '[': r->p++; return skip_container(r, depth, ']', 0);
    case '"': return str(r, &s, &n);
    case 't': return word(r, "true");
    case 'f': return word(r, "false");
    case 'n': return word(r, "null");
    default: return num(r, &s, &n);
    }
}

static int key_is(const char *k, size_t kn, const char *w)
{
    return kn == strlen(w) && memcmp(k, w, kn) == 0;
}

/* Unsigned integer from a number token (no sign, no fraction): 0 on error. */
static int tok_u32(const char *s, size_t n, uint32_t *out)
{
    uint64_t v = 0;
    if (n == 0U || n > 10U) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i])) {
            return 0;
        }
        v = v * 10U + (uint64_t)(s[i] - '0');
    }
    if (v > UINT32_MAX) {
        return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* Integer part of a number token, truncated toward -inf for negatives with a
 * fraction ("13.9" -> 13, "-2.5" -> -3). Range-checked to int16. */
static int tok_floor_i(const char *s, size_t n, int32_t *out)
{
    size_t i = 0;
    int neg = 0;
    if (n > 0U && s[0] == '-') {
        neg = 1;
        i = 1;
    }
    int32_t v = 0;
    int digits = 0;
    while (i < n && is_digit(s[i])) {
        v = v * 10 + (s[i] - '0');
        if (v > 32767) {
            return 0;
        }
        i++;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    int frac_nonzero = 0;
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && is_digit(s[i])) {
            frac_nonzero |= (s[i] != '0');
            i++;
        }
    }
    if (i != n) {
        return 0; /* exponent forms not accepted for powe */
    }
    *out = neg ? -(v + frac_nonzero) : v;
    return 1;
}

/* MHz decimal ("868.1", "869.525") to Hz exactly, without floating point. */
static int tok_mhz_to_hz(const char *s, size_t n, uint32_t *hz)
{
    size_t i = 0;
    uint64_t mhz = 0;
    int digits = 0;
    while (i < n && is_digit(s[i])) {
        mhz = mhz * 10U + (uint64_t)(s[i] - '0');
        if (mhz > 4000U) {
            return 0;
        }
        i++;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    uint64_t frac = 0;
    int fd = 0;
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && is_digit(s[i])) {
            if (fd < 6) {
                frac = frac * 10U + (uint64_t)(s[i] - '0');
                fd++;
            } else if (s[i] != '0') {
                return 0; /* sub-Hz precision: not a channel we would use */
            }
            i++;
        }
    }
    if (i != n) {
        return 0;
    }
    while (fd < 6) {
        frac *= 10U;
        fd++;
    }
    const uint64_t v = mhz * 1000000U + frac;
    if (v > UINT32_MAX) {
        return 0;
    }
    *hz = (uint32_t)v;
    return 1;
}

static int parse_datr(const char *s, size_t n, uint8_t *sf, uint16_t *bw)
{
    /* "SF<7..12>BW<125|250|500>" */
    if (n < 7U || s[0] != 'S' || s[1] != 'F') {
        return 0;
    }
    size_t i = 2;
    uint32_t v = 0;
    while (i < n && is_digit(s[i]) && v < 100U) {
        v = v * 10U + (uint32_t)(s[i] - '0');
        i++;
    }
    if (v < 7U || v > 12U || i + 2U > n || s[i] != 'B' || s[i + 1U] != 'W') {
        return 0;
    }
    uint32_t b = 0;
    if (!tok_u32(&s[i + 2U], n - i - 2U, &b) || (b != 125U && b != 250U && b != 500U)) {
        return 0;
    }
    *sf = (uint8_t)v;
    *bw = (uint16_t)b;
    return 1;
}

static int parse_codr(const char *s, size_t n, uint8_t *cr)
{
    if (n != 3U || s[0] != '4' || s[1] != '/' || s[2] < '5' || s[2] > '8') {
        return 0;
    }
    *cr = (uint8_t)(s[2] - '0');
    return 1;
}

int lwc_base64_decode(const char *in, size_t n, uint8_t *out, size_t cap)
{
    if (in == NULL || out == NULL) {
        return -1;
    }
    while (n > 0U && in[n - 1U] == '=') {
        n--;
    }
    if (n % 4U == 1U) {
        return -1;
    }
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const char c = in[i];
        int v;
        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '+') {
            v = 62;
        } else if (c == '/') {
            v = 63;
        } else {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) {
                return -1;
            }
            out[o++] = (uint8_t)((acc >> bits) & 0xFFU);
        }
    }
    return (int)o;
}

enum {
    F_FREQ = 1 << 0,
    F_POWE = 1 << 1,
    F_MODU = 1 << 2,
    F_DATR = 1 << 3,
    F_CODR = 1 << 4,
    F_DATA = 1 << 5,
    F_TIME = 1 << 6,
};

static lwc_txpk_err_t parse_txpk_obj(jr_t *r, lwc_txpk_t *t)
{
    unsigned have = 0;
    uint32_t size_field = 0;
    int has_size = 0;
    int modu_ok = 1;

    if (lit(r, '}')) {
        return LWC_TXPK_ERR_FIELD;
    }
    for (;;) {
        const char *k, *v;
        size_t kn, vn;
        if (!str(r, &k, &kn) || !lit(r, ':')) {
            return LWC_TXPK_ERR_JSON;
        }
        if (key_is(k, kn, "imme")) {
            if (word(r, "true")) {
                t->mode = LWC_TXPK_IMMEDIATE;
                have |= F_TIME;
            } else if (!word(r, "false")) {
                return LWC_TXPK_ERR_FIELD;
            }
        } else if (key_is(k, kn, "tmst")) {
            if (!num(r, &v, &vn) || !tok_u32(v, vn, &t->tmst)) {
                return LWC_TXPK_ERR_FIELD;
            }
            if (t->mode != LWC_TXPK_IMMEDIATE) {
                t->mode = LWC_TXPK_TIMED;
            }
            have |= F_TIME;
        } else if (key_is(k, kn, "tmms")) {
            if (!skip_value(r, 1)) {
                return LWC_TXPK_ERR_JSON;
            }
            if (!(have & F_TIME)) { /* imme and tmst win, as in the Semtech forwarder */
                t->mode = LWC_TXPK_GPS;
            }
            have |= F_TIME;
        } else if (key_is(k, kn, "freq")) {
            if (!num(r, &v, &vn) || !tok_mhz_to_hz(v, vn, &t->freq_hz)) {
                return LWC_TXPK_ERR_FIELD;
            }
            have |= F_FREQ;
        } else if (key_is(k, kn, "rfch")) {
            uint32_t u;
            if (!num(r, &v, &vn) || !tok_u32(v, vn, &u) || u > 1U) {
                return LWC_TXPK_ERR_FIELD;
            }
            t->rfch = (uint8_t)u;
        } else if (key_is(k, kn, "powe")) {
            int32_t p;
            if (!num(r, &v, &vn) || !tok_floor_i(v, vn, &p) || p < -20 || p > 40) {
                return LWC_TXPK_ERR_FIELD;
            }
            t->powe = (int8_t)p;
            have |= F_POWE;
        } else if (key_is(k, kn, "modu")) {
            if (!str(r, &v, &vn)) {
                return LWC_TXPK_ERR_FIELD;
            }
            modu_ok = (vn == 4U && memcmp(v, "LORA", 4) == 0);
            have |= F_MODU;
        } else if (key_is(k, kn, "datr")) {
            ws(r);
            if (r->p < r->end && *r->p == '"') {
                if (!str(r, &v, &vn) || !parse_datr(v, vn, &t->sf, &t->bw_khz)) {
                    return LWC_TXPK_ERR_FIELD;
                }
                have |= F_DATR;
            } else if (!skip_value(r, 1)) { /* FSK bit rate: rejected via modu */
                return LWC_TXPK_ERR_JSON;
            }
        } else if (key_is(k, kn, "codr")) {
            if (!str(r, &v, &vn) || !parse_codr(v, vn, &t->cr_denom)) {
                return LWC_TXPK_ERR_FIELD;
            }
            have |= F_CODR;
        } else if (key_is(k, kn, "ipol")) {
            if (word(r, "true")) {
                t->ipol = true;
            } else if (word(r, "false")) {
                t->ipol = false;
            } else {
                return LWC_TXPK_ERR_FIELD;
            }
        } else if (key_is(k, kn, "ncrc")) {
            if (word(r, "true")) {
                t->ncrc = true;
            } else if (word(r, "false")) {
                t->ncrc = false;
            } else {
                return LWC_TXPK_ERR_FIELD;
            }
        } else if (key_is(k, kn, "prea")) {
            uint32_t u;
            /* The HAL sends at least 6; above 64 a frame can hold the radio for minutes. */
            if (!num(r, &v, &vn) || !tok_u32(v, vn, &u) || u < 6U || u > 64U) {
                return LWC_TXPK_ERR_FIELD;
            }
            t->prea = (uint16_t)u;
        } else if (key_is(k, kn, "size")) {
            if (!num(r, &v, &vn) || !tok_u32(v, vn, &size_field) || size_field > 255U) {
                return LWC_TXPK_ERR_FIELD;
            }
            has_size = 1;
        } else if (key_is(k, kn, "data")) {
            if (!str(r, &v, &vn)) {
                return LWC_TXPK_ERR_FIELD;
            }
            const int dn = lwc_base64_decode(v, vn, t->payload, sizeof t->payload);
            if (dn < 0) {
                return LWC_TXPK_ERR_FIELD;
            }
            t->size = (uint16_t)dn;
            have |= F_DATA;
        } else if (!skip_value(r, 1)) {
            return LWC_TXPK_ERR_JSON;
        }
        if (lit(r, ',')) {
            continue;
        }
        if (!lit(r, '}')) {
            return LWC_TXPK_ERR_JSON;
        }
        break;
    }

    if (!modu_ok) {
        return LWC_TXPK_ERR_MODU;
    }
    const unsigned need = F_FREQ | F_MODU | F_DATR | F_CODR | F_DATA | F_TIME;
    if ((have & need) != need) {
        return LWC_TXPK_ERR_FIELD;
    }
    if (!(have & F_POWE)) {
        t->powe = 14; /* Semtech forwarder default */
    }
    if (has_size && size_field != t->size) {
        return LWC_TXPK_ERR_FIELD;
    }
    return LWC_TXPK_OK;
}

lwc_txpk_err_t lwc_txpk_parse(const char *json, size_t len, lwc_txpk_t *out)
{
    if (json == NULL || out == NULL) {
        return LWC_TXPK_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    jr_t r = {json, json + len};
    if (!lit(&r, '{')) {
        return LWC_TXPK_ERR_JSON;
    }
    if (lit(&r, '}')) {
        return LWC_TXPK_ERR_JSON;
    }
    lwc_txpk_err_t res = LWC_TXPK_ERR_JSON;
    int found = 0;
    for (;;) {
        const char *k;
        size_t kn;
        if (!str(&r, &k, &kn) || !lit(&r, ':')) {
            return LWC_TXPK_ERR_JSON;
        }
        if (!found && key_is(k, kn, "txpk")) {
            if (!lit(&r, '{')) {
                return LWC_TXPK_ERR_JSON;
            }
            res = parse_txpk_obj(&r, out);
            if (res != LWC_TXPK_OK) {
                return res;
            }
            found = 1;
        } else if (!skip_value(&r, 1)) {
            return LWC_TXPK_ERR_JSON;
        }
        if (lit(&r, ',')) {
            continue;
        }
        if (!lit(&r, '}')) {
            return LWC_TXPK_ERR_JSON;
        }
        break;
    }
    ws(&r);
    if (r.p != r.end) {
        return LWC_TXPK_ERR_JSON;
    }
    return found ? LWC_TXPK_OK : LWC_TXPK_ERR_JSON;
}

/* ==== airtime ============================================================ */

uint32_t lwc_lora_airtime_us(uint8_t sf, uint16_t bw_khz, uint8_t cr_denom, uint16_t preamble,
                             uint16_t size, bool crc, bool implicit_header)
{
    if (sf < 6U || sf > 12U || (bw_khz != 125U && bw_khz != 250U && bw_khz != 500U) ||
        cr_denom < 5U || cr_denom > 8U || size > 255U) {
        return 0;
    }
    /* Symbol time in ns: 2^SF / BW. Exact for these bandwidths. */
    const uint64_t tsym_ns = ((uint64_t)1U << sf) * 1000000U / bw_khz;
    const int de = tsym_ns >= 16000000U ? 1 : 0;
    const int64_t bits = 8 * (int64_t)size - 4 * (int64_t)sf + 28 + (crc ? 16 : 0) -
                        (implicit_header ? 20 : 0);
    const int64_t den = 4 * ((int64_t)sf - 2 * de);
    int64_t blocks = 0;
    if (bits > 0) {
        blocks = (bits + den - 1) / den;
    }
    const uint64_t payload_sym = 8U + (uint64_t)blocks * cr_denom;
    /* Preamble: (n + 4.25) symbols = (4n + 17) / 4 symbols. */
    const uint64_t total_ns =
        ((4U * (uint64_t)preamble + 17U) * tsym_ns) / 4U + payload_sym * tsym_ns;
    return (uint32_t)((total_ns + 999U) / 1000U);
}

/* ==== JIT queue ========================================================== */

static int32_t diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

void lwc_jit_init(lwc_jit_t *q, uint32_t min_lead_us, uint32_t max_advance_us, uint32_t guard_us)
{
    memset(q, 0, sizeof(*q));
    q->min_lead_us = min_lead_us;
    q->max_advance_us = max_advance_us;
    q->guard_us = guard_us;
}

lwc_jit_res_t lwc_jit_admit(lwc_jit_t *q, uint32_t now, uint32_t start, uint32_t airtime_us,
                            uint32_t id, size_t *slot)
{
    const int32_t lead = diff(start, now);
    if (lead < (int32_t)q->min_lead_us) {
        return LWC_JIT_TOO_LATE;
    }
    if ((uint32_t)lead > q->max_advance_us) {
        return LWC_JIT_TOO_EARLY;
    }
    size_t free_slot = LWC_JIT_SLOTS;
    for (size_t i = 0; i < LWC_JIT_SLOTS; i++) {
        const lwc_jit_slot_t *s = &q->slot[i];
        if (!s->used) {
            if (free_slot == LWC_JIT_SLOTS) {
                free_slot = i;
            }
            continue;
        }
        /* Overlap if neither frame ends (plus guard) before the other starts. */
        const int32_t a = diff(start, s->start);
        const int64_t new_end = (int64_t)a + airtime_us + q->guard_us;
        const int64_t old_end = (int64_t)s->airtime_us + q->guard_us;
        if (new_end > 0 && (int64_t)a < old_end) {
            return LWC_JIT_COLLISION;
        }
    }
    if (free_slot == LWC_JIT_SLOTS) {
        return LWC_JIT_FULL;
    }
    q->slot[free_slot] = (lwc_jit_slot_t){.used = true, .start = start, .airtime_us = airtime_us, .id = id};
    if (slot != NULL) {
        *slot = free_slot;
    }
    return LWC_JIT_OK;
}

int lwc_jit_due(const lwc_jit_t *q, uint32_t now, uint32_t lead_us, uint32_t min_send_us,
                int *expired)
{
    int best = -1;
    int32_t best_d = 0;
    if (expired != NULL) {
        *expired = -1;
    }
    for (size_t i = 0; i < LWC_JIT_SLOTS; i++) {
        if (!q->slot[i].used || q->slot[i].sent) {
            continue;
        }
        const int32_t d = diff(q->slot[i].start, now);
        if (d < (int32_t)min_send_us) {
            if (expired != NULL && *expired < 0) {
                *expired = (int)i;
            }
            continue;
        }
        if (d <= (int32_t)lead_us && (best < 0 || d < best_d)) {
            best = (int)i;
            best_d = d;
        }
    }
    return best;
}

void lwc_jit_sent(lwc_jit_t *q, size_t slot)
{
    if (slot < LWC_JIT_SLOTS) {
        q->slot[slot].sent = true;
    }
}

int lwc_jit_ended(const lwc_jit_t *q, uint32_t now, uint32_t margin_us)
{
    for (size_t i = 0; i < LWC_JIT_SLOTS; i++) {
        const lwc_jit_slot_t *s = &q->slot[i];
        if (s->used && s->sent &&
            (int64_t)diff(now, s->start) > (int64_t)s->airtime_us + margin_us) {
            return (int)i;
        }
    }
    return -1;
}

void lwc_jit_release(lwc_jit_t *q, size_t slot)
{
    if (slot < LWC_JIT_SLOTS) {
        q->slot[slot].used = false;
    }
}

/* ==== duty cycle ========================================================= */

static const struct {
    uint32_t lo, hi; /* Hz, [lo, hi) */
    uint32_t permille;
} k_bands[LWC_DC_BANDS] = {
    {863000000U, 865000000U, 1U},   /* 0.1 % */
    {865000000U, 868000000U, 10U},  /* 1 % */
    {868000000U, 868600000U, 10U},  /* g1: 1 % */
    {868700000U, 869200000U, 1U},   /* g2: 0.1 % */
    {869400000U, 869650000U, 100U}, /* g3: 10 % (RX2 869.525) */
    {869700000U, 870000000U, 10U},  /* g4: 1 % */
};

#define DC_WINDOW_US 3600000000LL

static int64_t capacity(size_t b)
{
    return DC_WINDOW_US * (int64_t)k_bands[b].permille / 1000;
}

void lwc_dc_init(lwc_dutycycle_t *dc, int64_t now_us)
{
    dc->last_us = now_us;
    for (size_t b = 0; b < LWC_DC_BANDS; b++) {
        dc->tokens_us[b] = capacity(b);
    }
}

int lwc_dc_band(uint32_t freq_hz)
{
    for (size_t b = 0; b < LWC_DC_BANDS; b++) {
        if (freq_hz >= k_bands[b].lo && freq_hz < k_bands[b].hi) {
            return (int)b;
        }
    }
    return -1;
}

uint32_t lwc_dc_permille(int band)
{
    return (band >= 0 && (size_t)band < LWC_DC_BANDS) ? k_bands[band].permille : 0U;
}

bool lwc_dc_take(lwc_dutycycle_t *dc, int64_t now_us, uint32_t freq_hz, uint32_t airtime_us)
{
    const int b = lwc_dc_band(freq_hz);
    if (b < 0) {
        return false;
    }
    const int64_t elapsed = now_us - dc->last_us;
    if (elapsed > 0) {
        for (size_t i = 0; i < LWC_DC_BANDS; i++) {
            const int64_t refill = elapsed * (int64_t)k_bands[i].permille / 1000;
            const int64_t t = dc->tokens_us[i] + refill;
            dc->tokens_us[i] = t > capacity(i) ? capacity(i) : t;
        }
        dc->last_us = now_us;
    }
    if (dc->tokens_us[b] < (int64_t)airtime_us) {
        return false;
    }
    dc->tokens_us[b] -= (int64_t)airtime_us;
    return true;
}
