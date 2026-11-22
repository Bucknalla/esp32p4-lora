/*
 * ESP32-C6 co-processor update over ESP-Hosted (begin / write / end /
 * activate RPCs), image read from the `cp_fw` partition. Based on ESP-Hosted
 * 3.1.0 examples/ota/coprocessor_ota (partition method).
 */
#include "c6_update.h"

#include <inttypes.h>
#include <string.h>

#include "eh_host_api_types.h"
#include "eh_host_cp_ota.h"
#include "eh_common_caps.h"
#include "eh_host_event.h"
#include "eh_host_mcu_transport_init_event.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "c6_update";

#define CP_FW_LABEL "cp_fw"
#define C6_UPDATE_MAX_TRIES 3
#define NVS_NS "c6upd"
#define NVS_KEY_TRIES "tries"
#define VERSION_WAIT_MS 15000
/* Below EH_HOST_CP_OTA_CHUNK_MAX (SDIO buffer minus headers), and small
 * enough for older C6 firmware's RPC buffers. */
#define CHUNK_SIZE 1024U
_Static_assert(CHUNK_SIZE <= EH_HOST_CP_OTA_CHUNK_MAX, "OTA chunk larger than the RPC limit");
/* Must outlast the C6's activate-restart timeout plus its boot (~600 ms
 * measured upstream). Resetting it earlier lets its bootloader roll back. */
#define RESTART_DELAY_MS 2000

static uint8_t s_chunk[CHUNK_SIZE];

static SemaphoreHandle_t s_cp_init;

static void on_cp_init(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    xSemaphoreGive(s_cp_init);
}

/* The C6's init event (sent at link-up) carries its chip ID and ESP-Hosted
 * version, and RPCs work only after it. Old firmware sends version 0 and does
 * not answer the version RPC, so read the version from the init event. */
static esp_err_t get_cp_version(eh_host_coprocessor_fwver_t *v)
{
    s_cp_init = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_cp_init != NULL, ESP_ERR_NO_MEM, TAG, "semaphore");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(EH_HOST_EVENT, EH_HOST_EVENT_CP_INIT,
                                                   on_cp_init, NULL),
                        TAG, "register CP_INIT");
    /* Registered first, then checked: if the chip ID is already known, the
     * event fired before we listened. */
    esp_err_t err = ESP_OK;
    if (eh_host_mcu_transport_get_chip_id() == EH_PRIV_FIRMWARE_CHIP_UNRECOGNIZED &&
        xSemaphoreTake(s_cp_init, pdMS_TO_TICKS(VERSION_WAIT_MS)) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
    }
    (void)esp_event_handler_unregister(EH_HOST_EVENT, EH_HOST_EVENT_CP_INIT, on_cp_init);
    vSemaphoreDelete(s_cp_init);
    s_cp_init = NULL;
    ESP_RETURN_ON_ERROR(err, TAG, "no init event from the C6");

    const uint8_t chip = eh_host_mcu_transport_get_chip_id();
    ESP_RETURN_ON_FALSE(chip == EH_PRIV_FIRMWARE_CHIP_ESP32C6, ESP_ERR_INVALID_RESPONSE, TAG,
                        "co-processor chip 0x%02x is not an ESP32-C6", chip);
    const uint32_t ver = eh_host_mcu_transport_get_fw_version();
    memset(v, 0, sizeof *v);
    v->major1 = EH_VERSION_MAJOR(ver);
    v->minor1 = EH_VERSION_MINOR(ver);
    v->patch1 = EH_VERSION_PATCH(ver);
    return ESP_OK;
}

static uint8_t tries_get(void)
{
    nvs_handle_t h;
    uint8_t n = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, NVS_KEY_TRIES, &n);
        nvs_close(h);
    }
    return n;
}

static void tries_set(uint8_t n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_TRIES, n);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "attempt counter: %s", esp_err_to_name(err));
    }
}

/* Size of an ESP application image: header, segments, padding, checksum,
 * optional SHA-256. The image is for the C6, so esp_image_verify() (which
 * checks the chip ID against the P4) cannot be used. */
static esp_err_t image_size(const esp_partition_t *p, size_t *size, char *ver, size_t ver_len)
{
    esp_image_header_t hdr;
    ESP_RETURN_ON_ERROR(esp_partition_read(p, 0, &hdr, sizeof hdr), TAG, "read header");
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGW(TAG, "'%s' holds no image (magic 0x%02x)", p->label, hdr.magic);
        return ESP_ERR_NOT_FOUND;
    }
    if (hdr.chip_id != ESP_CHIP_ID_ESP32C6) {
        ESP_LOGE(TAG, "'%s' image is for chip ID %u, not the ESP32-C6", p->label,
                 (unsigned)hdr.chip_id);
        return ESP_ERR_INVALID_ARG;
    }

    size_t off = sizeof hdr;
    for (uint8_t i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        ESP_RETURN_ON_ERROR(esp_partition_read(p, off, &seg, sizeof seg), TAG, "read segment");
        if (i == 0) {
            esp_app_desc_t desc;
            if (esp_partition_read(p, off + sizeof seg, &desc, sizeof desc) == ESP_OK &&
                desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
                strlcpy(ver, desc.version, ver_len);
            } else {
                strlcpy(ver, "unknown", ver_len);
            }
        }
        off += sizeof seg + seg.data_len;
        ESP_RETURN_ON_FALSE(off <= p->size, ESP_ERR_INVALID_SIZE, TAG, "segment past end");
    }
    off += (16U - (off % 16U)) % 16U; /* pad, then the checksum byte ends at a 16 B boundary */
    off += 1U;
    if (hdr.hash_appended == 1) {
        off += (16U - (off % 16U)) % 16U;
        off += 32U;
    }
    ESP_RETURN_ON_FALSE(off <= p->size, ESP_ERR_INVALID_SIZE, TAG, "image larger than partition");
    *size = off;
    return ESP_OK;
}

static esp_err_t push_image(const esp_partition_t *p, size_t size)
{
    esp_err_t begin = eh_host_cp_ota_begin();
    if (begin != ESP_OK) {
        ESP_LOGE(TAG, "ota begin rejected by the C6: %s", esp_err_to_name(begin));
        return begin;
    }
    ESP_LOGI(TAG, "ota begin accepted");
    size_t off = 0;
    unsigned last_pct = 0;
    while (off < size) {
        const size_t n = (size - off > CHUNK_SIZE) ? CHUNK_SIZE : size - off;
        esp_err_t err = esp_partition_read(p, off, s_chunk, n);
        if (err == ESP_OK) {
            err = eh_host_cp_ota_write(s_chunk, (uint32_t)n);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write at %u/%u: %s", (unsigned)off, (unsigned)size, esp_err_to_name(err));
            (void)eh_host_cp_ota_end();
            return err;
        }
        off += n;
        const unsigned pct = (unsigned)(off * 100U / size);
        if (pct >= last_pct + 10U) {
            ESP_LOGI(TAG, "%u%%", pct);
            last_pct = pct;
        }
    }
    return eh_host_cp_ota_end();
}

esp_err_t c6_update_check(void)
{
    eh_host_coprocessor_fwver_t cp = {0};
    esp_err_t err = get_cp_version(&cp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 version not available: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ESP-Hosted host %d.%d.%d, C6 %" PRIu32 ".%" PRIu32 ".%" PRIu32,
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1,
             cp.major1, cp.minor1, cp.patch1);

    if (cp.major1 == (uint32_t)ESP_HOSTED_VERSION_MAJOR_1 &&
        cp.minor1 == (uint32_t)ESP_HOSTED_VERSION_MINOR_1) {
        if (tries_get() != 0) {
            tries_set(0);
            ESP_LOGI(TAG, "C6 update confirmed");
        }
        return ESP_OK;
    }

    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CP_FW_LABEL);
    if (p == NULL) {
        ESP_LOGW(TAG, "no '%s' partition: C6 stays on its current firmware", CP_FW_LABEL);
        return ESP_OK;
    }
    size_t size = 0;
    char new_ver[32];
    err = image_size(p, &size, new_ver, sizeof new_ver);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C6 version mismatch, but no usable image in '%s'", CP_FW_LABEL);
        return ESP_OK;
    }

    const uint8_t tries = tries_get();
    if (tries >= C6_UPDATE_MAX_TRIES) {
        ESP_LOGE(TAG, "C6 update failed %u times: not trying again (erase NVS '%s' to retry)",
                 (unsigned)tries, NVS_NS);
        return ESP_FAIL;
    }
    tries_set((uint8_t)(tries + 1U));

    ESP_LOGW(TAG, "updating C6 to image %s (%u bytes), attempt %u of %u", new_ver,
             (unsigned)size, (unsigned)tries + 1U, (unsigned)C6_UPDATE_MAX_TRIES);
    err = push_image(p, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 update failed: %s. The C6 keeps its old firmware.", esp_err_to_name(err));
        return err;
    }

    /* The activate RPC exists from C6 firmware v2.6.0; older firmware
     * switches to the new image at ota_end. */
    if (cp.major1 > 2U || (cp.major1 == 2U && cp.minor1 > 5U)) {
        err = eh_host_cp_ota_activate();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "activate: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGW(TAG, "image sent; restarting to resync with the C6");
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
    return ESP_OK; /* not reached */
}
