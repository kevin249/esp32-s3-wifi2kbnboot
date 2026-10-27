#include "sd_media.h"
#include "diskio_impl.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static SemaphoreHandle_t mutex;
static sd_media_state_t state;
static FIL image, iso;
static bool image_open, iso_open, iso_split;
static unsigned iso_part;
static FATFS *files_fs;
static BYTE image_drive;
static char image_drive_name[8], sd_drive_name[4];
static unsigned web_users;

void sd_media_lock(void) { xSemaphoreTake(mutex, portMAX_DELAY); }
void sd_media_unlock(void) { xSemaphoreGive(mutex); }
static esp_err_t result(FRESULT r) { return r == FR_OK ? ESP_OK : ESP_FAIL; }
static esp_err_t image_io(uint64_t address, void *buffer, uint32_t length,
                          bool write) {
  if (!image_open || !media_range_valid(state.file_size, address, length))
    return ESP_ERR_INVALID_ARG;
  UINT count = 0;
  FRESULT r = f_lseek(&image, (FSIZE_t)address);
  if (r == FR_OK)
    r = write ? f_write(&image, buffer, length, &count)
              : f_read(&image, buffer, length, &count);
  if (r != FR_OK || count != length)
    return ESP_FAIL;
  return ESP_OK;
}
/* 已由 sd_media mutex 包围；内层卷通过这些回调访问外层SD文件。 */
static DSTATUS image_disk_init(BYTE drive) {
  (void)drive;
  return image_open ? 0 : STA_NOINIT;
}
static DRESULT image_disk_read(BYTE drive, BYTE *buffer, DWORD sector, UINT count) {
  (void)drive;
  return count <= UINT32_MAX / 512 && image_io((uint64_t)sector * 512, buffer,
                                               count * 512, false) == ESP_OK
             ? RES_OK
             : RES_ERROR;
}
static DRESULT image_disk_write(BYTE drive, const BYTE *buffer, DWORD sector,
                          UINT count) {
  (void)drive;
  return count <= UINT32_MAX / 512 &&
                 image_io((uint64_t)sector * 512, (void *)buffer, count * 512,
                          true) == ESP_OK
             ? RES_OK
             : RES_ERROR;
}
static DRESULT image_disk_ioctl(BYTE drive, BYTE command, void *buffer) {
  (void)drive;
  switch (command) {
  case CTRL_SYNC:
    return f_sync(&image) == FR_OK ? RES_OK : RES_ERROR;
  case GET_SECTOR_COUNT:
    *(DWORD *)buffer = state.file_size / 512;
    return RES_OK;
  case GET_SECTOR_SIZE:
    *(WORD *)buffer = 512;
    return RES_OK;
  case GET_BLOCK_SIZE:
    *(DWORD *)buffer = 1;
    return RES_OK;
  default:
    return RES_PARERR;
  }
}
static void image_path(char *out, size_t size, const char *name) {
  snprintf(out, size, "%s/%s", sd_drive_name, name);
}
esp_err_t sd_media_init(void) {
  mutex = xSemaphoreCreateMutex();
  if (!mutex)
    return ESP_ERR_NO_MEM;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = CONFIG_W2B_SD_CLK;
  slot.cmd = CONFIG_W2B_SD_CMD;
  slot.d0 = CONFIG_W2B_SD_D0;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  sdmmc_card_t *card = NULL;
  esp_vfs_fat_sdmmc_mount_config_t mount = {.format_if_mount_failed = false,
                                            .max_files = 12,
                                            .allocation_unit_size = 4096};
  esp_err_t err = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mount, &card);
  if (err != ESP_OK)
    return err;
  /* 本工程没有其他FatFs卷，SD首先挂载为0号。内层镜像由注册器选择空闲卷号。 */
  snprintf(sd_drive_name, sizeof(sd_drive_name), "0:");
  f_mkdir("0:/iso");
  state.sd_ready = true;
  state.file_size = (uint64_t)CONFIG_W2B_FILE_DISK_MIB * 1024 * 1024;
  char path[64];
  image_path(path, sizeof(path), "files.img");
  FILINFO info;
  bool create = f_stat(path, &info) == FR_NO_FILE;
  if (!create && f_stat(path, &info) != FR_OK)
    return ESP_FAIL;
  if (!create)
    state.file_size = info.fsize;
  if (state.file_size < 32 * 1024 * 1024 ||
      state.file_size > 1024ULL * 1024 * 1024 || state.file_size % 512)
    return ESP_ERR_INVALID_SIZE;
  char stage[64];
  image_path(stage, sizeof(stage), "files.creating");
  if (create) {
    if (f_open(&image, stage, FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK)
      return ESP_FAIL;
    image_open = true;
    uint8_t *zero = calloc(1, 16384);
    if (!zero)
      return ESP_ERR_NO_MEM;
    for (uint64_t n = 0; n < state.file_size; n += 16384) {
      UINT written;
      if (f_write(&image, zero, 16384, &written) != FR_OK || written != 16384) {
        free(zero);
        return ESP_FAIL;
      }
    }
    free(zero);
    if (f_sync(&image) != FR_OK)
      return ESP_FAIL;
  } else {
    if (f_open(&image, path, FA_READ | FA_WRITE) != FR_OK)
      return ESP_FAIL;
    image_open = true;
  }
  err = ff_diskio_get_drive(&image_drive);
  if (err != ESP_OK)
    return err;
  static const ff_diskio_impl_t impl = {.init = image_disk_init,
                                        .status = image_disk_init,
                                        .read = image_disk_read,
                                        .write = image_disk_write,
                                        .ioctl = image_disk_ioctl};
  ff_diskio_register(image_drive, &impl);
  snprintf(image_drive_name, sizeof(image_drive_name), "%u:", image_drive);
  esp_vfs_fat_conf_t conf = {
      .base_path = "/files", .fat_drive = image_drive_name, .max_files = 8};
  err = esp_vfs_fat_register_cfg(&conf, &files_fs);
  if (err != ESP_OK)
    return err;
  if (create) {
    uint8_t work[4096];
    /* 不设FM_SFD：生成带MBR的普通FAT16磁盘，提高BIOS识别机会。 */
    MKFS_PARM options = {
        .fmt = FM_FAT,
        .n_fat = 2,
        .n_root = 512,
        .au_size = state.file_size >= 256ULL * 1024 * 1024 ? 32768 : 4096};
    if (f_mkfs(image_drive_name, &options, work, sizeof(work)) != FR_OK)
      return ESP_FAIL;
    if (f_mount(files_fs, image_drive_name, 1) != FR_OK || files_fs->fs_type != FS_FAT16)
      return ESP_FAIL;
    if (f_mount(NULL, image_drive_name, 0) != FR_OK)
      return ESP_FAIL;
    if (f_sync(&image) != FR_OK || f_close(&image) != FR_OK)
      return ESP_FAIL;
    image_open = false;
    if (f_rename(stage, path) != FR_OK ||
        f_open(&image, path, FA_READ | FA_WRITE) != FR_OK)
      return ESP_FAIL;
    image_open = true;
  }
  /* 默认交给受控主机，启动时不在固件中挂载同一文件盘。 */
  state.host_owner = true;
  state.files_ready = true;
  return ESP_OK;
}
void sd_media_state(sd_media_state_t *output) {
  sd_media_lock();
  *output = state;
  sd_media_unlock();
}
esp_err_t sd_media_owner(bool host) {
  sd_media_lock();
  esp_err_t err = ESP_OK;
  if (!state.files_ready || web_users)
    err = ESP_ERR_INVALID_STATE;
  else if (host != state.host_owner) {
    err = result(host ? f_mount(NULL, image_drive_name, 0)
                      : f_mount(files_fs, image_drive_name, 1));
    if (err == ESP_OK && f_sync(&image) != FR_OK)
      err = ESP_FAIL;
    if (err == ESP_OK)
      state.host_owner = host;
  }
  sd_media_unlock();
  return err;
}
bool sd_media_web_begin(bool files) {
  sd_media_lock();
  bool allowed =
      state.sd_ready && (!files || (state.files_ready && !state.host_owner));
  if (allowed)
    ++web_users;
  sd_media_unlock();
  return allowed;
}
void sd_media_web_end(void) {
  sd_media_lock();
  if (web_users)
    --web_users;
  sd_media_unlock();
}
bool sd_media_iso_protected(const char *name) {
  bool protected;
  sd_media_lock();
  size_t n = strlen(state.iso_name);
  protected = state.iso_present && (!strcmp(name, state.iso_name) ||
                                    (iso_split && strlen(name) == n && n >= 3 &&
                                     !strncmp(name, state.iso_name, n - 3)));
  sd_media_unlock();
  return protected;
}
esp_err_t sd_media_select_iso(const char *name) {
  sd_media_lock();
  esp_err_t err = ESP_OK;
  if (!state.sd_ready || state.host_owner || web_users) {
    err = ESP_ERR_INVALID_STATE;
    goto done;
  }
  if (!name[0]) {
    if (iso_open)
      f_close(&iso);
    iso_open = false;
    state.iso_present = false;
    state.iso_size = 0;
    state.iso_name[0] = 0;
    goto done;
  }
  if (!media_name_valid(name)) {
    err = ESP_ERR_INVALID_ARG;
    goto done;
  }
  size_t length = strlen(name);
  bool split = length > 8 && !strcasecmp(name + length - 8, ".iso.000");
  if (!split && (length < 4 || strcasecmp(name + length - 4, ".iso"))) {
    err = ESP_ERR_INVALID_ARG;
    goto done;
  }
  uint64_t sizes[W2B_ISO_MAX_PARTS];
  size_t count = 0;
  bool gap = false;
  char path[256], part_name[W2B_NAME_MAX + 1];
  FILINFO info;
  for (unsigned i = 0; i < (split ? W2B_ISO_MAX_PARTS : 1); ++i) {
    snprintf(part_name, sizeof(part_name), "%s", name);
    if (split)
      snprintf(part_name + length - 3, 4, "%03u", i % 1000);
    snprintf(path, sizeof(path), "%s/iso/%s", sd_drive_name, part_name);
    FRESULT r = f_stat(path, &info);
    if (r == FR_NO_FILE) {
      gap = true;
      continue;
    }
    if (r != FR_OK || gap || (info.fattrib & AM_DIR)) {
      err = ESP_ERR_INVALID_SIZE;
      goto done;
    }
    sizes[count++] = info.fsize;
  }
  uint64_t total;
  if (!media_iso_sizes(sizes, count, split, &total)) {
    err = ESP_ERR_INVALID_SIZE;
    goto done;
  }
  /* 在全部验证成功后才替换原ISO；本函数只在USB停止读取期间调用。 */
  if (iso_open)
    f_close(&iso);
  snprintf(path, sizeof(path), "%s/iso/%s", sd_drive_name, name);
  iso_open = f_open(&iso, path, FA_READ) == FR_OK;
  if (!iso_open) {
    state.iso_present = false;
    state.iso_size = 0;
    err = ESP_FAIL;
    goto done;
  }
  iso_split = split;
  iso_part = 0;
  state.iso_size = total;
  state.iso_present = true;
  snprintf(state.iso_name, sizeof(state.iso_name), "%s", name);
done:
  sd_media_unlock();
  return err;
}
esp_err_t sd_media_read(unsigned lun, uint64_t address, void *buffer,
                        uint32_t length) {
  sd_media_lock();
  esp_err_t err = ESP_OK;
  if (lun == 1) {
    if (!state.host_owner)
      err = ESP_ERR_INVALID_STATE;
    else
      err = image_io(address, buffer, length, false);
  } else if (!state.iso_present ||
             !media_range_valid(state.iso_size, address, length))
    err = ESP_ERR_INVALID_ARG;
  else {
    uint8_t *out = buffer;
    while (length && err == ESP_OK) {
      unsigned part;
      uint32_t within, n;
      if (!media_iso_position(state.iso_size, iso_split, address, length, &part,
                              &within, &n)) {
        err = ESP_ERR_INVALID_ARG;
        break;
      }
      if (part != iso_part) {
        if (iso_open)
          f_close(&iso);
        char name[W2B_NAME_MAX + 1], path[256];
        snprintf(name, sizeof(name), "%s", state.iso_name);
        snprintf(name + strlen(name) - 3, 4, "%03u", part % 1000);
        snprintf(path, sizeof(path), "%s/iso/%s", sd_drive_name, name);
        iso_open = f_open(&iso, path, FA_READ) == FR_OK;
        iso_part = part;
      }
      UINT read = 0;
      if (!iso_open || f_lseek(&iso, within) != FR_OK ||
          f_read(&iso, out, n, &read) != FR_OK || read != n)
        err = ESP_FAIL;
      address += n;
      out += n;
      length -= n;
    }
  }
  sd_media_unlock();
  return err;
}
esp_err_t sd_media_write(uint64_t address, const void *buffer,
                         uint32_t length) {
  sd_media_lock();
  esp_err_t err = state.host_owner
                      ? image_io(address, (void *)buffer, length, true)
                      : ESP_ERR_INVALID_STATE;
  if (err == ESP_OK && f_sync(&image) != FR_OK)
    err = ESP_FAIL;
  sd_media_unlock();
  return err;
}
