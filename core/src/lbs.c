#include "lwc/lbs.h"

#include <inttypes.h>
#include <string.h>

#include "lwc/hex.h"
#include "lwc/json.h"
#include "lwc/lorawan.h"

/* ---- data rates ------------------------------------------------------------- */

/* EU868 (RP002-1.0.x): DR0-5 SF12-SF7 at 125 kHz, DR6 SF7 at 250 kHz, DR7 FSK. */
int lwc_lbs_dr(uint8_t sf, uint16_t bw_khz)
{
    if (bw_khz == 125U && sf >= 7U && sf <= 12U) {
        return 12 - (int)sf;
    }
    if (bw_khz == 250U && sf == 7U) {
        return 6;
    }
    return -1;
}

bool lwc_lbs_dr_params(uint8_t dr, uint8_t *sf, uint16_t *bw_khz)
{
    if (sf == NULL || bw_khz == NULL || dr > 6U) {
        return false;
    }
    *sf = dr == 6U ? 7U : (uint8_t)(12U - dr);
    *bw_khz = dr == 6U ? 250U : 125U;
    return true;
}

/* ---- xtime ------------------------------------------------------------------ */

#define XTIME_48 0xFFFFFFFFFFFFULL

void lwc_lbs_clock_init(lwc_lbs_clock_t *c, uint8_t sess)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof *c);
    c->sess = sess == 0U ? 1U : sess;
}

int64_t lwc_lbs_xtime(lwc_lbs_clock_t *c, uint32_t tmst)
{
    if (c == NULL) {
        return 0;
    }
    if (!c->have) {
        c->have = true;
        c->last = tmst;
        c->now48 = tmst;
    }
    /* signed distance from the last sample: forward moves the clock, a little
     * back (an uplink older than the last sample) maps without moving it */
    const int32_t d = (int32_t)(tmst - c->last);
    const uint64_t t48 = (c->now48 + (uint64_t)(int64_t)d) & XTIME_48;
    if (d > 0) {
        c->last = tmst;
        c->now48 = t48;
    }
    return (int64_t)(((uint64_t)c->sess << 48) | t48);
}

/* ---- output helpers --------------------------------------------------------- */

static void sb_hex(lwc_sb_t *b, const uint8_t *p, size_t n)
{
    if (b->overflow) {
        return;
    }
    if (b->cap - b->len <= 2U * n) {
        b->overflow = 1;
        return;
    }
    if (n > 0U) {
        lwc_hex_encode(p, n, b->p + b->len);
    } else {
        b->p[b->len] = '\0';
    }
    b->len += 2U * n;
}

static void sb_eui(lwc_sb_t *b, uint64_t eui)
{
    lwc_sb_printf(b, "\"%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X\"", (unsigned)(eui >> 56) & 0xFFU,
                  (unsigned)(eui >> 48) & 0xFFU, (unsigned)(eui >> 40) & 0xFFU, (unsigned)(eui >> 32) & 0xFFU,
                  (unsigned)(eui >> 24) & 0xFFU, (unsigned)(eui >> 16) & 0xFFU, (unsigned)(eui >> 8) & 0xFFU,
                  (unsigned)eui & 0xFFU);
}

/* Unix microseconds as seconds with six decimals, without a double. */
static void sb_unix_s(lwc_sb_t *b, int64_t unix_us)
{
    if (unix_us < 0) {
        lwc_sb_printf(b, "0");
        return;
    }
    lwc_sb_printf(b, "%" PRId64 ".%06" PRId64, unix_us / 1000000, unix_us % 1000000);
}

static lwc_lbs_err_t sb_done(const lwc_sb_t *b, size_t *len)
{
    if (b->overflow) {
        return LWC_LBS_ERR_SPACE;
    }
    if (len != NULL) {
        *len = b->len;
    }
    return LWC_LBS_OK;
}

static bool plain(const char *s)
{
    if (s == NULL) {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20U) {
            return false;
        }
    }
    return true;
}

static int32_t le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* ---- gateway to server ------------------------------------------------------ */

lwc_lbs_err_t lwc_lbs_version(char *out, size_t cap, const char *station, const char *firmware,
                              const char *package, const char *model, size_t *len)
{
    if (out == NULL || cap == 0U || !plain(station) || !plain(firmware) || !plain(package) || !plain(model)) {
        return LWC_LBS_ERR_ARG;
    }
    lwc_sb_t b = {out, cap, 0, 0};
    lwc_sb_printf(&b,
                  "{\"msgtype\":\"version\",\"station\":\"%s\",\"firmware\":\"%s\",\"package\":\"%s\","
                  "\"model\":\"%s\",\"protocol\":%d,\"features\":\"\"}",
                  station, firmware, package, model, LWC_LBS_PROTOCOL);
    return sb_done(&b, len);
}

static void sb_radio(lwc_sb_t *b, int dr, const lwc_lbs_rx_t *rx)
{
    lwc_sb_printf(b, ",\"DR\":%d,\"Freq\":%" PRIu32 ",\"upinfo\":{\"rctx\":0,\"xtime\":%" PRId64
                     ",\"gpstime\":0,\"rssi\":%.0f,\"snr\":%.2f,\"rxtime\":",
                  dr, rx->freq_hz, rx->xtime, (double)rx->rssi, (double)rx->snr);
    sb_unix_s(b, rx->unix_us);
    lwc_sb_printf(b, "}}");
}

lwc_lbs_err_t lwc_lbs_uplink(char *out, size_t cap, const uint8_t *phy, size_t n, const lwc_lbs_rx_t *rx,
                             size_t *len)
{
    if (out == NULL || cap == 0U || phy == NULL || rx == NULL) {
        return LWC_LBS_ERR_ARG;
    }
    if (n > 255U) {
        return LWC_LBS_ERR_FRAME; /* longer than any LoRa frame */
    }
    const int dr = lwc_lbs_dr(rx->sf, rx->bw_khz);
    if (dr < 0) {
        return LWC_LBS_ERR_DR;
    }
    lwc_sb_t b = {out, cap, 0, 0};
    const int mtype = lwc_lw_mtype(phy, n);
    if (mtype == LWC_LW_JOIN_REQUEST) {
        lwc_lw_join_req_t jr;
        if (lwc_lw_join_req_parse(phy, n, &jr) != LWC_LW_OK) {
            return LWC_LBS_ERR_FRAME;
        }
        lwc_sb_printf(&b, "{\"msgtype\":\"jreq\",\"MHdr\":%u,\"JoinEui\":", (unsigned)phy[0]);
        sb_eui(&b, jr.join_eui);
        lwc_sb_printf(&b, ",\"DevEui\":");
        sb_eui(&b, jr.dev_eui);
        lwc_sb_printf(&b, ",\"DevNonce\":%u,\"MIC\":%" PRId32, (unsigned)jr.dev_nonce, le32(&phy[n - 4U]));
    } else if (mtype == LWC_LW_UNCONFIRMED_UP || mtype == LWC_LW_CONFIRMED_UP) {
        lwc_lw_data_t d;
        if (lwc_lw_data_parse(phy, n, &d) != LWC_LW_OK) {
            return LWC_LBS_ERR_FRAME;
        }
        lwc_sb_printf(&b, "{\"msgtype\":\"updf\",\"MHdr\":%u,\"DevAddr\":%" PRId32 ",\"FCtrl\":%u,\"FCnt\":%u,\"FOpts\":\"",
                      (unsigned)phy[0], (int32_t)d.dev_addr, (unsigned)d.fctrl, (unsigned)d.fcnt16);
        sb_hex(&b, d.fopts, d.fopts_len);
        lwc_sb_printf(&b, "\",\"FPort\":%d,\"FRMPayload\":\"", d.has_port ? (int)d.fport : -1);
        sb_hex(&b, d.frm, d.has_port ? d.frm_len : 0U);
        lwc_sb_printf(&b, "\",\"MIC\":%" PRId32, le32(&phy[n - 4U]));
    } else if (mtype == LWC_LW_PROPRIETARY) {
        lwc_sb_printf(&b, "{\"msgtype\":\"propdf\",\"FRMPayload\":\"");
        sb_hex(&b, phy, n);
        lwc_sb_printf(&b, "\"");
    } else {
        return LWC_LBS_ERR_FRAME;
    }
    sb_radio(&b, dr, rx);
    return sb_done(&b, len);
}

lwc_lbs_err_t lwc_lbs_dntxed(char *out, size_t cap, int64_t diid, const char *dev_eui, int64_t rctx,
                             int64_t xtime, int64_t unix_us, size_t *len)
{
    if (out == NULL || cap == 0U || !plain(dev_eui)) {
        return LWC_LBS_ERR_ARG;
    }
    lwc_sb_t b = {out, cap, 0, 0};
    lwc_sb_printf(&b,
                  "{\"msgtype\":\"dntxed\",\"diid\":%" PRId64 ",\"DevEui\":\"%s\",\"rctx\":%" PRId64
                  ",\"xtime\":%" PRId64 ",\"txtime\":",
                  diid, dev_eui, rctx, xtime);
    sb_unix_s(&b, unix_us);
    lwc_sb_printf(&b, ",\"gpstime\":0}");
    return sb_done(&b, len);
}

/* ---- reading ---------------------------------------------------------------- */

static bool rd_i64(lwc_jr_t *r, int64_t *v)
{
    const char *s;
    size_t n;
    return lwc_jr_num(r, &s, &n) && lwc_jr_i64(s, n, v);
}

static bool rd_range(lwc_jr_t *r, int64_t lo, int64_t hi, int64_t *v)
{
    return rd_i64(r, v) && *v >= lo && *v <= hi;
}

/* Iterate an object: call with the reader before '{'. */
typedef struct {
    lwc_jr_t *r;
    bool first;
} obj_t;

static bool obj_open(obj_t *o, lwc_jr_t *r)
{
    o->r = r;
    o->first = true;
    return lwc_jr_lit(r, '{');
}

/* Next key: true with the key, false at '}' (sets *ok) or on an error. */
static bool obj_next(obj_t *o, const char **k, size_t *kn, bool *ok)
{
    *ok = false;
    if (o->first) {
        o->first = false;
        if (lwc_jr_lit(o->r, '}')) {
            *ok = true;
            return false;
        }
    } else if (!lwc_jr_lit(o->r, ',')) {
        *ok = lwc_jr_lit(o->r, '}');
        return false;
    }
    return lwc_jr_str(o->r, k, kn) && lwc_jr_lit(o->r, ':');
}

lwc_lbs_err_t lwc_lbs_msgtype(const char *json, size_t len, lwc_lbs_msg_t *type)
{
    if (json == NULL || type == NULL) {
        return LWC_LBS_ERR_ARG;
    }
    *type = LWC_LBS_MSG_OTHER;
    lwc_jr_t r = {json, json + len};
    obj_t o;
    if (!obj_open(&o, &r)) {
        return LWC_LBS_ERR_JSON;
    }
    const char *k;
    size_t kn;
    bool ok;
    while (obj_next(&o, &k, &kn, &ok)) {
        if (lwc_jr_key_is(k, kn, "msgtype")) {
            const char *v;
            size_t vn;
            if (!lwc_jr_str(&r, &v, &vn)) {
                return LWC_LBS_ERR_FIELD;
            }
            *type = lwc_jr_key_is(v, vn, "router_config") ? LWC_LBS_MSG_ROUTER_CONFIG
                    : lwc_jr_key_is(v, vn, "dnmsg")       ? LWC_LBS_MSG_DNMSG
                    : lwc_jr_key_is(v, vn, "dnsched")     ? LWC_LBS_MSG_DNSCHED
                                                          : LWC_LBS_MSG_OTHER;
            return LWC_LBS_OK;
        }
        if (!lwc_jr_skip(&r)) {
            return LWC_LBS_ERR_JSON;
        }
    }
    return ok ? LWC_LBS_ERR_FIELD : LWC_LBS_ERR_JSON;
}

static bool eui_chars(const char *s, size_t n)
{
    if (n == 0U || n >= LWC_LBS_EUI_STR) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-' || c == ':')) {
            return false;
        }
    }
    return true;
}

lwc_lbs_err_t lwc_lbs_parse_dnmsg(const char *json, size_t len, lwc_lbs_dnmsg_t *out)
{
    if (json == NULL || out == NULL) {
        return LWC_LBS_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    lwc_jr_t r = {json, json + len};
    obj_t o;
    if (!obj_open(&o, &r)) {
        return LWC_LBS_ERR_JSON;
    }
    bool is_dn = false, have_eui = false, have_diid = false, have_pdu = false, have_delay = false;
    bool gps = false, dr_form = false;
    bool r1dr = false, r1f = false, r2dr = false, r2f = false;
    int64_t v;
    const char *k, *s;
    size_t kn, sn;
    bool ok;
    while (obj_next(&o, &k, &kn, &ok)) {
        if (lwc_jr_key_is(k, kn, "msgtype")) {
            if (!lwc_jr_str(&r, &s, &sn)) {
                return LWC_LBS_ERR_FIELD;
            }
            is_dn = lwc_jr_key_is(s, sn, "dnmsg");
        } else if (lwc_jr_key_is(k, kn, "DevEui")) {
            if (!lwc_jr_str(&r, &s, &sn) || !eui_chars(s, sn)) {
                return LWC_LBS_ERR_FIELD;
            }
            memcpy(out->dev_eui, s, sn);
            out->dev_eui[sn] = '\0';
            have_eui = true;
        } else if (lwc_jr_key_is(k, kn, "dC")) {
            if (!rd_range(&r, 0, 2, &v)) {
                return LWC_LBS_ERR_FIELD;
            }
            out->dclass = (uint8_t)v;
        } else if (lwc_jr_key_is(k, kn, "diid") || lwc_jr_key_is(k, kn, "seqno")) {
            if (!rd_i64(&r, &out->diid)) {
                return LWC_LBS_ERR_FIELD;
            }
            have_diid = true;
        } else if (lwc_jr_key_is(k, kn, "pdu")) {
            if (!lwc_jr_str(&r, &s, &sn) || sn == 0U || sn % 2U != 0U || sn / 2U > sizeof out->pdu ||
                !lwc_hex_decode(s, sn, out->pdu, sn / 2U)) {
                return LWC_LBS_ERR_FIELD;
            }
            out->len = (uint16_t)(sn / 2U);
            have_pdu = true;
        } else if (lwc_jr_key_is(k, kn, "RxDelay")) {
            if (!rd_range(&r, 0, 15, &v)) {
                return LWC_LBS_ERR_FIELD;
            }
            out->rx_delay = v == 0 ? 1U : (uint8_t)v; /* Station: 0 means 1 */
            have_delay = true;
        } else if (lwc_jr_key_is(k, kn, "RX1DR") || lwc_jr_key_is(k, kn, "DR")) {
            if (!rd_range(&r, 0, 15, &v)) {
                return LWC_LBS_ERR_FIELD;
            }
            out->rx1_dr = (uint8_t)v;
            r1dr = true;
            dr_form = dr_form || lwc_jr_key_is(k, kn, "DR");
        } else if (lwc_jr_key_is(k, kn, "RX1Freq") || lwc_jr_key_is(k, kn, "Freq")) {
            if (!rd_range(&r, 1, UINT32_MAX, &v)) {
                return LWC_LBS_ERR_FIELD;
            }
            out->rx1_freq = (uint32_t)v;
            r1f = true;
        } else if (lwc_jr_key_is(k, kn, "RX2DR")) {
            if (!rd_range(&r, 0, 15, &v)) {
                return LWC_LBS_ERR_FIELD;
            }
            out->rx2_dr = (uint8_t)v;
            r2dr = true;
        } else if (lwc_jr_key_is(k, kn, "RX2Freq")) {
            if (!rd_range(&r, 1, UINT32_MAX, &v)) {
                return LWC_LBS_ERR_FIELD;
            }
            out->rx2_freq = (uint32_t)v;
            r2f = true;
        } else if (lwc_jr_key_is(k, kn, "xtime")) {
            if (!rd_range(&r, 0, INT64_MAX, &out->xtime)) {
                return LWC_LBS_ERR_FIELD;
            }
        } else if (lwc_jr_key_is(k, kn, "rctx")) {
            if (!rd_i64(&r, &out->rctx)) {
                return LWC_LBS_ERR_FIELD;
            }
        } else if (lwc_jr_key_is(k, kn, "priority")) {
            if (!rd_range(&r, 0, 255, &v)) {
                return LWC_LBS_ERR_FIELD;
            }
            out->priority = (uint8_t)v;
        } else if (lwc_jr_key_is(k, kn, "gpstime")) {
            if (!rd_i64(&r, &v)) {
                return LWC_LBS_ERR_FIELD;
            }
            gps = gps || v != 0;
        } else if (!lwc_jr_skip(&r)) { /* MuxTime, preamble, addcrc, dnmode, ... */
            return LWC_LBS_ERR_JSON;
        }
    }
    if (!ok) {
        return LWC_LBS_ERR_JSON;
    }
    if (!is_dn || !have_eui || !have_diid || !have_pdu || r1dr != r1f || r2dr != r2f || (!r1dr && !r2dr)) {
        return LWC_LBS_ERR_FIELD;
    }
    if (out->dclass == 1U || gps) {
        return LWC_LBS_ERR_UNSUPPORTED; /* class B, GPS time */
    }
    /* class C with no RX1 (ChirpStack "immediately"): sent at the next free
     * time on RX2, as the reference station does; xtime, if any, is not used */
    out->asap = out->dclass == 2U && !r1dr && r2dr;
    if (out->xtime == 0 && !out->asap) {
        return LWC_LBS_ERR_UNSUPPORTED; /* no time to answer */
    }
    if (dr_form) {
        out->rx_delay = 0; /* "DR"/"Freq": send at xtime itself (Station) */
    } else if (!have_delay) {
        out->rx_delay = 1; /* Station: absent means 1 */
    }
    out->rx1 = r1dr;
    out->rx2 = r2dr;
    return LWC_LBS_OK;
}

static bool fill_txpk(lwc_txpk_t *t, uint32_t tmst, uint32_t freq, uint8_t dr, const lwc_lbs_dnmsg_t *d)
{
    memset(t, 0, sizeof *t);
    if (!lwc_lbs_dr_params(dr, &t->sf, &t->bw_khz)) {
        return false;
    }
    t->mode = LWC_TXPK_TIMED;
    t->tmst = tmst;
    t->freq_hz = freq;
    t->rfch = 0;
    t->powe = (freq >= 869400000U && freq <= 869650000U) ? 27 : 16;
    t->cr_denom = 5;
    t->ipol = true;
    t->ncrc = true;
    t->prea = 8;
    t->size = d->len;
    memcpy(t->payload, d->pdu, d->len);
    return true;
}

lwc_lbs_err_t lwc_lbs_dn_txpk(const lwc_lbs_dnmsg_t *d, uint8_t sess, lwc_txpk_t *first, lwc_txpk_t *alt)
{
    if (d == NULL || first == NULL || alt == NULL || d->len == 0U || d->len > sizeof d->pdu) {
        return LWC_LBS_ERR_ARG;
    }
    memset(alt, 0, sizeof *alt);
    if (d->asap) {
        return LWC_LBS_ERR_UNSUPPORTED; /* lwc_lbs_dn_txpk_now */
    }
    if (d->xtime <= 0 || (uint8_t)((uint64_t)d->xtime >> 48) != sess) {
        return LWC_LBS_ERR_SESSION;
    }
    const uint32_t rx1 = (uint32_t)((uint64_t)d->xtime + (uint64_t)d->rx_delay * 1000000U);
    const uint32_t rx2 = rx1 + 1000000U;
    if (d->rx1) {
        if (!fill_txpk(first, rx1, d->rx1_freq, d->rx1_dr, d)) {
            return LWC_LBS_ERR_FIELD;
        }
        if (d->rx2 && !fill_txpk(alt, rx2, d->rx2_freq, d->rx2_dr, d)) {
            memset(alt, 0, sizeof *alt);
            return LWC_LBS_ERR_FIELD;
        }
        return LWC_LBS_OK;
    }
    if (d->rx2 && fill_txpk(first, rx2, d->rx2_freq, d->rx2_dr, d)) {
        return LWC_LBS_OK;
    }
    return LWC_LBS_ERR_FIELD;
}

lwc_lbs_err_t lwc_lbs_dn_txpk_now(const lwc_lbs_dnmsg_t *d, uint32_t now_tmst, lwc_txpk_t *first, lwc_txpk_t *alt)
{
    if (d == NULL || first == NULL || alt == NULL || d->len == 0U || d->len > sizeof d->pdu || !d->asap || !d->rx2) {
        return LWC_LBS_ERR_ARG;
    }
    memset(alt, 0, sizeof *alt);
    const uint32_t t = now_tmst + LWC_LBS_ASAP_LEAD_US;
    if (!fill_txpk(first, t, d->rx2_freq, d->rx2_dr, d) ||
        !fill_txpk(alt, t + LWC_LBS_ASAP_RETRY_US, d->rx2_freq, d->rx2_dr, d)) {
        memset(first, 0, sizeof *first);
        memset(alt, 0, sizeof *alt);
        return LWC_LBS_ERR_FIELD;
    }
    return LWC_LBS_OK;
}

/* ---- router_config ---------------------------------------------------------- */

typedef struct {
    bool en;
    int64_t radio, ifhz, freq, bw, sf;
} chan_rd_t;

static bool rd_chan2(lwc_jr_t *r, chan_rd_t *c)
{
    obj_t o;
    const char *k;
    size_t kn;
    bool ok;
    if (!obj_open(&o, r)) {
        return false;
    }
    while (obj_next(&o, &k, &kn, &ok)) {
        bool good = true;
        if (lwc_jr_key_is(k, kn, "enable")) {
            if (lwc_jr_word(r, "true")) {
                c->en = true;
            } else {
                good = lwc_jr_word(r, "false");
            }
        } else if (lwc_jr_key_is(k, kn, "radio")) {
            good = rd_range(r, 0, 1, &c->radio);
        } else if (lwc_jr_key_is(k, kn, "if")) {
            good = rd_range(r, -1000000, 1000000, &c->ifhz);
        } else if (lwc_jr_key_is(k, kn, "freq")) {
            good = rd_range(r, 0, UINT32_MAX, &c->freq);
        } else if (lwc_jr_key_is(k, kn, "bandwidth")) {
            good = rd_range(r, 0, 1000000, &c->bw);
        } else if (lwc_jr_key_is(k, kn, "spread_factor")) {
            good = rd_range(r, 0, 12, &c->sf);
        } else {
            good = lwc_jr_skip(r);
        }
        if (!good) {
            return false;
        }
    }
    return ok;
}

static bool rd_chan(lwc_jr_t *r, bool *en, int64_t *radio, int64_t *ifhz, int64_t *freq)
{
    obj_t o;
    const char *k;
    size_t kn;
    bool ok;
    if (!obj_open(&o, r)) {
        return false;
    }
    while (obj_next(&o, &k, &kn, &ok)) {
        if (lwc_jr_key_is(k, kn, "enable")) {
            if (lwc_jr_word(r, "true")) {
                *en = true;
            } else if (!lwc_jr_word(r, "false")) {
                return false;
            }
        } else if (lwc_jr_key_is(k, kn, "radio") && radio != NULL) {
            if (!rd_range(r, 0, 1, radio)) {
                return false;
            }
        } else if (lwc_jr_key_is(k, kn, "if") && ifhz != NULL) {
            if (!rd_range(r, -1000000, 1000000, ifhz)) {
                return false;
            }
        } else if (lwc_jr_key_is(k, kn, "freq") && freq != NULL) {
            if (!rd_range(r, 0, UINT32_MAX, freq)) {
                return false;
            }
        } else if (!lwc_jr_skip(r)) {
            return false;
        }
    }
    return ok;
}

static bool rd_sx1301(lwc_jr_t *r, lwc_lbs_router_config_t *c)
{
    obj_t o;
    const char *k;
    size_t kn;
    bool ok;
    if (!obj_open(&o, r)) {
        return false;
    }
    while (obj_next(&o, &k, &kn, &ok)) {
        bool en = false;
        int64_t radio = 0, ifhz = 0, freq = 0;
        if (lwc_jr_key_is(k, kn, "radio_0") || lwc_jr_key_is(k, kn, "radio_1")) {
            if (!rd_chan(r, &en, NULL, NULL, &freq)) {
                return false;
            }
            c->radio_hz[k[6] - '0'] = en ? (uint32_t)freq : 0U;
        } else if (lwc_jr_key_is(k, kn, "chan_Lora_std") || lwc_jr_key_is(k, kn, "chan_FSK")) {
            chan_rd_t ch = {0};
            if (!rd_chan2(r, &ch)) {
                return false;
            }
            if (k[5] == 'L') {
                c->std_on = ch.en;
                c->std_radio = (uint8_t)ch.radio;
                c->std_if = (int32_t)ch.ifhz;
                c->std_bw_hz = (uint32_t)ch.bw;
                c->std_sf = (uint8_t)ch.sf;
            } else {
                c->fsk_on = ch.en;
                c->fsk_radio = (uint8_t)ch.radio;
                c->fsk_if = (int32_t)ch.ifhz;
            }
        } else if (kn == 14U && memcmp(k, "chan_multiSF_", 13) == 0 && k[13] >= '0' && k[13] <= '7') {
            if (!rd_chan(r, &en, &radio, &ifhz, NULL)) {
                return false;
            }
            const unsigned i = (unsigned)(k[13] - '0');
            if (en) {
                c->multisf = (uint8_t)(c->multisf | (1U << i));
            }
            c->chan_radio[i] = (uint8_t)radio;
            c->chan_if[i] = (int32_t)ifhz;
        } else if (!lwc_jr_skip(r)) {
            return false;
        }
    }
    return ok;
}

/* DRs: 16 entries of [SF, BW, DNONLY]; 0-6 must match EU868. */
static bool rd_drs(lwc_jr_t *r, bool *match)
{
    *match = true;
    if (!lwc_jr_lit(r, '[')) {
        return false;
    }
    if (lwc_jr_lit(r, ']')) {
        *match = false;
        return true;
    }
    for (unsigned i = 0;; i++) {
        int64_t sf, bw, dn;
        if (!lwc_jr_lit(r, '[') || !rd_range(r, -1, 12, &sf) || !lwc_jr_lit(r, ',') || !rd_range(r, 0, 500, &bw) ||
            !lwc_jr_lit(r, ',') || !rd_range(r, 0, 1, &dn) || !lwc_jr_lit(r, ']')) {
            return false;
        }
        if (i <= 6U) {
            uint8_t want_sf;
            uint16_t want_bw;
            (void)lwc_lbs_dr_params((uint8_t)i, &want_sf, &want_bw);
            if (sf != want_sf || bw != want_bw || dn != 0) {
                *match = false;
            }
        }
        if (lwc_jr_lit(r, ',')) {
            if (i >= 31U) {
                return false;
            }
            continue;
        }
        if (i < 6U) {
            *match = false;
        }
        return lwc_jr_lit(r, ']');
    }
}

lwc_lbs_err_t lwc_lbs_parse_router_config(const char *json, size_t len, lwc_lbs_router_config_t *out)
{
    if (json == NULL || out == NULL) {
        return LWC_LBS_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    lwc_jr_t r = {json, json + len};
    obj_t o;
    if (!obj_open(&o, &r)) {
        return LWC_LBS_ERR_JSON;
    }
    bool is_rc = false;
    const char *k, *s;
    size_t kn, sn;
    bool ok;
    while (obj_next(&o, &k, &kn, &ok)) {
        int64_t a, b2;
        if (lwc_jr_key_is(k, kn, "msgtype")) {
            if (!lwc_jr_str(&r, &s, &sn)) {
                return LWC_LBS_ERR_FIELD;
            }
            is_rc = lwc_jr_key_is(s, sn, "router_config");
        } else if (lwc_jr_key_is(k, kn, "region")) {
            if (!lwc_jr_str(&r, &s, &sn) || sn >= sizeof out->region) {
                return LWC_LBS_ERR_FIELD;
            }
            memcpy(out->region, s, sn);
            out->region[sn] = '\0';
        } else if (lwc_jr_key_is(k, kn, "freq_range")) {
            if (!lwc_jr_lit(&r, '[') || !rd_range(&r, 0, UINT32_MAX, &a) || !lwc_jr_lit(&r, ',') ||
                !rd_range(&r, 0, UINT32_MAX, &b2) || !lwc_jr_lit(&r, ']')) {
                return LWC_LBS_ERR_FIELD;
            }
            out->freq_min = (uint32_t)a;
            out->freq_max = (uint32_t)b2;
        } else if (lwc_jr_key_is(k, kn, "DRs")) {
            if (!rd_drs(&r, &out->drs_ok)) {
                return LWC_LBS_ERR_FIELD;
            }
        } else if (lwc_jr_key_is(k, kn, "sx1301_conf")) {
            /* the first concentrator is checked; any more are skipped */
            if (!lwc_jr_lit(&r, '[')) {
                return LWC_LBS_ERR_FIELD;
            }
            if (!lwc_jr_lit(&r, ']')) {
                if (!rd_sx1301(&r, out)) {
                    return LWC_LBS_ERR_FIELD;
                }
                out->have_sx1301 = true;
                while (lwc_jr_lit(&r, ',')) {
                    if (!lwc_jr_skip(&r)) {
                        return LWC_LBS_ERR_JSON;
                    }
                }
                if (!lwc_jr_lit(&r, ']')) {
                    return LWC_LBS_ERR_JSON;
                }
            }
        } else if (!lwc_jr_skip(&r)) {
            return LWC_LBS_ERR_JSON;
        }
    }
    if (!ok) {
        return LWC_LBS_ERR_JSON;
    }
    return is_rc ? LWC_LBS_OK : LWC_LBS_ERR_FIELD;
}

bool lwc_lbs_router_plan(const lwc_lbs_router_config_t *c, lwc_plan_t *out)
{
    if (c == NULL || out == NULL || !c->have_sx1301) {
        return false;
    }
    lwc_plan_t p;
    memset(&p, 0, sizeof p);
    p.radio_hz[0] = c->radio_hz[0];
    p.radio_hz[1] = c->radio_hz[1];
    for (unsigned i = 0; i < LWC_PLAN_MULTISF; i++) {
        p.multisf[i] = (lwc_plan_chan_t){(c->multisf & (1U << i)) != 0U, c->chan_radio[i], c->chan_if[i]};
    }
    p.std = (lwc_plan_chan_t){c->std_on, c->std_radio, c->std_if};
    p.std_sf = c->std_on ? c->std_sf : 7U;
    p.std_bw_khz = c->std_on ? (uint16_t)(c->std_bw_hz / 1000U) : 250U;
    p.fsk = (lwc_plan_chan_t){c->fsk_on, c->fsk_radio, c->fsk_if};
    if (lwc_plan_check(&p) != LWC_PLAN_OK) {
        return false;
    }
    *out = p;
    return true;
}

