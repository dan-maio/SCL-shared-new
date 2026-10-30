# Implemented proposals

This repository is a fork of `SCL-shared` that turns the recommendations in
`SCL-projects-analysis.md` into code where the core can do so without hardware.
Items that need a board, an MPU map or an RTOS remain guidance.

## Status

| Proposal (analysis section) | Status | Where |
|---|---|---|
| Boot-time symbol table builder/view | **Implemented** | `include/scl/symtab-runtime.hpp`, `src/symtab-runtime.cpp` |
| Buffered collector for semihosting (two section reads) | **Implemented** | `collect_buffered()` |
| `collect()` from `.symtab`/`.strtab`, no heap | **Implemented** | same |
| SCLS blob serializer compatible with `gen-symtab` | **Implemented** | `build_blob()` |
| Blob view / lookup / `symbol_source` adapter | **Implemented** | `view`, `source()` |
| Compile-time registration (no host tool, no parser) | **Implemented** | `include/scl/symtab-register.h`, `registered()` |
| External main table for `dynamic-nolib` (non-PIE) | **Implemented** | `fw::dl_set_symtab()` |
| Eager binding switch (no first-call loader code) | **Implemented** | `fw::dl_set_lazy_binding(false)` |
| `scl_sym_entry` overload for XIP newlib | **Implemented** | `xip::newlib::set_symtab(const scl_sym_entry*, ...)` |
| Boot table for `scl::static` | **Implemented** | `symbol_table::add_entries()` |
| Central RTOS lock hooks for every loader | **Implemented** | `include/scl/os-hooks.h`, `src/port/os-hooks.c`; call sites in nolib/newlib/static/XIP/reloc/optimal/overlay |
| External main table for `scl::optimal` | **Implemented** | `xip::nopic::set_main_symbols()` |
| Cache clean/invalidate hooks | **Implemented (weak defaults)** | `include/scl/cache.h`, `src/port/cache.c` |
| Release/publication helper | **Implemented** | `scl_publish_release()` |
| Eager binding documented for RTOS use | **Implemented** | `docs/RTOS-MULTICORE.md` |
| Boot-time resolution guide + sidecar linker example | **Implemented (guide)** | `docs/BOOT-SYMTAB.md`, `cmake/ldscripts/sidecar-elf-example.ld` |
| Compile-time table linker fragment | **Implemented** | `cmake/ldscripts/scl-syms.ld` |
| Host tests for the new APIs | **Implemented** | `tests/symtab_test.cpp`, `tests/linkctx_test.cpp`, `tests/run.sh` |
| Host tools retained and supported | **Implemented** | `tools/gen-symtab.cpp` (build-time table), `common-elf-reader/tools/elf2json.cpp` (AetherELF GUI) |
| Per-core loader windows | **Guidance** | `docs/RTOS-MULTICORE.md` §5 |
| Recursive lock / dependency-loading rule | **Guidance** | `docs/RTOS-MULTICORE.md` §1 |
| Unload quiescence / lifecycle | **Guidance** | `docs/RTOS-MULTICORE.md` §4 |
| MPU programming | **Guidance** | `docs/RTOS-MULTICORE.md` §3/§7 |
| TLS for dynamic loaders | **Not implemented** | `scl::static` TLS remains the supported path |
| Per-core arena allocator | **Not implemented** | `fw::image_arena()` is still global |
| Multi-core hardware validation | **Done for boot-time load** | `SCL-TESTS-shared-new/pico2-pizero-shared` (RP2350, µOS++ SMP) |

## What was changed relative to SCL-shared

- `common-elf-reader` is **vendored** in this fork (the submodule link was
  removed) so the repository is self-contained.
- `symbol_resolver` gained an empty default constructor; `fw::link_context`
  gained a default constructor, `set_symtab()`, and table-first resolution.
- `dynamic-nolib` gained an external main table, eager-binding control, lock
  guards (including its call-time resolvers), and cache maintenance before
  constructors.
- RAM `dynamic-newlib` gained lock guards and cache maintenance before
  constructors.
- `static` gained `symbol_table::add_entries()`, `symtab-runtime.cpp`, and cache
  maintenance before constructors.
- XIP adapters gained lock guards, cache maintenance of RAM data and flash image,
  and (newlib) a `scl_sym_entry` overload.
- `reloc`, `optimal` and `overlay` gained lock guards and cache maintenance.
  `optimal` (through `xip::nopic`) also gained `set_main_symbols()`.
- Weak lock hooks moved from `static/module.cpp` to `src/port/os-hooks.c` so all
  loaders share them.
- `scl-common` now also builds `symtab-runtime`, `os-hooks` and `cache`.
- The unused `cmake/embed-symtab.cmake` was removed; `scl_embed_symtab()` is
  defined by the platform code in the test suites.

## Migrating from SCL-shared

`link_context::resolve()` moved from the header to `src/link-context.cpp`. A
project with its own copy of the old `link-context.cpp` now fails to link with an
undefined reference to `resolve`. Delete the copy and link `scl::self-resolve`.

## Verifying

```sh
./tests/run.sh
```

Expected: `RESULT: SUCCESS` and `all host tests: PASS`.

The tests parse `tests/fixtures/411ce-100Mhz.elf`, build/validate an
SCLS blob, exercise the zero-allocation collect path, check the
`loader::symbol_source` adapter, and verify `SCL_REGISTER_SYM` registration.
`linkctx_test` verifies that `fw::link_context` prefers an externally supplied
table.

Firmware-side code was syntax-checked with:

```sh
arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m4 -mthumb \
  -mfloat-abi=hard -mfpu=fpv4-sp-d16 -fsyntax-only ...
```

The retained host tools were also built and smoke-tested:

```sh
cmake --build <build> --target gen-symtab-host
cmake --build <build> --target elf2json
<build>/tools/elf2json tests/fixtures/411ce-100Mhz.elf /tmp/test.json
```

A full firmware link/run requires an application and board (or the test suites in
`SCL-TESTS-shared-new`), which are outside this repository.
