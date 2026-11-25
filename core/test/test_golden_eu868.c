/*
 * EU868 golden snapshot (ADR-0023): every output of the region-dependent core
 * functions over a grid of inputs, as text, compared with
 * core/test/golden_eu868.txt. Taken before the region layer was introduced,
 * so the refactor is shown to leave EU868 byte-identical. A change here is a
 * change of behaviour: regenerate the file only on purpose
 * (test_golden_eu868 --write <file>).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwc/aes.h"
#include "lwc/lbs.h"
#include "lwc/lorawan.h"
#include "lwc/mac.h"
#include "lwc/ns.h"
#include "lwc/plan.h"
#include "lwc/txpk.h"

static char *s_out;
static size_t s_len, s_cap;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#include <stdarg.h>

#include "lwc/region.h"
#define EU lwc_region(LWC_REGION_EU868)
#define C868 lwc_card(LWC_CARD_868)
static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char line[512];
    const int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (s_len + (size_t)n + 1U > s_cap) {
        s_cap = (s_len + (size_t)n + 1U) * 2U;
        s_out = realloc(s_out, s_cap);
    }
    memcpy(s_out + s_len, line, (size_t)n);
    s_len += (size_t)n;
    s_out[s_len] = '\0';
}

static void hex(const char *tag, const uint8_t *b, size_t n)
{
    emit("%s", tag);
    for (size_t i = 0; i < n; i++) {
        emit("%02X", b[i]);
    }
    emit("\n");
}

static void txpk(const char *tag, const lwc_txpk_t *t)
{
    emit("%s mode=%d tmst=%" PRIu32 " freq=%" PRIu32 " rfch=%u powe=%d sf=%u bw=%u cr=%u ipol=%d ncrc=%d prea=%u size=%u\n",
         tag, (int)t->mode, t->tmst, t->freq_hz, t->rfch, t->powe, t->sf, t->bw_khz, t->cr_denom, t->ipol, t->ncrc,
         t->prea, t->size);
}

static const uint8_t KEY[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
                                0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};

static lwc_ns_t ns;

static void grid(void)
{
    /* data rates and sizes */
    static const uint16_t bws[] = {0, 125, 250, 500};
    for (uint8_t sf = 5; sf <= 13; sf++) {
        for (size_t b = 0; b < 4; b++) {
            emit("ns_dr sf=%u bw=%u -> %d | lbs_dr -> %d\n", sf, bws[b], lwc_ns_dr(EU, sf, bws[b]), lwc_lbs_dr(EU, sf, bws[b]));
        }
        emit("required_snr10 sf=%u -> %d\n", sf, lwc_ns_required_snr10(sf));
    }
    for (int dr = -1; dr <= 16; dr++) {
        uint8_t sf = 0;
        uint16_t bw = 0;
        const bool ok = dr >= 0 && lwc_lbs_dr_params(EU, (uint8_t)dr, &sf, &bw);
        emit("max_frm dr=%d -> %zu | lbs_dr_params -> %d sf=%u bw=%u\n", dr, lwc_ns_max_frm(EU, dr), ok, sf, bw);
    }
    /* receive window settings */
    static const uint32_t rxf[] = {862999999, 863000000, 868100000, 869525000, 870000000, 870000001};
    for (uint8_t d = 0; d <= 16; d += 4) {
        for (size_t f = 0; f < 6; f++) {
            for (uint8_t dr = 0; dr <= 8; dr += 2) {
                emit("rx_ok d=%u f=%" PRIu32 " dr=%u -> %d\n", d, rxf[f], dr, lwc_ns_rx_ok(EU, d, rxf[f], dr));
            }
        }
    }
    /* duty cycle: bands, budgets, a sequence of takes */
    for (uint32_t f = 862900000; f <= 870100000; f += 50000) {
        const int band = lwc_dc_band(f);
        emit("dc_band f=%" PRIu32 " -> %d permille=%" PRIu32 "\n", f, band, lwc_dc_permille(band));
    }
    lwc_dutycycle_t dc;
    lwc_dc_init(&dc, 0);
    static const uint32_t tf[] = {868100000, 869525000, 868800000, 867100000, 869525000};
    int64_t now = 0;
    for (int i = 0; i < 60; i++) {
        const uint32_t f = tf[i % 5];
        const uint32_t air = 50000U + (uint32_t)i * 37000U;
        emit("dc_take t=%" PRId64 " f=%" PRIu32 " air=%" PRIu32 " -> %d\n", now, f, air, lwc_dc_take(&dc, now, f, air));
        now += 1000000 + (int64_t)i * 250000;
    }
    /* the default plan, packed, and its CFList */
    lwc_plan_t p;
    (void)lwc_plan_default(&p, EU);
    uint8_t packed[LWC_PLAN_PACKED];
    emit("plan_check -> %d\n", (int)lwc_plan_check(&p, EU, C868));
    if (lwc_plan_pack(&p, packed, sizeof packed)) {
        hex("plan_pack ", packed, sizeof packed);
    }
    uint32_t cf[5];
    const size_t ncf = lwc_plan_cflist(&p, EU, cf);
    emit("plan_cflist n=%zu %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", ncf, cf[0], cf[1], cf[2],
         cf[3], cf[4]);
    uint8_t cfb[16];
    lwc_lw_cflist_freqs(cf, cfb);
    hex("cflist_bytes ", cfb, sizeof cfb);
    /* ADR steps */
    for (int16_t snr = -200; snr <= 150; snr += 25) {
        for (uint8_t dr = 0; dr <= 5; dr++) {
            for (uint8_t pw = 0; pw <= 5; pw += 5) {
                uint8_t ndr, npw;
                lwc_ns_adr(snr, dr, pw, LWC_NS_ADR_MAX_DR, LWC_NS_ADR_MAX_POWER, &ndr, &npw);
                emit("adr snr=%d dr=%u pw=%u -> dr=%u pw=%u\n", snr, dr, pw, ndr, npw);
            }
        }
    }
    /* LinkADRReq bytes */
    for (uint8_t dr = 0; dr <= 6; dr += 3) {
        uint8_t b[8];
        size_t n = 0;
        if (lwc_mac_link_adr_req(b, sizeof b, &n, dr, 3, 0x00FFU, 0, 1)) {
            hex("link_adr_req ", b, n);
        }
    }
    /* class A windows */
    static const uint8_t sfs[] = {7, 9, 12};
    for (int rx2 = 0; rx2 <= 1; rx2++) {
        for (size_t s = 0; s < 3; s++) {
            lwc_txpk_t t;
            lwc_ns_downlink(rx2 != 0, 1000000U, 868300000U, sfs[s], 125, 1000000U, 869525000U, 0, 14, KEY, 12, &t);
            char tag[32];
            snprintf(tag, sizeof tag, "downlink rx2=%d sf=%u", rx2, sfs[s]);
            txpk(tag, &t);
        }
    }
    /* a join: the join-accept bytes with the default plan's CFList and receive windows */
    lwc_ns_init(&ns);
    lwc_ns_set_plan(&ns, &p);
    size_t idx = 0;
    (void)lwc_ns_add(&ns, 0x0011223344556677ULL, 0x70B3D57ED0000000ULL, KEY, LWC_NS_MAC_1_0_4, "golden", &idx);
    uint8_t jr[23];
    jr[0] = 0x00;
    for (int i = 0; i < 8; i++) {
        jr[1 + i] = (uint8_t)(0x70B3D57ED0000000ULL >> (8 * i));
        jr[9 + i] = (uint8_t)(0x0011223344556677ULL >> (8 * i));
    }
    jr[17] = 0x05;
    jr[18] = 0x00;
    uint8_t mic[16];
    lwc_aes_cmac(KEY, jr, 19, mic);
    memcpy(&jr[19], mic, 4);
    uint8_t acc[64];
    size_t alen = 0;
    const lwc_ns_join_res_t jres = lwc_ns_join(&ns, jr, sizeof jr, acc, sizeof acc, &alen, &idx);
    emit("join -> %d len=%zu addr=%08" PRIX32 " rx1=%u rx2=%" PRIu32 " dr=%u\n", (int)jres, alen, ns.dev[idx].dev_addr,
         ns.dev[idx].rx1_delay_s, ns.dev[idx].rx2_hz, ns.dev[idx].rx2_dr);
    hex("join_accept ", acc, alen);
    /* Basics Station: router_config plan and dnmsg windows */
    static const char dn[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"dC\":0,\"diid\":9,\"pdu\":\"60AB\",\"RxDelay\":1,"
                             "\"RX1DR\":5,\"RX1Freq\":868100000,\"RX2DR\":0,\"RX2Freq\":869525000,\"xtime\":4785074604081153}";
    lwc_lbs_dnmsg_t d;
    if (lwc_lbs_parse_dnmsg(dn, strlen(dn), &d) == LWC_LBS_OK) {
        lwc_txpk_t a, b;
        emit("lbs_dn_txpk -> %d\n", (int)lwc_lbs_dn_txpk(&d, EU, 0x11, &a, &b));
        txpk("lbs_first", &a);
        txpk("lbs_alt", &b);
    }
}

int main(int argc, char **argv)
{
    grid();
    if (argc == 3 && strcmp(argv[1], "--write") == 0) {
        FILE *f = fopen(argv[2], "wb");
        if (f == NULL || fwrite(s_out, 1, s_len, f) != s_len) {
            fprintf(stderr, "cannot write %s\n", argv[2]);
            return 1;
        }
        fclose(f);
        printf("test_golden_eu868: wrote %zu bytes\n", s_len);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: test_golden_eu868 <golden file> | --write <file>\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    char *want = malloc(s_len + 2U);
    const size_t got = fread(want, 1, s_len + 1U, f);
    fclose(f);
    int rc = 0;
    if (got != s_len || memcmp(want, s_out, s_len) != 0) {
        /* the first line that differs */
        size_t line = 1, i = 0;
        while (i < s_len && i < got && want[i] == s_out[i]) {
            line += s_out[i] == '\n' ? 1U : 0U;
            i++;
        }
        fprintf(stderr, "test_golden_eu868: EU868 output changed at line %zu (got %zu bytes, golden %zu)\n", line, s_len,
                got);
        rc = 1;
    } else {
        printf("test_golden_eu868: EU868 unchanged (%zu bytes)\n", s_len);
    }
    free(want);
    free(s_out);
    return rc;
}
