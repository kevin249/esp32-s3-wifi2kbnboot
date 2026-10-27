#pragma once
#include "esp_err.h"
#include "ff.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>
#include <stddef.h>
typedef struct {bool format_if_mount_failed;int max_files;size_t allocation_unit_size;} esp_vfs_fat_sdmmc_mount_config_t;
typedef struct {const char *base_path,*fat_drive;size_t max_files;} esp_vfs_fat_conf_t;
esp_err_t esp_vfs_fat_sdmmc_mount(const char *,const sdmmc_host_t *,const sdmmc_slot_config_t *,const esp_vfs_fat_sdmmc_mount_config_t *,sdmmc_card_t **);
esp_err_t esp_vfs_fat_register_cfg(const esp_vfs_fat_conf_t *,FATFS **);
