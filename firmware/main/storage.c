#include "storage.h"
#include "block_cache.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char version[65];
    uint64_t size;
    uint32_t refresh;
} image_t;
typedef enum { JOB_MANIFEST, JOB_BLOCK } job_kind_t;
typedef struct {
    job_kind_t kind;
    image_t image;
    uint64_t base;
    uint32_t length;
    bool speculative;
} job_t;
typedef struct {
    job_t job;
    image_t manifest;
    bool ok;
    int64_t elapsed_us;
} result_t;
typedef struct {
    uint8_t *destination;
    size_t capacity, used;
    bool overflow;
    char version[65], offset[32];
} response_t;
typedef struct {
    bool valid;
    uint64_t address;
    uint32_t length;
    void *buffer;
    int64_t started_us;
} pending_t;

static const char *TAG = "虚拟磁盘";
static block_cache_t cache;
static QueueHandle_t jobs, results;
/* 网络任务只写中转区；结果被 USB 任务消费后才提交下一项任务。 */
static uint8_t *network_buffer;
static bool network_busy, ready, refresh_wait, disconnected;
static image_t active, latest;
static pending_t pending;
static uint64_t previous_end, prefetch_next, prefetch_limit;
static unsigned sequential;
static int64_t last_read_us, next_manifest_us, reconnect_us, next_log_us;
static uint64_t net_requests, net_wait_us, read_bytes, network_errors;

static bool version_valid(const char *s) {
    if (!s || strlen(s) != 64) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
static bool json_integer(cJSON *root, const char *name, double min, double max, double *out) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < min || value->valuedouble > max || floor(value->valuedouble) != value->valuedouble) return false;
    *out = value->valuedouble;
    return true;
}
static bool parse_manifest(char *text, image_t *image) {
    cJSON *root = cJSON_Parse(text);
    if (!root) return false;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    double size = 0, refresh = 0, sector = 0, block = 0;
    bool ok = cJSON_IsString(version) && version_valid(version->valuestring) &&
        json_integer(root, "size", 512, (double)UINT32_MAX * 512, &size) &&
        json_integer(root, "refresh", 0, UINT32_MAX, &refresh) &&
        json_integer(root, "sector_size", 512, 512, &sector) &&
        json_integer(root, "block_size", W2B_BLOCK_SIZE, W2B_BLOCK_SIZE, &block) &&
        (uint64_t)size % 512 == 0;
    if (ok) {
        snprintf(image->version, sizeof(image->version), "%s", version->valuestring);
        image->size = (uint64_t)size;
        image->refresh = (uint32_t)refresh;
    }
    cJSON_Delete(root);
    return ok;
}
static esp_err_t http_event(esp_http_client_event_t *event) {
    response_t *r = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        if (!strcasecmp(event->header_key, "X-Image-Version"))
            snprintf(r->version, sizeof(r->version), "%s", event->header_value);
        if (!strcasecmp(event->header_key, "X-Offset"))
            snprintf(r->offset, sizeof(r->offset), "%s", event->header_value);
    } else if (event->event_id == HTTP_EVENT_ON_DATA) {
        if ((size_t)event->data_len > r->capacity - r->used) r->overflow = true;
        else {
            memcpy(r->destination + r->used, event->data, event->data_len);
            r->used += event->data_len;
        }
    }
    return ESP_OK;
}
static void network_task(void *argument) {
    (void)argument;
    response_t response = {0};
    esp_http_client_config_t config = {
        .url = CONFIG_W2B_BACKEND_URL,
        .event_handler = http_event,
        .user_data = &response,
        .timeout_ms = CONFIG_W2B_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    configASSERT(client);
    char authorization[256];
    int count = snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_W2B_TOKEN);
    configASSERT(count > 0 && count < sizeof(authorization));
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Authorization", authorization));
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Accept-Encoding", "identity"));
    job_t job;
    for (;;) {
        xQueueReceive(jobs, &job, portMAX_DELAY);
        result_t result = {.job = job};
        char url[512], manifest[1024];
        response = (response_t){0};
        if (job.kind == JOB_MANIFEST) {
            count = snprintf(url, sizeof(url), "%s/api/manifest", CONFIG_W2B_BACKEND_URL);
            response.destination = (uint8_t *)manifest;
            response.capacity = sizeof(manifest) - 1;
        } else {
            count = snprintf(url, sizeof(url), "%s/api/block?version=%s&offset=%" PRIu64 "&length=%" PRIu32,
                CONFIG_W2B_BACKEND_URL, job.image.version, job.base, job.length);
            response.destination = network_buffer;
            response.capacity = job.length;
        }
        int64_t start = esp_timer_get_time();
        bool ok = count > 0 && count < sizeof(url) && esp_http_client_set_url(client, url) == ESP_OK;
        if (ok) ok = esp_http_client_perform(client) == ESP_OK &&
            esp_http_client_get_status_code(client) == 200 && !response.overflow &&
            esp_http_client_get_content_length(client) == response.used;
        if (ok && job.kind == JOB_MANIFEST) {
            manifest[response.used] = '\0';
            ok = parse_manifest(manifest, &result.manifest);
        } else if (ok) {
            char offset[32];
            snprintf(offset, sizeof(offset), "%" PRIu64, job.base);
            ok = response.used == job.length && !strcmp(response.version, job.image.version) &&
                !strcmp(response.offset, offset);
        }
        result.ok = ok;
        result.elapsed_us = esp_timer_get_time() - start;
        if (!ok) esp_http_client_close(client);
        xQueueSend(results, &result, portMAX_DELAY);
    }
}
static void submit(job_t job) {
    configASSERT(!network_busy);
    ESP_ERROR_CHECK(xQueueSend(jobs, &job, 0) == pdTRUE ? ESP_OK : ESP_FAIL);
    network_busy = true;
}
static void request_block(uint64_t base, bool speculative) {
    uint64_t remaining = active.size - base;
    submit((job_t){.kind = JOB_BLOCK, .image = active, .base = base,
        .length = remaining < W2B_BLOCK_SIZE ? (uint32_t)remaining : W2B_BLOCK_SIZE,
        .speculative = speculative});
}
static void note_read(uint64_t address, uint32_t length) {
    last_read_us = esp_timer_get_time();
    read_bytes += length;
    if (address == previous_end) ++sequential;
    else {
        sequential = 0;
        prefetch_next = prefetch_limit = 0;
    }
    previous_end = address + length;
    if (sequential >= 2) {
        prefetch_next = (previous_end + W2B_BLOCK_SIZE - 1) / W2B_BLOCK_SIZE * W2B_BLOCK_SIZE;
        prefetch_limit = prefetch_next + CONFIG_W2B_PREFETCH_BLOCKS * W2B_BLOCK_SIZE;
        if (prefetch_limit > active.size) prefetch_limit = active.size;
    }
}
void w2b_msc_cancel(void) {
    /* 总线重置/BOT 重置发生在 USB 任务；网络任务从不持有 USB 缓冲区。 */
    pending.valid = false;
    sequential = 0;
    previous_end = UINT64_MAX;
    prefetch_next = prefetch_limit = 0;
}
void storage_init(void) {
    size_t count = CONFIG_W2B_CACHE_BLOCKS;
    uint8_t *bytes = heap_caps_malloc(count * W2B_BLOCK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bytes) {
        if (count > 4) count = 4;
        bytes = heap_caps_malloc(count * W2B_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGW(TAG, "PSRAM不可用，改用 %u KiB 内部缓存", (unsigned)(count * 16));
    }
    cache_line_t *lines = calloc(count, sizeof(*lines));
    network_buffer = heap_caps_malloc(W2B_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    configASSERT(bytes && lines && network_buffer);
    cache_init(&cache, lines, bytes, count);
    jobs = xQueueCreate(1, sizeof(job_t));
    results = xQueueCreate(1, sizeof(result_t));
    configASSERT(jobs && results);
    ESP_ERROR_CHECK(xTaskCreate(network_task, "磁盘网络", 6144, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    previous_end = UINT64_MAX;
}
bool storage_ready(void) { return ready; }

void storage_tick(void) {
    int64_t now = esp_timer_get_time();
    result_t result;
    if (xQueueReceive(results, &result, 0) == pdTRUE) {
        network_busy = false;
        ++net_requests;
        net_wait_us += result.elapsed_us;
        if (!result.ok) ++network_errors;
        if (result.job.kind == JOB_MANIFEST) {
            next_manifest_us = now + 2000000;
            if (result.ok) {
                latest = result.manifest;
                if (!ready && !disconnected) {
                    active = latest;
                    ready = true;
                    ESP_LOGI(TAG, "磁盘已就绪，大小=%" PRIu64 " 字节", active.size);
                } else if (latest.refresh != active.refresh) refresh_wait = true;
            }
        } else if (ready && !strcmp(result.job.image.version, active.version)) {
            if (result.ok) cache_insert(&cache, result.job.base, network_buffer,
                result.job.length, result.job.speculative);
            else if (pending.valid && pending.address / W2B_BLOCK_SIZE * W2B_BLOCK_SIZE == result.job.base) {
                pending.valid = false;
                tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
            }
        }
    }
    if (pending.valid) {
        if (cache_read(&cache, pending.address, pending.buffer, pending.length)) {
            uint32_t length = pending.length;
            pending.valid = false;
            tud_msc_async_io_done((int32_t)length, false);
        } else if (now - pending.started_us > 2LL * CONFIG_W2B_HTTP_TIMEOUT_MS * 1000) {
            pending.valid = false;
            tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
        }
    }
    if (refresh_wait && !pending.valid && !network_busy && !disconnected &&
        now - last_read_us >= (int64_t)CONFIG_W2B_REFRESH_IDLE_MS * 1000) {
        ESP_LOGW(TAG, "准备切换镜像，USB键鼠与U盘将短暂重连");
        tud_disconnect();
        ready = false;
        disconnected = true;
        w2b_msc_cancel();
        reconnect_us = now + 1000000;
    }
    if (disconnected && now >= reconnect_us) {
        active = latest;
        cache_clear(&cache);
        ready = true;
        disconnected = refresh_wait = false;
        tud_connect();
    }
    if (!network_busy && !disconnected) {
        if (pending.valid) request_block(pending.address / W2B_BLOCK_SIZE * W2B_BLOCK_SIZE, false);
        else if (now >= next_manifest_us) submit((job_t){.kind = JOB_MANIFEST});
        else if (ready && !refresh_wait && prefetch_next < prefetch_limit && prefetch_next < active.size) {
            while (prefetch_next < prefetch_limit && cache_contains(&cache, prefetch_next)) prefetch_next += W2B_BLOCK_SIZE;
            if (prefetch_next < prefetch_limit && prefetch_next < active.size) {
                request_block(prefetch_next, true);
                prefetch_next += W2B_BLOCK_SIZE;
            }
        }
    }
    if (now >= next_log_us) {
        next_log_us = now + 10000000;
        ESP_LOGI(TAG, "读取=%" PRIu64 " 缓存命中=%" PRIu64 " 未命中探测=%" PRIu64
            " 网络请求=%" PRIu64 " 网络耗时毫秒=%" PRIu64 " 失败=%" PRIu64 " 未使用预读淘汰=%" PRIu64,
            read_bytes, cache.hits, cache.misses, net_requests, net_wait_us / 1000, network_errors, cache.unused_evictions);
    }
}
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor[8], uint8_t product[16], uint8_t revision[4]) {
    (void)lun;
    memcpy(vendor, "W2B     ", 8);
    memcpy(product, "WiFi Boot Disk  ", 16);
    memcpy(revision, "0100", 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!ready) tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0);
    return ready;
}
void tud_msc_capacity_cb(uint8_t lun, uint32_t *count, uint16_t *size) {
    (void)lun;
    *size = 512;
    *count = ready ? (uint32_t)(active.size / 512) : 0;
}
bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return false; }
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power, bool start, bool eject) {
    (void)lun; (void)power; (void)start; (void)eject;
    return true;
}
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
    uint64_t address = (uint64_t)lba * 512 + offset;
    if (!ready || address >= active.size || size > active.size - address) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
        return TUD_MSC_RET_ERROR;
    }
    uint32_t available = W2B_BLOCK_SIZE - address % W2B_BLOCK_SIZE;
    if (size > available) size = available;
    note_read(address, size);
    if (cache_read(&cache, address, buffer, size)) return (int32_t)size;
    configASSERT(!pending.valid);
    pending = (pending_t){.valid = true, .address = address, .buffer = buffer,
        .length = size, .started_us = esp_timer_get_time()};
    return TUD_MSC_RET_ASYNC;
}
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size) {
    (void)lba; (void)offset; (void)buffer; (void)size;
    tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0);
    return TUD_MSC_RET_ERROR;
}
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const command[16], void *buffer, uint16_t size) {
    (void)buffer; (void)size;
    if (command[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL || command[0] == 0x35 /* SYNCHRONIZE CACHE(10) */) return 0;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
    return TUD_MSC_RET_ERROR;
}
