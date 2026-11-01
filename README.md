# SCL — Shared-Code Loader for Cortex-M (ARM32) — proposals fork

This repository starts from `SCL-shared` and implements the boot-time,
RTOS and multi-core proposals analysed in `SCL-projects-analysis.md`.

**New in this fork**

- **Boot-time symbol resolution** (`scl/symtab-runtime.hpp`): build a symbol
  table from a sidecar ELF at runtime with `collect()`/`build_blob()`/`view`,
  semihosting or the compile-time `SCL_REGISTER_SYM()` section.
- **Host tools retained**: `tools/gen-symtab.cpp` (build-time table for hosted
  `dynamic-newlib`) and `common-elf-reader/tools/elf2json.cpp` (AetherELF GUI)
  are still built and supported. The boot-time paths are an alternative, not a
  replacement.
- **External main table for `dynamic-nolib`**: `fw::dl_set_symtab()` removes
  the non-PIE `from_self()` requirement.
- **Eager binding switch**: `fw::dl_set_lazy_binding(false)` keeps loader code
  out of the first call path.
- **External main table for `scl::optimal`**:
  `xip::nopic::set_main_symbols()`.
- **RTOS/multi-core hooks**: shared `scl_loader_lock/unlock` (must be recursive),
  cache clean/invalidate hooks, `scl_publish_release()`, lock guards and cache
  maintenance on every loader.
- **Static table helper**: `symbol_table::add_entries()`.
- **Main tables for the XIP adapters**: `xip::stat::set_symtab()`, `xip::nolib::set_symtab()`,
  so none of them needs a `-pie` main.
- **C++ exceptions in XIP modules**: unwind tables are registered, and `flash_base` on
  `patch_main_imports()` / `bind_host_imports()` prelinks what catch tables read.
- **Missing imports reported by name**: `first_missing_import()` on every loader that allows
  unresolved imports. Call it after loading the set; the load itself still succeeds.
- **Host tests** for the new APIs: `./tests/run.sh`.

See `docs/IMPLEMENTED-PROPOSALS.md`, `docs/BOOT-SYMTAB.md` and
`docs/RTOS-MULTICORE.md`. Items that require a board, an MPU map or an RTOS are
documented as guidance rather than claimed as implemented.

`common-elf-reader` is **vendored** in this fork (no git submodule), so the
repository is self-contained.

## Loader cases

| Case | Target | Module built | Runs code from | `r10` |
|---|---|---|---|---|
| **relocatable** | `scl::reloc` | `ET_REL` (`-fno-pic -r`) | RAM (arena) | free |
| **static-full-ram** | `scl::static` | `-fPIC .so` | RAM (buffer) | free |
| **static** (XIP) | `scl::xip-static` | `-fPIC` split `.so` | flash (XIP) | used |
| **optimal** (XIP) | `scl::optimal` | `-fno-pic` fixed windows | flash-window | free |
| **dynamic** (no libc) | `scl::dynamic-nolib` | `-fPIC .so` | RAM (buffer) | used |
| **dynamic** (newlib) | `scl::dynamic-newlib` | `-fPIC .so`, POSIX `dlopen` | RAM (buffer) | used |
| **overlay** | `scl::overlay` | linked with the firmware | one SRAM window | free |

Everything is an INTERFACE "source-code library": the loader `.cpp` compile into
*your* firmware with *your* flags; there are no `.a` archives.

`scl::dynamic-newlib` needs `-DSCL_HOSTED=ON`.

Supporting targets: `scl::common` (parser/resolver/relocation core plus the new
symtab/cache/OS-hook sources), `scl::xip-common` (XIP primitives),
`scl::loader-nopic` (the no-pic loader under `scl::optimal`),
`scl::xip-dynamic-nolib` and `scl::xip-dynamic-newlib` (XIP adapters for the
dynamic loaders), and `scl::self-resolve` (resolve a module's imports against
main's own `.dynsym` via `link_context::from_self()`).

## Use it

```sh
git clone <url>/SCL-shared-new.git
```
```cmake
add_subdirectory(SCL-shared-new scl)
target_link_libraries(my_fw PRIVATE scl::optimal)   # or another scl::* target
```
or pick one with the option and link the generic alias:
```sh
cmake -DSCL_LOADER=optimal ...
```
```cmake
target_link_libraries(my_fw PRIVATE scl::loader)
```

## Boot-time symbol table sketch

```cpp
loader::memory_byte_source src{sidecar_elf_span};
auto img = loader::elf_image::parse(src);
std::size_t n = scl::symtab::collect(*img, entries, cap, names, names_cap);
fw::dl_set_symtab(entries, n);
```

`docs/BOOT-SYMTAB.md` has the sidecar and `SCL_REGISTER_SYM` variants.

## Tests

```sh
./tests/run.sh          # host g++ (parses an ARM ELF as data)
```

Firmware sources are cross-compiled with the ARM toolchain (the xPack
`arm-none-eabi-g++`), not with the host compiler.

## Writing flash with both cores running

On the RP2350, link `scl::rp2350-flash`. While one core erases or programs, it parks the other in
SRAM. It is optional, and is not part of any loader. `include/scl/rp2350-flash.h` has the API and
its rules.

## Tested on

| Board | CPU | Where | Result (2026-10-30) |
|---|---|---|---|
| WeAct F411CE + W25Q64 | Cortex-M4 | `SCL-TESTS-shared-new`, `SCL-EXAMPLES-shared-new` | 22 tests and 25 examples pass at `9f1320f` |
| Luckfox Lyra B + SD card | 3x Cortex-A7, µOS++ SMP | `SCL-TESTS-shared-new/luckfox-lyra-shared`, `SCL-EXAMPLES-shared-new/luckfox-lyra` | 48 tests and 67 example firmwares pass; tests at `86c83d2`, examples at `1eafdbe` (2026-11-01) |
| Pi-Zero RP2350B + PSRAM | 2x Cortex-M33, µOS++ SMP | `SCL-TESTS-shared-new/pico2-pizero-shared`, `SCL-EXAMPLES-shared-new/pizero` | 77 tests and 76 example firmwares pass at `86c83d2` |

## Further documentation

The loader-model guides and test benches live in the sibling repo
`SCL-TESTS-shared-new` (`docs/LOADER-CHOICE.md`, `docs/import_in_project_full.md`,
`docs/resolving-module-imports.md`). Worked WeAct examples are in
`SCL-EXAMPLES-shared`, which still uses the original `SCL-shared`.
