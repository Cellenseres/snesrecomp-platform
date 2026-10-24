#ifndef SNESRECOMP_PLATFORM_ABI_H
#define SNESRECOMP_PLATFORM_ABI_H

/* Keep public C ABI assertions usable by both C11 and C++ consumers. */
#ifdef __cplusplus
#define SNESRECOMP_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define SNESRECOMP_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

#endif
