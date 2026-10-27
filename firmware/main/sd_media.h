#pragma once
#include "esp_err.h"
#include "media_rules.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  bool sd_ready, files_ready, host_owner, iso_present;
  uint64_t file_size, iso_size;
  char iso_name[W2B_NAME_MAX + 1];
} sd_media_state_t;
esp_err_t sd_media_init(void);
void sd_media_state(sd_media_state_t *state);
esp_err_t sd_media_owner(bool host);
esp_err_t sd_media_select_iso(const char *name);
esp_err_t sd_media_read(unsigned lun, uint64_t address, void *buffer,
                        uint32_t length);
esp_err_t sd_media_write(uint64_t address, const void *buffer, uint32_t length);
/* 网页文件访问与介质交接互斥；ISO 上传不会开放内部文件盘给两端同时使用。 */
bool sd_media_web_begin(bool files);
void sd_media_web_end(void);
bool sd_media_iso_protected(const char *name);
void sd_media_lock(void);
void sd_media_unlock(void);
