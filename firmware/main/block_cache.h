#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define W2B_BLOCK_SIZE 16384u
typedef struct {
    uint64_t base, stamp;
    uint32_t length;
    bool valid, speculative;
} cache_line_t;
typedef struct {
    cache_line_t *lines;
    uint8_t *bytes;
    size_t count;
    uint64_t clock, hits, misses, unused_evictions;
} block_cache_t;

/* 只由 USB 所有者任务访问，无网络线程直接写入缓存。 */
void cache_init(block_cache_t *cache, cache_line_t *lines, void *bytes, size_t count);
void cache_clear(block_cache_t *cache);
void cache_invalidate(block_cache_t *cache, uint64_t address, uint32_t length);
bool cache_contains(const block_cache_t *cache, uint64_t base);
bool cache_read(block_cache_t *cache, uint64_t address, void *output, uint32_t length);
void cache_insert(block_cache_t *cache, uint64_t base, const void *data, uint32_t length, bool speculative);
