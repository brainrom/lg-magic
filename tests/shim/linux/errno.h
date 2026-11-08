#ifndef LGMAGIC_TEST_LINUX_ERRNO_H
#define LGMAGIC_TEST_LINUX_ERRNO_H

/*
 * glibc's <bits/errno.h> includes <linux/errno.h> itself, so use
 * include_next to reach the real UAPI header instead of recursing.
 */
#include_next <linux/errno.h>

#endif
