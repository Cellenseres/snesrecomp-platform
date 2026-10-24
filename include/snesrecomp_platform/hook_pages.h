#ifndef SNESRECOMP_PLATFORM_HOOK_PAGES_H
#define SNESRECOMP_PLATFORM_HOOK_PAGES_H
#include <stdint.h>
/* Conservative membership of mirrored 256-byte code pages. A negative answer
 * skips only the callback lookup; the original opcode and bus accesses run. */
enum { SNESRECOMP_HOOK_PAGE_WORDS = 1024 };
static inline void snesrecomp_hook_pages_clear(uint32_t pages[SNESRECOMP_HOOK_PAGE_WORDS]) {
    for (unsigned i=0;i<SNESRECOMP_HOOK_PAGE_WORDS;++i) pages[i]=0;
}
static inline void snesrecomp_hook_pages_add(uint32_t pages[SNESRECOMP_HOOK_PAGE_WORDS], uint32_t pc) {
    const uint32_t page=(pc&0x7fffffu)>>8;
    pages[page>>5]|=UINT32_C(1)<<(page&31u);
}
static inline int snesrecomp_hook_pages_contains(const uint32_t pages[SNESRECOMP_HOOK_PAGE_WORDS], uint32_t pc) {
    const uint32_t page=(pc&0x7fffffu)>>8;
    return (pages[page>>5]>>(page&31u))&1u;
}
#endif
