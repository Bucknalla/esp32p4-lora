#include "lwc/frame.h"

#include <string.h>

#define MHDR_LEN 1U
#define FHDR_MIN_LEN 7U /* DevAddr(4) + FCtrl(1) + FCnt(2) */
#define DATA_MIN_LEN (MHDR_LEN + FHDR_MIN_LEN + LWC_MIC_LEN)
#define JOIN_REQUEST_LEN (MHDR_LEN + 8U + 8U + 2U + LWC_MIC_LEN)
#define MAJOR_LORAWAN_R1 0U

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p)
{
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

int lwc_mtype_is_uplink_data(lwc_mtype_t mtype)
{
    return (mtype == LWC_MTYPE_UNCONFIRMED_UP) || (mtype == LWC_MTYPE_CONFIRMED_UP);
}

static lwc_frame_err_t parse_join_request(const uint8_t *buf, size_t len, lwc_frame_t *out)
{
    if (len != JOIN_REQUEST_LEN) {
        return LWC_FRAME_ERR_LENGTH;
    }
    out->u.join.join_eui = rd_le64(&buf[1]);
    out->u.join.dev_eui = rd_le64(&buf[9]);
    out->u.join.dev_nonce = rd_le16(&buf[17]);
    return LWC_FRAME_OK;
}

static lwc_frame_err_t parse_data(const uint8_t *buf, size_t len, lwc_frame_t *out)
{
    lwc_mac_payload_t *d = &out->u.data;

    if (len < DATA_MIN_LEN) {
        return LWC_FRAME_ERR_LENGTH;
    }

    /* MACPayload spans buf[1 .. len - MIC - 1]. */
    const size_t mac_end = len - LWC_MIC_LEN;
    size_t pos = MHDR_LEN;

    d->dev_addr = rd_le32(&buf[pos]);
    pos += 4U;
    d->fctrl = buf[pos];
    pos += 1U;
    d->fcnt16 = rd_le16(&buf[pos]);
    pos += 2U;

    d->fopts_len = (uint8_t)(d->fctrl & LWC_FCTRL_FOPTS_LEN_MASK);
    if ((size_t)d->fopts_len > (mac_end - pos)) {
        return LWC_FRAME_ERR_LENGTH;
    }
    d->fopts = (d->fopts_len > 0U) ? &buf[pos] : NULL;
    pos += d->fopts_len;

    d->fport = -1;
    d->payload = NULL;
    d->payload_len = 0U;

    if (pos < mac_end) {
        d->fport = buf[pos];
        pos += 1U;
        if ((d->fport == 0) && (d->fopts_len > 0U)) {
            return LWC_FRAME_ERR_FOPTS_PORT0;
        }
        if (pos < mac_end) {
            d->payload = &buf[pos];
            d->payload_len = (uint8_t)(mac_end - pos);
        }
    }
    return LWC_FRAME_OK;
}

lwc_frame_err_t lwc_frame_parse(const uint8_t *buf, size_t len, lwc_frame_t *out)
{
    if ((buf == NULL) || (out == NULL)) {
        return LWC_FRAME_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));

    if ((len < (MHDR_LEN + LWC_MIC_LEN)) || (len > LWC_PHY_MAX_LEN)) {
        return LWC_FRAME_ERR_LENGTH;
    }

    out->raw = buf;
    out->raw_len = len;
    out->mhdr = buf[0];
    out->mtype = (lwc_mtype_t)(buf[0] >> 5);

    if ((buf[0] & 0x03U) != MAJOR_LORAWAN_R1) {
        return LWC_FRAME_ERR_MAJOR;
    }

    out->mic = rd_le32(&buf[len - LWC_MIC_LEN]);

    switch (out->mtype) {
    case LWC_MTYPE_JOIN_REQUEST:
        return parse_join_request(buf, len, out);
    case LWC_MTYPE_UNCONFIRMED_UP:
    case LWC_MTYPE_UNCONFIRMED_DOWN:
    case LWC_MTYPE_CONFIRMED_UP:
    case LWC_MTYPE_CONFIRMED_DOWN:
        return parse_data(buf, len, out);
    case LWC_MTYPE_JOIN_ACCEPT:
    case LWC_MTYPE_REJOIN_REQUEST:
    case LWC_MTYPE_PROPRIETARY:
    default:
        return LWC_FRAME_ERR_UNSUPPORTED;
    }
}
