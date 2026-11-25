/*
 * Payload codecs (ADR-0021). See codecs.h.
 *
 * One task runs all JavaScript, on a stack in PSRAM: it never touches flash
 * (no NVS, no files), so it only pauses while another task writes flash.
 * Callers (the MQTT task for decodes, the web server for uploads) post a
 * request and wait; the codec limits bound the wait, and the supervisor
 * restarts the gateway if the task stops answering.
 *
 * The table of compiled codecs is written only by that task, under s_mx;
 * other tasks read it under s_mx.
 */
#include "codecs.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sv.h"

static const char *TAG = "codec";

#define PART_LABEL "storage"
#define BASE "/storage"
#define DIR_CODECS BASE "/codecs"
#define TASK_STACK (48 * 1024) /* in PSRAM; the engine may use STACK_LIMIT of it */
#define STACK_LIMIT (32 * 1024)
#define TASK_PRIO 2
#define TASK_CORE 0

typedef struct {
    bool used;
    bool ok;
    char name[CODEC_NAME_MAX + 1U];
    uint32_t src_len;
    lwc_codec_bin_t bin;
    char error[LWC_CODEC_ERR_MAX];
} entry_t;

typedef enum { OP_DECODE, OP_ENCODE, OP_INSTALL, OP_DROP } op_t;

typedef struct {
    op_t op;
    const char *name;
    /* install */
    const char *src;
    size_t n;
    bool keep_failed; /* boot: list a codec that does not compile, with its error */
    /* encode */
    const char *object;
    size_t object_len;
    uint8_t *bytes;
    size_t bytes_cap;
    size_t *bytes_len;
    uint8_t *port_out;
    /* decode */
    uint8_t port;
    const uint8_t *data;
    size_t len;
    int64_t recv_ms;
    char *out;
    size_t cap;
    char *warn;
    size_t wcap;
    /* result */
    bool ok;
    char *err;
    SemaphoreHandle_t done;
} req_t;

static entry_t *s_ent; /* [CODECS_MAX], PSRAM */
static SemaphoreHandle_t s_mx;
static QueueHandle_t s_q;
static bool s_storage;
static atomic_uint s_decoded, s_encoded, s_failed;

/* ---- engine environment: PSRAM, limits ----------------------------------------- */

static void *p_malloc(size_t n)
{
    return heap_caps_malloc(n == 0U ? 1U : n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *p_realloc(void *p, size_t n)
{
    if (n == 0U) {
        heap_caps_free(p);
        return NULL;
    }
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void p_free(void *p)
{
    heap_caps_free(p);
}

static size_t p_usable(const void *p)
{
    return heap_caps_get_allocated_size((void *)p);
}

static int64_t p_now(void)
{
    return esp_timer_get_time();
}

static const lwc_codec_env_t ENV = {.malloc_fn = p_malloc,
                                    .realloc_fn = p_realloc,
                                    .free_fn = p_free,
                                    .usable_fn = p_usable,
                                    .now_us = p_now,
                                    .mem_limit = 1024 * 1024,
                                    .stack_limit = STACK_LIMIT,
                                    .call_ms = 200,
                                    .compile_ms = 2000};

/* ---- table ----------------------------------------------------------------------- */

bool codecs_name_ok(const char *s)
{
    if (s == NULL) {
        return false;
    }
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        const char c = s[n];
        if (n >= CODEC_NAME_MAX || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return n > 0U;
}

static entry_t *find(const char *name)
{
    for (size_t i = 0; s_ent != NULL && i < CODECS_MAX; i++) {
        if (s_ent[i].used && strcmp(s_ent[i].name, name) == 0) {
            return &s_ent[i];
        }
    }
    return NULL;
}

static size_t count(void)
{
    size_t n = 0;
    for (size_t i = 0; s_ent != NULL && i < CODECS_MAX; i++) {
        n += s_ent[i].used ? 1U : 0U;
    }
    return n;
}

bool codecs_exists(const char *name)
{
    if (s_mx == NULL || !codecs_name_ok(name)) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    const bool yes = find(name) != NULL;
    xSemaphoreGive(s_mx);
    return yes;
}

/* ---- the codec task --------------------------------------------------------------- */

static void do_install(req_t *r)
{
    static lwc_codec_bin_t bin;
    char e[LWC_CODEC_ERR_MAX] = "";
    const bool ok = lwc_codec_compile(&ENV, r->src, r->n, &bin, e);
    if (!ok && !r->keep_failed) {
        snprintf(r->err, LWC_CODEC_ERR_MAX, "%s", e);
        r->ok = false;
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    entry_t *t = find(r->name);
    for (size_t i = 0; t == NULL && i < CODECS_MAX; i++) {
        if (!s_ent[i].used) {
            t = &s_ent[i];
        }
    }
    if (t == NULL) {
        xSemaphoreGive(s_mx);
        lwc_codec_bin_free(&ENV, &bin);
        snprintf(r->err, LWC_CODEC_ERR_MAX, "no room: %u codecs at most", (unsigned)CODECS_MAX);
        r->ok = false;
        return;
    }
    lwc_codec_bin_free(&ENV, &t->bin);
    memset(t, 0, sizeof *t);
    t->used = true;
    t->ok = ok;
    snprintf(t->name, sizeof t->name, "%s", r->name);
    t->src_len = (uint32_t)r->n;
    t->bin = bin; /* moved */
    memset(&bin, 0, sizeof bin);
    snprintf(t->error, sizeof t->error, "%s", ok ? "" : e);
    xSemaphoreGive(s_mx);
    r->ok = ok;
    if (!ok) {
        snprintf(r->err, LWC_CODEC_ERR_MAX, "%s", e);
    }
}

static void do_drop(req_t *r)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    entry_t *t = find(r->name);
    if (t != NULL) {
        lwc_codec_bin_free(&ENV, &t->bin);
        memset(t, 0, sizeof *t);
    }
    xSemaphoreGive(s_mx);
    r->ok = t != NULL;
}

static void do_decode(req_t *r)
{
    /* only this task changes the table: the entry stays valid during the call */
    const entry_t *t = find(r->name);
    if (t == NULL) {
        snprintf(r->err, LWC_CODEC_ERR_MAX, "no codec named %s", r->name);
        r->ok = false;
    } else if (!t->ok) {
        snprintf(r->err, LWC_CODEC_ERR_MAX, "codec does not compile: %.120s", t->error);
        r->ok = false;
    } else {
        r->ok = lwc_codec_decode(&ENV, &t->bin, r->port, r->data, r->len, r->recv_ms, r->out, r->cap, r->warn,
                                 r->wcap, r->err);
    }
    atomic_fetch_add(r->ok ? &s_decoded : &s_failed, 1U);
}

static void do_encode(req_t *r)
{
    const entry_t *t = find(r->name);
    if (t == NULL) {
        snprintf(r->err, LWC_CODEC_ERR_MAX, "no codec named %s", r->name);
        r->ok = false;
    } else if (!t->ok) {
        snprintf(r->err, LWC_CODEC_ERR_MAX, "codec does not compile: %.120s", t->error);
        r->ok = false;
    } else if (!t->bin.has_encode) {
        snprintf(r->err, LWC_CODEC_ERR_MAX, "codec %s has no encodeDownlink, Encoder or Encode", r->name);
        r->ok = false;
    } else {
        r->ok = lwc_codec_encode(&ENV, &t->bin, r->object, r->object_len, r->port, r->bytes, r->bytes_cap,
                                 r->bytes_len, r->port_out, r->warn, r->wcap, r->err);
    }
    atomic_fetch_add(r->ok ? &s_encoded : &s_failed, 1U);
}

static void codec_task(void *arg)
{
    (void)arg;
    const sv_id_t sv = sv_register("codec", 30000);
    for (;;) {
        sv_beat(sv);
        req_t *r = NULL;
        if (xQueueReceive(s_q, &r, pdMS_TO_TICKS(1000)) != pdTRUE || r == NULL) {
            continue;
        }
        switch (r->op) {
        case OP_DECODE:
            do_decode(r);
            break;
        case OP_ENCODE:
            do_encode(r);
            break;
        case OP_INSTALL:
            do_install(r);
            break;
        case OP_DROP:
            do_drop(r);
            break;
        default:
            r->ok = false;
            break;
        }
        xSemaphoreGive(r->done);
    }
}

/* Post a request and wait for it (the limits bound the wait). */
static bool call(req_t *r)
{
    if (s_q == NULL) {
        snprintf(r->err, LWC_CODEC_ERR_MAX, "codecs are off");
        return false;
    }
    StaticSemaphore_t buf;
    r->done = xSemaphoreCreateBinaryStatic(&buf);
    if (xQueueSend(s_q, &r, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    (void)xSemaphoreTake(r->done, portMAX_DELAY);
    vSemaphoreDelete(r->done);
    return r->ok;
}

bool codecs_decode(const char *name, uint8_t port, const uint8_t *data, size_t len, int64_t recv_unix_ms, char *out,
                   size_t cap, char *warn, size_t wcap, char err[LWC_CODEC_ERR_MAX])
{
    req_t r = {.op = OP_DECODE,
               .name = name,
               .port = port,
               .data = data,
               .len = len,
               .recv_ms = recv_unix_ms,
               .out = out,
               .cap = cap,
               .warn = warn,
               .wcap = wcap,
               .err = err};
    return call(&r);
}

bool codecs_encode(const char *name, const char *object, size_t n, uint8_t port, uint8_t *out, size_t cap, size_t *len,
                   uint8_t *port_out, char *warn, size_t wcap, char err[LWC_CODEC_ERR_MAX])
{
    req_t r = {.op = OP_ENCODE,
               .name = name,
               .object = object,
               .object_len = n,
               .port = port,
               .bytes = out,
               .bytes_cap = cap,
               .bytes_len = len,
               .port_out = port_out,
               .warn = warn,
               .wcap = wcap,
               .err = err};
    return call(&r);
}

/* ---- storage ---------------------------------------------------------------------- */

static void path_of(const char *name, char *out, size_t cap, bool tmp)
{
    (void)snprintf(out, cap, DIR_CODECS "/%s.js%s", name, tmp ? ".tmp" : "");
}

esp_err_t codecs_put(const char *name, const char *src, size_t n, char err[LWC_CODEC_ERR_MAX])
{
    err[0] = '\0';
    if (!codecs_name_ok(name) || src == NULL || n == 0U || n > CODEC_SRC_MAX) {
        snprintf(err, LWC_CODEC_ERR_MAX, "name: 1 to %u of a-z 0-9 _ -; script: 1 B to %u KB", (unsigned)CODEC_NAME_MAX,
                 (unsigned)(CODEC_SRC_MAX / 1024U));
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_storage) {
        snprintf(err, LWC_CODEC_ERR_MAX, "storage is not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    const bool full = find(name) == NULL && count() >= CODECS_MAX;
    xSemaphoreGive(s_mx);
    if (full) {
        snprintf(err, LWC_CODEC_ERR_MAX, "no room: %u codecs at most", (unsigned)CODECS_MAX);
        return ESP_ERR_NO_MEM;
    }
    /* compile first: a script that does not compile is never stored */
    req_t r = {.op = OP_INSTALL, .name = name, .src = src, .n = n, .err = err};
    if (!call(&r)) {
        return ESP_ERR_INVALID_ARG;
    }
    char tmp[64], fin[64];
    path_of(name, tmp, sizeof tmp, true);
    path_of(name, fin, sizeof fin, false);
    FILE *f = fopen(tmp, "wb");
    bool written = f != NULL && fwrite(src, 1, n, f) == n;
    if (f != NULL && fclose(f) != 0) {
        written = false; /* littlefs writes on close */
    }
    /* littlefs renames over an existing file in one step */
    if (!written || rename(tmp, fin) != 0) {
        (void)remove(tmp);
        req_t d = {.op = OP_DROP, .name = name, .err = err};
        (void)call(&d);
        snprintf(err, LWC_CODEC_ERR_MAX, "storing it failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "codec %s stored (%u B)", name, (unsigned)n);
    return ESP_OK;
}

esp_err_t codecs_delete(const char *name)
{
    if (!codecs_exists(name)) {
        return ESP_ERR_NOT_FOUND;
    }
    char p[64], e[LWC_CODEC_ERR_MAX];
    path_of(name, p, sizeof p, false);
    (void)remove(p);
    req_t r = {.op = OP_DROP, .name = name, .err = e};
    (void)call(&r);
    ESP_LOGI(TAG, "codec %s deleted", name);
    return ESP_OK;
}

esp_err_t codecs_source(const char *name, char **buf, size_t *len)
{
    *buf = NULL;
    *len = 0;
    if (!codecs_exists(name)) {
        return ESP_ERR_NOT_FOUND;
    }
    char p[64];
    path_of(name, p, sizeof p, false);
    FILE *f = fopen(p, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    char *b = heap_caps_malloc(CODEC_SRC_MAX + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t n = b != NULL ? fread(b, 1, CODEC_SRC_MAX, f) : 0U;
    fclose(f);
    if (b == NULL) {
        return ESP_ERR_NO_MEM;
    }
    b[n] = '\0';
    *buf = b;
    *len = n;
    return ESP_OK;
}

static void put_json_str(char *out, size_t cap, size_t *o, const char *s)
{
    if (*o < cap) {
        out[(*o)++] = '"';
    }
    for (; *s != '\0' && *o + 7U < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[(*o)++] = '\\';
            out[(*o)++] = (char)c;
        } else if (c < 0x20U) {
            *o += (size_t)snprintf(out + *o, cap - *o, "\\u%04x", c);
        } else {
            out[(*o)++] = (char)c;
        }
    }
    if (*o < cap) {
        out[(*o)++] = '"';
    }
}

size_t codecs_json(char *out, size_t cap)
{
    if (cap < 3U) {
        return 0;
    }
    size_t o = 0;
    out[o++] = '[';
    if (s_mx != NULL) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        bool first = true;
        for (size_t i = 0; i < CODECS_MAX && o + 400U < cap; i++) {
            const entry_t *t = &s_ent[i];
            if (!t->used) {
                continue;
            }
            o += (size_t)snprintf(out + o, cap - o, "%s{\"name\":\"%s\",\"size\":%u,\"ok\":%s,\"encode\":%s,\"error\":",
                                  first ? "" : ",", t->name, (unsigned)t->src_len, t->ok ? "true" : "false",
                                  t->bin.has_encode ? "true" : "false");
            put_json_str(out, cap, &o, t->error);
            if (o + 1U < cap) {
                out[o++] = '}';
            }
            first = false;
        }
        xSemaphoreGive(s_mx);
    }
    if (o + 1U < cap) {
        out[o++] = ']';
    }
    out[o < cap ? o : cap - 1U] = '\0';
    return o;
}

void codecs_status_line(char *buf, size_t len)
{
    size_t n = 0;
    if (s_mx != NULL) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        n = count();
        xSemaphoreGive(s_mx);
    }
    snprintf(buf, len, "codecs=%u storage=%s decoded=%u encoded=%u failed=%u", (unsigned)n, s_storage ? "on" : "off",
             atomic_load(&s_decoded), atomic_load(&s_encoded), atomic_load(&s_failed));
}

/* A partition never used holds only 0xFF: only then may it be formatted. */
static bool blank(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    if (p == NULL) {
        return false;
    }
    static uint8_t b[512];
    for (size_t off = 0; off < 16U * 1024U; off += sizeof b) {
        if (esp_partition_read(p, off, b, sizeof b) != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < sizeof b; i++) {
            if (b[i] != 0xFFU) {
                return false;
            }
        }
    }
    return true;
}

static bool mount(void)
{
    const esp_vfs_littlefs_conf_t c = {.base_path = BASE, .partition_label = PART_LABEL};
    esp_err_t err = esp_vfs_littlefs_register(&c);
    if (err != ESP_OK && blank()) {
        ESP_LOGW(TAG, "storage partition is blank: formatting it (littlefs)");
        err = esp_littlefs_format(PART_LABEL);
        if (err == ESP_OK) {
            err = esp_vfs_littlefs_register(&c);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage not mounted (%s): codecs off; the partition is left as it is", esp_err_to_name(err));
        return false;
    }
    struct stat st;
    if (stat(DIR_CODECS, &st) != 0 && mkdir(DIR_CODECS, 0775) != 0) {
        ESP_LOGE(TAG, "cannot create " DIR_CODECS);
        return false;
    }
    size_t total = 0, used = 0;
    (void)esp_littlefs_info(PART_LABEL, &total, &used);
    ESP_LOGI(TAG, "storage mounted: %u of %u KB used", (unsigned)(used / 1024U), (unsigned)(total / 1024U));
    return true;
}

/* Compile every stored codec (one that no longer compiles is listed with its error). */
static void load_all(void)
{
    DIR *d = opendir(DIR_CODECS);
    if (d == NULL) {
        return;
    }
    char *buf = heap_caps_malloc(CODEC_SRC_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    struct dirent *e;
    unsigned n = 0, bad = 0;
    while (buf != NULL && (e = readdir(d)) != NULL) {
        char name[CODEC_NAME_MAX + 1U];
        const size_t l = strlen(e->d_name);
        if (l <= 3U || l - 3U > CODEC_NAME_MAX || strcmp(e->d_name + l - 3U, ".js") != 0) {
            continue;
        }
        memcpy(name, e->d_name, l - 3U);
        name[l - 3U] = '\0';
        if (!codecs_name_ok(name)) {
            continue;
        }
        char p[64], err[LWC_CODEC_ERR_MAX];
        path_of(name, p, sizeof p, false);
        FILE *f = fopen(p, "rb");
        const size_t got = f != NULL ? fread(buf, 1, CODEC_SRC_MAX, f) : 0U;
        if (f != NULL) {
            fclose(f);
        }
        req_t r = {.op = OP_INSTALL, .name = name, .src = buf, .n = got, .keep_failed = true, .err = err};
        if (got == 0U || !call(&r)) {
            bad++;
            ESP_LOGW(TAG, "codec %s does not compile: %s", name, got == 0U ? "empty" : err);
        }
        n++;
    }
    closedir(d);
    heap_caps_free(buf);
    ESP_LOGI(TAG, "%u codec(s) loaded, %u with errors", n, bad);
}

esp_err_t codecs_start(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_ent = heap_caps_calloc(CODECS_MAX, sizeof(entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_q = xQueueCreate(4, sizeof(req_t *));
    ESP_RETURN_ON_FALSE(s_mx != NULL && s_ent != NULL && s_q != NULL, ESP_ERR_NO_MEM, TAG, "alloc");
    const BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(codec_task, "codec", TASK_STACK, NULL, TASK_PRIO, NULL,
                                                          TASK_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        ESP_LOGE(TAG, "no codec task: codecs off");
        return ESP_ERR_NO_MEM;
    }
    s_storage = mount();
    if (s_storage) {
        load_all();
    }
    return ESP_OK;
}
