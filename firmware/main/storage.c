#include "storage.h"
#include "block_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tusb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef enum { READ, WRITE, OWNER, ISO } operation_t;
typedef struct {
  operation_t op;
  unsigned lun;
  uint64_t address, ticket;
  uint32_t length;
  bool host, speculative;
  char name[W2B_NAME_MAX + 1];
} job_t;
typedef struct {
  job_t job;
  esp_err_t error;
  sd_media_state_t state;
} result_t;
typedef struct {
  job_t job;
  void *buffer;
  bool valid, submitted;
  int64_t started;
} pending_t;
static block_cache_t cache;
static QueueHandle_t jobs, results, commands;
static uint8_t *io_buffer;
static sd_media_state_t active;
static pending_t pending;
static bool busy, switching, control_valid;
static job_t control;
static uint64_t serial, previous_end = UINT64_MAX, prefetch_next,
                        prefetch_limit;
static unsigned previous_lun, prefetch_lun, sequential;
static int64_t last_io, reconnect_at;
static esp_err_t last_error;
static portMUX_TYPE public_guard = portMUX_INITIALIZER_UNLOCKED;
static storage_public_t public_state;
static uint64_t key(unsigned lun, uint64_t address) {
  return ((uint64_t)lun << 48) + address;
}
static unsigned map_lun(unsigned lun) {
#ifdef CONFIG_W2B_BIOS_DISK_ONLY
  return lun == 0 ? 1 : 2;
#else
  return lun;
#endif
}
static void publish(void) {
  portENTER_CRITICAL(&public_guard);
  public_state.media = active;
  public_state.switching = switching || control_valid;
  public_state.last_error = last_error;
  portEXIT_CRITICAL(&public_guard);
}
void storage_public(storage_public_t *output) {
  portENTER_CRITICAL(&public_guard);
  *output = public_state;
  portEXIT_CRITICAL(&public_guard);
}
static void io_task(void *argument) {
  (void)argument;
  for (;;) {
    job_t job;
    xQueueReceive(jobs, &job, portMAX_DELAY);
    result_t r = {.job = job};
    if (job.op == READ)
      r.error = sd_media_read(job.lun, job.address, io_buffer, job.length);
    else if (job.op == WRITE)
      r.error = sd_media_write(job.address, io_buffer, job.length);
    else if (job.op == OWNER)
      r.error = sd_media_owner(job.host);
    else
      r.error = sd_media_select_iso(job.name);
    if (job.op == OWNER || job.op == ISO)
      sd_media_state(&r.state);
    xQueueSend(results, &r, portMAX_DELAY);
  }
}
static void submit(job_t job) {
  configASSERT(!busy);
  ESP_ERROR_CHECK(xQueueSend(jobs, &job, 0) == pdTRUE ? ESP_OK : ESP_FAIL);
  busy = true;
}
static bool media_ready(unsigned logical_lun) {
  unsigned lun = map_lun(logical_lun);
  return !switching && lun < 2 &&
         (lun == 0 ? active.iso_present
                   : active.files_ready && active.host_owner);
}
void w2b_msc_cancel(void) {
  pending.valid = false;
  sequential = 0;
  previous_end = UINT64_MAX;
  prefetch_next = prefetch_limit = 0;
}
void storage_init(void) {
  esp_err_t error = sd_media_init();
  if (error != ESP_OK)
    ESP_LOGE("SD卡", "初始化未完成：%s；不会格式化现有SD卡",
             esp_err_to_name(error));
  sd_media_state(&active);
  last_error = error;
  size_t count = CONFIG_W2B_CACHE_BLOCKS;
  uint8_t *bytes = heap_caps_malloc(count * W2B_BLOCK_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!bytes) {
    if (count > 4)
      count = 4;
    bytes = heap_caps_malloc(count * W2B_BLOCK_SIZE,
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  cache_line_t *lines = calloc(count, sizeof(*lines));
  io_buffer =
      heap_caps_malloc(W2B_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ESP_ERROR_CHECK(bytes && lines && io_buffer ? ESP_OK : ESP_ERR_NO_MEM);
  cache_init(&cache, lines, bytes, count);
  jobs = xQueueCreate(1, sizeof(job_t));
  results = xQueueCreate(1, sizeof(result_t));
  commands = xQueueCreate(4, sizeof(job_t));
  ESP_ERROR_CHECK(jobs && results && commands ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(xTaskCreate(io_task, "SD磁盘I/O", 8192, NULL, 4, NULL) ==
                          pdPASS
                      ? ESP_OK
                      : ESP_ERR_NO_MEM);
  publish();
}
esp_err_t storage_request_owner(bool host) {
  job_t command = {.op = OWNER, .host = host};
  return xQueueSend(commands, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
esp_err_t storage_request_iso(const char *name) {
#ifdef CONFIG_W2B_BIOS_DISK_ONLY
  (void)name;
  return ESP_ERR_NOT_SUPPORTED;
#else
  storage_public_t snapshot;
  storage_public(&snapshot);
  if (snapshot.media.host_owner || snapshot.switching)
    return ESP_ERR_INVALID_STATE;
  if (name[0] && !media_name_valid(name))
    return ESP_ERR_INVALID_ARG;
  job_t command = {.op = ISO};
  snprintf(command.name, sizeof(command.name), "%s", name);
  return xQueueSend(commands, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
#endif
}
void storage_tick(void) {
  int64_t now = esp_timer_get_time();
  result_t r;
  if (xQueueReceive(results, &r, 0) == pdTRUE) {
    busy = false;
    if (r.job.op == OWNER || r.job.op == ISO) {
      active = r.state;
      last_error = r.error;
      control_valid = false;
      cache_clear(&cache);
      reconnect_at = now + 1000000;
    } else {
      if (r.job.op == READ && r.error == ESP_OK)
        cache_insert(&cache, key(r.job.lun, r.job.address), io_buffer,
                     r.job.length, r.job.speculative);
      if (r.job.op == WRITE)
        cache_invalidate(&cache, key(1, r.job.address), r.job.length);
      if (pending.valid && pending.job.ticket == r.job.ticket &&
          (r.job.op == WRITE || r.error != ESP_OK)) {
        uint32_t length = pending.job.length;
        pending.valid = false;
        tud_msc_async_io_done(
            r.error == ESP_OK ? (int32_t)length : TUD_MSC_RET_ERROR, false);
      }
    }
    publish();
  }
  if (pending.valid && pending.job.op == READ &&
      cache_read(&cache, key(pending.job.lun, pending.job.address),
                 pending.buffer, pending.job.length)) {
    uint32_t length = pending.job.length;
    pending.valid = false;
    tud_msc_async_io_done((int32_t)length, false);
  }
  if (pending.valid && now - pending.started > 10000000) {
    pending.valid = false;
    tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
  }
  if (!switching && !control_valid &&
      xQueueReceive(commands, &control, 0) == pdTRUE) {
    control_valid = true;
    if (control.op == OWNER && control.host == active.host_owner)
      control_valid = false;
    if (control.op == ISO && active.host_owner) {
      control_valid = false;
      last_error = ESP_ERR_INVALID_STATE;
    }
    publish();
  }
  if (control_valid && !switching && !pending.valid && !busy &&
      now - last_io >= (int64_t)CONFIG_W2B_REFRESH_IDLE_MS * 1000) {
    tud_disconnect();
    w2b_msc_cancel();
    switching = true;
    reconnect_at = INT64_MAX;
    submit(control);
    publish();
  }
  if (switching && !busy && !control_valid && now >= reconnect_at) {
    switching = false;
    tud_connect();
    publish();
  }
  if (!busy && !switching) {
    if (pending.valid && !pending.submitted) {
      job_t job = pending.job;
      if (job.op == READ) {
        job.address = job.address / W2B_BLOCK_SIZE * W2B_BLOCK_SIZE;
        uint64_t capacity = job.lun == 0 ? active.iso_size : active.file_size,
                 remaining = capacity - job.address;
        job.length =
            remaining < W2B_BLOCK_SIZE ? (uint32_t)remaining : W2B_BLOCK_SIZE;
      } else
        memcpy(io_buffer, pending.buffer, job.length);
      pending.submitted = true;
      submit(job);
    } else if (!control_valid && prefetch_next < prefetch_limit) {
      while (prefetch_next < prefetch_limit &&
             cache_contains(&cache, key(prefetch_lun, prefetch_next)))
        prefetch_next += W2B_BLOCK_SIZE;
      uint64_t capacity =
          prefetch_lun == 0 ? active.iso_size : active.file_size;
      if (prefetch_next < prefetch_limit && prefetch_next < capacity) {
        uint64_t remaining = capacity - prefetch_next;
        submit((job_t){.op = READ,
                       .lun = prefetch_lun,
                       .address = prefetch_next,
                       .length = remaining < W2B_BLOCK_SIZE
                                     ? (uint32_t)remaining
                                     : W2B_BLOCK_SIZE,
                       .speculative = true});
        prefetch_next += W2B_BLOCK_SIZE;
      }
    }
  }
}
uint8_t tud_msc_get_maxlun_cb(void) {
#ifdef CONFIG_W2B_BIOS_DISK_ONLY
  return 1;
#else
  return 2;
#endif
}
uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t *r,
                             uint32_t size) {
  if (size < sizeof(*r))
    return 0;
  bool files = map_lun(lun) == 1;
  r->peripheral_device_type = files ? 0 : 5;
  r->is_removable = 1;
  memcpy(r->vendor_id, "W2B     ", 8);
  memcpy(r->product_id, files ? "SD FAT USB Disk " : "SD ISO CD-ROM   ", 16);
  memcpy(r->product_rev, "0300", 4);
  return sizeof(*r);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  bool ready = media_ready(lun);
  if (!ready)
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0);
  return ready;
}
void tud_msc_capacity_cb(uint8_t lun, uint32_t *count, uint16_t *size) {
  bool files = map_lun(lun) == 1;
  *size = files ? 512 : 2048;
  *count =
      media_ready(lun)
          ? (uint32_t)((files ? active.file_size : active.iso_size) / *size)
          : 0;
}
bool tud_msc_is_writable_cb(uint8_t lun) {
  return map_lun(lun) == 1 && media_ready(lun);
}
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power, bool start, bool eject) {
  (void)power;
  if (map_lun(lun) == 1 && eject && !start)
    storage_request_owner(false);
  return true;
}
int32_t tud_msc_read10_cb(uint8_t logical, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t length) {
  unsigned lun = map_lun(logical);
  uint64_t address = (uint64_t)lba * (lun == 0 ? 2048 : 512) + offset;
  uint64_t capacity = lun == 0 ? active.iso_size : active.file_size;
  if (!media_ready(logical) || !media_range_valid(capacity, address, length))
    return TUD_MSC_RET_ERROR;
  uint32_t available = W2B_BLOCK_SIZE - address % W2B_BLOCK_SIZE;
  if (length > available)
    length = available;
  last_io = esp_timer_get_time();
  if (previous_lun == lun && previous_end == address)
    ++sequential;
  else {
    sequential = 0;
    prefetch_next = prefetch_limit = 0;
  }
  previous_lun = lun;
  previous_end = address + length;
  if (sequential >= 2) {
    prefetch_lun = lun;
    prefetch_next =
        (previous_end + W2B_BLOCK_SIZE - 1) / W2B_BLOCK_SIZE * W2B_BLOCK_SIZE;
    prefetch_limit =
        prefetch_next + CONFIG_W2B_PREFETCH_BLOCKS * W2B_BLOCK_SIZE;
    if (prefetch_limit > capacity)
      prefetch_limit = capacity;
  }
  if (cache_read(&cache, key(lun, address), buffer, length))
    return length;
  configASSERT(!pending.valid);
  pending = (pending_t){.valid = true,
                        .buffer = buffer,
                        .started = last_io,
                        .job = {.op = READ,
                                .lun = lun,
                                .address = address,
                                .length = length,
                                .ticket = ++serial}};
  return TUD_MSC_RET_ASYNC;
}
int32_t tud_msc_write10_cb(uint8_t logical, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t length) {
  uint64_t address = (uint64_t)lba * 512 + offset;
  if (map_lun(logical) != 1 || !media_ready(logical) ||
      !media_range_valid(active.file_size, address, length))
    return TUD_MSC_RET_ERROR;
  last_io = esp_timer_get_time();
  prefetch_next = prefetch_limit = 0;
  configASSERT(!pending.valid);
  pending = (pending_t){.valid = true,
                        .buffer = buffer,
                        .started = last_io,
                        .job = {.op = WRITE,
                                .lun = 1,
                                .address = address,
                                .length = length,
                                .ticket = ++serial}};
  return TUD_MSC_RET_ASYNC;
}

static void be32(uint8_t *p, uint32_t value) {
  p[0] = value >> 24;
  p[1] = value >> 16;
  p[2] = value >> 8;
  p[3] = value;
}
static void toc_address(uint8_t *p, uint32_t lba, bool msf) {
  if (!msf) {
    be32(p, lba);
    return;
  }
  lba += 150;
  p[0] = 0;
  p[1] = lba / (60 * 75);
  p[2] = lba / 75 % 60;
  p[3] = lba % 75;
}
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const command[16], void *buffer,
                        uint16_t size) {
  uint8_t response[40] = {0};
  uint16_t length = 0, allocation = size;
  if (command[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL || command[0] == 0x35)
    return 0;
  if (map_lun(lun) == 0 && media_ready(lun)) {
    allocation = ((uint16_t)command[7] << 8) | command[8];
    if (command[0] == 0x43 && (command[2] & 15) == 0 &&
        (command[6] == 0 || command[6] == 1 ||
         command[6] == 0xAA)) { /* READ TOC, format 0 */
      bool leadout_only = command[6] == 0xAA;
      length = leadout_only ? 12 : 20;
      response[1] = length - 2;
      response[2] = 1;
      response[3] = 1;
      response[5] = 0x14;
      response[6] = leadout_only ? 0xAA : 1;
      toc_address(response + 8, leadout_only ? active.iso_size / 2048 : 0,
                  command[1] & 2);
      if (!leadout_only) {
        response[13] = 0x14;
        response[14] = 0xAA;
        toc_address(response + 16, active.iso_size / 2048, command[1] & 2);
      }
    } else if (command[0] ==
               0x46) { /* GET CONFIGURATION，当前配置为 CD-ROM。 */
      length = 8;
      response[3] = 4;
      response[7] = 8;
    } else if (command[0] ==
               0x51) { /* READ DISC INFORMATION，完整单会话光盘。 */
      length = 34;
      response[1] = 32;
      response[2] = 0x0E;
      response[3] = 1;
      response[4] = 1;
      response[5] = 1;
      response[6] = 1;
    } else if (command[0] == 0x5A) { /* MODE SENSE(10)，只读。 */
      length = 8;
      response[1] = 6;
      response[3] = 0x80;
    } else if (command[0] == 0x4A) { /* GET EVENT STATUS，当前无事件。 */
      length = 4;
      response[1] = 2;
      response[2] = 0x80;
      response[3] = 0x10;
    }
  }
  if (length) {
    if (length > allocation)
      length = allocation;
    if (length > size)
      length = size;
    memcpy(buffer, response, length);
    return length;
  }
  tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
  return TUD_MSC_RET_ERROR;
}
