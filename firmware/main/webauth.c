/* Web UI identity and passwords (ADR-0010). See webauth.h. */
#include "webauth.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"

static const char *TAG = "webauth";

#define NVS_NS "web"
#define KEY_CRT "crt"
#define KEY_KEY "key"
#define SALT_LEN 16
#define HASH_LEN 32
#define PBKDF2_ITER 10000U /* ~P4 time measured at first use; see ADR-0010 */
#define DER_MAX 1024

typedef struct {
    uint32_t iter;
    uint8_t salt[SALT_LEN];
    uint8_t hash[HASH_LEN];
} pw_rec_t;

static uint8_t *s_crt, *s_key;
static size_t s_crt_len, s_key_len;

static const char *pw_key(lwc_role_t role)
{
    return role == LWC_ROLE_ADMIN ? "pw_admin" : role == LWC_ROLE_VIEWER ? "pw_viewer" : NULL;
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- > 0U) {
        *v++ = 0;
    }
}

/* ---- certificate ---------------------------------------------------------- */

static esp_err_t load_blob(nvs_handle_t h, const char *k, uint8_t **out, size_t *len)
{
    size_t n = 0;
    ESP_RETURN_ON_ERROR(nvs_get_blob(h, k, NULL, &n), TAG, "size %s", k);
    uint8_t *b = heap_caps_malloc(n, MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(b != NULL, ESP_ERR_NO_MEM, TAG, "alloc");
    esp_err_t err = nvs_get_blob(h, k, b, &n);
    if (err != ESP_OK) {
        heap_caps_free(b);
        return err;
    }
    *out = b;
    *len = n;
    return ESP_OK;
}

/* mbedtls writes DER at the END of the buffer; move it to the start. */
static esp_err_t der_tail(uint8_t *buf, int ret, uint8_t **out, size_t *len)
{
    if (ret <= 0) {
        return ESP_FAIL;
    }
    uint8_t *b = heap_caps_malloc((size_t)ret, MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(b != NULL, ESP_ERR_NO_MEM, TAG, "alloc");
    memcpy(b, buf + DER_MAX - ret, (size_t)ret);
    *out = b;
    *len = (size_t)ret;
    return ESP_OK;
}

static esp_err_t make_cert(void)
{
    ESP_LOGW(TAG, "no web certificate: creating an ECDSA P-256 key and self-signed certificate");
    const int64_t t0 = esp_timer_get_time();
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t kid = MBEDTLS_SVC_KEY_ID_INIT;
    ESP_RETURN_ON_FALSE(psa_generate_key(&a, &kid) == PSA_SUCCESS, ESP_FAIL, TAG, "keygen");

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    uint8_t *buf = heap_caps_malloc(DER_MAX, MALLOC_CAP_DEFAULT);
    esp_err_t err = buf != NULL ? ESP_OK : ESP_ERR_NO_MEM;

    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_BASE);
    char subject[96];
    snprintf(subject, sizeof subject, "CN=lorawan-gateway-%02x%02x%02x,O=esp32p4-lora", mac[3], mac[4],
             mac[5]);
    uint8_t serial[16];
    esp_fill_random(serial, sizeof serial);
    serial[0] &= 0x7FU; /* positive */

    static const char dns[] = "lorawan-gateway.local";
    mbedtls_x509_san_list san = {0};
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = (unsigned char *)dns;
    san.node.san.unstructured_name.len = sizeof dns - 1U;

    if (err == ESP_OK && mbedtls_pk_copy_from_psa(kid, &pk) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
        if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0 ||
            mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0 ||
            mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial) != 0 ||
            /* No trusted clock at first start: a wide fixed window. */
            mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20991231235959") != 0 ||
            mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0 ||
            mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san) != 0) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        err = der_tail(buf, mbedtls_x509write_crt_der(&crt, buf, DER_MAX), &s_crt, &s_crt_len);
    }
    if (err == ESP_OK) {
        err = der_tail(buf, mbedtls_pk_write_key_der(&pk, buf, DER_MAX), &s_key, &s_key_len);
    }
    if (buf != NULL) {
        wipe(buf, DER_MAX);
        heap_caps_free(buf);
    }
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    (void)psa_destroy_key(kid);
    ESP_RETURN_ON_ERROR(err, TAG, "certificate");

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs");
    err = nvs_set_blob(h, KEY_CRT, s_crt, s_crt_len);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_KEY, s_key, s_key_len);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "certificate created in %" PRId64 " ms (%u + %u bytes DER)",
             (esp_timer_get_time() - t0) / 1000, (unsigned)s_crt_len, (unsigned)s_key_len);
    return err;
}

esp_err_t webauth_cert(const uint8_t **crt, size_t *crt_len, const uint8_t **key, size_t *key_len)
{
    if (s_crt == NULL) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
        if (err == ESP_OK) {
            err = load_blob(h, KEY_CRT, &s_crt, &s_crt_len);
            if (err == ESP_OK) {
                err = load_blob(h, KEY_KEY, &s_key, &s_key_len);
            }
            nvs_close(h);
        }
        if (err != ESP_OK || s_crt == NULL || s_key == NULL) {
            heap_caps_free(s_crt); /* a half-written pair: start again */
            heap_caps_free(s_key);
            s_crt = s_key = NULL;
            ESP_RETURN_ON_ERROR(make_cert(), TAG, "make");
        }
    }
    *crt = s_crt;
    *crt_len = s_crt_len;
    *key = s_key;
    *key_len = s_key_len;
    return ESP_OK;
}

void webauth_fingerprint(char *out, size_t cap)
{
    out[0] = '\0';
    uint8_t d[32];
    size_t n = 0;
    if (s_crt == NULL ||
        psa_hash_compute(PSA_ALG_SHA_256, s_crt, s_crt_len, d, sizeof d, &n) != PSA_SUCCESS) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3U < cap; i++) {
        o += (size_t)snprintf(out + o, cap - o, i ? ":%02X" : "%02X", d[i]);
    }
}

/* ---- passwords -------------------------------------------------------------- */

static bool pbkdf2(const char *pass, size_t len, const uint8_t *salt, uint32_t iter,
                   uint8_t out[HASH_LEN])
{
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    const bool ok =
        psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
        psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iter) == PSA_SUCCESS &&
        psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, SALT_LEN) ==
            PSA_SUCCESS &&
        psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                       (const uint8_t *)pass, len) == PSA_SUCCESS &&
        psa_key_derivation_output_bytes(&op, out, HASH_LEN) == PSA_SUCCESS;
    (void)psa_key_derivation_abort(&op);
    return ok;
}

esp_err_t webauth_set_password(lwc_role_t role, const char *pass, size_t len)
{
    const char *k = pw_key(role);
    ESP_RETURN_ON_FALSE(k != NULL && len >= 8U && len <= 63U, ESP_ERR_INVALID_ARG, TAG, "arg");
    ESP_RETURN_ON_FALSE(psa_crypto_init() == PSA_SUCCESS, ESP_FAIL, TAG, "psa init");
    pw_rec_t r = {.iter = PBKDF2_ITER};
    esp_fill_random(r.salt, sizeof r.salt);
    const int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_FALSE(pbkdf2(pass, len, r.salt, r.iter, r.hash), ESP_FAIL, TAG, "pbkdf2");
    ESP_LOGI(TAG, "PBKDF2 (%" PRIu32 " rounds) took %" PRId64 " ms", r.iter,
             (esp_timer_get_time() - t0) / 1000);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, k, &r, sizeof r);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    wipe(&r, sizeof r);
    return err;
}

static bool load_pw(lwc_role_t role, pw_rec_t *r)
{
    const char *k = pw_key(role);
    nvs_handle_t h;
    if (k == NULL || nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = sizeof *r;
    const bool ok = nvs_get_blob(h, k, r, &n) == ESP_OK && n == sizeof *r && r->iter >= 1000U &&
                    r->iter <= 1000000U;
    nvs_close(h);
    return ok;
}

bool webauth_has_password(lwc_role_t role)
{
    pw_rec_t r;
    const bool ok = load_pw(role, &r);
    wipe(&r, sizeof r);
    return ok;
}

bool webauth_verify(lwc_role_t role, const char *pass, size_t len)
{
    pw_rec_t r;
    uint8_t got[HASH_LEN];
    bool ok = load_pw(role, &r) && len <= 64U && pbkdf2(pass, len, r.salt, r.iter, got);
    if (ok) {
        uint8_t d = 0;
        for (size_t i = 0; i < HASH_LEN; i++) {
            d |= (uint8_t)(got[i] ^ r.hash[i]);
        }
        ok = d == 0U;
    }
    wipe(&r, sizeof r);
    wipe(got, sizeof got);
    return ok;
}
