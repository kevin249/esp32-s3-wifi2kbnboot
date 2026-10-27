/* 原样编译生产sd_media.c，使用同版本FatFs及原生磁盘替代SD硬件。 */
#include "sd_media.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

PARTITION VolToPart[FF_VOLUMES]={{0,0},{1,0}};
static ff_diskio_impl_t disks[2];
static FATFS outer,inner;
static int backing;
static pthread_mutex_t fat_mutex[FF_VOLUMES+1];
int ff_mutex_create(int volume){return pthread_mutex_init(&fat_mutex[volume],NULL)==0;}
void ff_mutex_delete(int volume){pthread_mutex_destroy(&fat_mutex[volume]);}
int ff_mutex_take(int volume){return pthread_mutex_lock(&fat_mutex[volume])==0;}
void ff_mutex_give(int volume){pthread_mutex_unlock(&fat_mutex[volume]);}
void *ff_memalloc(UINT length){return malloc(length);}
void ff_memfree(void *p){free(p);}
DWORD get_fattime(void){return ((2026-1980)<<25)|(10<<21)|(27<<16);}
void ff_diskio_register(BYTE drive,const ff_diskio_impl_t *impl){disks[drive]=*impl;}
esp_err_t ff_diskio_get_drive(BYTE *drive){*drive=1;return ESP_OK;}
DSTATUS disk_initialize(BYTE d){return disks[d].init(d);}
DSTATUS disk_status(BYTE d){return disks[d].status(d);}
DRESULT disk_read(BYTE d,BYTE *b,LBA_t s,UINT n){return disks[d].read(d,b,s,n);}
DRESULT disk_write(BYTE d,const BYTE *b,LBA_t s,UINT n){return disks[d].write(d,b,s,n);}
DRESULT disk_ioctl(BYTE d,BYTE cmd,void *b){return disks[d].ioctl(d,cmd,b);}
static DSTATUS native_init(BYTE d){(void)d;return 0;}
static DRESULT native_read(BYTE d,BYTE *b,DWORD s,UINT n){(void)d;return pread(backing,b,n*512,(uint64_t)s*512)==(ssize_t)n*512?RES_OK:RES_ERROR;}
static DRESULT native_write(BYTE d,const BYTE *b,DWORD s,UINT n){(void)d;return pwrite(backing,b,n*512,(uint64_t)s*512)==(ssize_t)n*512?RES_OK:RES_ERROR;}
static DRESULT native_ioctl(BYTE d,BYTE cmd,void *b){
    (void)d;
    switch(cmd){case CTRL_SYNC:return fsync(backing)==0?RES_OK:RES_ERROR;
        case GET_SECTOR_COUNT:*(DWORD *)b=256*1024*2;return RES_OK;
        case GET_SECTOR_SIZE:*(WORD *)b=512;return RES_OK;
        case GET_BLOCK_SIZE:*(DWORD *)b=1;return RES_OK;default:return RES_PARERR;}
}
esp_err_t esp_vfs_fat_sdmmc_mount(const char *path,const sdmmc_host_t *h,const sdmmc_slot_config_t *s,const esp_vfs_fat_sdmmc_mount_config_t *c,sdmmc_card_t **card){
    (void)path;(void)h;(void)s;(void)card;assert(!c->format_if_mount_failed);
    backing=open("sd-native.img",O_RDWR|O_CREAT|O_TRUNC,0600);assert(backing>=0);assert(ftruncate(backing,256*1024*1024)==0);
    disks[0]=(ff_diskio_impl_t){native_init,native_init,native_read,native_write,native_ioctl};
    MKFS_PARM options={.fmt=FM_FAT32};BYTE work[4096];assert(f_mkfs("0:",&options,work,sizeof(work))==FR_OK);
    assert(f_mount(&outer,"0:",1)==FR_OK);return ESP_OK;
}
esp_err_t esp_vfs_fat_register_cfg(const esp_vfs_fat_conf_t *conf,FATFS **out){assert(!strcmp(conf->fat_drive,"1:"));*out=&inner;return ESP_OK;}
static void write_file(const char *name,const void *data,unsigned length){
    FIL f;UINT n;assert(f_open(&f,name,FA_CREATE_ALWAYS|FA_WRITE)==FR_OK);assert(f_write(&f,data,length,&n)==FR_OK&&n==length);
    assert(f_sync(&f)==FR_OK);assert(f_close(&f)==FR_OK);
}
static void check_file(const char *name,const void *expected,unsigned length){
    BYTE data[8192];FIL f;UINT n;assert(length<=sizeof(data));assert(f_open(&f,name,FA_READ)==FR_OK);
    assert(f_read(&f,data,length,&n)==FR_OK&&n==length);assert(!memcmp(data,expected,length));assert(f_close(&f)==FR_OK);
}
static uint32_t le32(const BYTE *p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int main(void){
    assert(sd_media_init()==ESP_OK);sd_media_state_t state;sd_media_state(&state);
    assert(state.sd_ready&&state.files_ready&&state.host_owner);
    BYTE sector[512];assert(sd_media_read(1,0,sector,512)==ESP_OK);
    assert(sector[510]==0x55&&sector[511]==0xAA);uint32_t partition=le32(sector+454);assert(partition>0);
    assert(sd_media_read(1,(uint64_t)partition*512,sector,512)==ESP_OK);assert(!memcmp(sector+54,"FAT16   ",8));
    assert(!sd_media_web_begin(true));assert(sd_media_owner(false)==ESP_OK);
    assert(sd_media_web_begin(true));assert(sd_media_owner(true)==ESP_ERR_INVALID_STATE);sd_media_web_end();
    BYTE bios[8192];for(unsigned i=0;i<sizeof(bios);++i)bios[i]=i;
    write_file("1:/主板-BIOS.bin",bios,sizeof(bios));assert(sd_media_owner(true)==ESP_OK);
    FATFS host_fs;assert(f_mount(&host_fs,"1:",1)==FR_OK);check_file("1:/主板-BIOS.bin",bios,sizeof(bios));
    write_file("1:/FROMHOST.TXT","from-host",9);assert(f_mount(NULL,"1:",0)==FR_OK);
    assert(sd_media_owner(false)==ESP_OK);check_file("1:/FROMHOST.TXT","from-host",9);
    assert(sd_media_read(1,0,sector,512)==ESP_ERR_INVALID_STATE);
    write_file("0:/iso/test.iso",bios,6144);assert(sd_media_select_iso("test.iso")==ESP_OK);
    BYTE actual[4096];assert(sd_media_read(0,1024,actual,sizeof(actual))==ESP_OK);assert(!memcmp(actual,bios+1024,sizeof(actual)));
    assert(sd_media_read(0,6144,sector,512)!=ESP_OK);assert(sd_media_select_iso("../bad.iso")!=ESP_OK);
    assert(sd_media_select_iso("test.iso.000")!=ESP_OK);
    sd_media_state(&state);assert(state.iso_present&&state.iso_size==6144);
    assert(sd_media_select_iso("")==ESP_OK);sd_media_state(&state);assert(!state.iso_present);
    puts("实际生产SD代码通过：MBR/FAT16、双向交接、中文文件名、只读ISO与失败保留");
    close(backing);unlink("sd-native.img");return 0;
}
