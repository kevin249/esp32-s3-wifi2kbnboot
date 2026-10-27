#pragma once
#define CFG_TUSB_MCU OPT_MCU_ESP32S3
#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_DEBUG 0
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_MSC 1
#include "sdkconfig.h"
#ifdef CONFIG_W2B_BIOS_DISK_ONLY
#define CFG_TUD_HID 0
#else
#define CFG_TUD_HID 2
#endif
#define CFG_TUD_MSC_EP_BUFSIZE 8192
#define CFG_TUD_HID_EP_BUFSIZE 16
#define CFG_TUD_TASK_QUEUE_SZ 32
#define CFG_TUD_MEM_ALIGN __attribute__((aligned(4)))
