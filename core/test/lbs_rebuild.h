/*
 * Test helper: rebuild the frame bytes from a jreq/updf/propdf message, as the
 * server does (TTS lbslns upstream.go). Used by test_lbs and fuzz_lbs to check
 * that every uplink message carries its frame exactly.
 */
#ifndef LBS_REBUILD_H
#define LBS_REBUILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lwc/hex.h"
#include "lwc/json.h"

static bool rb_int(lwc_jr_t *r, int64_t *v)
{
    const char *s;
    size_t n;
    return lwc_jr_num(r, &s, &n) && lwc_jr_i64(s, n, v);
}

static bool rb_hex(lwc_jr_t *r, uint8_t *out, size_t cap, size_t *n)
{
    const char *s;
    size_t sn;
    if (!lwc_jr_str(r, &s, &sn) || sn % 2U != 0U || sn / 2U > cap) {
        return false;
    }
    *n = sn / 2U;
    return sn == 0U || lwc_hex_decode(s, sn, out, *n);
}

/* "HH-HH-HH-HH-HH-HH-HH-HH" to 8 bytes, little-endian as in the frame. */
static bool rb_eui_le(lwc_jr_t *r, uint8_t out[8])
{
    const char *s;
    size_t sn;
    if (!lwc_jr_str(r, &s, &sn) || sn != 23U) {
        return false;
    }
    for (size_t i = 0; i < 8U; i++) {
        uint8_t b;
        if (!lwc_hex_decode(&s[3U * i], 2, &b, 1) || (i < 7U && s[3U * i + 2U] != '-')) {
            return false;
        }
        out[7U - i] = b;
    }
    return true;
}

static void rb_le32(uint8_t *p, int64_t v)
{
    const uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);
    p[3] = (uint8_t)(u >> 24);
}

/* Returns the frame length, or 0 if the message cannot be rebuilt. */
static size_t lbs_rebuild(const char *json, size_t len, uint8_t *out, size_t cap)
{
    lwc_jr_t r = {json, json + len};
    if (cap < 255U || !lwc_jr_lit(&r, '{')) {
        return 0;
    }
    int type = 0; /* 1 jreq, 2 updf, 3 propdf */
    int64_t mhdr = -1, devaddr = 0, fctrl = 0, fcnt = 0, fport = -2, mic = 0, nonce = 0;
    uint8_t join[8] = {0}, dev[8] = {0}, fopts[16], frm[255];
    size_t nfo = 0, nfrm = 0;
    bool have_mic = false;
    for (bool first = true;; first = false) {
        if (!first && !lwc_jr_lit(&r, ',')) {
            break;
        }
        const char *k, *s;
        size_t kn, sn;
        if (!lwc_jr_str(&r, &k, &kn) || !lwc_jr_lit(&r, ':')) {
            return 0;
        }
        bool ok = true;
        if (lwc_jr_key_is(k, kn, "msgtype")) {
            ok = lwc_jr_str(&r, &s, &sn);
            type = lwc_jr_key_is(s, sn, "jreq") ? 1 : lwc_jr_key_is(s, sn, "updf") ? 2 : lwc_jr_key_is(s, sn, "propdf") ? 3 : 0;
        } else if (lwc_jr_key_is(k, kn, "MHdr")) {
            ok = rb_int(&r, &mhdr);
        } else if (lwc_jr_key_is(k, kn, "JoinEui")) {
            ok = rb_eui_le(&r, join);
        } else if (lwc_jr_key_is(k, kn, "DevEui")) {
            ok = rb_eui_le(&r, dev);
        } else if (lwc_jr_key_is(k, kn, "DevNonce")) {
            ok = rb_int(&r, &nonce);
        } else if (lwc_jr_key_is(k, kn, "MIC")) {
            ok = rb_int(&r, &mic);
            have_mic = true;
        } else if (lwc_jr_key_is(k, kn, "DevAddr")) {
            ok = rb_int(&r, &devaddr);
        } else if (lwc_jr_key_is(k, kn, "FCtrl")) {
            ok = rb_int(&r, &fctrl);
        } else if (lwc_jr_key_is(k, kn, "FCnt")) {
            ok = rb_int(&r, &fcnt);
        } else if (lwc_jr_key_is(k, kn, "FOpts")) {
            ok = rb_hex(&r, fopts, sizeof fopts, &nfo);
        } else if (lwc_jr_key_is(k, kn, "FPort")) {
            ok = rb_int(&r, &fport);
        } else if (lwc_jr_key_is(k, kn, "FRMPayload")) {
            ok = rb_hex(&r, frm, sizeof frm, &nfrm);
        } else {
            ok = lwc_jr_skip(&r);
        }
        if (!ok) {
            return 0;
        }
    }
    if (!lwc_jr_lit(&r, '}')) {
        return 0;
    }
    size_t n = 0;
    if (type == 3) {
        memcpy(out, frm, nfrm);
        return nfrm;
    }
    if (mhdr < 0 || mhdr > 255 || !have_mic) {
        return 0;
    }
    out[n++] = (uint8_t)mhdr;
    if (type == 1) {
        memcpy(&out[n], join, 8);
        memcpy(&out[n + 8U], dev, 8);
        n += 16U;
        out[n++] = (uint8_t)nonce;
        out[n++] = (uint8_t)(nonce >> 8);
    } else if (type == 2) {
        if (nfo != (size_t)(fctrl & 0x0F) || (fport == -1 && nfrm != 0U) || 8U + nfo + 1U + nfrm + 4U > 255U) {
            return 0;
        }
        rb_le32(&out[n], devaddr);
        n += 4U;
        out[n++] = (uint8_t)fctrl;
        out[n++] = (uint8_t)fcnt;
        out[n++] = (uint8_t)(fcnt >> 8);
        memcpy(&out[n], fopts, nfo);
        n += nfo;
        if (fport >= 0) {
            out[n++] = (uint8_t)fport;
            memcpy(&out[n], frm, nfrm);
            n += nfrm;
        }
    } else {
        return 0;
    }
    rb_le32(&out[n], mic);
    return n + 4U;
}

#endif /* LBS_REBUILD_H */
