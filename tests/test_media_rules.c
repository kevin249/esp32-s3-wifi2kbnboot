#include "media_rules.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(media_name_valid("主板-BIOS.bin"));assert(!media_name_valid("../BIOS.bin"));
    assert(!media_name_valid("a/b"));assert(!media_name_valid("a\\b"));assert(!media_name_valid("a?"));
    assert(!media_name_valid("a."));assert(!media_name_valid("a "));assert(!media_name_valid(".upload-x"));
    assert(media_range_valid(4096,3584,512));assert(!media_range_valid(4096,3584,1024));
    assert(!media_range_valid(UINT64_MAX,UINT64_MAX-1,4));assert(!media_range_valid(4096,0,0));
    uint64_t total,sizes[]={W2B_ISO_PART_SIZE,W2B_ISO_PART_SIZE,4096};
    assert(media_iso_sizes(sizes,3,true,&total)&&total==2*W2B_ISO_PART_SIZE+4096);
    unsigned part;uint32_t within,length;
    assert(media_iso_position(total,true,W2B_ISO_PART_SIZE-2048,4096,&part,&within,&length));
    assert(part==0&&within==W2B_ISO_PART_SIZE-2048&&length==2048);
    assert(media_iso_position(total,true,W2B_ISO_PART_SIZE,4096,&part,&within,&length));assert(part==1&&within==0&&length==4096);
    sizes[0]=2048;assert(!media_iso_sizes(sizes,3,true,&total));
    uint64_t single[]={3ULL*1024*1024*1024};assert(media_iso_sizes(single,1,false,&total));
    assert(media_iso_position(total,false,UINT64_C(2500000000),4096,&part,&within,&length));
    assert(part==0&&within==UINT32_C(2500000000));
    puts("文件名、磁盘边界与大ISO分卷测试通过");return 0;
}
