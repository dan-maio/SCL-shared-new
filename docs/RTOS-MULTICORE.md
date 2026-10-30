# RTOS and multi-core porting guide

This fork adds the hooks and options proposed in `SCL-projects-analysis.md` §6.
The default behaviour on bare-metal Cortex-M4 is unchanged: all new hooks are
weak no-ops unless the application overrides them.

## 1. Serialize loader entry points

Every loader now calls the same weak hooks:

```c
#include "scl/os-hooks.h"

void scl_loader_lock(void);
void scl_loader_unlock(void);
```

Covered entry points:

- `scl::static` — `Module::load_*`, `Module::unload`
- `scl::dynamic-nolib` — `dlopen_source`, `dlsym`, `dlclose`, and the call-time
  resolvers `dl_fixup` and `dl_cross_lookup`
- `scl::dynamic-newlib` — `load_from_source`, `lookup`, `unload`
- XIP adapters (`xip::static`, `xip::nolib`, `xip::newlib`) — load/unload and
  `xip_cross_lookup`
- `scl::reloc` — `load`, `load_set`
- `scl::optimal` — `load_and_link`, and in `xip::nopic`: `load`, `resolve_any`,
  `resolve_all`, `patch_main`, `unload`
- `scl::overlay` — `scl_overlay_load` (see the warning below)

FreeRTOS example:

```c
#include "scl/os-hooks.h"
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t g_scl_lock;

void scl_loader_lock(void)   { xSemaphoreTakeRecursive(g_scl_lock, portMAX_DELAY); }
void scl_loader_unlock(void) { xSemaphoreGiveRecursive(g_scl_lock); }
```

The mutex **must be recursive**: `optimal::load_and_link()` calls
`xip::nopic::load()`, dependency loading (`Module::dep_loader`) can re-enter the
loader, and constructors may call back into it. On SMP add a hardware spinlock
(or a mutex that disables interrupts and takes the cross-core lock) under the
RTOS mutex.

These hooks serialize *load/unload*. They **are held while constructors and
destructors run**, so a constructor must not wait on a task that needs the
loader. They are not held during normal calls into a loaded module, with one
exception: a lazy or cross-call slot takes the lock while it resolves. Never
reach such a slot from an ISR, because the RTOS mutex cannot be taken there.

## 2. Bind eagerly

`dynamic-nolib` re-arms `.rel.dyn` JUMP_SLOTs for lazy resolution by default.
Disable it before loading when tasks/interrupts may call the module:

```cpp
fw::dl_set_lazy_binding(false);
fw::dl_set_symtab(main_symtab, main_symtab_count);  // non-PIE main
```

With lazy binding off, every slot is bound at load and a resolved call does not
enter loader code. `.rel.plt` slots are eager in either mode.

`fw::dl_enable_cross_call(true)` is the exception: it routes external slots to
`dl_cross_call`, which resolves the target on every call. Leave it off in code
that must stay out of the loader. The XIP adapters use a similar trampoline for
calls between modules (`xip_cross_lookup`).

## 3. Cache maintenance and publication

```c
#include "scl/cache.h"

void scl_cache_clean(const void* addr, size_t len);
void scl_cache_invalidate(const void* addr, size_t len);
void scl_cache_clean_invalidate(const void* addr, size_t len);
void scl_publish_release(void);   // DMB + SEV
```

Defaults are barriers only, and the default `scl_cache_clean_invalidate()` calls
the other two hooks. Every loader calls these hooks before running constructors:

| Loader | Hook call |
|---|---|
| `static`, `reloc`, `dynamic-nolib`, `dynamic-newlib` | `scl_cache_clean_invalidate()` on the RAM image |
| `xip::static`, `xip::nolib`, `xip::newlib` | `scl_cache_clean_invalidate()` on the RAM data/GOT, `scl_cache_invalidate()` on the flash image |
| `xip::nopic` (`optimal`) | `scl_cache_clean_invalidate()` on both windows, again after `resolve_all()`; `scl_cache_clean()` on each main GOT slot patched by `patch_main()` |
| `overlay` | `scl_cache_clean_invalidate()` on the window after each swap |

On a cached core (Cortex-M7) override the two base hooks with CMSIS. Overriding
`scl_cache_clean_invalidate()` as well is optional:

```c
void scl_cache_clean(const void* a, size_t n) {
    SCB_CleanDCache_by_Addr((uint32_t*)a, (int32_t)n);
}
void scl_cache_invalidate(const void* a, size_t n) {
    SCB_InvalidateICache_by_Addr((uint32_t*)a, (int32_t)n);
}
```

CMSIS `*_by_Addr` expects 32-byte aligned addresses and sizes; round the loaded
window outward, or mark the window non-cacheable in the MPU instead.

After publishing a module to another core, call `scl_publish_release()` (or use
a release/acquire pair on a shared flag) and wake the other core with an IPI/SEV.

## 4. Recommended handoff

```
LOAD PHASE (one context, scheduler stopped)
  dl_set_lazy_binding(false)
  install main table (dl_set_symtab / set_symtab / add_entries)
  load images, run constructors
  scl_cache_clean_invalidate(image)
  scl_publish_release()
  start other cores / scheduler
EXECUTE PHASE
  direct calls, no loader in the path
  unload only after every task, ISR and core is quiesced
```

## 5. Per-core memory

The loader windows are linker singletons:

- `_simage_arena_start/_end` (RAM arena)
- `_sxip_text/_sxip_data` (no-PIC fixed windows)
- the overlay window

In SMP, partition them per core (separate linker regions and symbols per image),
or run a **single loader core** and request loads from the other core. In AMP,
each core has its own image, its own windows and its own symbol table; a module
loaded by core A is not visible to core B. Cross-core calls need shared memory
plus an IPC layer, not SCL symbol resolution.

## 6. Loader readiness

| Loader | RTOS-ready now | Multi-core caveat |
|---|---|---|
| `static` | Lock hooks + existing TLS | Cross-core lock; cache maintenance on M7 |
| `dynamic-nolib` | Lock hooks, eager binding, external table | Global handle pool and arena are still shared; one loader context |
| `dynamic-newlib` | Lock hooks in the RAM loader and XIP adapter; no lazy binding | Hosted heap and global error state remain |
| `reloc` | Lock hooks; caller-owned state | Give each task/core its own arena |
| `optimal` | Lock hooks; no lazy binding | Partition windows per core |
| `overlay` | Lock hook | The lock does not quiesce execution; never swap while another context runs from the window |

## 7. Remaining gaps (documented, not claimed)

- `fw::image_arena()` is still a single global allocator; per-core arenas require
  separate linker regions and a wrapper in the application.
- Dynamic loaders still do not implement `R_ARM_TLS_*`; use `scl::static` for
  `thread_local` modules.
- MPU region programming is application responsibility.
- `dlerror` state in the hosted facade is global, not thread-local.
- `SCL-TESTS-shared-new/pico2-pizero-shared` tests boot-time loading on a dual-core
  RP2350. Loading or unloading while both cores run is untested.
- Load after static constructors. The loaders' globals are C++ objects, so a load from
  an earlier constructor is wiped when they are constructed.
- A missing import does not fail the load, because a later module may provide it. Call
  the loader's `first_missing_import()` after loading the set: otherwise the first call
  faults, with the name logged by the call-time lookup.
- Cross-call lookups take the loader lock on every call, on every core. Use a real
  recursive lock with cross-call on SMP.
- Exceptions: this toolchain's `libstdc++` is `Thread model: single`, so one global state is
  shared. Throw under a lock. Every loader except `scl::overlay` registers unwind tables. Link the full `libsupc++` with `--specs=nano.specs`, keep `.ARM.exidx` as its own
  output section, and pass `flash_base` when provisioning a flash module.
- A PLT in a flash module reads a GOT in flash, which the loader cannot patch. Either
  prelink the image before programming it (`patch_main_imports()`, `bind_host_imports()`),
  or build with `-mlong-calls`, so imports go through the RAM GOT via `r10`.
- RP2350 has no global exclusive monitor: `LDREX`/`STREX` in a module do not protect
  data shared across cores. Import a lock from main instead.
- Cache overrides are examples; the core defaults only issue barriers.
- Writing flash with both cores running needs the other core out of flash, and the loader does not
  do this. `SCL-EXAMPLES-shared-new/pizero/` (`flashsmp`) shows it on the RP2350: a forced TIMER1
  interrupt parks the other core in SRAM. Core 1 must enable coprocessor 7 first, because the
  bootrom's flash code uses it.
