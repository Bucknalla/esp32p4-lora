#include "lwc/lorawan.h"

#include <string.h>

#include "lwc/aes.h"

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 8; i != 0U; i--) {
        v = (v << 8) | p[i - 1U];
    }
    return v;
}

static void put_le(uint8_t *p, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8U * i));
    }
}

int lwc_lw_mtype(const uint8_t *p, size_t n)
{
    return (p == NULL || n == 0U) ? -1 : (int)(p[0] >> 5);
}

static lwc_lw_err_t check_mhdr(const uint8_t *p, size_t n, size_t min)
{
    if (p == NULL) {
        return LWC_LW_ERR_ARG;
    }
    if (n < min) {
        return LWC_LW_ERR_SHORT;
    }
    if ((p[0] & 0x03U) != 0U) {
        return LWC_LW_ERR_MAJOR;
    }
    return LWC_LW_OK;
}

lwc_lw_err_t lwc_lw_join_req_parse(const uint8_t *p, size_t n, lwc_lw_join_req_t *out)
{
    if (out == NULL) {
        return LWC_LW_ERR_ARG;
    }
    const lwc_lw_err_t e = check_mhdr(p, n, 1);
    if (e != LWC_LW_OK) {
        return e;
    }
    if ((p[0] >> 5) != LWC_LW_JOIN_REQUEST) {
        return LWC_LW_ERR_MTYPE;
    }
    if (n != LWC_LW_JOIN_REQ_LEN) {
        return LWC_LW_ERR_SHORT;
    }
    out->join_eui = le64(&p[1]);
    out->dev_eui = le64(&p[9]);
    out->dev_nonce = (uint16_t)(p[17] | (p[18] << 8));
    return LWC_LW_OK;
}

/* Compare MICs without an early exit. */
static bool mic_eq(const uint8_t *a, const uint8_t *b)
{
    uint8_t d = 0;
    for (unsigned i = 0; i < 4U; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0U;
}

bool lwc_lw_join_req_mic_ok(const uint8_t app_key[16], const uint8_t *p, size_t n)
{
    if (app_key == NULL || p == NULL || n != LWC_LW_JOIN_REQ_LEN) {
        return false;
    }
    uint8_t mac[16];
    lwc_aes_cmac(app_key, p, n - 4U, mac);
    return mic_eq(mac, &p[n - 4U]);
}

lwc_lw_err_t lwc_lw_data_parse(const uint8_t *p, size_t n, lwc_lw_data_t *out)
{
    if (out == NULL) {
        return LWC_LW_ERR_ARG;
    }
    /* MHDR(1) DevAddr(4) FCtrl(1) FCnt(2) ... MIC(4) */
    const lwc_lw_err_t e = check_mhdr(p, n, 12);
    if (e != LWC_LW_OK) {
        return e;
    }
    const unsigned mt = p[0] >> 5;
    if (mt < LWC_LW_UNCONFIRMED_UP || mt > LWC_LW_CONFIRMED_DOWN) {
        return LWC_LW_ERR_MTYPE;
    }
    memset(out, 0, sizeof *out);
    out->mtype = (lwc_lw_mtype_t)mt;
    out->dev_addr = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) |
                    ((uint32_t)p[4] << 24);
    out->fctrl = p[5];
    out->fcnt16 = (uint16_t)(p[6] | (p[7] << 8));
    out->fopts_len = (uint8_t)(p[5] & LWC_LW_FCTRL_FOPTS_LEN);
    const size_t end = n - 4U; /* MIC */
    size_t i = 8U + out->fopts_len;
    if (i > end) {
        return LWC_LW_ERR_SHORT;
    }
    out->fopts = out->fopts_len != 0U ? &p[8] : NULL;
    if (i < end) {
        out->has_port = true;
        out->fport = p[i++];
        out->frm = &p[i];
        out->frm_len = end - i;
        if (out->fport == 0U && out->fopts_len != 0U) {
            return LWC_LW_ERR_FORMAT; /* MAC commands in both places */
        }
    }
    return LWC_LW_OK;
}

uint32_t lwc_lw_fcnt32(uint32_t last, uint16_t fcnt16)
{
    uint32_t c = (last & 0xFFFF0000U) | fcnt16;
    if (c < last) {
        c += 0x10000U;
    }
    return c;
}

/* B0 (MIC) and Ai (encryption) blocks share this layout. */
static void block(uint8_t b[16], uint8_t first, bool downlink, uint32_t dev_addr, uint32_t fcnt32,
                  uint8_t last)
{
    memset(b, 0, 16);
    b[0] = first;
    b[5] = downlink ? 1U : 0U;
    put_le(&b[6], dev_addr, 4);
    put_le(&b[10], fcnt32, 4);
    b[15] = last;
}

void lwc_lw_data_mic(const uint8_t nwk_s_key[16], bool downlink, uint32_t dev_addr, uint32_t fcnt32,
                     const uint8_t *msg, size_t len, uint8_t mic[4])
{
    uint8_t buf[16 + LWC_LW_MAX_FRAME];
    if (len > LWC_LW_MAX_FRAME) {
        memset(mic, 0, 4);
        return;
    }
    block(buf, 0x49, downlink, dev_addr, fcnt32, (uint8_t)len);
    memcpy(&buf[16], msg, len);
    uint8_t mac[16];
    lwc_aes_cmac(nwk_s_key, buf, 16U + len, mac);
    memcpy(mic, mac, 4);
}

bool lwc_lw_data_mic_ok(const uint8_t nwk_s_key[16], bool downlink, uint32_t dev_addr, uint32_t fcnt32,
                        const uint8_t *p, size_t n)
{
    if (nwk_s_key == NULL || p == NULL || n < 12U || n > LWC_LW_MAX_FRAME) {
        return false;
    }
    uint8_t mic[4];
    lwc_lw_data_mic(nwk_s_key, downlink, dev_addr, fcnt32, p, n - 4U, mic);
    return mic_eq(mic, &p[n - 4U]);
}

void lwc_lw_crypt(const uint8_t key[16], bool downlink, uint32_t dev_addr, uint32_t fcnt32,
                  const uint8_t *in, size_t len, uint8_t *out)
{
    lwc_aes128_t ctx;
    lwc_aes128_init(&ctx, key);
    for (size_t off = 0, i = 1; off < len; off += 16U, i++) {
        uint8_t a[16], s[16];
        block(a, 0x01, downlink, dev_addr, fcnt32, (uint8_t)i);
        lwc_aes128_encrypt(&ctx, a, s);
        const size_t m = len - off < 16U ? len - off : 16U;
        for (size_t k = 0; k < m; k++) {
            out[off + k] = (uint8_t)(in[off + k] ^ s[k]);
        }
    }
    lwc_aes128_wipe(&ctx);
}

size_t lwc_lw_data_build(lwc_lw_mtype_t mtype, uint32_t dev_addr, uint8_t fctrl, uint32_t fcnt32,
                         const uint8_t *fopts, size_t fopts_len, bool has_port, uint8_t port,
                         const uint8_t *frm, size_t frm_len, const uint8_t nwk_s_key[16],
                         const uint8_t app_s_key[16], uint8_t *out, size_t cap)
{
    if (mtype < LWC_LW_UNCONFIRMED_UP || mtype > LWC_LW_CONFIRMED_DOWN || nwk_s_key == NULL || out == NULL ||
        fopts_len > 15U || (fopts_len != 0U && fopts == NULL) || (has_port && port == 0U && fopts_len != 0U) ||
        (!has_port && frm_len != 0U) || (frm_len != 0U && frm == NULL) ||
        (has_port && port != 0U && frm_len != 0U && app_s_key == NULL)) {
        return 0;
    }
    const size_t len = 8U + fopts_len + (has_port ? 1U + frm_len : 0U) + 4U;
    if (len > cap || len > LWC_LW_MAX_FRAME) {
        return 0;
    }
    const bool down = mtype == LWC_LW_UNCONFIRMED_DOWN || mtype == LWC_LW_CONFIRMED_DOWN;
    out[0] = (uint8_t)((unsigned)mtype << 5);
    put_le(&out[1], dev_addr, 4);
    out[5] = (uint8_t)((fctrl & 0xF0U) | fopts_len);
    put_le(&out[6], fcnt32, 2);
    size_t i = 8;
    if (fopts_len != 0U) {
        memcpy(&out[i], fopts, fopts_len);
        i += fopts_len;
    }
    if (has_port) {
        out[i++] = port;
        if (frm_len != 0U) {
            lwc_lw_crypt(port == 0U ? nwk_s_key : app_s_key, down, dev_addr, fcnt32, frm, frm_len, &out[i]);
            i += frm_len;
        }
    }
    lwc_lw_data_mic(nwk_s_key, down, dev_addr, fcnt32, out, i, &out[i]);
    return len;
}

void lwc_lw_session_keys(const uint8_t app_key[16], uint32_t join_nonce, uint32_t net_id,
                         uint16_t dev_nonce, uint8_t nwk_s_key[16], uint8_t app_s_key[16])
{
    lwc_aes128_t ctx;
    lwc_aes128_init(&ctx, app_key);
    uint8_t b[16] = {0};
    put_le(&b[1], join_nonce, 3);
    put_le(&b[4], net_id, 3);
    put_le(&b[7], dev_nonce, 2);
    b[0] = 0x01;
    lwc_aes128_encrypt(&ctx, b, nwk_s_key);
    b[0] = 0x02;
    lwc_aes128_encrypt(&ctx, b, app_s_key);
    lwc_aes128_wipe(&ctx);
}

size_t lwc_lw_join_accept(const uint8_t app_key[16], uint32_t join_nonce, uint32_t net_id,
                          uint32_t dev_addr, uint8_t dl_settings, uint8_t rx_delay,
                          const uint8_t *cflist, uint8_t *out, size_t cap)
{
    const size_t len = cflist != NULL ? 33U : 17U;
    if (app_key == NULL || out == NULL || cap < len) {
        return 0;
    }
    uint8_t f[33];
    f[0] = (uint8_t)(LWC_LW_JOIN_ACCEPT << 5);
    put_le(&f[1], join_nonce, 3);
    put_le(&f[4], net_id, 3);
    put_le(&f[7], dev_addr, 4);
    f[11] = dl_settings;
    f[12] = rx_delay;
    if (cflist != NULL) {
        memcpy(&f[13], cflist, 16);
    }
    uint8_t mac[16];
    lwc_aes_cmac(app_key, f, len - 4U, mac);
    memcpy(&f[len - 4U], mac, 4);
    /* The network "decrypts" so the device can use AES encrypt (spec 6.2.3). */
    lwc_aes128_t ctx;
    lwc_aes128_init(&ctx, app_key);
    out[0] = f[0];
    for (size_t off = 1; off < len; off += 16U) {
        lwc_aes128_decrypt(&ctx, &f[off], &out[off]);
    }
    lwc_aes128_wipe(&ctx);
    return len;
}

void lwc_lw_cflist_freqs(const uint32_t freq_hz[5], uint8_t out[16])
{
    for (unsigned i = 0; i < 5U; i++) {
        put_le(&out[i * 3U], freq_hz[i] / 100U, 3);
    }
    out[15] = 0; /* CFListType 0: frequencies */
}
