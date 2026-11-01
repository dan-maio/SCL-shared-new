---
title: "Using shared libraries with SCL-shared-new"
subtitle: "Every loader method, where the ELF parser is used, and how semihosting fits"
author: "SCL / SCL-shared-new"
date: "2026-11-01"
---

# 1. The short version

SCL loads a normal **ELF32 little-endian ARM** shared library into a running
firmware.  There are nine ways to do it, because boards differ in where code may
run from, how much RAM they have, whether they have an MMU/cache, and whether the
application may unload a library again.

Every method follows the same three-step shape:

```text
   module bytes  ──►  loader  ──►  memory image  ──►  the app calls it
   (ELF32 ARM)         │           code + data
                       │
                       ├─ copies sections/segments
                       ├─ applies relocations
                       ├─ resolves imported symbols
                       ├─ registers .ARM.exidx
                       └─ runs .init_array
```

The loader core **never opens a file and never issues a semihosting trap**.  The
application supplies the module either as a `std::span<const std::byte>` or as a
`loader::byte_source`.  That is the seam where semihosting, SD cards, flash
archives, or a host test all plug in.  (One convenience, `scl::static`'s
`Module::load_file()`, can open a file through `fd_byte_source`; its `load_buf()`
does not.)

The one rule that explains most of the design: **do the I/O once, then load from
memory**.  Semihosting is a debugger trap per call; it is excellent for fetching a
module at boot and terrible inside a relocation loop.

The parser (`common-elf-reader`, now the `elf32_read_parse-new.git` submodule) is
the component that understands the ELF file.  Every method except two uses it:

* `scl::reloc` parses `ET_REL` objects itself, using the parser's types and
  helpers but not `elf_image::parse`.
* `scl::overlay` does no parsing at load time; the host linker already did the
  work.

Everything else — static, XIP, optimal, and the dynamic loaders — starts with
`loader::elf_image::parse`.

---

# 2. The pieces, in plain English

## 2.1 The module

A module is an ELF32 ARM file produced by the normal GNU toolchain:

* `ET_REL` — a relocatable `.o` (for `scl::reloc`).
* `ET_DYN` — a shared object, `.so` (for static, XIP, optimal, dynamic).
* Linked with the firmware — an overlay module, not a file at all.

The file contains:

* **segments** (`PT_LOAD`) — what gets copied into memory;
* **sections** — code, read-only data, writable data, `.got`, `.dynsym`,
  `.dynstr`, `.rel.dyn`, `.rel.plt`, `.init_array`, `.ARM.exidx`, `.ARM.extab`;
* **symbols** — what the module exports and what it imports;
* **relocations** — the places that must be fixed after the copy.

The ELF parser is only interested in reading these structures correctly and
safely.  The loader is interested in turning them into a working image.

## 2.2 The byte source

`loader::byte_source` is a tiny interface:

```cpp
class byte_source {
  virtual std::size_t size() const noexcept = 0;
  virtual result<void> read_at(std::size_t offset,
                               std::span<std::byte> out) noexcept = 0;
};
```

It hides *where the bytes come from*.  Two implementations ship with the parser:

| Source | Use |
|---|---|
| `loader::memory_byte_source` | the file is already in RAM (the normal case) |
| `loader::fd_byte_source` | read from a POSIX/semihosting file descriptor |

A loader may also accept the bytes directly as a `std::span`; it then reads them
with bounds-checked helpers.  Either way, the loader core contains no `open()`,
no `read()`, and no semihosting instruction.

## 2.3 The parser

`common-elf-reader` (the `elf32_read_parse-new.git` submodule at `f650889`)
provides:

| Header | What it gives the loader |
|---|---|
| `loader/elf-types.hpp` | ELF32-LE structs, relocation numbers, symbol bits, ARM/Thumb helpers |
| `loader/elf-image.hpp` | `elf_image::parse`, `header()`, `program_header()`, `section_header()`, `find_section()`, `image_size()`, `image_base()`, `collect_relocations()` |
| `loader/detail/bytes.hpp` | bounds-checked `load_word`/`store_word`/`name_at`/`for_each_rel` |
| `loader/arm-reloc.hpp` | A32/T32 relocation encoder/decoder (`CALL`, `JUMP24`, `MOVW/MOVT`, `THM_*`, ...) |
| `loader/arm-attributes.hpp` | `.ARM.attributes` (EABI, VFP args, CPU profile) |
| `loader/arm-abi.hpp` | hard/soft-float ABI check |
| `loader/sources/...` | memory and fd byte sources |
| `loader/symbol-resolver.hpp` | `.dynsym`/`.hash` name lookup for the dynamic loaders |

The parser validates the file header, class, data encoding, machine, EABI
version, and header sizes.  It does **not** decide where code should run; that is
the loader's job.

## 2.4 The loader

The loader is a set of C++ sources compiled into *your* firmware (these are
INTERFACE "source libraries", not `.a` archives).  A loader:

1. parses/validates the module;
2. decides the memory placement;
3. copies code and data;
4. applies relocations;
5. resolves imports;
6. registers exception tables;
7. runs constructors;
8. returns a handle or module object.

## 2.5 The symbol source

A module imports symbols from the firmware ("main") and from other modules.  SCL
supports several ways to provide main's symbols:

| Method | When to use |
|---|---|
| main's own `.dynsym` (`-pie -Wl,-E`) | the firmware is linked `-pie`; `link_context::from_self()` |
| an external `scl_sym_entry` table | non-PIE firmware; table built at boot or compile time |
| `SCL_REGISTER_SYM()` | no ELF file and no boot-time parser; the table is in a linker section |
| a sidecar ELF / firmware ELF | the platform reads `.symtab` once (semihosting, flash) and builds entries |
| a build-time table (`gen-symtab`) | hosted `dynamic-newlib`, no runtime parser for main |

The loader's own exports are found through the module's `.dynsym`/`.hash` (or a
linear scan) and are available to later loads.

## 2.6 The hooks

Every loader calls these weak hooks:

| Hook | Purpose |
|---|---|
| `scl_loader_lock()` / `scl_loader_unlock()` | recursive lock around load/unload; make it an RTOS mutex or a cross-core spinlock |
| `scl_cache_clean()` / `scl_cache_invalidate()` | write back code/data and discard stale instructions (Cortex-A/M7) |
| `scl_publish_release()` | release fence + event before another core/thread starts using the module |
| `scl_log_puts()` / `scl_log_putc()` | diagnostics; default is silent; the app may route to UART or semihosting |

The core never prints to a UART or a semihosting console by itself.

---

# 3. The four module shapes

The build command decides which loader can consume the module.

| Shape | Build | Typical consumer | Key property |
|---|---|---|---|
| **Relocatable object** | `-fno-pic -mlong-calls -r` (`.rel.o`) | `scl::reloc` | section headers + `.symtab`; no GOT/PLT |
| **PIC shared object** | `-fPIC -shared` (`.so`, `.pic.so`) | `scl::static`, `scl::dynamic-nolib`, `scl::dynamic-newlib` | GOT/PLT; text is position-independent |
| **Split PIC shared object** | `-fPIC -shared -T xip-split.ld` | `scl::xip-static`, `scl::xip-dynamic-*` | one R-X segment and one RW segment at known VAs |
| **No-pic fixed-address object** | `-fno-pic -mlong-calls -shared -T nopic-split.ld` | `scl::optimal` | linked at fixed windows; no GOT, no r10 |
| **Overlay module** | ordinary `.o`, linked into the firmware at a shared VMA with `AT>RAM` | `scl::overlay` | no relocations at load; only one resident at a time |

Two practical warnings:

* `-mlong-calls` does **not** guarantee literal-pool `ABS32` relocations.  ARM
  `-O0`/`-O2` may emit `MOVW/MOVT`; `-Os` often uses `ABS32`.  The loader's
  relocation engine supports both.
* For the no-pic/reloc shapes, the compiler must not assume a GOT or r10.  Use
  `-fno-pic` (and usually `-mlong-calls`).

---

# 4. The nine methods

## 4.0 How the application links a loader

The loaders are CMake **INTERFACE source libraries**: their `.cpp` files compile
into your firmware with your flags.  There is no `.a` archive to link.

```cmake
add_subdirectory(SCL-shared scl)

target_link_libraries(my_firmware PRIVATE scl::reloc)
# or scl::static, scl::optimal, scl::xip-static, scl::dynamic-nolib,
#    scl::dynamic-newlib, scl::xip-dynamic-nolib, scl::xip-dynamic-newlib,
#    scl::overlay
```

Or pick one case and link the generic alias:

```sh
cmake -DSCL_LOADER=optimal ...
```

```cmake
target_link_libraries(my_firmware PRIVATE scl::loader)
```

`SCL_HOSTED=ON` is required for `scl::dynamic-newlib` (and for
`elf_image::collect_relocations` / the hosted relocation planner).  The other
loaders are freestanding.

The application must also provide the hooks it wants: `scl_loader_lock/unlock`,
`scl_cache_clean/invalidate`, `scl_publish_release`, and optionally
`scl_log_puts/putc`.  Weak defaults exist: barriers-only cache hooks, a no-op log,
and a no-op lock.  A single-core bare-metal app can start with the defaults; an
RTOS or multi-core app should not.

The names below are the CMake targets, not a ranking.  "Main" means the firmware
that contains the loader.

## 4.1 `scl::reloc` — `ET_REL` in RAM

**Best for:** the smallest, simplest loader; one or a few modules loaded into a
RAM arena; no unload requirement; modules that import from main or from each
other.

**Module:** `-fno-pic -mlong-calls -r`, an `ET_REL` `.o` with `.symtab`,
`.strtab`, `.text`, `.data`, `.bss`, `.init_array`.

**Placement:** every `SHF_ALLOC` section is copied into a caller-supplied arena,
packed and aligned.  The loader records each section's runtime base.  For a set of
mutually dependent modules, `load_set()` places all of them first, then resolves
all relocations, so A→B and B→A both work.

**Relocations:** `R_ARM_ABS32`, `TARGET1`, `TARGET2`, `REL32`, `NONE`, `PREL31`,
`THM_CALL`, and the A32 set (`CALL`, `JUMP24`, `PC24`, `PLT32`, `MOVW/MOVT`,
`THM_MOVW/MOVT`, `THM_JUMP24`, `V4BX`, ...).  The shared `loader::arm::apply()`
encoder is used; it preserves bit 0 and reports range/interworking errors.

**Symbols:** defined symbols are `section_base + st_value`; the `st_value` bit 0
is preserved (Thumb odd, ARM even).  Undefined symbols go to the
`loader::symbol_source` the app supplies.  `sym()` returns the exported address.

**Constructors/unwind:** `.init_array` is called after relocation; `.ARM.exidx`
sections are merged, sorted and registered for the unwinder.

**API:** `reloc::load(image, arena, resolve)`, `reloc::load_set(...)`,
`reloc::sym(...)`, `module::first_missing_import()`.

**Limits:** the arena is fixed at build time; a module must fit its slot; no
unload (the next load reuses the arena); `ET_REL` only.

**Parser use:** **manual** `ET_REL` parsing.  It does not call
`elf_image::parse`; it uses `loader/elf-types.hpp`, `loader/detail/bytes.hpp`,
and `loader/arm-reloc.hpp`.  See §5.

## 4.2 `scl::static` — a PIC `.so` copied whole into RAM

**Best for:** normal Linux-style shared objects, unload/reload, byte-exact revert,
simple direct calls after load.

**Module:** `-fPIC -shared` (`.so`/`.pic.so`) with `.dynsym`, `.dynstr`, `.hash`,
`.rel.dyn`, `.rel.plt`, `.got`, `.init_array`, `.fini_array`, `.ARM.exidx`.

**Placement:** the whole image is copied into a caller-provided RAM buffer
(`exec_buf`).  The loader patches the module's GOT and main's GOT.  Unload
restores every word written into main from a journal, runs `.fini_array`, and
releases the buffer.

**Relocations:** `RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS32`, `REL32`, `COPY`,
plus TLS (`TPOFF32`/`DTPOFF32`/`DTPMOD32`) and the exidx/personality fix-ups.

**Symbols:** `elf_loader::symbol_table` (the app's table) plus main's
`.dynsym`; exports are published for later loads.  `sym()` does a runtime lookup.

**Constructors/unwind:** `.init_array` after patching; `.ARM.exidx` registered;
`.fini_array` on unload.

**API:** `scl::Module::load_file(path, buf, size, syms)`,
`load_buf(bytes, ...)`, `image_size(path)`, `sym(name)`, `unload()`,
`first_missing_import()`.

**Limits:** needs a RAM buffer large enough for the whole module; `load_file`
uses the C library's file calls (semihosting/POSIX) and an `fd_byte_source`;
`load_buf` avoids file I/O.

**Parser use:** an `elf_loader::elf_reader` wrapper over
`loader::elf_image::parse`, `program_header()`, `section_header()`,
`image_size()`, `source().read_at()`, with `fd_byte_source` or
`memory_byte_source`.

## 4.3 `scl::xip-static` — split PIC `.so`, code in place, data in RAM

**Best for:** real execute-in-place flash (RP2350), or a DRAM image region that
must behave like XIP; modules with a GOT and r10; no runtime `dlopen` service.

**Module:** a PIC `.so` linked with a two-segment script (`xip-split.ld`): one
R-X segment and one RW segment at known VAs.

**Placement:** the R-X segment is copied to the flash/image address (or runs
directly from flash); the RW segment is copied to RAM.  `r10` is the module's
`got_base`.  `bind_host_imports()` must pre-link external function imports into
the image before it is programmed to flash; data imports are patched in the RAM
GOT at load.

**Relocations:** the split-base relocation applier:
`RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS32`, `REL32`, `COPY`, plus exidx.  Text
relocations that would write to read-only flash are rejected (or must be resolved
by the linker).

**Symbols:** main's `.dynsym` by default (`-pie -Wl,-E`), or an external
`scl_sym_entry` table (`set_symtab`).  Cross-library calls use the shared
`xip_cross_call` trampoline (r10 swap).

**API:** `xip::stat::load(blob, SModule&, patch_host_got)`,
`bind_host_imports(image, flash_base)`, `resolve(module, name)`, `unload`.

**Limits:** requires a split image and a linker script; `bind_host_imports` must
run before flashing; text must be immutable for real flash.

**Parser use:** `memory_byte_source` + `elf_image::parse`,
`program_header()` (via `SegmentMap`), `find_section()`, `source().read_at()`;
`bind_host_imports()` parses again and walks `.rel.plt`/`.rel.dyn` with
`for_each_rel`; the common `xip::detail` helpers also call `elf_image::parse`.

## 4.4 `scl::optimal` — no-pic, fixed windows, no r10

**Best for:** the simplest XIP/no-pic model; Cortex-M0/M3 and Cortex-A; several
modules resident in fixed sub-windows; direct function pointers (no `pic_call_*`).

**Module:** `-fno-pic -mlong-calls -shared -T nopic-split.ld`, linked at the
firmware's `_sxip_text_*` / `_sxip_data_*` windows.

**Placement:** a 0-bias copy: text to its link VA in the text window, data to its
link VA in the data window, `.bss` zeroed.

**Relocations:** `RELATIVE` is left alone (0 bias); `ABS32`/`REL32` are applied;
A32/T32 `CALL`, `JUMP24`, `MOVW/MOVT`, `PREL31`, `V4BX` are applied by the shared
engine.  Imports are resolved against loaded modules first, then main.

**Symbols:** `resolve_any()`; `patch_main()` can also patch main's GOT so the app
can call the module by name.  `set_main_symbols()` supplies an external table.

**API:** `xip::nopic::load(blob, PModule&)`, `resolve`, `resolve_any`,
`resolve_all` (for cycles), `patch_main`, `unload`, `first_missing_import`.

**Limits:** the image must be linked at the windows; modules must fit their
sub-slots; `-fno-pic` and `-mlong-calls` are required.

**Parser use:** `memory_byte_source` + `elf_image::parse`, `program_header()`,
`find_section()` (via `sec()`), direct blob spans; the relocation engine
(`loader::arm::apply`) handles all types.

## 4.5 `scl::dynamic-nolib` — freestanding `dlopen`

**Best for:** normal PIC `.so` modules, `dlopen`-style handles, no libc/POSIX,
RTOS/multi-core; eager binding is available for RT use.

**Module:** `-fPIC -shared` (`.so`), with `.dynsym`, `.dynstr`, `.hash`,
`.rel.dyn`, `.rel.plt`, `.got`, `.init_array`, `.ARM.exidx`.

**Placement:** `dlopen_source(byte_source&)` parses the source, allocates the image
in `fw::image_arena()`, copies `PT_LOAD` segments, reads the section header table
and `.shstrtab` once (a section cache), and applies relocations.

**Relocations:** `RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS32`, `REL32`, `COPY`.
Imports resolve through a chain: already loaded libraries → main's `.dynsym`
(or `dl_set_symtab` table).  Optional lazy PLT and cross-call trampolines exist;
disable lazy binding for RT/multi-core.

**Symbols:** `fw::dlsym(handle, name)`, `dl::dlsym`; `dl_first_missing_import()`.

**Constructors/unwind:** `.init_array` after relocation; `.ARM.exidx` registered;
`dlclose` runs `.fini_array` and frees the image.

**API:** `fw::dlopen_source(src, name)`, `dlsym`, `dlclose`,
`dl_set_symtab`, `dl_set_lazy_binding(false)`, `dl_enable_cross_call`.

**Limits:** needs r10 (`-msingle-pic-base -mpic-register=r10`); the image must fit
the arena; lazy binding runs loader code on the first call (not RT-safe).

**Parser use:** `elf_image::parse`, `image_size`, `program_header`; a custom
section-header cache instead of `find_section` per lookup; `source().read_at` for
the initial copy.  It does **not** use `collect_relocations` (it walks
`.rel.dyn`/`.rel.plt` from the copied image).

## 4.6 `scl::dynamic-newlib` — hosted POSIX `dlopen`

**Best for:** the most complete Linux-like loader; link-time binding + runtime
GOT patching with a journaled revert; `SCL_HOSTED=ON`.

**Module:** `-fPIC -shared` (`.pic.so`).

**Placement:** `dynamic_linker::load_from_source(byte_source&)` parses the image,
allocates the image plus journal capacity, copies segments, builds a relocation
plan, applies it, patches main's GOT, and runs constructors.  Unload reverts the
journal and frees the image.

**Relocations:** a `loader::relocator` plan: `RELATIVE`, `GLOB_DAT`,
`JUMP_SLOT`, `ABS32`, `REL32`, `COPY`; pending imports are re-patched when a
later library arrives.

**Symbols:** `firmware::link_context` (main's `_DYNAMIC`/`.dynsym`, or an external
`sym_entry` table), loaded libraries, and cross-library lookup.

**API:** `firmware::dynamic_linker(ctx)` with `load_from_source`, `lookup`,
`unload`, `first_missing_import`; `dl::dlopen` if the app wants POSIX names.

**Limits:** `SCL_HOSTED=ON` (uses `std::vector`); larger code; the image and
journal are heap/arena-backed.

**Parser use:** `elf_image::parse`, `collect_relocations()` (hosted only),
`find_section()` for `.dynsym`, `.dynstr`, `.hash`, `.init_array`, `.ARM.exidx`,
and `source().read_at()` to read those sections.

## 4.7 `scl::xip-dynamic-nolib` and `scl::xip-dynamic-newlib`

These are the dynamic loaders with the two-base split: text runs in place from
flash/DRAM, data is copied to RAM, and the adapter decides at load time whether
the image can really be XIP (`classify_segments` + `text_is_immutable`).

**Module:** a split PIC `.so` (`xip-split.ld`), same as `xip-static`.

**Relocations:** the split-base applier (`RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`,
`ABS32`, `REL32`, `COPY`); the text segment is not written at load on real
flash.  External function slots are pre-linked (`bind_host_imports` /
`patch_main_imports`) before the image is programmed, or resolved through the
cross-call trampoline.

**Symbols:** main's `.dynsym` or an external table; cross-library calls via the
trampoline.

**API:** `xip::nolib::dlopen`, `xip::newlib::dlopen`, plus
`bind_host_imports`/`patch_main_imports`; `dlsym`, `dlclose`,
`first_missing_import`.

**Limits:** same text-relocation rule as `xip-static`; the fallback path (not
XIP) copies the whole image to RAM.

**Parser use:** `elf_image::parse`, `SegmentMap` (`program_header`),
`find_section` for `.got`/`.init_array`/`.ARM.exidx`, direct image spans; the
adapter-base helpers parse again for import binding.

## 4.8 `scl::overlay` — linked-in modules sharing one window

**Best for:** several mutually exclusive modules that must time-share scarce RAM;
no runtime symbol resolution; the host linker already resolved everything.

**Module:** an ordinary object linked with the firmware.  The linker places all
overlay modules at the **same VMA** (one window) and stores each at its own LMA
(`AT>RAM`).  `NOCROSSREFS` guarantees they do not reference each other.

**Load:** `scl_overlay_load(&region, index)` is a `memcpy(LMA → VMA)`, a cache
clean/invalidate, and a `DSB`/`ISB`.  It takes the loader lock.  No relocation,
no symbol lookup, no constructors at swap time (the constructors are part of the
firmware's `.init_array`, run at boot).

**API:** `scl_overlay_init`, `scl_overlay_load`, `scl_overlay_current`; the app
owns the linker script and the descriptor table (`vma`, `lma`, `size`, `name`).

**Limits:** only one module resident at a time; no cross-overlay calls; the app
must know the order of use.

**Parser use:** **none**.  The parser is not involved; the host linker did the
work.

---

## 4.9 Before relocation: how the bytes get into the app

This section slows down and looks at the phase that happens **before** the first
relocation or patch.  It answers: which file is opened, who reads it, which
buffer is used, which parser calls happen, and where the copy ends.

### 4.9.1 Two different "loads"

Do not confuse these:

| What is loaded | By whom | Which file | When |
|---|---|---|---|
| the **firmware** (loader + app) | OpenOCD / debugger / boot ROM | the firmware `.elf` or `.hex` | flashing/debug |
| the **module** (shared library) | the application, through a storage front-end | `.rel.o`, `.so`, `.pic.so`, `.opt.so`, a flash blob, or nothing (overlay) | at boot, or before use |

The debugger flashes the **firmware**.  It does not normally flash the module for
the RAM-based loaders: the app reads the module itself.  For XIP, the module
image is instead provisioned to flash/DRAM (by the build, by the app, or by a
debugger command), and the loader consumes that image as a blob.

So "loading a shared library" starts with an ordinary application read — not with
OpenOCD.

### 4.9.2 Where the module bytes come from

| Front-end | Typical code | What the app passes to the loader |
|---|---|---|
| **Semihosting host file** | `SYS_OPEN`/`SYS_FLEN`/`SYS_READ`/`SYS_CLOSE` into a RAM buffer | `std::span<const std::byte>` or `memory_byte_source` |
| **POSIX / semihosting fd** | `fd_byte_source::open_file(path)` | a `loader::byte_source&` |
| **SD card / FatFs** | `sd::SdCard`, `fatfshw::load_file("tests/sclsd.bin", ...)` | a RAM span (or a custom `byte_source`) |
| **Flash archive / linked blob** | the app points at a flash address or a const array | a `std::span` over flash/const data |
| **Linked-in overlay** | the linker emits `LMA`/`VMA`/`SIZE` symbols | no bytes; a descriptor table |
| **Host test** | file read or `std::vector` | `memory_byte_source` |

The loader core only ever sees the last column.  That is why the same code runs
in host tests, QEMU, and on three boards.

### 4.9.3 The buffer and the byte source

There are two storage styles:

* **Whole file in RAM** — the app allocates a buffer, reads the file into it, and
  passes a `std::span<const std::byte>`.  This is the normal case in the
  examples and tests.
* **Random-access source** — the app passes a `loader::byte_source`; the parser
  reads exactly the bytes it needs.  `memory_byte_source` wraps a RAM span;
  `fd_byte_source` wraps a file descriptor (POSIX or semihosting-backed).

Sizing the buffer:

| Loader | How the size is known |
|---|---|
| `scl::reloc` | the arena is fixed by the app; sections are packed into it |
| `scl::static` | `Module::image_size(path)`, or the caller's `exec_buf` size |
| `scl::optimal` | the fixed `_sxip_text_*`/`_sxip_data_*` windows |
| `scl::xip-static` | the text is in the image/flash; the data segment is copied to a RAM arena |
| `scl::dynamic-nolib` | `elf_image::image_size()` (highest `PT_LOAD` end) |
| `scl::dynamic-newlib` | `elf_image::image_size()` plus journal capacity |
| `scl::xip-dynamic-*` | the data segment is copied to a RAM arena; text stays in the image |
| `scl::overlay` | `SIZEOF(.ovN)` from the linker script |

For a semihosting read, `SYS_FLEN` gives the file length before the read; for
`fd_byte_source`, `lseek(fd, 0, SEEK_END)` does the same.

### 4.9.4 The universal pre-relocation sequence

Every method does these steps in some form; only the function names and the
destination differ.

```text
1. obtain the bytes         semihosting/SD/flash/linked-in
2. wrap them                span or byte_source
3. parse the ELF header     elf_image::parse (or the ET_REL parser)
4. enumerate what to copy   PT_LOAD segments / SHF_ALLOC sections
5. validate the ranges      file offset + filesz, vaddr + memsz, destination
6. reserve the destination  arena / exec buffer / fixed window / heap
7. copy                      memcpy filesz bytes
8. zero the tail             memset memsz - filesz (bss)
9. (then) relocations        apply / patch / resolve
```

Steps 1–8 are the "load of the binary file".  Step 9 is the relocation phase,
covered in §4.1–4.8 and §5.

### 4.9.5 Exact call chains per method

#### `scl::reloc`

```text
app: read_host_file("name.rel.o")          // semihosting/span
  → reloc::load_set({images}, arena, resolve)
      → place_module(image, arena, used, m)
          - checks ELF magic, EM_ARM, ET_REL
          - reads e_shoff/e_shentsize/e_shnum with load_word/load16
          - for each SHF_ALLOC section:
              align `used`, check arena, record m.sec_base[i]
              SHT_NOBITS  → memset(arena + used, 0, size)
              SHT_PROGBITS→ memcpy(arena + used, image + sh_offset, size)
      → apply_relocs(...)                  // relocation starts here
```

No `elf_image::parse`; the loader reads the section headers itself with
`loader/detail/bytes.hpp`.  The arena is the destination.

#### `scl::static`

```text
app: Module::load_file(path, exec_buf, exec_sz, syms)
  → elf_reader::open_elf(path)
      → fd_byte_source::open_file(path)    // open + seek-end for size
      → verify_elf() → elf_image::parse(*src_)
  → scl_loader_lock()
  → Module::build(...)
      → shlib_loader::load_bin(exec_buf, exec_sz)
          - needed = efr_.get_binary_size()  (= elf_image::image_size)
          - check needed <= exec_sz
          - for each program header:
              efr_.load_segments({exec_buf, needed}, ph)
                → read_at_offset({exec_buf + p_vaddr, p_filesz}, p_offset)
                   → src_->read_at(p_offset, ...)   // fd/semihosting
                → memset(exec_buf + p_vaddr + p_filesz, 0, memsz - filesz)
      → populate_helper_sections()         // .rel.dyn/.rel.plt/...
      → patch_shlib()                      // relocation starts here
```

`load_buf()` is the same `build()` with a `memory_byte_source` instead of the fd.
`Module::image_size(path)` opens the file only to return the size.

#### `scl::xip-static`

```text
provision time (before flash):
  xip::stat::bind_host_imports(image, flash_base)
    → elf_image::parse(memory_byte_source)
    → set_extab_thumb_bits, prelink_flash_data
    → walk .rel.plt/.rel.dyn, patch JUMP_SLOT slots
  (then write `image` to flash, or leave it in the DRAM image region)

load time:
  xip::stat::load(blob, module)
    → elf_image::parse(memory_byte_source)
    → classify_segments(blob, seg)        // program headers, split check
    → text_is_immutable(blob, seg)
    → alloc(data_memsz) in fw::image_arena()
    → memset(ram, 0, data_memsz)
    → memcpy(ram, blob + seg.data_off, seg.data_filesz)
    → text_base = blob.data() + seg.text_off   // already in flash/DRAM
    → apply_rel_xip(...)                  // relocation starts here
```

The text is **not copied** at load; it is already at `text_base`.  Only the RW
segment is copied to RAM.  This is the two-base model.

#### `scl::optimal`

```text
app: read the .opt.so into a RAM span
  → xip::nopic::load(blob, m)
      → elf_image::parse(memory_byte_source(blob))
      → program_header loop: find R-X and RW PT_LOAD
      → check both VAs fit _sxip_text_*/_sxip_data_*
      → memcpy(text_vaddr, blob + text_off, text_filesz)
      → memcpy(data_vaddr, blob + data_off, data_filesz)
      → memset(data_vaddr + data_filesz, 0, data_memsz - data_filesz)
      → sec(".dynsym"/".dynstr"/".rel.dyn"/".rel.plt")  // find_section + blob spans
      → relocation loop                    // relocation starts here
```

Text and data are copied to the fixed windows; the image is a 0-bias copy.

#### `scl::dynamic-nolib`

```text
app: fw::dlopen_source(src, name)          // src = memory or fd byte_source
  → elf_image::parse(src)
  → image_size() = highest PT_LOAD end
  → image_arena().alloc(size)
  → memset(mem, 0, size)
  → for each PT_LOAD:
        src.read_at(p_offset, mem + p_vaddr, p_filesz)
  → g_sc.load(img, src)                    // read shdrs + .shstrtab once
  → reloc_applier::apply_rel(...)          // relocation starts here
```

With a `memory_byte_source`, the app has already read the whole file into RAM.
With `fd_byte_source`, each `read_at` is a seek+read (potentially semihosting).

#### `scl::dynamic-newlib`

```text
app: dynamic_linker::load_from_source(src)
  → elf_image::parse(src)
  → image_base() + image_size()
  → image_memory::load_segments(img, journal_extra)
      - make_unique<byte[]>(size + extra)
      - for each PT_LOAD:
          img.source().read_at(p_offset, dst + (p_vaddr - base), p_filesz)
          memset(dst + ..., 0, memsz - filesz)
  → collect_relocations()                  // all SHT_REL in one buffer
  → read .dynsym/.dynstr into vectors      // find_section + source().read_at
  → relocator::compute(...)                // build the plan
  → apply_plan(...)                        // relocation starts here
```

#### `scl::xip-dynamic-nolib` / `scl::xip-dynamic-newlib`

```text
app: xip::nolib::dlopen(so_image, name)   // or xip::newlib::dlopen
  → elf_image::parse(memory_byte_source(so_image))
  → classify_segments(so_image, seg)
  → text_is_immutable(so_image, seg)
  → can_xip ?
      yes: alloc(data_memsz) in image_arena()
           memset + memcpy(data_filesz)   // RW only
           text_base = so_image.data() + seg.text_off
      no:  alloc(max_va - min_va)
           memset + copy every PT_LOAD     // whole image in RAM
  → apply_rel_xip(...)                     // relocation starts here
```

`patch_main_imports` / `bind_host_imports` are provision-time calls that parse
the image again and pre-link external `JUMP_SLOT`s before it is written to flash.

#### `scl::overlay`

```text
link time:  the linker places every overlay module at the same VMA,
            each with its own LMA, and emits ADDR/LOADADDR/SIZEOF symbols.

boot/load:  scl_overlay_load(&region, index)
              → memcpy(vma, lma, size)
              → cache clean/invalidate
              → DSB/ISB
```

No file is opened, no ELF is parsed, no relocation is applied.  The host linker
did all of that.

### 4.9.6 File names and what the file extension means

The loader checks the ELF magic, not the extension.  The extensions below are
conventions used by the examples/tests:

| Name | Built as | Consumer |
|---|---|---|
| `floatlib.rel.o`, `libprov.rel.o` | `-r` ET_REL | `scl::reloc` |
| `floatlib.so`, `libprov.so` | `-fpic -msingle-pic-base ... -shared` | `scl::dynamic-nolib`, `scl::xip-dynamic-*` |
| `floatlib.pic.so`, `libprov.pic.so` | `-fPIC -shared` | `scl::static`, `scl::dynamic-newlib` |
| `floatlib.opt.so`, `libprov.opt.so` | `-fno-pic -mlong-calls -shared -T nopic-split.ld` | `scl::optimal` |
| a split `.so` | `-T xip-split.ld` | `scl::xip-static` |
| no file | linked into the firmware | `scl::overlay` |

In the SCL tests, `FIXTURE_DIR` and `FIXTURE_EXT` decide the path, e.g.
`fixtures/libprov.rel.o`, `fixtures/libprov.so`, `fixtures/libprov.pic.so`,
`fixtures/libprov.opt.so`.  In the examples, `EXAMPLE_FIXTURE_DIR` and the
loader adapter choose the name.

The **firmware** file (`<name>-hwd`, `.elf`, `.hex`) is what OpenOCD flashes.
The module files stay on the host for semihosting, or on SD/flash for the other
front-ends.

### 4.9.7 What the parser does *not* do in this phase

At the end of steps 1–8:

* the ELF has been validated and read;
* the right bytes are at the right runtime addresses;
* `.bss` is zeroed;
* the section/segment tables are still needed and are kept (in the source span,
  the `elf_image`, or the loader's own metadata).

No relocation has been applied, no imported symbol has been resolved, no
constructor has run, and no cache has been published.  Those are the next
phase.

### 4.9.8 A complete semihosting read + load

AArch32 (`SVC 0x123456`):

```cpp
using u32 = std::uint32_t;   // plus <cstdint>, <cstring>, <span>

static unsigned semi(unsigned op, const void* arg) {
    register unsigned r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = arg;
    __asm__ volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory", "lr");
    return r0;
}

// SYS_OPEN=0x01, SYS_CLOSE=0x02, SYS_READ=0x06, SYS_FLEN=0x0C
std::span<const std::byte> read_file(const char* path, std::byte* buf, std::size_t cap) {
    u32 open_args[3] = {(u32)path, 1u, (u32)strlen(path)};   // mode 1 = "rb"
    int h = (int)semi(0x01, open_args);
    if (h < 0) return {};
    u32 handle = (u32)h;
    std::size_t n = semi(0x0C, &handle);                     // SYS_FLEN
    if (n == 0 || n > cap) { semi(0x02, &handle); return {}; }
    u32 read_args[3] = {handle, (u32)buf, (u32)n};
    if (semi(0x06, read_args) != 0) { semi(0x02, &handle); return {}; }
    semi(0x02, &handle);                                     // SYS_CLOSE
    return {buf, n};
}
```

Cortex-M uses `BKPT 0xAB` instead of `SVC`; the rest is identical.

Then:

```cpp
auto bytes = read_file("floatlib.rel.o", buffer, sizeof buffer);
reloc::load(bytes, arena, resolve);
```

or:

```cpp
loader::memory_byte_source src{bytes};
auto h = fw::dlopen_source(src, "floatlib");
```

The loader call is where `elf_image::parse` or the ET_REL parser begins.

### 4.9.9 Checks that belong to this phase

The parser and the per-loader copy code are responsible for:

* `e_ident` magic, `ELFCLASS32`, `ELFDATA2LSB`, `EM_ARM`, EABI5, no BE8;
* `e_ehsize`, `e_phentsize`, `e_shentsize`, `e_shstrndx` sanity;
* `p_filesz <= p_memsz`, `p_offset + p_filesz <= source size`;
* `p_vaddr + p_memsz <= destination size`;
* `SHF_ALLOC` sections fit the arena; `sh_addralign` respected;
* `.bss` (`p_memsz > p_filesz`, or `SHT_NOBITS`) zeroed;
* text/data windows not overlapped by two modules;
* no integer wrap in `vaddr + memsz` or `offset + size`.

Typical failures at this point (before any relocation):

| Message | Meaning |
|---|---|
| `short read` / `SHORT_READ` | the file is truncated or the fd ended early |
| `not an ELF` / `NOT_ELF` | wrong file or bad magic |
| `not ARM32` / `NOT_ARM32` | wrong class, endianness or machine |
| `ELF parse failed` | bad header sizes or EABI |
| `segment exceeds buffer` | `exec_buf`/arena too small |
| `R-X VA outside the text window` | wrong link script or wrong loader |
| `need a split R-X / RW image` | a non-split `.so` for an XIP loader |

### 4.9.10 The first call after the copy

| Method | First relocation/patch call |
|---|---|
| `scl::reloc` | `apply_relocs()` (then `register_unwind`, `run_ctors`) |
| `scl::static` | `populate_helper_sections()` then `patch_shlib()` |
| `scl::xip-static` | `apply_rel_xip()` |
| `scl::optimal` | the relocation loop in `nopic.cpp` (`RELATIVE` skip, `ABS32`/... apply) |
| `scl::dynamic-nolib` | `reloc_applier::apply_rel()` |
| `scl::dynamic-newlib` | `relocator::compute()` then `apply_plan()` |
| `scl::xip-dynamic-*` | `apply_rel_xip()` (or the RAM-fallback path) |
| `scl::overlay` | none; the copy is the load |

After that phase, the loader may clean/invalidate caches, register
`.ARM.exidx`, run `.init_array`, and publish the module.

---

# 5. Where `common-elf-reader` is used — the complete matrix

Legend for the table:

* **P** — `elf_image::parse`
* **H** — `elf_image::header()`
* **PH** — `program_header()` (or `SegmentMap`, which uses it)
* **SH** — `section_header()` (directly or via a wrapper)
* **FS** — `find_section()` (or a wrapper `find_section_by_name`)
* **RA** — `source().read_at()`
* **CR** — `collect_relocations()` (hosted only)
* **SZ** — `image_size()` / `image_base()`
* **BS** — the byte source used
* **AR** — `loader/arm-reloc.hpp` engine

| Method | P | H | PH | SH | FS | RA | CR | SZ | BS | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| `scl::reloc` | — | — | — | — | — | — | — | — | span | **manual ET_REL parser**; uses `elf-types`/`detail`/`arm-reloc` |
| `scl::static` | ✓ | ✓ | ✓ | ✓ | ✓* | ✓ | — | ✓ | fd or memory | `elf_reader` wrapper; `*` own `find_section_by_name` |
| `scl::xip-static` | ✓ | ✓ | ✓ | — | ✓ | ✓ | — | — | memory | `SegmentMap`; `bind_host_imports` re-parses |
| `scl::optimal` | ✓ | ✓ | ✓ | — | ✓ | —** | — | — | memory | `**` reads section bytes directly from the blob |
| `scl::dynamic-nolib` | ✓ | ✓ | ✓ | — | —*** | ✓ | — | ✓ | any `byte_source` | `***` custom section-header cache |
| `scl::dynamic-newlib` | ✓ | ✓ | ✓ | — | ✓ | ✓ | ✓ | ✓ | any `byte_source` | `CR` builds the relocation plan |
| `scl::xip-dynamic-nolib` | ✓ | ✓ | ✓ | — | ✓ | —** | — | — | memory | adapter-base re-parses for imports |
| `scl::xip-dynamic-newlib` | ✓ | ✓ | ✓ | — | ✓ | —** | — | — | memory | adapter-base re-parses for imports |
| `scl::overlay` | — | — | — | — | — | — | — | — | none | linked-in; host linker resolved |

### What each API means in practice

**`elf_image::parse`** — the first call in every method except `reloc` and
`overlay`.  It validates the header and gives access to sections, segments and
the byte source.  It does not allocate or relocate.

**`header()` / `program_header()`** — the loader reads `PT_LOAD` flags
(`PF_X`, `PF_W`) to decide what is code, what is writable data, and where the
segments go.  `SegmentMap` in `scl::xip-common` is the shared helper.

**`section_header()` / `find_section()`** — used to find `.dynsym`, `.dynstr`,
`.hash`, `.got`, `.rel.dyn`, `.rel.plt`, `.init_array`, `.ARM.exidx`,
`.ARM.extab`.  `scl::static` has its own `find_section_by_name` wrapper over the
parser's section headers; `scl::dynamic-nolib` uses a one-shot section-header
cache instead.

**`source().read_at()`** — used when the file is not resident.  `scl::static`
can read sections through an fd; `dynamic-newlib` reads `.dynsym`/`.dynstr` into
vectors; the XIP adapters read the image spans.  `scl::optimal` has the whole
blob in memory and indexes it directly.

**`collect_relocations()`** — only `scl::dynamic-newlib` uses it.  It
concatenates the allocated `SHT_REL` sections into one buffer for the hosted
relocation planner.

**`image_size()` / `image_base()`** — `scl::dynamic-nolib` uses the highest
`PT_LOAD` end to size the arena; `scl::dynamic-newlib` uses the lowest base to
translate addresses; `scl::static::image_size(path)` uses the wrapper.

**`fd_byte_source`** — `scl::static::load_file()` and `image_size(path)` open the
file.  On a bare-metal target, the C library's `open/read/lseek` can be backed by
semihosting; `scl::static::load_buf()` avoids that path.

**`memory_byte_source`** — the normal path for every span-based loader: `reloc`,
`optimal`, the XIP adapters, and any dynamic loader given a RAM image.

**`arm-reloc`** — `scl::reloc` and `scl::optimal` dispatch through it; the other
loaders deal only with dynamic relocations (and use `reloc-applier`/`xip::relocate`).

**`arm-attributes` / `arm-abi`** — available to the parser: EABI5, `.ARM.attributes`,
`Tag_ABI_VFP_args`, CPU profile, hard/soft-float.  The loader calls
`elf_image::parse` (which checks EABI5/BE8) and `check_arm_abi()` can be called by
the app/platform for a policy check.

**`symbol-resolver`** — used by the dynamic loaders and XIP adapters to search a
module's `.dynsym`/`.hash`; it is part of the parser package, not the ELF image
itself.

---

# 6. Semihosting: what it is, and where it belongs

## 6.1 What semihosting is

Semihosting lets target code ask the **debugger** (OpenOCD, a J-Link, etc.) to do
host I/O: open a file, read it, write a console, exit.  On ARM:

* Cortex-M: `BKPT 0xAB` with the operation number in `r0` and the argument in `r1`.
* Cortex-A (AArch32): `SVC 0x123456` ("Angel" trap), same arguments.

The debugger must have semihosting enabled.  Each call stops the target, lets the
host do the work, and resumes it.  That is why semihosting is excellent at boot
and bad in a loop.

## 6.2 The core rule

**The SCL loader core does not issue semihosting calls itself.**  It consumes:

* a `std::span<const std::byte>` (the whole file), or
* a `loader::byte_source` (random access to the file).

`fd_byte_source` is the one place where the package delegates to the platform's
`open/read/lseek`; on a bare-metal target those can be semihosting stubs.  That is
still a storage front-end, not a semihosting instruction inside the relocation
engine.

This keeps the core portable (host tests, flash archives, SD cards, QEMU) and
lets the application choose when and how to do I/O.

## 6.3 Pattern A — read the file into RAM, then load from memory

This is what the SCL examples and the Lyra tests do.  The application opens the
module with semihosting, reads it into a RAM buffer, and passes the span to the
loader.

A minimal AArch32 reader:

```cpp
static unsigned semi(unsigned op, const void* arg) {
    register unsigned r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = arg;
    __asm__ volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory", "lr");
    return r0;
}
// SYS_OPEN=0x01, SYS_CLOSE=0x02, SYS_READ=0x06, SYS_FLEN=0x0C
```

(The Cortex-M equivalent uses `bkpt 0xAB`; the parser's `test-arm/src/semihost.c`
is a complete example.)

The load then looks like:

```cpp
auto bytes = read_host_file("floatlib.rel.o");      // semihosting → RAM
reloc::load(bytes, arena, resolve);
```

or, for a dynamic loader:

```cpp
loader::memory_byte_source src{bytes};
auto h = fw::dlopen_source(src, "floatlib");
```

The loader sees only memory.  The semihosting cost is one open + one size + one
read + one close.

## 6.4 Pattern B — parse directly from an fd

`scl::static::Module::load_file(path, ...)` uses `fd_byte_source`, which calls
`open/lseek/read` on every `read_at`.  On a target those calls can be backed by
semihosting, so the parser reads the ELF header, section headers, then each
section it needs.  This avoids holding the whole file in RAM, but each read is a
debugger round-trip.  For small modules it is convenient; for big ones, read once
into RAM and use `load_buf`.

`fd_byte_source` is optional; a platform with its own storage can implement
`loader::byte_source` directly.

## 6.5 Pattern C — build main's symbol table once

The dynamic loaders need main's exported symbols.  Three ways:

1. **`-pie -Wl,-E` main** — `firmware::link_context::from_self()` reads main's
   own `_DYNAMIC`/`.dynsym`; no file I/O.
2. **External table** — the platform reads the firmware ELF's `.symtab`/`.strtab`
   over semihosting once, builds `scl_sym_entry` entries, and calls
   `dl_set_symtab()` (nolib) or `link_context::set_symtab()` (newlib).
   `scl::symtab::collect_buffered()` is designed for this: one read per section,
   exactly because semihosting is slow.
3. **Compile-time table** — `SCL_REGISTER_SYM()` puts entries in a linker
   section; `scl::symtab::source_registered()` uses it.  No ELF, no semihosting.

For the XIP adapters, `set_symtab()` does the same job; `set_main_symbols()` does
it for `optimal`.

## 6.6 Pattern D — logging

`scl_log_puts()`/`scl_log_putc()` are weak hooks.  The default is silent.  An
application can implement them over:

* semihosting `SYS_WRITE0` (one trap per string; fine at boot, slow in a loop);
* a UART (the usual choice);
* an in-memory trace buffer.

The loader's own code calls the hooks, never `printf` or a UART directly.

## 6.7 Semihosting and XIP

For XIP (real flash or a DRAM image region), the module is **provisioned once**:
read it over semihosting (or SD/flash), patch it if needed
(`bind_host_imports`), write it to the flash/image region, and register it.  At
run time the loader and the application touch only the flash/DRAM image; no
semihosting is in the call path.

On a board with no application flash (Luckfox Lyra), the "XIP" image is placed in
DRAM; the same code is used, and the image is writable at provisioning time.

## 6.8 Semihosting, RTOS, and multiple cores

Do all semihosting **before** the scheduler starts or before releasing the other
cores.  Semihosting stops the core; doing it while another core is running the
same module is a recipe for timeouts.  After the load:

* take the loader lock around load/unload;
* clean/invalidate caches;
* call `scl_publish_release()` before another core uses the module.

The SCL examples load on core 0 before starting the kernel on the Lyra; the
Pi-Zero examples load before starting the second core.

## 6.9 Where semihosting appears in this repository

| Place | What it does |
|---|---|
| `common-elf-reader/include/loader/sources/fd-byte-source.hpp` | file descriptor byte source; the fd can be semihosting-backed |
| `common-elf-reader/include/loader/byte-source.hpp` | documents the semihosting/flash/host seam |
| `include/scl/symtab-runtime.hpp` (`collect_buffered`) | one read per section for slow sources such as semihosting |
| `include/static/module.h` (`load_file`) | file path path for `scl::static` |
| `src/static/elf-reader.cpp` | `fd_byte_source`/`memory_byte_source` selection |
| `include/dynamic-nolib/dl.hpp` | "over semihosting" storage front-end |
| `include/dynamic-newlib/dynamic-linker.hpp` | "wrap this with your own storage front-end (semihosting file, ...)" |
| `SCL-EXAMPLES-shared-new/luckfox-lyra/common/lyra.cpp` | AArch32 `SVC 0x123456` `read_host_file` |
| `SCL-TESTS-shared-new/luckfox-lyra-shared/src/fixtures-main.cpp` | AArch32 semihosting fixture loader |
| `common-elf-reader/test-arm/src/semihost.c` | Cortex-M `BKPT 0xAB` implementation (test) |
| `pico2-pizero-shared`, `luckfox-lyra-*` | boot-time semihosting read, then load from RAM |

The SCL **core** has no semihosting code.  That is deliberate and is the reason
the same loader runs in host tests, QEMU, and on three different boards.

## 6.10 Semihosting pitfalls

* **No debugger, no semihosting.**  The call hangs or faults if the probe is not
  attached and semihosting is not enabled.
* **Every call is a stop/resume.**  Read the file once; do not parse section by
  section from semihosting if you can avoid it.
* **Paths are host paths**, relative to the debugger's working directory.
* **Exit is a semihosting call.**  `SYS_EXIT` ends the debug session; do not call
  it while other cores are running.
* **Semihosting in an ISR** is almost always wrong.
* **File length** must be known before allocating the RAM buffer; `SYS_FLEN`
  gives it.
* **Permissions/encoding** are the host's; a binary read uses mode 1 (`"rb"`).

---

# 7. Choosing a method

| Requirement | Recommended method |
|---|---|
| Smallest code; one core; no unload | `scl::reloc` |
| Unload/reload, byte-exact revert, POSIX-like | `scl::static` or `scl::dynamic-newlib` |
| Execute from real flash, save RAM | `scl::xip-static`, `scl::optimal`, `scl::xip-dynamic-*` |
| No r10, plain function pointers | `scl::reloc` (ET_REL) or `scl::optimal` |
| No libc / freestanding | `scl::reloc`, `scl::optimal`, `scl::dynamic-nolib` |
| Hosted libc/POSIX, full dynamic linker | `scl::dynamic-newlib` |
| Several modules share one RAM window | `scl::overlay` |
| Modules import from each other cyclically | `scl::reloc` (`load_set`), `scl::optimal` (`resolve_all`), any dynamic loader (later-load resolution) |
| C++ exceptions across boundaries | any loader with `.ARM.exidx` support; add `libsupc++` and the right `-fexceptions`/`-frtti` flags |
| Hard real-time / no loader code after boot | eager binding (`dl_set_lazy_binding(false)`), load before releasing cores |
| One module, no file system | `scl::overlay` or a span-based loader with a compile-time image |

A rough memory/feature trade-off:

| Method | RAM for image | Can unload | Runtime symbol lookup | Needs r10 | Code size |
|---|---|---|---|---|---|
| reloc | arena (slots) | no | no | no | smallest |
| static | whole image buffer | yes | yes | no | medium |
| optimal | fixed windows | no | load-time only | no | small |
| xip-static | data segment only | yes | load-time only | yes | medium |
| dynamic-nolib | arena image | yes | yes | yes | medium |
| dynamic-newlib | image + journal | yes | yes | yes | largest |
| overlay | one shared window | swap only | no | no | smallest |

---

# 8. Architectures and boards

## 8.1 Cortex-M (M0/M0+/M3/M4/M7)

* Thumb-only; every function address has bit 0 set.
* No MMU; MPU optional; M7 has a data cache (the cache hooks matter).
* `scl::reloc`, `scl::optimal`, `scl::static`, `scl::xip-*`, and the dynamic
  loaders all work.
* The r10 models use `-msingle-pic-base -mpic-register=r10`; a module that must
  run with a GOT needs that register.
* Semihosting uses `BKPT 0xAB`.
* On the RP2350 (Cortex-M33, two cores, XIP flash), use the XIP loaders and
  `scl::rp2350-flash` if one core must write flash while the other runs.

## 8.2 Cortex-A (A7/A53, AArch32)

* ARM and Thumb coexist; the parser and `loader::arm` engine preserve the
  symbol's own bit 0.
* MMU and L1 caches are on; `scl_cache_clean_invalidate()` is required after
  writing code; `scl_publish_release()` before another core uses it.
* Semihosting uses `SVC 0x123456`.
* Modules may be ARM or Thumb; `R_ARM_CALL`/`BLX` handles interworking, and
  `MOVW/MOVT` materialises 32-bit addresses.
* On the Luckfox Lyra (RK3506, 3x A7) there is no application flash: the "XIP"
  loaders run in place from a DRAM image region; storage is the SD card.
* The three cores make the loader lock and publication rules mandatory.

## 8.3 Hard float vs soft float

The module and the firmware must agree.  `elf_image::parse` checks EABI5 and
rejects BE8; `check_arm_abi()` compares the module's `.ARM.attributes`
(`Tag_ABI_VFP_args`) or `e_flags` with the firmware's `-mfloat-abi`.  A mismatch
is an argument-passing bug that no relocation can fix.

## 8.4 Boards in this project

| Board | CPU | Memory model | Notes |
|---|---|---|---|
| WeAct F411CE | Cortex-M4F, 1 core | 512 KiB flash, 128 KiB RAM | simple; `scl::reloc`/`optimal`/`static`; W25Q64 for storage |
| Pi-Zero RP2350B | 2x Cortex-M33 | XIP flash + PSRAM | `xip-static`/`optimal`/dynamic; `scl::rp2350-flash` for flash writes with both cores |
| Luckfox Lyra B | 3x Cortex-A7 | DRAM only; SD card | all loaders; XIP = DRAM image region; A32/T32; MMU/caches/SMP |

---

# 9. Rules, pitfalls, and a checklist

## 9.1 Loader lock and publication

* `scl_loader_lock()` must be **recursive** (a load can allocate, and allocation
  can take it).
* Wrap load/unload, not ordinary calls.  After publication, no loader code may
  run on the call path.
* `scl_publish_release()` is the release fence/event for another core.
* Use `dl_set_lazy_binding(false)` and `dl_enable_cross_call(false)` for RT.

## 9.2 Cache

* After writing code: `scl_cache_clean_invalidate(addr, len)`.
* On Cortex-A/M7 the instruction cache must be invalidated; the default hooks
  are barriers only, so a board must supply strong versions.
* Clean before anyone else reads the data; invalidate before executing the code.

## 9.3 Relocations and text

* A relocation that writes to `.text` prevents true flash XIP.  Use a PIC module
  with a GOT in data, or resolve it at link time.
* `R_ARM_CALL`/`JUMP24` range is ±32 MiB; `MOVW/MOVT` is the escape for far
  targets.  A loader that cannot reach a target reports an error, not a truncated
  branch.
* `R_ARM_ABS32` to a Thumb function must carry bit 0; to an ARM function it must
  not.  Do not force Thumb.

## 9.4 r10 and symbol entry

* For the r10 models, every call into a module must set r10 to that module's
  `got_base` (`pic_call_*` or the cross-call trampoline).
* A module's own internal calls keep their own r10; a lib→lib call needs the
  trampoline.
* The app must not call a module function through a plain pointer without the
  correct r10.

## 9.5 Constructors, destructors, exceptions

* `.init_array` runs at load; `.fini_array` at unload.
* `.ARM.exidx`/`.ARM.extab` must be registered for the unwinder; the XIP/dynamic
  adapters do this at load.
* Throwing across a C function pointer requires the unwind tables to be
  registered and the function pointers to be interworking-correct.

## 9.6 Missing imports

* A load may succeed while an import is unresolved; calling it faults.
* After loading the whole set, call `first_missing_import()` and treat a name as
  a configuration error.

## 9.7 Memory and unload

* The arena/buffer/window is fixed by the application.  Size it with
  `image_size()` where available.
* Do not unload while another core/thread can still call the module.  Quiesce,
  then unload; the journal restores main's words byte-exactly.
* `scl::overlay` has no unload; loading another module simply overwrites the
  window.

## 9.8 Semihosting checklist

* Read once into RAM; parse from RAM.
* Keep the buffer alive while the loader/module uses it.
* Do the read before starting the scheduler/other cores.
* Do not call semihosting from an ISR or after publishing a module.
* Use `collect_buffered()` for a sidecar symbol table.
* On Cortex-M use `BKPT 0xAB`; on AArch32 use `SVC 0x123456`.

---

# 10. Where the code is

## 10.1 CMake targets

| Target | Main sources |
|---|---|
| `scl::reloc` | `src/reloc/reloc-loader.cpp`, `src/port/unwind-find-exidx.c` |
| `scl::static` | `src/static/elf-reader.cpp`, `shlib-loader.cpp`, `module.cpp`, `logger.cpp`, `tls.c` |
| `scl::xip-common` | `src/xip/segment-map.cpp`, `relocate.cpp`, `compact.cpp` |
| `scl::xip-static` | `src/xip/static/adapter.cpp`, `xip-cross-call.c`, `pic-trampoline.c`, `image-memory.cpp` |
| `scl::optimal` | `src/loader.cpp` + `src/xip/static/nopic.cpp` + `xip-common` |
| `scl::dynamic-nolib` | `src/dynamic-nolib/dl.cpp`, `pic-trampoline.c`, `dl-cross-call.c`, `lazy-resolve.c`, `image-memory.cpp`, `link-context.cpp` |
| `scl::dynamic-newlib` | `src/dynamic-newlib/dynamic-linker.cpp`, `patch-journal.cpp`, `image-memory.cpp`, `link-context.cpp` |
| `scl::xip-dynamic-nolib` | `src/xip/dynamic-nolib/adapter.cpp`, `xip-cross-call.c` |
| `scl::xip-dynamic-newlib` | `src/xip/dynamic-newlib/adapter.cpp`, `xip-cross-call.c`, `pic-trampoline.c` |
| `scl::overlay` | `src/overlay/overlay.c` |

## 10.2 Parser

`common-elf-reader/` (submodule `elf32_read_parse-new.git` `f650889`):

* `include/loader/elf-image.hpp`, `src/elf-image.cpp` — parse/access
* `include/loader/elf-types.hpp` — ELF32 types + relocation numbers
* `include/loader/detail/bytes.hpp` — bounded byte/word/name helpers
* `include/loader/arm-reloc.hpp` — A32/T32 relocation engine
* `include/loader/arm-attributes.hpp`, `arm-abi.hpp` — ABI checks
* `include/loader/sources/{memory,fd}-byte-source.hpp`
* `include/loader/symbol-resolver.hpp`

## 10.3 Examples and tests

| Board | Tests | Examples |
|---|---|---|
| WeAct F411CE | `SCL-TESTS-shared-new/` (`relocatable-shared`, `static-shared`, `dynamic-shared`, `optimal-shared`, `overlay-shared`, `api-shared`) | `SCL-EXAMPLES-shared-new/{float,spifs,combo,...}-*` |
| Pi-Zero | `SCL-TESTS-shared-new/pico2-pizero-shared` | `SCL-EXAMPLES-shared-new/pizero` |
| Lyra B | `SCL-TESTS-shared-new/luckfox-lyra-shared`, `luckfox-lyra-arm-shared` | `SCL-EXAMPLES-shared-new/luckfox-lyra` |

---

# 11. Source code examples

The snippets below are the **minimal shapes** of each method: the calls and the
ownership.  Real applications add the file reader, error printing, buffer
definitions and board hooks; the complete working versions are listed in §11.10.
All snippets assume the loader's CMake target is linked and the headers below are
included.

## 11.0 A common symbol source and a file reader

Most examples first define what main exports and how a module asks for it.

```cpp
#include "scl/symtab-register.h"

extern "C" int      host_f(int);
extern "C" unsigned HOST_K;

// A hand-built table, or SCL_REGISTER_SYM(host_f) in the defining TU.
static const scl_sym_entry kSyms[] = {
    {"host_f", static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&host_f))},
    {"HOST_K", static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&HOST_K))},
};

// loader::symbol_source = result<uint32_t>(*)(void*, string_view)
static loader::result<std::uint32_t>
resolve(void*, std::string_view name) {
    for (const auto& e : kSyms)
        if (name == e.name) return e.addr;
    return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
}
```

`SCL_REGISTER_SYM(host_f)` in the definition's translation unit puts the same
entry into the `scl_syms` linker section; `scl::symtab::source_registered()`
returns a `loader::symbol_source` over it.  With that source you do not need the
hand table.

The file reader used by the snippets is the one from §4.9.8:

```cpp
// AArch32 SVC 0x123456, or Cortex-M BKPT 0xAB, plus SYS_OPEN/FLEN/READ/CLOSE.
std::span<const std::byte>
read_semihost_file(const char* path, std::byte* buf, std::size_t cap);
```

## 11.1 `scl::reloc`

```cpp
#include "reloc/reloc-loader.hpp"

alignas(8) static std::byte arena[96 * 1024];   // 4 slots of 24 KiB

bool load_floatlib() {
    std::byte file[32 * 1024];
    auto bytes = read_semihost_file("floatlib.rel.o", file, sizeof file);
    if (bytes.empty()) return false;

    std::span<const std::byte> images[] = {bytes};
    auto set = reloc::load_set(images, arena, resolve);
    if (!set) return false;

    auto add = reinterpret_cast<float (*)(float, float)>(
        reloc::sym(*set, "float_add"));
    if (!add) return false;
    float v = add(2.5f, 3.5f);                   // 6.0
    (void)v;
    return true;
}
```

For two modules that import from each other, pass two spans to `load_set()`; it
places both before resolving, so a cycle is fine.  For a single extra module,
`reloc::load(bytes, slot_arena, resolve)` and `reloc::sym(module, name)`.

`reloc::sym()` returns the address with its own Thumb/ARM bit.  The function
pointer call above is valid for ARM state; for a Thumb module the compiler's
function-pointer call uses `blx`.  A module using r10 needs a `pic_call_*`
wrapper instead (see §9.4).

## 11.2 `scl::static`

```cpp
#include "static/module.h"

alignas(8) static std::uint8_t exec_buf[64 * 1024];
static elf_loader::symbol_table syms;

bool load_prov() {
    syms.add_entries(kSyms, sizeof kSyms / sizeof kSyms[0]);

    scl::Module mod;
    if (!mod.load_file("libprov.pic.so", exec_buf, sizeof exec_buf, &syms))
        return false;

    auto prov = mod.sym<int (*)(int)>("prov_value");
    if (!prov) { mod.unload(); return false; }
    int v = prov(5);                             // 105
    (void)v;

    mod.unload();                                // runs .fini_array, reverts main
    return true;
}
```

From memory, replace `load_file` with:

```cpp
mod.load_buf(reinterpret_cast<const std::uint8_t*>(bytes.data()),
             static_cast<std::uint32_t>(bytes.size()),
             exec_buf, sizeof exec_buf, &syms);
```

To size the buffer instead of guessing:

```cpp
std::uint32_t need = scl::Module::image_size("libprov.pic.so");
```

## 11.3 `scl::xip-static`

```cpp
#include "xip/static/adapter.hpp"

// The image is a writable copy at provision time (RAM), then programmed to
// flash/DRAM.  At load time it is the flash-resident blob.
extern std::uint8_t module_image[];
extern std::uint32_t module_size;

bool provision_and_load() {
    std::span<std::byte> writable{module_image, module_size};
    xip::stat::bind_host_imports(writable);      // before flashing (JUMP_SLOTs)

    // ... program module_image to flash here, if this is real flash ...

    xip::stat::SModule m;
    if (!xip::stat::load(std::span<const std::byte>{module_image, module_size}, m))
        return false;

    auto prov = reinterpret_cast<int (*)(int)>(xip::stat::resolve(m, "prov_value"));
    if (!prov) return false;
    int v = prov(5);                             // 105
    (void)v;
    return true;
}
```

`bind_host_imports()` is optional only if every external function import is
already bound at link time; for a normal `.so` with a PLT, it is required before
the image is written to write-once flash.  On a DRAM "XIP" region it can be done
at boot.

## 11.4 `scl::optimal`

```cpp
#include "xip/static/nopic.hpp"
#include "scl/symtab-runtime.hpp"

bool load_prov_optimal(std::span<const std::byte> bytes) {
    xip::nopic::set_main_symbols(scl::symtab::source_registered());

    xip::nopic::PModule m;
    if (!xip::nopic::load(bytes, m)) return false;
    xip::nopic::resolve_all();                   // fill cyclic imports

    auto prov = reinterpret_cast<int (*)(int)>(xip::nopic::resolve(m, "prov_value"));
    if (!prov) return false;
    int v = prov(5);                             // 105
    (void)v;
    return true;
}
```

`patch_main(m)` after the load lets the app call the module **by name** through
main's GOT; `resolve_all()` is the call that makes mutually dependent modules
work.  The image must be linked at the `_sxip_text_*`/`_sxip_data_*` windows.

## 11.5 `scl::dynamic-nolib`

```cpp
#include "dynamic-nolib/dl.hpp"

bool load_prov_nolib(std::span<const std::byte> bytes) {
    fw::dl_set_symtab(kSyms, sizeof kSyms / sizeof kSyms[0]);
    fw::dl_set_lazy_binding(false);              // no loader code after publish

    loader::memory_byte_source src{bytes};
    auto h = fw::dlopen_source(src, "libprov");
    if (!h) return false;

    auto p = fw::dlsym(**h, "prov_value");
    if (!p) { fw::dlclose(**h); return false; }
    int v = reinterpret_cast<int (*)(int)>(*p)(5);   // 105
    (void)v;

    fw::dlclose(**h);
    return true;
}
```

`h` is a `result<handle*>`: `*h` is `handle*`, `**h` is `handle&`.  From a file
descriptor use:

```cpp
auto f = loader::fd_byte_source::open_file("libprov.so");
auto h = fw::dlopen_source(*f, "libprov");
```

## 11.6 `scl::dynamic-newlib`

```cpp
#include "dynamic-newlib/dynamic-linker.hpp"

bool load_prov_newlib(std::span<const std::byte> bytes) {
    firmware::link_context ctx;
    ctx.set_symtab(reinterpret_cast<const firmware::sym_entry*>(kSyms),
                   sizeof kSyms / sizeof kSyms[0]);

    firmware::dynamic_linker linker(ctx);
    loader::memory_byte_source src{bytes};
    auto lib = linker.load_from_source(src);
    if (!lib) return false;

    auto p = linker.lookup(*lib, "prov_value");
    if (p) {
        int v = reinterpret_cast<int (*)(int)>(*p)(5);   // 105
        (void)v;
    }
    linker.unload(*lib);
    return p.has_value();
}
```

With a `-pie -Wl,-E` firmware, leave `ctx` empty; `link_context` reads main's own
`_DYNAMIC`/`.dynsym`.  The external table is for non-PIE firmware and for
`collect_buffered()` (see §11.9).

## 11.7 `scl::xip-dynamic-nolib` and `scl::xip-dynamic-newlib`

```cpp
#include "xip/dynamic-nolib/adapter.hpp"   // or xip/dynamic-newlib/adapter.hpp

void* load_prov_xip(std::span<std::byte> writable, std::span<const std::byte> image) {
    xip::nolib::set_symtab(kSyms, sizeof kSyms / sizeof kSyms[0]);
    xip::nolib::patch_main_imports(writable);    // provision time

    void* h = xip::nolib::dlopen(image, "libprov");
    if (!h) return nullptr;

    auto prov = reinterpret_cast<int (*)(int)>(xip::nolib::dlsym(h, "prov_value"));
    (void)prov;
    // xip::nolib::dlclose(h) when done
    return h;
}
```

For the newlib adapter, replace `xip::nolib` with `xip::newlib` and cast the
table to `const firmware::sym_entry*` (the layouts are identical).  The adapter
decides at load time whether the image is really XIP; if it is not, it falls back
to the copy-whole path.

## 11.8 `scl::overlay`

```cpp
#include "overlay/overlay.h"

extern "C" {
extern std::uint8_t _ovl_vma[], _ov1_lma[], _ov1_size[], _ov2_lma[], _ov2_size[];
int ov1_use_fm(void);       // linked-in symbol, resolved by the host linker
}

static const scl_overlay_module mods[] = {
    {_ovl_vma, _ov1_lma, static_cast<std::uint32_t>(
         reinterpret_cast<std::uintptr_t>(_ov1_size)), "ov1"},
    {_ovl_vma, _ov2_lma, static_cast<std::uint32_t>(
         reinterpret_cast<std::uintptr_t>(_ov2_size)), "ov2"},
};
static scl_overlay_region region;

bool use_ov1() {
    scl_overlay_init(&region, mods, 2);
    if (scl_overlay_load(&region, 0) != 0) return false;
    int v = ov1_use_fm();       // module now resident in the shared window
    (void)v;
    return true;
}
```

`_ov1_size` is a linker symbol whose **address** is the size; the cast above
turns it into the number.  Only one module is resident at a time; loading index 1
overwrites index 0.

## 11.9 Parser-only examples

### Parse an image and walk its loadable segments

```cpp
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"

void inspect(std::span<const std::byte> bytes) {
    loader::memory_byte_source src{bytes};
    auto img = loader::elf_image::parse(src);
    if (!img) return;

    for (loader::elf::half i = 0; i < img->header().e_phnum; ++i) {
        loader::elf::Phdr ph{};
        if (!img->program_header(i, ph)) continue;
        if (ph.p_type != loader::elf::PT_LOAD) continue;
        // ph.p_vaddr, ph.p_memsz, ph.p_filesz, ph.p_flags
    }

    if (auto text = img->find_section(".text"); text && text->sh_size) {
        std::byte buf[256];
        std::size_t n = text->sh_size < sizeof buf ? text->sh_size : sizeof buf;
        if (img->source().read_at(text->sh_offset, {buf, n})) {
            // buf[0..n) is the start of .text
        }
    }
}
```

### Parse directly from a file descriptor

```cpp
#include "loader/sources/fd-byte-source.hpp"

auto f = loader::fd_byte_source::open_file("libprov.pic.so");
if (f) {
    auto img = loader::elf_image::parse(*f);
    // ...
}
```

### Build main's symbol table over semihosting, one read per section

```cpp
#include "scl/symtab-runtime.hpp"

std::byte symtab_buf[4096];
char      strtab_buf[4096];
scl::symtab::entry entries[64];

std::size_t n = scl::symtab::collect_buffered(
    *img, entries, 64, symtab_buf, sizeof symtab_buf,
    strtab_buf, sizeof strtab_buf);
// entries[0..n) are layout-compatible with firmware::sym_entry
ctx.set_symtab(reinterpret_cast<const firmware::sym_entry*>(entries), n);
```

### Compile-time registration (no file, no parser)

```cpp
#include "scl/symtab-register.h"
#include "scl/symtab-runtime.hpp"

extern "C" int host_f(int);
SCL_REGISTER_SYM(host_f);

loader::symbol_source main_syms = scl::symtab::source_registered();
```

### Use the A32 relocation engine directly

```cpp
#include "loader/arm-reloc.hpp"

// type = the R_ARM_* number, p = instruction/slot, S = symbol, A = addend,
// P = relocation site.
std::uint32_t addend = 0;
if (loader::arm::decode_addend(type, p, addend)) {
    loader::arm::context c{S, addend, P, loader::arm::isa::arm};
    if (loader::arm::apply(type, p, c) != loader::error::OK) {
        // RELOC_OVERFLOW or RELOC_INTERWORK
    }
}
```

### Check the ARM ABI

```cpp
#include "loader/arm-attributes.hpp"
#include "loader/arm-abi.hpp"

auto a = loader::read_arm_attributes(*img);
if (a.valid && a.has_vfp_args && a.vfp_args == 1) {
    // the module uses VFP registers for arguments
}
if (auto r = loader::check_arm_abi(*img, true); !r) {
    // the module and the hard-float firmware disagree
}
```

## 11.10 Where to find the full working versions

The snippets above leave out board setup, error text and the exact file reader.
The complete versions are in:

| What | File |
|---|---|
| All loader branches, one app | `SCL-TESTS-shared-new/luckfox-lyra-shared/src/fixtures-main.cpp` |
| Semihosting fixture reader (AArch32) | same file, `read_fixtures()` |
| Lyra examples: boot, semihosting, SD | `SCL-EXAMPLES-shared-new/luckfox-lyra/common/lyra.cpp` |
| Lyra loader adapters | `SCL-EXAMPLES-shared-new/luckfox-lyra/loaders/*.cpp` |
| WeAct examples: boot and loaders | `SCL-EXAMPLES-shared-new/float-*/`, `common/` of each |
| Pi-Zero examples | `SCL-EXAMPLES-shared-new/pizero/` |
| Pi-Zero tests (all methods) | `SCL-TESTS-shared-new/pico2-pizero-shared/src/` |
| Optimal raw test | `SCL-TESTS-shared-new/optimal-shared/test-ram/src/main.cpp` |
| Dynamic raw test | `SCL-TESTS-shared-new/dynamic-shared/test-dynamic/src/` |
| Static raw test | `SCL-TESTS-shared-new/static-shared/test-static/src/` |
| Overlay test | `SCL-TESTS-shared-new/overlay-shared/test-overlay/test1/src/` |

Those files are the authority; if a snippet and a test differ, the test is what
runs on hardware.

---

# Glossary

* **Main / firmware** — the program that contains the loader and calls the
  module.
* **Module / library** — the ELF file being loaded.
* **ET_REL** — relocatable object (`.o`); section-based, no GOT.
* **ET_DYN** — shared object (`.so`); segment-based, usually PIC.
* **PIC** — position-independent code; uses a GOT and (often) r10.
* **r10 / got_base** — the register/address a PIC module uses to reach its GOT.
* **GOT / PLT** — tables of data addresses / function stubs.
* **LMA / VMA** — load address in storage / run address in memory.
* **XIP** — execute in place; code runs from flash (or a DRAM image region)
  instead of being copied to a RAM arena.
* **Overlay** — several modules linked at one VMA, swapped by copying.
* **Relocation** — a fix-up applied to code/data after the copy.
* **Symbol source** — how imported names are resolved (main's `.dynsym`, a table,
  etc.).
* **Semihosting** — debugger-mediated I/O (`BKPT 0xAB` on Cortex-M,
  `SVC 0x123456` on AArch32).
* **`.ARM.exidx` / `.ARM.extab`** — ARM exception/unwind tables.

---

*End of document.*
