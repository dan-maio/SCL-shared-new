/** @file cache.c @brief Weak cache hooks: barriers by default, override on M7. */
#include "scl/cache.h"
#include "scl/barrier.h"

__attribute__((weak)) void scl_cache_clean(const void* addr, size_t len) {
    (void)addr; (void)len;
    scl_memory_barrier();
}

__attribute__((weak)) void scl_cache_invalidate(const void* addr, size_t len) {
    (void)addr; (void)len;
    scl_memory_barrier();
}

__attribute__((weak)) void scl_cache_clean_invalidate(const void* addr, size_t len) {
    (void)addr; (void)len;
    scl_memory_barrier();
}

__attribute__((weak)) void scl_publish_release(void) {
#if defined(__arm__) || defined(__thumb__)
    __asm volatile("dmb 0xf" ::: "memory");
    __asm volatile("sev" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}
