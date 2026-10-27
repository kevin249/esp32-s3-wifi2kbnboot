#pragma once
typedef struct {int max_freq_khz;} sdmmc_host_t;
typedef struct {int width,clk,cmd,d0,flags;} sdmmc_slot_config_t;
#define SDMMC_HOST_DEFAULT() ((sdmmc_host_t){0})
#define SDMMC_SLOT_CONFIG_DEFAULT() ((sdmmc_slot_config_t){0})
#define SDMMC_FREQ_DEFAULT 20000
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP 1
