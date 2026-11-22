#include "lwc/ns.h"

#include <string.h>

#include "lwc/lorawan.h"
#include "lwc/mac.h"

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n-- != 0U) {
        *v++ = 0;
    }
}

void lwc_ns_init(lwc_ns_t *ns)
{
    memset(ns, 0, sizeof *ns);
    ns->join_nonce = 1;
    ns->next_addr = 1;
}

int lwc_ns_find(const lwc_ns_t *ns, uint64_t dev_eui)
{
    for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
        if (ns->dev[i].used && ns->dev[i].dev_eui == dev_eui) {
            return (int)i;
        }
    }
    return -1;
}

lwc_ns_err_t lwc_ns_add(lwc_ns_t *ns, uint64_t dev_eui, uint64_t join_eui, const uint8_t app_key[16],
                        lwc_ns_mac_t mac, const char *name, size_t *idx)
{
    if (ns == NULL || app_key == NULL || (mac != LWC_NS_MAC_1_0_2 && mac != LWC_NS_MAC_1_0_4)) {
        return LWC_NS_ERR_ARG;
    }
    if (lwc_ns_find(ns, dev_eui) >= 0) {
        return LWC_NS_ERR_EXISTS;
    }
    for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
        lwc_ns_dev_t *d = &ns->dev[i];
        if (!d->used) {
            memset(d, 0, sizeof *d);
            d->used = true;
            d->dev_eui = dev_eui;
            d->join_eui = join_eui;
            memcpy(d->app_key, app_key, 16);
            d->mac = (uint8_t)mac;
            if (name != NULL) {
                size_t k = 0;
                for (; k < LWC_NS_NAME_MAX && name[k] != '\0'; k++) {
                    const char c = name[k];
                    d->name[k] = (c >= 0x20 && c < 0x7F) ? c : '?'; /* printable ASCII only */
                }
                d->name[k] = '\0';
            }
            if (idx != NULL) {
                *idx = i;
            }
            return LWC_NS_OK;
        }
    }
    return LWC_NS_ERR_FULL;
}

lwc_ns_err_t lwc_ns_remove(lwc_ns_t *ns, uint64_t dev_eui)
{
    const int i = lwc_ns_find(ns, dev_eui);
    if (i < 0) {
        return LWC_NS_ERR_NOT_FOUND;
    }
    wipe(&ns->dev[i], sizeof ns->dev[i]);
    memset(&ns->link[i], 0, sizeof ns->link[i]);
    return LWC_NS_OK;
}

static bool addr_in_use(const lwc_ns_t *ns, uint32_t a)
{
    for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
        if (ns->dev[i].used && ns->dev[i].joined && ns->dev[i].dev_addr == a) {
            return true;
        }
    }
    return false;
}

static bool alloc_addr(lwc_ns_t *ns, uint32_t *out)
{
    for (uint32_t tries = 0; tries <= LWC_NS_MAX_DEVICES; tries++) {
        uint32_t a = ns->next_addr;
        if (a == 0U || a > LWC_NS_ADDR_MAX) {
            a = 1;
        }
        ns->next_addr = a + 1U;
        if (!addr_in_use(ns, a)) {
            *out = a;
            return true;
        }
    }
    return false;
}

static bool nonce_ok(const lwc_ns_dev_t *d, uint16_t nonce)
{
    for (uint8_t k = 0; k < d->nonce_count; k++) {
        if (d->nonces[k] == nonce) {
            return false;
        }
    }
    if (d->mac == LWC_NS_MAC_1_0_4 && d->nonce_count != 0U) {
        const uint8_t last = (uint8_t)((d->nonce_pos + LWC_NS_NONCE_HISTORY - 1U) % LWC_NS_NONCE_HISTORY);
        return nonce > d->nonces[last];
    }
    return true;
}

static void nonce_push(lwc_ns_dev_t *d, uint16_t nonce)
{
    d->nonces[d->nonce_pos] = nonce;
    d->nonce_pos = (uint8_t)((d->nonce_pos + 1U) % LWC_NS_NONCE_HISTORY);
    if (d->nonce_count < LWC_NS_NONCE_HISTORY) {
        d->nonce_count++;
    }
}

lwc_ns_join_res_t lwc_ns_join(lwc_ns_t *ns, const uint8_t *frame, size_t n, uint8_t *accept,
                              size_t cap, size_t *accept_len, size_t *idx)
{
    lwc_lw_join_req_t jr;
    if (ns == NULL || accept == NULL || accept_len == NULL || cap < 33U ||
        lwc_lw_join_req_parse(frame, n, &jr) != LWC_LW_OK) {
        return LWC_NS_JOIN_MALFORMED;
    }
    const int i = lwc_ns_find(ns, jr.dev_eui);
    if (i < 0 || ns->dev[i].join_eui != jr.join_eui) {
        return LWC_NS_JOIN_NOT_OURS;
    }
    lwc_ns_dev_t *d = &ns->dev[i];
    if (!lwc_lw_join_req_mic_ok(d->app_key, frame, n)) {
        return LWC_NS_JOIN_BAD_MIC;
    }
    if (!nonce_ok(d, jr.dev_nonce)) {
        return LWC_NS_JOIN_REPLAY;
    }
    uint32_t addr;
    if (d->joined) {
        addr = d->dev_addr; /* keep its address across rejoins */
    } else if (!alloc_addr(ns, &addr)) {
        return LWC_NS_JOIN_NO_ADDR;
    }
    const uint32_t jn = ns->join_nonce & 0xFFFFFFU;
    ns->join_nonce = (jn + 1U) & 0xFFFFFFU;
    static const uint32_t ch[5] = {867100000, 867300000, 867500000, 867700000, 867900000};
    uint8_t cflist[16];
    lwc_lw_cflist_freqs(ch, cflist);
    *accept_len = lwc_lw_join_accept(d->app_key, jn, LWC_NS_NET_ID, addr, LWC_NS_DL_SETTINGS,
                                     LWC_NS_RX_DELAY, cflist, accept, cap);
    nonce_push(d, jr.dev_nonce);
    lwc_lw_session_keys(d->app_key, jn, LWC_NS_NET_ID, jr.dev_nonce, d->nwk_s_key, d->app_s_key);
    d->joined = true;
    d->dev_addr = addr;
    d->has_up = false;
    d->fcnt_up = 0;
    d->fcnt_down = 0;
    memset(&ns->link[i], 0, sizeof ns->link[i]); /* new session */
    if (idx != NULL) {
        *idx = (size_t)i;
    }
    return LWC_NS_JOIN_ACCEPT;
}

lwc_ns_up_res_t lwc_ns_uplink(lwc_ns_t *ns, const uint8_t *frame, size_t n, lwc_ns_uplink_t *out)
{
    lwc_lw_data_t f;
    if (ns == NULL || out == NULL || lwc_lw_data_parse(frame, n, &f) != LWC_LW_OK) {
        return LWC_NS_UP_MALFORMED;
    }
    if (f.mtype != LWC_LW_UNCONFIRMED_UP && f.mtype != LWC_LW_CONFIRMED_UP) {
        return LWC_NS_UP_MALFORMED;
    }
    bool seen = false;
    for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
        lwc_ns_dev_t *d = &ns->dev[i];
        if (!d->used || !d->joined || d->dev_addr != f.dev_addr) {
            continue;
        }
        seen = true;
        const uint32_t fcnt = d->has_up ? lwc_lw_fcnt32(d->fcnt_up, f.fcnt16) : f.fcnt16;
        if (!lwc_lw_data_mic_ok(d->nwk_s_key, false, f.dev_addr, fcnt, frame, n)) {
            continue; /* another device on the same address? */
        }
        lwc_ns_link_t *l = &ns->link[i];
        memset(out, 0, sizeof *out);
        out->idx = i;
        out->fcnt = fcnt;
        out->confirmed = f.mtype == LWC_LW_CONFIRMED_UP;
        out->adr = (f.fctrl & LWC_LW_FCTRL_ADR) != 0U;
        out->adr_ack_req = (f.fctrl & LWC_LW_FCTRL_ADR_ACK_REQ) != 0U;
        if (d->has_up && fcnt <= d->fcnt_up) {
            /* The same counter again: a device that missed its ACK repeats the
             * frame. Answer a few times, then treat it as a replay. */
            if (fcnt == d->fcnt_up && l->retrans < LWC_NS_RETRANS_MAX) {
                l->retrans++;
                return LWC_NS_UP_RETRANS;
            }
            return LWC_NS_UP_REPLAY;
        }
        d->has_up = true;
        d->fcnt_up = fcnt;
        l->retrans = 0;
        out->fopts_len = f.fopts_len;
        if (f.fopts_len != 0U) {
            memcpy(out->fopts, f.fopts, f.fopts_len);
        }
        out->has_port = f.has_port;
        if (f.has_port) {
            out->port = f.fport;
            out->len = f.frm_len <= sizeof out->payload ? f.frm_len : sizeof out->payload;
            /* port 0 carries MAC commands, encrypted with the network key */
            lwc_lw_crypt(f.fport == 0U ? d->nwk_s_key : d->app_s_key, false, f.dev_addr, fcnt, f.frm,
                         out->len, out->payload);
        }
        return LWC_NS_UP_OK;
    }
    return seen ? LWC_NS_UP_BAD_MIC : LWC_NS_UP_NOT_OURS;
}

int16_t lwc_ns_required_snr10(uint8_t sf)
{
    if (sf < 7U) {
        sf = 7U;
    }
    if (sf > 12U) {
        sf = 12U;
    }
    return (int16_t)(-75 - 25 * (int)(sf - 7U));
}

bool lwc_ns_answer(lwc_ns_t *ns, lwc_ns_up_res_t res, const lwc_ns_uplink_t *up, const lwc_ns_rx_t *rx,
                   lwc_ns_dl_t *dl)
{
    if (ns == NULL || up == NULL || rx == NULL || dl == NULL || up->idx >= LWC_NS_MAX_DEVICES ||
        (res != LWC_NS_UP_OK && res != LWC_NS_UP_RETRANS)) {
        return false;
    }
    lwc_ns_dev_t *d = &ns->dev[up->idx];
    lwc_ns_link_t *l = &ns->link[up->idx];
    if (!d->used || !d->joined) {
        return false;
    }
    memset(dl, 0, sizeof *dl);
    uint8_t fopts[15];
    size_t fo = 0;
    bool need = up->confirmed || up->adr_ack_req; /* ADRACKReq: any downlink will do */
    if (res == LWC_NS_UP_OK) {
        lwc_mac_up_t m;
        if (up->has_port && up->port == 0U) {
            (void)lwc_mac_parse_up(up->payload, up->len, &m);
        } else {
            (void)lwc_mac_parse_up(up->fopts, up->fopts_len, &m);
        }
        if (m.dev_status_ans) {
            l->has_status = true;
            l->battery = m.battery;
            l->margin = m.margin;
            l->status_s = rx->now_s;
        }
        if (m.link_check_req) {
            const int margin = (rx->snr10 - lwc_ns_required_snr10(rx->sf)) / 10;
            dl->link_check = lwc_mac_link_check_ans(fopts, sizeof fopts, &fo,
                                                    (uint8_t)(margin < 0 ? 0 : margin > 254 ? 254 : margin), 1);
            need = true;
        }
        if (m.device_time_req && rx->time_ok) {
            dl->device_time = lwc_mac_device_time_ans(fopts, sizeof fopts, &fo, rx->gps_s, rx->gps_frac);
            need = true;
        }
    }
    if (!need) {
        return false;
    }
    /* ride along: battery and margin once a day */
    if (!l->status_asked || rx->now_s - l->status_req_s >= LWC_NS_STATUS_PERIOD_S) {
        dl->dev_status = lwc_mac_dev_status_req(fopts, sizeof fopts, &fo);
        if (dl->dev_status) {
            l->status_asked = true;
            l->status_req_s = rx->now_s;
        }
    }
    dl->ack = up->confirmed;
    dl->fcnt = d->fcnt_down;
    dl->len = lwc_lw_data_build(LWC_LW_UNCONFIRMED_DOWN, d->dev_addr, dl->ack ? LWC_LW_FCTRL_ACK : 0U,
                                dl->fcnt, fopts, fo, false, 0, NULL, 0, d->nwk_s_key, NULL, dl->frame,
                                sizeof dl->frame);
    if (dl->len == 0U) {
        return false;
    }
    d->fcnt_down++;
    return true;
}

bool lwc_ns_forward(lwc_mode_t mode, bool handled_locally)
{
    switch (mode) {
    case LWC_MODE_TTN:
        return true;
    case LWC_MODE_HYBRID:
        return !handled_locally;
    case LWC_MODE_STANDALONE:
    default:
        return false;
    }
}

void lwc_ns_downlink(bool rx2, uint32_t up_tmst, uint32_t up_freq_hz, uint8_t up_sf, uint16_t up_bw_khz,
                     uint32_t delay_us, int8_t power_dbm, const uint8_t *payload, size_t len,
                     lwc_txpk_t *out)
{
    memset(out, 0, sizeof *out);
    out->mode = LWC_TXPK_TIMED;
    out->tmst = up_tmst + delay_us + (rx2 ? 1000000U : 0U);
    out->freq_hz = rx2 ? LWC_NS_RX2_FREQ_HZ : up_freq_hz;
    out->sf = rx2 ? LWC_NS_RX2_SF : up_sf;
    out->bw_khz = rx2 ? 125U : up_bw_khz;
    out->cr_denom = 5;
    out->rfch = 0;
    out->powe = power_dbm;
    out->ipol = true;
    out->ncrc = true;
    out->size = (uint16_t)(len <= sizeof out->payload ? len : sizeof out->payload);
    if (payload != NULL) {
        memcpy(out->payload, payload, out->size);
    }
}
