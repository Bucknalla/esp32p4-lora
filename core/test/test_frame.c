#include "lwc/frame.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_unconfirmed_uplink(void)
{
    /* MHDR | DevAddr 0x01020304 | FCtrl ADR | FCnt 42 | FPort 10 | DE AD | MIC */
    const uint8_t f[] = {0x40, 0x04, 0x03, 0x02, 0x01, 0x80, 0x2A, 0x00,
                         0x0A, 0xDE, 0xAD, 0x11, 0x22, 0x33, 0x44};
    lwc_frame_t fr;
    CHECK(lwc_frame_parse(f, sizeof f, &fr) == LWC_FRAME_OK);
    CHECK(fr.mtype == LWC_MTYPE_UNCONFIRMED_UP);
    CHECK(lwc_mtype_is_uplink_data(fr.mtype));
    CHECK(fr.u.data.dev_addr == 0x01020304U);
    CHECK(fr.u.data.fctrl == LWC_FCTRL_ADR);
    CHECK(fr.u.data.fcnt16 == 42U);
    CHECK(fr.u.data.fopts_len == 0U && fr.u.data.fopts == NULL);
    CHECK(fr.u.data.fport == 10);
    CHECK(fr.u.data.payload_len == 2U && fr.u.data.payload == &f[9]);
    CHECK(fr.mic == 0x44332211U);
}

static void test_fopts_without_port(void)
{
    /* Confirmed up, FOptsLen 2 (LinkCheckReq 0x02 + DevStatus ans-ish), no FPort. */
    const uint8_t f[] = {0x80, 0x01, 0x00, 0x00, 0x00, 0x02, 0x05, 0x00,
                         0x02, 0x06, 0xAA, 0xBB, 0xCC, 0xDD};
    lwc_frame_t fr;
    CHECK(lwc_frame_parse(f, sizeof f, &fr) == LWC_FRAME_OK);
    CHECK(fr.mtype == LWC_MTYPE_CONFIRMED_UP);
    CHECK(fr.u.data.fopts_len == 2U && fr.u.data.fopts == &f[8]);
    CHECK(fr.u.data.fport == -1);
    CHECK(fr.u.data.payload == NULL && fr.u.data.payload_len == 0U);
}

static void test_fopts_and_port0_rejected(void)
{
    const uint8_t f[] = {0x40, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
                         0x02, 0x00, 0x01, 0x11, 0x22, 0x33, 0x44};
    lwc_frame_t fr;
    CHECK(lwc_frame_parse(f, sizeof f, &fr) == LWC_FRAME_ERR_FOPTS_PORT0);
}

static void test_fopts_len_overruns_frame(void)
{
    /* FOptsLen 15 but only 1 byte before MIC. */
    const uint8_t f[] = {0x40, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00,
                         0x02, 0x11, 0x22, 0x33, 0x44};
    lwc_frame_t fr;
    CHECK(lwc_frame_parse(f, sizeof f, &fr) == LWC_FRAME_ERR_LENGTH);
}

static void test_join_request(void)
{
    const uint8_t f[] = {0x00,
                         /* JoinEUI 70B3D57ED0000001 (LE) */
                         0x01, 0x00, 0x00, 0xD0, 0x7E, 0xD5, 0xB3, 0x70,
                         /* DevEUI 0004A30B001C0530 (LE) */
                         0x30, 0x05, 0x1C, 0x00, 0x0B, 0xA3, 0x04, 0x00,
                         /* DevNonce 0x1234 */
                         0x34, 0x12,
                         0xDE, 0xAD, 0xBE, 0xEF};
    lwc_frame_t fr;
    CHECK(sizeof f == 23U);
    CHECK(lwc_frame_parse(f, sizeof f, &fr) == LWC_FRAME_OK);
    CHECK(fr.mtype == LWC_MTYPE_JOIN_REQUEST);
    CHECK(fr.u.join.join_eui == 0x70B3D57ED0000001ULL);
    CHECK(fr.u.join.dev_eui == 0x0004A30B001C0530ULL);
    CHECK(fr.u.join.dev_nonce == 0x1234U);
    CHECK(fr.mic == 0xEFBEADDEU);

    CHECK(lwc_frame_parse(f, sizeof f - 1U, &fr) == LWC_FRAME_ERR_LENGTH);
}

static void test_bounds_and_args(void)
{
    uint8_t big[LWC_PHY_MAX_LEN + 1U];
    lwc_frame_t fr;
    memset(big, 0, sizeof big);
    big[0] = 0x40;
    CHECK(lwc_frame_parse(big, sizeof big, &fr) == LWC_FRAME_ERR_LENGTH);
    CHECK(lwc_frame_parse(big, LWC_PHY_MAX_LEN, &fr) == LWC_FRAME_OK);
    CHECK(lwc_frame_parse(big, 4U, &fr) == LWC_FRAME_ERR_LENGTH);
    CHECK(lwc_frame_parse(big, 11U, &fr) == LWC_FRAME_ERR_LENGTH);
    CHECK(lwc_frame_parse(NULL, 12U, &fr) == LWC_FRAME_ERR_ARG);
    CHECK(lwc_frame_parse(big, 12U, NULL) == LWC_FRAME_ERR_ARG);

    big[0] = 0x41; /* Major 1 */
    CHECK(lwc_frame_parse(big, 12U, &fr) == LWC_FRAME_ERR_MAJOR);

    big[0] = 0xE0; /* proprietary: unsupported but still identified */
    CHECK(lwc_frame_parse(big, 12U, &fr) == LWC_FRAME_ERR_UNSUPPORTED);
    CHECK(fr.mtype == LWC_MTYPE_PROPRIETARY && fr.raw == big);
}

int main(void)
{
    test_unconfirmed_uplink();
    test_fopts_without_port();
    test_fopts_and_port0_rejected();
    test_fopts_len_overruns_frame();
    test_join_request();
    test_bounds_and_args();

    if (failures != 0) {
        fprintf(stderr, "test_frame: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_frame: all passed\n");
    return 0;
}
