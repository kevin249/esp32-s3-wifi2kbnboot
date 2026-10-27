#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define W2B_NAME_MAX 180
#define W2B_ISO_PART_SIZE (UINT64_C(1) << 30)
#define W2B_ISO_MAX_PARTS 32
bool media_name_valid(const char *name);
bool media_range_valid(uint64_t capacity, uint64_t offset, uint32_t length);
bool media_iso_sizes(const uint64_t *sizes, size_t count, bool split,
                     uint64_t *total);
bool media_iso_position(uint64_t total, bool split, uint64_t offset,
                        uint32_t wanted, unsigned *part, uint32_t *within,
                        uint32_t *length);
