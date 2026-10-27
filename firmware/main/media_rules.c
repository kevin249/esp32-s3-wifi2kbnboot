#include "media_rules.h"
#include <string.h>
bool media_name_valid(const char *name) {
  if (!name)
    return false;
  size_t length = strlen(name);
  if (!length || length > W2B_NAME_MAX || name[0] == '.' ||
      name[length - 1] == '.' || name[length - 1] == ' ')
    return false;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    if (*p < 32 || *p == 127 || strchr("/\\:*?\"<>|%", *p))
      return false;
  return true;
}
bool media_range_valid(uint64_t capacity, uint64_t offset, uint32_t length) {
  return length && offset < capacity && length <= capacity - offset;
}
bool media_iso_sizes(const uint64_t *sizes, size_t count, bool split,
                     uint64_t *total) {
  if (!sizes || !total || !count || count > W2B_ISO_MAX_PARTS ||
      (!split && count != 1))
    return false;
  uint64_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    uint64_t single_limit = UINT32_MAX;
#ifdef W2B_EXFAT_ENABLED
    single_limit = (uint64_t)UINT32_MAX * 2048;
#endif
    if (!sizes[i] || sizes[i] % 2048 ||
        sizes[i] > (split ? W2B_ISO_PART_SIZE : single_limit))
      return false;
    if (split && i + 1 < count && sizes[i] != W2B_ISO_PART_SIZE)
      return false;
    sum += sizes[i];
  }
  *total = sum;
  return true;
}
bool media_iso_position(uint64_t total, bool split, uint64_t offset,
                        uint32_t wanted, unsigned *part, uint64_t *within,
                        uint32_t *length) {
  if (!media_range_valid(total, offset, wanted))
    return false;
  *part = split ? (unsigned)(offset / W2B_ISO_PART_SIZE) : 0;
  *within = split ? offset % W2B_ISO_PART_SIZE : offset;
  uint64_t available = split ? W2B_ISO_PART_SIZE - *within : total - offset;
  *length = wanted < available ? wanted : (uint32_t)available;
  return true;
}
