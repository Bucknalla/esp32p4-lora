#include "lwc/ns.h"

#include <string.h>

#include "lwc/lorawan.h"
#include "lwc/mac.h"
#include "lwc/region.h"

#define LWC_NS_CH_RETRY 4U /* uplinks between tries of the sub-band pair */

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
    ns->subband = -1;
    (void)lwc_ns_set_region(ns, lwc_region(LWC_REGION_EU868));
}

bool lwc_ns_region_ok(const lwc_region_t *r)
{
    return r != NULL && (!r->fixed || (r->n_up125 == 64U && r->n_up500 == 8U));
}

bool lwc_ns_set_region(lwc_ns_t *ns, const lwc_region_t *r)
{
    lwc_plan_t p;
    if (ns == NULL || !lwc_ns_region_ok(r) || !lwc_plan_default(&p, r)) {
        return false;
    }
    ns->region = r;
    lwc_ns_set_plan(ns, &p);
    return lwc_ns_set_rx(ns, 1U, r->rx2_hz, r->rx2_dr);
}

bool lwc_ns_rx_ok(const lwc_region_t *r, uint8_t rx1_delay_s, uint32_t rx2_hz, uint8_t rx2_dr)
{
    if (!lwc_ns_region_ok(r) || rx1_delay_s < 1U || rx1_delay_s > 15U || rx2_dr > 15U ||
        r->dr[rx2_dr].kind != LWC_DR_LORA) {
        return false;
    }
    if (r->fixed) { /* US915/AU915: a downlink DR (500 kHz), RX2 on its fixed frequency */
        return rx2_dr >= r->down.min_dr && rx2_dr <= r->down.max_dr && rx2_hz == r->rx2_hz;
    }
    if (r->dr[rx2_dr].bw_khz != 125U) {
        return false;
    }
    if (r->id == LWC_REGION_EU868) {
        return rx2_hz >= 863000000U && rx2_hz <= 870000000U;
    }
    return rx2_hz == r->rx2_hz;
}

uint16_t lwc_ns_ch_mask(const lwc_ns_t *ns)
{
    if (ns == NULL || ns->region == NULL) {
        return 0;
    }
    unsigned n = ns->region->n_default;
    for (size_t i = 0; i < 5U; i++) {
        n += ns->cflist_hz[i] != 0U ? 1U : 0U;
    }
    return (uint16_t)((1U << n) - 1U);
}

bool lwc_ns_set_rx(lwc_ns_t *ns, uint8_t rx1_delay_s, uint32_t rx2_hz, uint8_t rx2_dr)
{
    if (ns == NULL || !lwc_ns_rx_ok(ns->region, rx1_delay_s, rx2_hz, rx2_dr)) {
        return false;
    }
    ns->rx1_delay_s = rx1_delay_s;
    ns->rx2_hz = rx2_hz;
    ns->rx2_dr = rx2_dr;
    return true;
}

uint32_t lwc_ns_rx1_delay_us(const lwc_ns_dev_t *d)
{
    const uint8_t s = (d != NULL && d->rx1_delay_s >= 1U && d->rx1_delay_s <= 15U) ? d->rx1_delay_s : 1U;
    return (uint32_t)s * 1000000U;
}

void lwc_ns_set_plan(lwc_ns_t *ns, const lwc_plan_t *plan)
{
    if (ns != NULL && plan != NULL) {
        (void)lwc_plan_cflist(plan, ns->region, ns->cflist_hz);
        const int8_t sb = (int8_t)(ns->region->fixed ? lwc_plan_subband_of(plan, ns->region) : -1);
        if (sb != ns->subband) { /* another sub-band: every device must be moved again */
            for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
                ns->link[i].ch_done = false;
                ns->link[i].ch_pending = false;
                ns->link[i].ch_wait = 0;
                ns->link[i].ch_tries = 0;
            }
        }
        ns->subband = sb;
    }
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
    if (ns->region->fixed && ns->subband < 0) {
        return LWC_NS_JOIN_NO_PLAN; /* nothing changed: the device may try again */
    }
    uint32_t addr;
    if (d->joined) {
        addr = d->dev_addr; /* keep its address across rejoins */
    } else if (!alloc_addr(ns, &addr)) {
        return LWC_NS_JOIN_NO_ADDR;
    }
    const uint32_t jn = ns->join_nonce & 0xFFFFFFU;
    ns->join_nonce = (jn + 1U) & 0xFFFFFFU;
    uint8_t cflist[16];
    const uint8_t *cf = cflist;
    if (!ns->region->fixed) {
        lwc_lw_cflist_freqs(ns->cflist_hz, cflist);
    } else if (d->mac != LWC_NS_MAC_1_0_4 || !lwc_region_cflist_subband(ns->region, (uint8_t)ns->subband, cflist)) {
        cf = NULL; /* 1.0.2/1.0.3 in US915/AU915: no CFList; the LinkADRReq pair moves it (ADR-0026) */
    }
    /* DLSettings: RX1DROffset 0, RX2 data rate; RxDelay: RX1 delay in seconds */
    *accept_len = lwc_lw_join_accept(d->app_key, jn, LWC_NS_NET_ID, addr, (uint8_t)(ns->rx2_dr & 0x0FU),
                                     ns->rx1_delay_s, cf, accept, cap);
    d->rx1_delay_s = ns->rx1_delay_s;
    d->rx2_dr = ns->rx2_dr;
    d->rx2_hz = ns->rx2_hz;
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
        out->ack = (f.fctrl & LWC_LW_FCTRL_ACK) != 0U;
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

/* SF of an uplink DR (LoRa), else as DR0-5 = SF12-7 */
static uint8_t sf_of(const lwc_region_t *r, uint8_t dr)
{
    if (r != NULL && dr <= 15U && r->dr[dr].kind == LWC_DR_LORA) {
        return r->dr[dr].sf;
    }
    return (uint8_t)(12U - (dr > 5U ? 5U : dr));
}

void lwc_ns_adr(const lwc_region_t *r, int16_t snr_max10, uint8_t dr, uint8_t tx_power, uint8_t max_dr,
                uint8_t max_power, uint8_t *new_dr, uint8_t *new_power)
{
    const int req = lwc_ns_required_snr10(sf_of(r, dr));
    const int margin = (int)snr_max10 - req - LWC_NS_ADR_MARGIN10;
    int nstep = margin / 30; /* margin / 3 dB, truncated toward zero (as ChirpStack) */
    for (; nstep > 0; nstep--) {
        if (dr < max_dr) {
            dr++;
        } else if (tx_power < max_power) {
            tx_power++;
        }
    }
    for (; nstep < 0; nstep++) {
        if (tx_power > 0U) {
            tx_power--;
        }
    }
    *new_dr = dr;
    *new_power = tx_power;
}

/* ADR bookkeeping for an accepted uplink: the answer to our last LinkADRReq,
 * and the SNR history. */
static void adr_rx(const lwc_region_t *r, lwc_ns_link_t *l, const lwc_mac_up_t *m, const lwc_ns_uplink_t *up,
                   const lwc_ns_rx_t *rx)
{
    if (l->adr_pending) {
        l->adr_pending = false;
        if (m->link_adr_ans == 0U) {
            l->adr_wait = LWC_NS_ADR_HISTORY; /* request or answer lost: wait, then try again */
        } else if (m->link_adr_status == LWC_MAC_ADR_ALL_ACK) {
            if (l->pend_power != l->tx_power) {
                l->snr_n = 0; /* SNR at the old power says nothing about the new one */
                l->snr_pos = 0;
            }
            l->tx_power = l->pend_power;
            l->adr_nacks = 0;
            /* The answer normally comes on an uplink sent at the new rate. A
             * device that accepts a rate and keeps a lower one (its application
             * fixes the rate) would get the same request on every uplink. The
             * first time: wait a full history. The second time in a row: ask
             * for no higher rate than it uses, until it joins again. */
            const int used = rx->bw_khz == 125U ? lwc_ns_dr(r, rx->sf, rx->bw_khz) : -1;
            if (used >= 0) {
                if ((uint8_t)used < l->pend_dr) {
                    if (++l->dr_kept >= 2U) {
                        l->dr_cap = (uint8_t)(used + 1); /* 1 + the rate it uses */
                    } else {
                        l->adr_wait = LWC_NS_ADR_HISTORY;
                    }
                } else {
                    l->dr_kept = 0;
                }
            }
        } else {
            /* Refused (the device applies none of it): ask for less next time. */
            if ((m->link_adr_status & LWC_MAC_ADR_POWER_ACK) == 0U && l->pend_power > l->tx_power) {
                l->power_cap = l->pend_power; /* 1 + (pend_power - 1) */
            }
            if ((m->link_adr_status & LWC_MAC_ADR_DR_ACK) == 0U && l->pend_dr > 0U) {
                l->dr_cap = l->pend_dr;
            }
            if (l->adr_nacks < UINT8_MAX) {
                l->adr_nacks++;
            }
            l->adr_wait = LWC_NS_ADR_HISTORY;
        }
    }
    if (up->adr && rx->bw_khz == 125U) {
        l->snr10[l->snr_pos] = rx->snr10;
        l->snr_pos = (uint8_t)((l->snr_pos + 1U) % LWC_NS_ADR_HISTORY);
        if (l->snr_n < LWC_NS_ADR_HISTORY) {
            l->snr_n++;
        }
    }
}

/* Whether to send a LinkADRReq now, and with what. */
static bool adr_want(const lwc_region_t *r, lwc_ns_link_t *l, const lwc_ns_uplink_t *up, const lwc_ns_rx_t *rx,
                     uint8_t *dr, uint8_t *pw)
{
    const int used = rx->bw_khz == 125U ? lwc_ns_dr(r, rx->sf, rx->bw_khz) : -1;
    if (!up->adr || used < 0) {
        return false;
    }
    if (l->adr_wait != 0U) {
        l->adr_wait--;
        return false;
    }
    if (l->snr_n == 0U) {
        return false;
    }
    int16_t best = l->snr10[0];
    for (uint8_t k = 1; k < l->snr_n; k++) {
        if (l->snr10[k] > best) {
            best = l->snr10[k];
        }
    }
    const uint8_t cur = (uint8_t)used;
    const uint8_t top = r->fixed ? r->up125.max_dr : LWC_NS_ADR_MAX_DR; /* the fastest 125 kHz uplink DR */
    const uint8_t max_dr = l->dr_cap != 0U ? (uint8_t)(l->dr_cap - 1U) : top;
    const uint8_t max_pw = l->power_cap != 0U ? (uint8_t)(l->power_cap - 1U) : LWC_NS_ADR_MAX_POWER;
    lwc_ns_adr(r, best, cur, l->tx_power, max_dr < top ? max_dr : top, max_pw, dr, pw);
    /* Raising the power waits for a full history at the current power, so the
     * power does not go up and down (ChirpStack); faster and quieter can come
     * at once. */
    if (*pw < l->tx_power && l->snr_n < LWC_NS_ADR_HISTORY) {
        return false;
    }
    return *dr != cur || *pw != l->tx_power;
}

int lwc_ns_dr(const lwc_region_t *r, uint8_t sf, uint16_t bw_khz)
{
    return lwc_region_dr_of(r, sf, bw_khz, false);
}

size_t lwc_ns_max_frm(const lwc_region_t *r, int dr)
{
    /* EU868: 51 (DR0-2), 115 (DR3), 222 (DR4-7). LR-FHSS is uplink only and the
     * SX1302 cannot receive it: 0. */
    if (r == NULL || dr < 0 || dr > 15 || (r->dr[dr].kind != LWC_DR_LORA && r->dr[dr].kind != LWC_DR_FSK)) {
        return 0U;
    }
    return lwc_region_max_n(r, dr, r->tx_param_setup);
}

bool lwc_ns_answer(lwc_ns_t *ns, lwc_ns_up_res_t res, const lwc_ns_uplink_t *up, const lwc_ns_rx_t *rx,
                   lwc_ns_dl_t *dl)
{
    return lwc_ns_answer_app(ns, res, up, rx, NULL, dl);
}

bool lwc_ns_answer_app(lwc_ns_t *ns, lwc_ns_up_res_t res, const lwc_ns_uplink_t *up, const lwc_ns_rx_t *rx,
                       const lwc_ns_app_t *app, lwc_ns_dl_t *dl)
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
        const lwc_region_t *r = ns->region;
        bool refused = false;
        if (l->ch_pending) { /* the sub-band pair: a refused field discards the whole block */
            l->ch_pending = false;
            if (m.link_adr_ans != 0U && m.link_adr_status == LWC_MAC_ADR_ALL_ACK) {
                l->ch_done = true;
            } else {
                /* counted from the next uplink; doubles per refusal, up to 16 x */
                l->ch_wait = (uint8_t)(LWC_NS_CH_RETRY << (l->ch_tries < 4U ? l->ch_tries : 4U));
                if (l->ch_tries < UINT8_MAX) {
                    l->ch_tries++;
                }
                refused = true;
            }
        }
        adr_rx(r, l, &m, up, rx);
        uint8_t dr = 0, pw = 0;
        const bool adr = adr_want(r, l, up, rx, &dr, &pw);
        uint16_t mk[2];
        uint8_t cn[2];
        const bool sub = r->fixed && ns->subband >= 0 && lwc_region_link_adr_subband(r, (uint8_t)ns->subband, mk, cn);
        if (sub && !l->ch_done && !l->ch_pending && !refused) {
            /* US915/AU915 (ADR-0026): move the device to the sub-band: ChMaskCntl 7
             * (125 kHz channels off, the 500 kHz one on), then its 8 channels. Both
             * or neither (the first alone leaves no 125 kHz channel), with an ADR
             * change folded in, else the rate and power it has. */
            if (l->ch_wait != 0U) {
                l->ch_wait--;
            } else if (fo + 10U <= sizeof fopts) {
                uint8_t pdr = dr, ppw = pw;
                if (!adr) {
                    const int cur = lwc_ns_dr(r, rx->sf, rx->bw_khz);
                    pdr = d->mac == LWC_NS_MAC_1_0_4 ? 15U : (uint8_t)(cur < 0 ? 0 : cur); /* 15: keep (1.0.4) */
                    ppw = d->mac == LWC_NS_MAC_1_0_4 ? 15U : l->tx_power;
                }
                dl->ch_mask = lwc_mac_link_adr_req(fopts, sizeof fopts, &fo, pdr, ppw, mk[0], cn[0], 1) &&
                              lwc_mac_link_adr_req(fopts, sizeof fopts, &fo, pdr, ppw, mk[1], cn[1], 1);
                if (dl->ch_mask) {
                    need = true;
                    if (adr) {
                        dl->link_adr = true;
                        dl->adr_dr = dr;
                        dl->adr_power = pw;
                    }
                }
            }
        }
        if (adr && !dl->ch_mask && (!r->fixed || sub) &&
            lwc_mac_link_adr_req(fopts, sizeof fopts, &fo, dr, pw, r->fixed ? mk[1] : lwc_ns_ch_mask(ns),
                                 r->fixed ? cn[1] : 0U, 1)) {
            dl->link_adr = true;
            dl->adr_dr = dr;
            dl->adr_power = pw;
            need = true;
        }
    }
    size_t lim = 0;
    if (app != NULL && app->port >= 1U && app->port <= 223U && (app->len == 0U || app->data != NULL)) {
        /* RX1 at RX1DROffset 0 (the join-accept's DLSettings) */
        const int up_dr = lwc_ns_dr(ns->region, rx->sf, rx->bw_khz);
        const size_t n1 = lwc_ns_max_frm(ns->region, lwc_region_rx1_dr(ns->region, up_dr, 0, false));
        const size_t n2 = lwc_ns_max_frm(ns->region, d->rx2_dr);
        lim = n1 < n2 ? n1 : n2;
        dl->app = app->len + fo <= lim;
        dl->app_too_big = !dl->app;
    } else if (app != NULL) {
        dl->app_too_big = true; /* not sendable at all */
    }
    need = need || dl->app;
    if (!need) {
        return false;
    }
    /* ride along: battery and margin once a day (one byte; not if it would crowd out the payload) */
    if ((!dl->app || app->len + fo + 1U <= lim) &&
        (!l->status_asked || rx->now_s - l->status_req_s >= LWC_NS_STATUS_PERIOD_S)) {
        dl->dev_status = lwc_mac_dev_status_req(fopts, sizeof fopts, &fo);
        if (dl->dev_status) {
            l->status_asked = true;
            l->status_req_s = rx->now_s;
        }
    }
    dl->ack = up->confirmed;
    dl->confirmed = dl->app && app->confirmed;
    dl->fcnt = d->fcnt_down;
    /* FCtrl.ADR: this network controls the data rate (when the device lets it) */
    const uint8_t fctrl = (uint8_t)(LWC_LW_FCTRL_ADR | (dl->ack ? LWC_LW_FCTRL_ACK : 0U) |
                                    (dl->app && app->more ? LWC_LW_FCTRL_FPENDING : 0U));
    dl->len = dl->app ? lwc_lw_data_build(dl->confirmed ? LWC_LW_CONFIRMED_DOWN : LWC_LW_UNCONFIRMED_DOWN, d->dev_addr, fctrl, dl->fcnt, fopts, fo, true,
                                          app->port, app->data, app->len, d->nwk_s_key, d->app_s_key, dl->frame,
                                          sizeof dl->frame)
                      : lwc_lw_data_build(LWC_LW_UNCONFIRMED_DOWN, d->dev_addr, fctrl, dl->fcnt, fopts, fo, false, 0,
                                          NULL, 0, d->nwk_s_key, NULL, dl->frame, sizeof dl->frame);
    if (dl->len == 0U) {
        return false;
    }
    if (dl->link_adr) {
        l->adr_pending = true;
        l->pend_dr = dl->adr_dr;
        l->pend_power = dl->adr_power;
    }
    if (dl->ch_mask) {
        l->ch_pending = true;
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

/* RX2 SF and bandwidth: a DR the server accepts for RX2 (lwc_ns_rx_ok), else
 * the region's default RX2 DR */
static void rx2_params(const lwc_region_t *r, uint8_t dr, uint8_t *sf, uint16_t *bw)
{
    if (r == NULL) {
        *sf = 12;
        *bw = 125;
        return;
    }
    const lwc_region_dr_t *d = dr <= 15U ? &r->dr[dr] : NULL;
    const bool ok = d != NULL && d->kind == LWC_DR_LORA &&
                    (r->fixed ? dr >= r->down.min_dr && dr <= r->down.max_dr : d->bw_khz == 125U);
    if (!ok) {
        d = &r->dr[r->rx2_dr];
    }
    *sf = d->sf;
    *bw = d->bw_khz;
}

void lwc_ns_downlink(const lwc_region_t *r, bool rx2, uint32_t up_tmst, uint32_t up_freq_hz, uint8_t up_sf,
                     uint16_t up_bw_khz, uint32_t delay_us, uint32_t rx2_hz, uint8_t rx2_dr, int8_t power_dbm,
                     const uint8_t *payload, size_t len, lwc_txpk_t *out)
{
    memset(out, 0, sizeof *out);
    out->mode = LWC_TXPK_TIMED;
    out->tmst = up_tmst + delay_us + (rx2 ? 1000000U : 0U);
    if (rx2) {
        out->freq_hz = rx2_hz;
        rx2_params(r, rx2_dr, &out->sf, &out->bw_khz);
    } else {
        /* RX1DROffset 0: the region's RX1 DR and frequency for the uplink */
        const uint32_t f = lwc_region_rx1_hz(r, up_freq_hz, up_bw_khz);
        out->freq_hz = f != 0U ? f : up_freq_hz;
        out->sf = up_sf;
        out->bw_khz = up_bw_khz;
        const int ud = lwc_region_dr_of(r, up_sf, up_bw_khz, false);
        const int d1 = ud >= 0 ? lwc_region_rx1_dr(r, ud, 0, false) : -1;
        if (d1 >= 0 && r->dr[d1].kind == LWC_DR_LORA) {
            out->sf = r->dr[d1].sf;
            out->bw_khz = r->dr[d1].bw_khz;
        }
    }
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

lwc_ns_err_t lwc_ns_set_class(lwc_ns_t *ns, uint64_t dev_eui, lwc_ns_class_t cls)
{
    if (ns == NULL || (cls != LWC_NS_CLASS_A && cls != LWC_NS_CLASS_C)) {
        return LWC_NS_ERR_ARG;
    }
    const int i = lwc_ns_find(ns, dev_eui);
    if (i < 0) {
        return LWC_NS_ERR_NOT_FOUND;
    }
    ns->dev[i].dev_class = (uint8_t)cls;
    return LWC_NS_OK;
}

bool lwc_ns_class_c_down(lwc_ns_t *ns, size_t idx, const lwc_ns_app_t *app, lwc_ns_dl_t *dl)
{
    if (ns == NULL || app == NULL || dl == NULL || idx >= LWC_NS_MAX_DEVICES) {
        return false;
    }
    lwc_ns_dev_t *d = &ns->dev[idx];
    memset(dl, 0, sizeof *dl);
    if (!d->used || !d->joined || !d->has_up || d->dev_class != LWC_NS_CLASS_C || app->port < 1U ||
        app->port > 223U || (app->len > 0U && app->data == NULL)) {
        return false;
    }
    if (app->len > lwc_ns_max_frm(ns->region, d->rx2_dr)) {
        dl->app_too_big = true;
        return false;
    }
    dl->app = true;
    dl->confirmed = app->confirmed;
    dl->fcnt = d->fcnt_down;
    const uint8_t fctrl = (uint8_t)(LWC_LW_FCTRL_ADR | (app->more ? LWC_LW_FCTRL_FPENDING : 0U));
    dl->len = lwc_lw_data_build(app->confirmed ? LWC_LW_CONFIRMED_DOWN : LWC_LW_UNCONFIRMED_DOWN, d->dev_addr, fctrl,
                                dl->fcnt, NULL, 0, true, app->port, app->data, app->len, d->nwk_s_key, d->app_s_key,
                                dl->frame, sizeof dl->frame);
    if (dl->len == 0U) {
        return false;
    }
    d->fcnt_down++;
    return true;
}

void lwc_ns_downlink_at(const lwc_region_t *r, uint32_t tmst, uint32_t freq_hz, uint8_t dr, int8_t power_dbm,
                        const uint8_t *payload, size_t len, lwc_txpk_t *out)
{
    /* the RX2 form of lwc_ns_downlink, at `tmst` itself */
    lwc_ns_downlink(r, true, tmst - 1000000U, 0, 0, 0, 0, freq_hz, dr, power_dbm, payload, len, out);
}

lwc_ns_err_t lwc_ns_set_c_ack(lwc_ns_t *ns, uint64_t dev_eui, uint16_t seconds)
{
    if (ns == NULL || (seconds != 0U && (seconds < LWC_NS_C_ACK_MIN_S || seconds > LWC_NS_C_ACK_MAX_S))) {
        return LWC_NS_ERR_ARG;
    }
    const int i = lwc_ns_find(ns, dev_eui);
    if (i < 0) {
        return LWC_NS_ERR_NOT_FOUND;
    }
    ns->dev[i].c_ack_s = seconds;
    return LWC_NS_OK;
}

uint16_t lwc_ns_c_ack_s(const lwc_ns_dev_t *d)
{
    return d == NULL || d->c_ack_s == 0U ? (uint16_t)LWC_NS_C_ACK_DEFAULT_S : d->c_ack_s;
}
