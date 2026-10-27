#include "storage.h"
#include "block_cache.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
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

typedef struct { char version[65]; uint64_t size; uint16_t sector; bool present; } image_t;
typedef struct { image_t media[2]; uint32_t refresh; bool host; } manifest_t;
typedef enum { MANIFEST, CLAIM, RELEASE, READ, WRITE } operation_t;
typedef struct {
    operation_t op;
    image_t image;
    uint8_t lun;
    uint64_t address, ticket;
    uint32_t length;
    bool speculative;
} job_t;
typedef struct { job_t job; manifest_t manifest; bool ok; int64_t elapsed; } result_t;
typedef struct {
    uint8_t *bytes;
    size_t capacity, used;
    bool overflow;
    char version[65], offset[32];
} response_t;
typedef struct {
    job_t job;
    void *buffer;
    bool valid, submitted;
    int64_t started;
} pending_t;

static const char *TAG = "双逻辑盘";
static block_cache_t cache;
static QueueHandle_t jobs, results;
static uint8_t *network_buffer;
static manifest_t active, latest;
static pending_t pending;
static bool initialized, network_busy, claimed, switching, switch_requested, usb_started, force_release;
static uint64_t serial, previous_end = UINT64_MAX, prefetch_next, prefetch_limit;
static uint8_t previous_lun, prefetch_lun;
static unsigned sequential;
static int64_t last_io, next_manifest, next_control, reconnect_at, next_log;
static uint64_t net_requests, net_elapsed, net_errors, read_bytes, written_bytes;

static uint64_t key(uint8_t lun, uint64_t address) { return ((uint64_t)lun << 48) + address; }
static bool hex_id(const char *text) {
    if (!text || strlen(text) != 64) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}
static bool number(cJSON *root, const char *name, double min, double max, double *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < min ||
        item->valuedouble > max || floor(item->valuedouble) != item->valuedouble) return false;
    *out = item->valuedouble;
    return true;
}
static bool parse_image(cJSON *root, image_t *image, uint16_t sector) {
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    double size = 0, block = 0;
    if (!cJSON_IsString(version) || !hex_id(version->valuestring) ||
        !number(root, "size", sector, (double)UINT32_MAX * sector, &size) ||
        !number(root, "sector_size", sector, sector, &block) || (uint64_t)size % sector) return false;
    snprintf(image->version, sizeof(image->version), "%s", version->valuestring);
    image->size = (uint64_t)size;
    image->sector = sector;
    image->present = true;
    return true;
}
static bool parse_manifest(cJSON *root, manifest_t *manifest) {
    double refresh = 0, block = 0;
    cJSON *iso = cJSON_GetObjectItemCaseSensitive(root, "iso");
    cJSON *transfer = cJSON_GetObjectItemCaseSensitive(root, "transfer");
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(transfer, "mode");
    *manifest = (manifest_t){0};
    if (!number(root, "refresh", 0, UINT32_MAX, &refresh) ||
        !number(root, "block_size", W2B_BLOCK_SIZE, W2B_BLOCK_SIZE, &block) ||
        !cJSON_IsString(mode) || (strcmp(mode->valuestring, "host") && strcmp(mode->valuestring, "server")) ||
        !parse_image(transfer, &manifest->media[1], 512)) return false;
    if (!cJSON_IsNull(iso) && !parse_image(iso, &manifest->media[0], 2048)) return false;
    manifest->media[0].sector = 2048;
    manifest->refresh = (uint32_t)refresh;
    manifest->host = !strcmp(mode->valuestring, "host");
    return true;
}
static esp_err_t http_event(esp_http_client_event_t *event) {
    response_t *r = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        if (!strcasecmp(event->header_key, "X-Image-Version")) snprintf(r->version, sizeof(r->version), "%s", event->header_value);
        if (!strcasecmp(event->header_key, "X-Offset")) snprintf(r->offset, sizeof(r->offset), "%s", event->header_value);
    } else if (event->event_id == HTTP_EVENT_ON_DATA) {
        if ((size_t)event->data_len > r->capacity - r->used) r->overflow = true;
        else { memcpy(r->bytes + r->used, event->data, event->data_len); r->used += event->data_len; }
    }
    return ESP_OK;
}
static void network_task(void *arg) {
    (void)arg;
    response_t response = {0};
    esp_http_client_config_t config = {.url = CONFIG_W2B_BACKEND_URL, .event_handler = http_event,
        .user_data = &response, .timeout_ms = CONFIG_W2B_HTTP_TIMEOUT_MS, .keep_alive_enable = true,
        .disable_auto_redirect = true, .buffer_size = 2048};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    configASSERT(client);
    char auth[256], session[65] = "", device[64];
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(device, sizeof(device), "{\"device\":\"%02x%02x%02x%02x%02x%02x\"}", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_W2B_TOKEN);
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Authorization", auth));
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Accept-Encoding", "identity"));
    for (;;) {
        job_t job;
        xQueueReceive(jobs, &job, portMAX_DELAY);
        result_t result = {.job = job};
        char url[512], text[2048];
        const char *body = "";
        size_t body_length = 0;
        esp_http_client_method_t method = HTTP_METHOD_GET;
        response = (response_t){.bytes = (uint8_t *)text, .capacity = sizeof(text) - 1};
        int count;
        if (job.op == READ) {
            count = snprintf(url, sizeof(url), "%s/api/block?lun=%u&version=%s&offset=%" PRIu64 "&length=%" PRIu32,
                CONFIG_W2B_BACKEND_URL, job.lun, job.image.version, job.address, job.length);
            response.bytes = network_buffer; response.capacity = job.length;
        } else if (job.op == WRITE) {
            count = snprintf(url, sizeof(url), "%s/api/transfer/write?version=%s&offset=%" PRIu64,
                CONFIG_W2B_BACKEND_URL, job.image.version, job.address);
            method = HTTP_METHOD_POST; body = (const char *)network_buffer; body_length = job.length;
        } else {
            const char *path = job.op == MANIFEST ? "/api/manifest" :
                job.op == CLAIM ? "/api/transfer/claim" : "/api/transfer/release";
            count = snprintf(url, sizeof(url), "%s%s", CONFIG_W2B_BACKEND_URL, path);
            if (job.op != MANIFEST) method = HTTP_METHOD_POST;
            if (job.op == CLAIM) { body = device; body_length = strlen(device); }
        }
        int64_t start = esp_timer_get_time();
        bool ok = count > 0 && count < sizeof(url) && esp_http_client_set_url(client, url) == ESP_OK;
        if (ok) ok = esp_http_client_set_method(client, method) == ESP_OK &&
            esp_http_client_set_header(client, "X-Session", session) == ESP_OK &&
            esp_http_client_set_header(client, "Content-Type", job.op == WRITE ? "application/octet-stream" : "application/json") == ESP_OK &&
            esp_http_client_set_post_field(client, body, body_length) == ESP_OK;
        if (ok) ok = esp_http_client_perform(client) == ESP_OK && esp_http_client_get_status_code(client) == 200 &&
            !response.overflow && esp_http_client_get_content_length(client) == response.used;
        if (ok && job.op == READ) {
            char offset[32]; snprintf(offset, sizeof(offset), "%" PRIu64, job.address);
            ok = response.used == job.length && !strcmp(response.version, job.image.version) && !strcmp(response.offset, offset);
        } else if (ok) {
            text[response.used] = 0;
            cJSON *root = cJSON_Parse(text);
            if (!root) ok = false;
            else if (job.op == MANIFEST) ok = parse_manifest(root, &result.manifest);
            else if (job.op == CLAIM) {
                cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "session");
                image_t image = {0};
                ok = cJSON_IsString(id) && hex_id(id->valuestring) && parse_image(root, &image, 512) &&
                    !strcmp(image.version, job.image.version) && image.size == job.image.size;
                if (ok) snprintf(session, sizeof(session), "%s", id->valuestring);
            } else if (job.op == WRITE) {
                double length = 0, offset = 0;
                cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
                ok = number(root, "written", job.length, job.length, &length) &&
                    number(root, "offset", job.address, job.address, &offset) &&
                    cJSON_IsString(version) && !strcmp(version->valuestring, job.image.version);
            } else if (job.op == RELEASE) session[0] = 0;
            cJSON_Delete(root);
        }
        result.ok = ok;
        result.elapsed = esp_timer_get_time() - start;
        if (!ok) esp_http_client_close(client);
        xQueueSend(results, &result, portMAX_DELAY);
    }
}
static void submit(job_t job) {
    configASSERT(!network_busy);
    ESP_ERROR_CHECK(xQueueSend(jobs, &job, 0) == pdTRUE ? ESP_OK : ESP_FAIL);
    network_busy = true;
}
static bool media_ready(uint8_t lun) {
    return lun < 2 && initialized && !switching && active.media[lun].present && (lun == 0 || claimed);
}
void w2b_msc_cancel(void) {
    pending.valid = false;
    sequential = 0; previous_end = UINT64_MAX;
    prefetch_next = prefetch_limit = 0;
}
void storage_init(void) {
    size_t count = CONFIG_W2B_CACHE_BLOCKS;
    uint8_t *bytes = heap_caps_malloc(count * W2B_BLOCK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bytes) {
        if (count > 4) count = 4;
        bytes = heap_caps_malloc(count * W2B_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGW(TAG, "使用%uKiB内部缓存", (unsigned)(count * 16));
    }
    cache_line_t *lines = calloc(count, sizeof(*lines));
    network_buffer = heap_caps_malloc(W2B_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    configASSERT(bytes && lines && network_buffer);
    cache_init(&cache, lines, bytes, count);
    jobs = xQueueCreate(1, sizeof(job_t)); results = xQueueCreate(1, sizeof(result_t));
    configASSERT(jobs && results);
    ESP_ERROR_CHECK(xTaskCreate(network_task, "磁盘网络", 8192, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
bool storage_ready(void) { return initialized && (!active.host || claimed); }
void storage_usb_started(void) { usb_started = true; }

void storage_tick(void) {
    int64_t now = esp_timer_get_time();
    result_t r;
    if (xQueueReceive(results, &r, 0) == pdTRUE) {
        network_busy = false; ++net_requests; net_elapsed += r.elapsed;
        if (!r.ok) ++net_errors;
        if (r.job.op == MANIFEST) {
            next_manifest = now + 2000000;
            if (r.ok) {
                latest = r.manifest;
                if (!initialized) { active = latest; initialized = true; }
                else {
                    bool changed = strcmp(latest.media[1].version, active.media[1].version) != 0;
                    if (changed) claimed = false;
                    if (latest.refresh != active.refresh || latest.host != active.host || changed) switch_requested = true;
                }
            }
        } else if (r.job.op == CLAIM) {
            claimed = r.ok;
            next_control = now + 2000000;
        } else if (r.job.op == RELEASE) {
            if (r.ok) { claimed = false; force_release = false; next_manifest = 0; }
            next_control = now + 2000000;
        } else {
            bool same_image = !strcmp(r.job.image.version, active.media[r.job.lun].version);
            if (same_image && r.job.op == READ && r.ok)
                cache_insert(&cache, key(r.job.lun, r.job.address), network_buffer, r.job.length, r.job.speculative);
            if (same_image && r.job.op == WRITE)
                cache_invalidate(&cache, key(1, r.job.address), r.job.length);
            if (pending.valid && pending.job.ticket == r.job.ticket &&
                (r.job.op == WRITE || !r.ok)) {
                pending.valid = false;
                if (r.ok && r.job.op == WRITE) written_bytes += r.job.length;
                tud_msc_async_io_done(r.ok ? (int32_t)r.job.length : TUD_MSC_RET_ERROR, false);
            }
        }
    }
    if (pending.valid && pending.job.op == READ && cache_read(&cache, key(pending.job.lun, pending.job.address), pending.buffer, pending.job.length)) {
        uint32_t length = pending.job.length; pending.valid = false;
        tud_msc_async_io_done((int32_t)length, false);
    }
    if (pending.valid && now - pending.started > 2LL * CONFIG_W2B_HTTP_TIMEOUT_MS * 1000) {
        pending.valid = false; tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
    }
    if (force_release) { latest.host = false; switch_requested = true; }
    if (switch_requested && !switching && !pending.valid && !network_busy &&
        now - last_io >= (int64_t)CONFIG_W2B_REFRESH_IDLE_MS * 1000) {
        if (usb_started) tud_disconnect();
        switching = true; reconnect_at = now + 1000000; w2b_msc_cancel();
    }
    if (switching && now >= reconnect_at && !network_busy &&
        (latest.host ? claimed : !claimed)) {
        active = latest; cache_clear(&cache);
        switching = switch_requested = false;
        if (usb_started) tud_connect();
    }
    if (!network_busy) {
        if (switching && now >= next_control && claimed && !latest.host) submit((job_t){.op = RELEASE});
        else if (initialized && now >= next_control && !claimed && (switching ? latest.host : active.host))
            submit((job_t){.op = CLAIM, .image = (switching ? latest : active).media[1]});
        else if (!switching && pending.valid && !pending.submitted) {
            job_t job = pending.job;
            if (job.op == READ) {
                job.address = job.address / W2B_BLOCK_SIZE * W2B_BLOCK_SIZE;
                uint64_t remaining = job.image.size - job.address;
                job.length = remaining < W2B_BLOCK_SIZE ? (uint32_t)remaining : W2B_BLOCK_SIZE;
            } else memcpy(network_buffer, pending.buffer, job.length);
            pending.submitted = true; submit(job);
        } else if (now >= next_manifest && !switching) submit((job_t){.op = MANIFEST});
        else if (!switching && !switch_requested && prefetch_next < prefetch_limit && media_ready(prefetch_lun)) {
            while (prefetch_next < prefetch_limit && cache_contains(&cache, key(prefetch_lun, prefetch_next))) prefetch_next += W2B_BLOCK_SIZE;
            if (prefetch_next < prefetch_limit && prefetch_next < active.media[prefetch_lun].size) {
                uint64_t remaining = active.media[prefetch_lun].size - prefetch_next;
                submit((job_t){.op = READ, .lun = prefetch_lun, .image = active.media[prefetch_lun], .address = prefetch_next,
                    .length = remaining < W2B_BLOCK_SIZE ? (uint32_t)remaining : W2B_BLOCK_SIZE, .speculative = true});
                prefetch_next += W2B_BLOCK_SIZE;
            }
        }
    }
    if (now >= next_log) {
        next_log = now + 10000000;
        ESP_LOGI(TAG, "读=%" PRIu64 " 写=%" PRIu64 " 命中=%" PRIu64 " 网络请求=%" PRIu64
            " 网络毫秒=%" PRIu64 " 失败=%" PRIu64 " 文件盘=%s", read_bytes, written_bytes, cache.hits,
            net_requests, net_elapsed / 1000, net_errors, claimed ? "主机使用" : "服务器使用");
    }
}
/* 此版本 TinyUSB 要求返回逻辑盘数量，内部再减1得到协议中的最大编号。 */
uint8_t tud_msc_get_maxlun_cb(void) { return 2; }
uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t *response, uint32_t size) {
    if (size < sizeof(*response)) return 0;
    response->peripheral_device_type = lun == 0 ? 5 : 0;
    response->is_removable = 1;
    memcpy(response->vendor_id, "W2B     ", 8);
    memcpy(response->product_id, lun == 0 ? "WiFi ISO CD-ROM " : "WiFi Files Disk ", 16);
    memcpy(response->product_rev, "0200", 4);
    return sizeof(*response);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    bool ready = media_ready(lun);
    if (!ready) tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0);
    return ready;
}
void tud_msc_capacity_cb(uint8_t lun, uint32_t *count, uint16_t *size) {
    *size = lun == 0 ? 2048 : 512;
    *count = media_ready(lun) ? active.media[lun].size / *size : 0;
}
bool tud_msc_is_writable_cb(uint8_t lun) { return lun == 1 && media_ready(lun); }
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power, bool start, bool eject) {
    (void)power;
    if (lun == 1 && eject && !start) force_release = true;
    return true;
}
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t length) {
    uint64_t address = (uint64_t)lba * (lun == 0 ? 2048 : 512) + offset;
    if (!media_ready(lun) || address >= active.media[lun].size || length > active.media[lun].size - address)
        return TUD_MSC_RET_ERROR;
    uint32_t available = W2B_BLOCK_SIZE - address % W2B_BLOCK_SIZE;
    if (length > available) length = available;
    last_io = esp_timer_get_time(); read_bytes += length;
    if (previous_lun == lun && address == previous_end) ++sequential;
    else { sequential = 0; prefetch_next = prefetch_limit = 0; }
    previous_lun = lun; previous_end = address + length;
    if (sequential >= 2) {
        prefetch_lun = lun;
        prefetch_next = (previous_end + W2B_BLOCK_SIZE - 1) / W2B_BLOCK_SIZE * W2B_BLOCK_SIZE;
        prefetch_limit = prefetch_next + CONFIG_W2B_PREFETCH_BLOCKS * W2B_BLOCK_SIZE;
        if (prefetch_limit > active.media[lun].size) prefetch_limit = active.media[lun].size;
    }
    if (cache_read(&cache, key(lun, address), buffer, length)) return length;
    configASSERT(!pending.valid);
    pending = (pending_t){.valid = true, .buffer = buffer, .started = last_io,
        .job = {.op = READ, .lun = lun, .image = active.media[lun], .address = address, .length = length, .ticket = ++serial}};
    return TUD_MSC_RET_ASYNC;
}
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t length) {
    uint64_t address = (uint64_t)lba * 512 + offset;
    if (lun != 1 || !media_ready(lun) || address >= active.media[1].size || length > active.media[1].size - address) {
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0); return TUD_MSC_RET_ERROR;
    }
    last_io = esp_timer_get_time(); prefetch_next = prefetch_limit = 0;
    configASSERT(!pending.valid);
    pending = (pending_t){.valid = true, .buffer = buffer, .started = last_io,
        .job = {.op = WRITE, .lun = 1, .image = active.media[1], .address = address, .length = length, .ticket = ++serial}};
    return TUD_MSC_RET_ASYNC;
}
static void be32(uint8_t *p, uint32_t value) { p[0]=value>>24; p[1]=value>>16; p[2]=value>>8; p[3]=value; }
static void toc_address(uint8_t *p, uint32_t lba, bool msf) {
    if (!msf) { be32(p, lba); return; }
    lba += 150; p[0]=0; p[1]=lba/(60*75); p[2]=lba/75%60; p[3]=lba%75;
}
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const command[16], void *buffer, uint16_t size) {
    uint8_t response[40] = {0}; uint16_t length = 0, allocation = size;
    if (command[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL || command[0] == 0x35) return 0;
    if (lun == 0 && media_ready(0)) {
        allocation = ((uint16_t)command[7] << 8) | command[8];
        if (command[0] == 0x43 && (command[2] & 15) == 0 &&
            (command[6] == 0 || command[6] == 1 || command[6] == 0xAA)) { /* READ TOC, format 0 */
            bool leadout_only = command[6] == 0xAA;
            length = leadout_only ? 12 : 20;
            response[1]=length-2; response[2]=1; response[3]=1;
            response[5]=0x14; response[6]=leadout_only ? 0xAA : 1;
            toc_address(response+8, leadout_only ? active.media[0].size/2048 : 0, command[1]&2);
            if (!leadout_only) {
                response[13]=0x14; response[14]=0xAA;
                toc_address(response+16, active.media[0].size/2048, command[1]&2);
            }
        } else if (command[0] == 0x46) { /* GET CONFIGURATION，当前配置为 CD-ROM。 */
            length=8; response[3]=4; response[7]=8;
        } else if (command[0] == 0x51) { /* READ DISC INFORMATION，完整单会话光盘。 */
            length=34; response[1]=32; response[2]=0x0E; response[3]=1;
            response[4]=1; response[5]=1; response[6]=1;
        } else if (command[0] == 0x5A) { /* MODE SENSE(10)，只读。 */
            length=8; response[1]=6; response[3]=0x80;
        } else if (command[0] == 0x4A) { /* GET EVENT STATUS，当前无事件。 */
            length=4; response[1]=2; response[2]=0x80; response[3]=0x10;
        }
    }
    if (length) {
        if (length > allocation) length = allocation;
        if (length > size) length = size;
        memcpy(buffer, response, length); return length;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
    return TUD_MSC_RET_ERROR;
}
