#include "block_cache.h"
#include <assert.h>
#include <string.h>

void cache_init(block_cache_t *c, cache_line_t *lines, void *bytes, size_t count) {
    assert(count > 0);
    *c = (block_cache_t){.lines = lines, .bytes = bytes, .count = count};
    cache_clear(c);
}
void cache_clear(block_cache_t *c) {
    memset(c->lines, 0, c->count * sizeof(*c->lines));
    c->clock = 0;
}
bool cache_contains(const block_cache_t *c, uint64_t base) {
    for (size_t i = 0; i < c->count; ++i)
        if (c->lines[i].valid && c->lines[i].base == base) return true;
    return false;
}
void cache_invalidate(block_cache_t *c, uint64_t address, uint32_t length) {
    for (size_t i = 0; i < c->count; ++i) {
        cache_line_t *line = &c->lines[i];
        if (line->valid && line->base < address + length && line->base + line->length > address)
            line->valid = false;
    }
}
bool cache_read(block_cache_t *c, uint64_t address, void *output, uint32_t length) {
    uint64_t base = address / W2B_BLOCK_SIZE * W2B_BLOCK_SIZE;
    uint32_t offset = (uint32_t)(address - base);
    for (size_t i = 0; i < c->count; ++i) {
        cache_line_t *line = &c->lines[i];
        if (line->valid && line->base == base && offset <= line->length && length <= line->length - offset) {
            memcpy(output, c->bytes + i * W2B_BLOCK_SIZE + offset, length);
            line->stamp = ++c->clock;
            line->speculative = false;
            ++c->hits;
            return true;
        }
    }
    ++c->misses;
    return false;
}
void cache_insert(block_cache_t *c, uint64_t base, const void *data, uint32_t length, bool speculative) {
    assert(base % W2B_BLOCK_SIZE == 0 && length && length <= W2B_BLOCK_SIZE);
    size_t victim = 0;
    for (size_t i = 0; i < c->count; ++i) {
        cache_line_t *line = &c->lines[i];
        if (!line->valid || line->base == base) { victim = i; break; }
        cache_line_t *old = &c->lines[victim];
        if ((line->speculative && !old->speculative) ||
            (line->speculative == old->speculative && line->stamp < old->stamp)) victim = i;
    }
    if (c->lines[victim].valid && c->lines[victim].speculative) ++c->unused_evictions;
    memcpy(c->bytes + victim * W2B_BLOCK_SIZE, data, length);
    c->lines[victim] = (cache_line_t){.base = base, .stamp = ++c->clock,
        .length = length, .valid = true, .speculative = speculative};
}
