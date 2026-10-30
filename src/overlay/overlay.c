/**
 * @file overlay.c
 * @brief Implementation of the linker-overlay manager (see overlay/overlay.h).
 */
#include "overlay/overlay.h"
#include "scl/log.h"
#include "scl/barrier.h"
#include "scl/os-hooks.h"
#include "scl/cache.h"
#include <string.h>

void scl_overlay_init(scl_overlay_region* r,
                      const scl_overlay_module* modules, int count) {
    r->modules  = modules;
    r->count    = count;
    r->resident = SCL_OVERLAY_NONE;
}

int scl_overlay_load(scl_overlay_region* r, int index) {
    if (index < 0 || index >= r->count) return -1;

    const scl_overlay_module* m = &r->modules[index];
    scl_loader_lock();
    if (index == r->resident) {
        scl_loader_unlock();
        return 0;
    }
    memcpy(m->vma, m->lma, m->size);
    scl_cache_clean_invalidate(m->vma, m->size);
    scl_memory_barrier();
    r->resident = index;
    scl_loader_unlock();

    scl_log_puts("  [ovl-load] ");
    scl_log_puts(m->name ? m->name : "?");
    scl_log_put_hex(" lma=0x", (uint32_t)(uintptr_t)m->lma);
    scl_log_put_hex(" -> vma=0x", (uint32_t)(uintptr_t)m->vma);
    scl_log_putc('\n');
    return 0;
}

int scl_overlay_current(const scl_overlay_region* r) {
    return r->resident;
}
