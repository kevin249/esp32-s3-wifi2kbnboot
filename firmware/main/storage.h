#pragma once
#include "sd_media.h"
typedef struct {
  sd_media_state_t media;
  bool switching;
  esp_err_t last_error;
} storage_public_t;
void storage_init(void);
/* 必须从唯一 USB 任务调用。 */
void storage_tick(void);
void w2b_msc_cancel(void);
void storage_public(storage_public_t *output);
esp_err_t storage_request_owner(bool host);
esp_err_t storage_request_iso(const char *name);
