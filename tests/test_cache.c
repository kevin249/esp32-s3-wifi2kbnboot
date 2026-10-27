/* 主机上验证真实 C 缓存实现，不重复编写一份 Python 模型。 */
#include "block_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    block_cache_t cache;
    cache_line_t lines[3];
    static uint8_t bytes[3 * W2B_BLOCK_SIZE], data[W2B_BLOCK_SIZE], out[W2B_BLOCK_SIZE];
    cache_init(&cache, lines, bytes, 3);
    memset(data, 0x11, sizeof(data));
    assert(!cache_read(&cache, 0, out, 512));
    cache_insert(&cache, 0, data, sizeof(data), false);
    assert(cache_read(&cache, 512, out, 8192) && out[0] == 0x11);
    assert(!cache_read(&cache, W2B_BLOCK_SIZE - 512, out, 1024));
    memset(data, 0x22, sizeof(data));
    cache_insert(&cache, W2B_BLOCK_SIZE, data, sizeof(data), true);
    cache_insert(&cache, 2 * W2B_BLOCK_SIZE, data, sizeof(data), false);
    cache_insert(&cache, 3 * W2B_BLOCK_SIZE, data, sizeof(data), false);
    assert(cache_contains(&cache, 0));
    assert(!cache_contains(&cache, W2B_BLOCK_SIZE));
    assert(cache.unused_evictions == 1);
    cache_insert(&cache, 4 * W2B_BLOCK_SIZE, data, 512, true);
    assert(cache_read(&cache, 4 * W2B_BLOCK_SIZE, out, 512));
    assert(!cache_read(&cache, 4 * W2B_BLOCK_SIZE, out, 1024));
    cache_clear(&cache);
    for (size_t i = 0; i < 5; ++i) assert(!cache_contains(&cache, i * W2B_BLOCK_SIZE));
    /* 64 位磁盘地址不可截断成 32 位。 */
    uint64_t high = UINT64_C(1) << 35;
    cache_insert(&cache, high, data, sizeof(data), false);
    assert(cache_read(&cache, high + 123, out, 512));
    assert(!cache_read(&cache, 123, out, 512));
    cache_insert(&cache, 0, data, sizeof(data), false);
    cache_invalidate(&cache, high + 512, 512);
    assert(!cache_contains(&cache, high));
    assert(cache_contains(&cache, 0));
    puts("分块缓存测试通过");
    return 0;
}
