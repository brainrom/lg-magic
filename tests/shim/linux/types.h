#ifndef LGMAGIC_TEST_LINUX_TYPES_H
#define LGMAGIC_TEST_LINUX_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint16_t __le16;
typedef uint32_t __le32;

/* The tests only run on little-endian hosts. */
#define le16_to_cpu(value) (value)
#define le32_to_cpu(value) (value)

#endif
