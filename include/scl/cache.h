/**
 * @file cache.h
 * @brief Cache-maintenance and release-publication hooks for loaded images.
 *
 * Defaults are weak: barriers only. On a Cortex-M7 (or any cached core) provide
 * strong implementations with CMSIS SCB_CleanDCache_by_Addr() /
 * SCB_InvalidateICache_by_Addr(), or mark the loaded windows non-cacheable in
 * the MPU. On Cortex-M4 the defaults are correct (no data cache).
 *
 * scl_cache_clean() writes data back to memory; scl_cache_invalidate() discards
 * stale instructions. The default scl_cache_clean_invalidate() calls both, so
 * those two are the only hooks a port has to override.
 */
#ifndef SCL_CACHE_H
#define SCL_CACHE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void scl_cache_clean(const void* addr, size_t len);
void scl_cache_invalidate(const void* addr, size_t len);
void scl_cache_clean_invalidate(const void* addr, size_t len);

/// Release fence + event for publishing a loaded module to another core/task.
void scl_publish_release(void);

#ifdef __cplusplus
}
#endif

#endif /* SCL_CACHE_H */
