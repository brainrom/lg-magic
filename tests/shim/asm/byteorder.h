#ifndef LGMAGIC_TEST_ASM_BYTEORDER_H
#define LGMAGIC_TEST_ASM_BYTEORDER_H

#include <linux/types.h>

/* The tests only run on little-endian hosts. */
#define le16_to_cpu(value) (value)
#define le32_to_cpu(value) (value)

#endif
