#ifndef LGMAGIC_TEST_LINUX_BUILD_BUG_H
#define LGMAGIC_TEST_LINUX_BUILD_BUG_H

#define static_assert(expr, msg) _Static_assert(expr, msg)

#endif
