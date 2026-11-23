#ifndef LGMAGIC_TEST_LINUX_PRINTK_H
#define LGMAGIC_TEST_LINUX_PRINTK_H

/*
 * The host tests do not need the kernel log.  Keep the parser's deprecation
 * message a no-op here; the kernel build uses the real printk.h.
 */
#define pr_warn_once(fmt, ...) do { } while (0)

#endif
