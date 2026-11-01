---
title: "Using shared libraries with SCL-shared-new"
subtitle: "The lifecycle: files arrive, are loaded, parsed, and used"
author: "SCL / SCL-shared-new"
date: "2026-11-01"
---

# 0. The lifecycle in one page

Every SCL shared-library load follows the same four stages:

```text
  1. THE FILE ARRIVES          2. IT IS LOADED              3. IT IS PARSED           4. IT IS USED
  ┌──────────────────┐        ┌──────────────────┐         ┌──────────────────┐      ┌──────────────────┐
  │ semihosting      │  bytes │ byte_source or   │  image  │ elf_image::parse │      │ relocations      │
  │ SD / FatFs       │ ─────► │ a RAM span;      │ ──────► │ sections,        │ ───► │ symbols, GOT/PLT │
  │ flash archive    │        │ copy segments /  │         │ segments,        │      │ constructors,    │
  │ SPI NOR          │        │ sections, zero   │         │ relocations,     │      │ .ARM.exidx,      │
  │ linked-in        │        │ bss              │         │ symbols          │      │ call / unload    │
  └──────────────────┘        └──────────────────┘         └──────────────────┘      └──────────────────┘
        Part 1                       Part 2                      Part 3                    Part 4
```

The split is deliberate:

* **The app owns stage 1.**  It knows whether the file is on a host, an SD
  card, an internal flash archive, or already linked in.
* **The loader core is I/O-free.**  It consumes a `std::span<const std::byte>` or
  a `loader::byte_source`, never `open()`/`read()`/semihosting.
* **The parser is `common-elf-reader`.**  It reads and validates the ELF; every
  method except `scl::reloc` and `scl::overlay` starts with
  `elf_image::parse`.
* **The method is the policy.**  There are nine: `reloc`, `static`,
  `xip-static`, `optimal`, `dynamic-nolib`, `dynamic-newlib`,
  `xip-dynamic-nolib`, `xip-dynamic-newlib`, and `overlay`.  They differ in
  where code runs, how symbols are resolved, and whether the image can be
  unloaded.

Read Parts 1–4 in order the first time.  Part 5 is the decision guide,
architecture notes and the pitfalls checklist.

---

# Part 1 — The file arrives on the app

## 1.1 Two different "loads"

Do not confuse these:

| What | Loaded by | Which file | When |
|---|---|---|---|
| the **firmware** (loader + app) | OpenOCD, a debugger, or the boot ROM | the firmware `.elf` or `.hex` | flashing / debug |
| the **module** (shared library) | the application, through a storage front-end | `.rel.o`, `.so`, `.pic.so`, `.opt.so`, a flash blob, or nothing (overlay) | at boot, or before first use |

The debugger flashes the **firmware**.  For the RAM loaders it does not flash
the module; the app reads the module itself.  For XIP the module image is
provisioned to flash/DRAM and the loader consumes that image as a blob.

## 1.2 The module file shapes

The build command determines which loaders can consume the file.

| Shape | Build | Consumers | Key property |
|---|---|---|---|
| **Relocatable object** | `-fno-pic -mlong-calls -r` (`.rel.o`) | `scl::reloc` | section headers + `.symtab`; no GOT/PLT |
| **PIC shared object** | `-fPIC -shared` (`.so`, `.pic.so`) | `static`, `dynamic-nolib`, `dynamic-newlib` | GOT/PLT; text is position-independent |
| **Split PIC shared object** | `-fPIC -shared -T xip-split.ld` | `xip-static`, `xip-dynamic-*` | one R-X and one RW segment at known VAs |
| **No-pic fixed object** | `-fno-pic -mlong-calls -shared -T nopic-split.ld` | `optimal` | fixed windows; no GOT, no r10 |
| **Overlay object** | ordinary `.o`, linked into the firmware at one VMA with `AT>RAM` | `overlay` | no relocation at load; one resident |

File names are conventions; the loader checks the ELF magic, not the extension.

| Name | Built as | Typically consumed by |
|---|---|---|
| `libprov.rel.o` | `-r` ET_REL | `scl::reloc` |
| `libprov.so` | `-fpic -msingle-pic-base -mpic-register=r10 -shared` | `dynamic-nolib`, `xip-dynamic-*` |
| `libprov.pic.so` | `-fPIC -shared` | `static`, `dynamic-newlib` |
| `libprov.opt.so` | `-fno-pic -mlong-calls -shared -T nopic-split.ld` | `optimal` |
| a split `.so` | `-T xip-split.ld` | `xip-static` |
| no file | linked into the firmware | `overlay` |

A practical warning: `-mlong-calls` does not guarantee literal-pool `ABS32`
relocations.  ARM `-O0`/`-O2` may emit `MOVW/MOVT`; `-Os` often uses `ABS32`.
SCL supports both.

## 1.3 Semihosting: read the whole file into RAM

This is the path used by the SCL examples and by the Lyra/Pi-Zero tests.

On Cortex-M the trap is `BKPT 0xAB`; on Cortex-A (AArch32) it is `SVC 0x123456`.
The operation number goes in `r0`, the argument pointer in `r1`.

```cpp
// includes: <cstdint>, <cstddef>, <cstring>, <span>
// AArch32.  Cortex-M: replace the svc with bkpt 0xAB.
static unsigned semi(unsigned op, const void* arg) {
    register unsigned r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = arg;
    __asm__ volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory", "lr");
    return r0;
}

// SYS_OPEN=0x01, SYS_CLOSE=0x02, SYS_READ=0x06, SYS_FLEN=0x0C
static std::byte g_file[32 * 1024];

std::span<const std::byte>
read_semihost_file(const char* path, std::byte* buf, std::size_t cap) {
    std::uint32_t open_args[3] = {
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(path)),
        1u,                                     // mode 1 = "rb"
        static_cast<std::uint32_t>(std::strlen(path))
    };
    int h = static_cast<int>(semi(0x01, open_args));
    if (h < 0) return {};
    std::uint32_t handle = static_cast<std::uint32_t>(h);

    const std::size_t n = semi(0x0C, &handle);   // SYS_FLEN
    if (n == 0 || n > cap) { semi(0x02, &handle); return {}; }

    std::uint32_t read_args[3] = {
        handle, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(buf)),
        static_cast<std::uint32_t>(n)
    };
    if (semi(0x06, read_args) != 0) { semi(0x02, &handle); return {}; }
    semi(0x02, &handle);                         // SYS_CLOSE
    return {buf, n};
}
```

The file then becomes a memory span:

```cpp
auto bytes = read_semihost_file("libprov.so", g_file, sizeof g_file);
if (bytes.empty()) return false;

loader::memory_byte_source src{bytes};
auto h = fw::dlopen_source(src, "libprov");
```

Properties and rules:

* one open, one length, one read, one close per module;
* the buffer must stay alive while the loader uses it;
* the path is a **host** path, relative to the debugger's working directory;
* each call stops and resumes the target — read once at boot, not in a loop;
* the debugger must have semihosting enabled.

## 1.4 Semihosting/POSIX fd: let the parser read on demand

`fd_byte_source` does not read the whole file.  It caches the size and performs
`lseek` + `read` for each `read_at`.

```cpp
#include "loader/sources/fd-byte-source.hpp"

auto f = loader::fd_byte_source::open_file("libprov.pic.so");
if (!f) return false;

auto img = loader::elf_image::parse(*f);      // reads only the header
auto text = img->find_section(".text");       // reads the section table
// the loader reads the sections it needs, one read_at at a time
```

Internals:

* `open_file()`: `::open(path, O_RDONLY)`, `lseek(fd, 0, SEEK_END)` for the
  size, then seek back;
* `read_at()`: `lseek(offset, SEEK_SET)` plus a loop of `::read()` until `out`
  is full;
* the destructor closes the fd.

On a bare-metal target `open/read/lseek` are the C library's calls; the platform
can back them with semihosting.  If it does not, read the whole file into RAM
and use `memory_byte_source`.

`scl::static::Module::load_file()` is the loader that uses this path.  Its
`load_buf()` is the equivalent for an in-memory image.

## 1.5 SD card / FatFs: read a file into RAM

The Lyra has a microSD card.  The board support exposes `sd::SdCard` and
`fatfshw`; the card is the **boot card**, so files live under `tests/` and the
FAT root is never touched.

```cpp
#include <sd.hpp>
#include <fatfs_hw.hpp>

static sd::SdCard card;
static std::byte  buf[64 * 1024];

bool load_from_sd(const char* leaf) {          // e.g. "libprov.so"
    if (!card.init()) return false;            // needs the kernel clock
    fatfshw::bind_card(card);
    if (!fatfshw::mount_volume()) return false;
    if (!fatfshw::ensure_tests_dir()) return false;

    std::uint32_t size = 0;
    if (!fatfshw::file_size(leaf, size)) return false;
    if (size > sizeof buf) return false;
    if (!fatfshw::load_file(leaf, buf, size)) return false;

    loader::memory_byte_source src{
        std::span<const std::byte>{buf, size}};
    auto h = fw::dlopen_source(src, leaf);
    return h.has_value();
}
```

Rules:

* names are 8.3 under `tests/`; `leaf` is just the file name;
* `file_size()` first, then `load_file()` with the exact size; no allocation
  inside the helper;
* card I/O is one core/task at a time; mount before releasing the other cores;
* the current Lyra examples use the card for the `tests/sclsd.bin` data file;
  the same path loads a module.

## 1.6 Internal flash flat filesystem: a memory-mapped span

On a board whose internal flash is memory-mapped, the archive returns a pointer;
there is no copy.

```cpp
#include "fs-archive.hpp"        // the test's flatfs, not part of SCL

if (const fw::FsEntry* e = fw::fs_find("libprov.so")) {
    std::span<const std::byte> bytes{fw::fs_blob(*e), e->size};
    loader::memory_byte_source src{bytes};
    auto h = fw::dlopen_source(src, "libprov");
}
```

The flat archive is a small append-log (`FsSuper`, `FsEntry`, `fs_add`,
`fs_find`, `fs_entry`, `fs_blob`).  A module is added at provisioning time:

```cpp
if (!fw::fs_valid()) (void)fw::fs_format();
if (auto r = fw::fs_add("libprov.so", bytes.data(), bytes.size()); !r) {
    // TOC or region full
}
```

`fs_blob()` points into mapped flash, so `memory_byte_source` is zero-copy.
The archive is a test utility, not part of the SCL core; a product can use any
flash layout that returns a span.

## 1.7 SPI NOR: read into RAM first

The WeAct W25Q64 is SPI NOR: it is **not** memory-mapped.  The SPI archive's
`fs_blob()` returns null; the module must be read into RAM by the SPI driver,
then wrapped in `memory_byte_source`:

```cpp
// The SPI archive exposes provisioning and a driver read; the exact read call
// is the board's (fs_init installs the function pointers).
std::byte ram[64 * 1024];
if (!spi_read_file("libprov.so", ram, sizeof ram, &size)) return false;

loader::memory_byte_source src{{ram, size}};
auto h = fw::dlopen_source(src, "libprov");
```

This is also the rule for any storage that is not directly addressable: read
once into RAM, then load.

## 1.8 Linked-in overlay: no file at all

An overlay module is linked with the firmware.  The linker gives every module
the same VMA and its own LMA; "loading" is a copy.

```cpp
#include "overlay/overlay.h"

extern "C" {
extern std::uint8_t _ovl_vma[], _ov1_lma[], _ov1_size[], _ov2_lma[], _ov2_size[];
int ov1_use_fm(void);
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
    int v = ov1_use_fm();
    (void)v;
    return true;
}
```

No parser, no relocation, no symbol lookup.  Only one module is resident at a
time.

## 1.9 Host tests and QEMU: a vector or a file

There is no hardware storage.  A test reads the file into a `std::vector` or
opens it with an fd source:

```cpp
std::vector<std::byte> bytes = read_file("fixtures/libprov.so");
loader::memory_byte_source src{bytes};
auto img = loader::elf_image::parse(src);
```

or, on a POSIX host:

```cpp
auto f = loader::fd_byte_source::open_file("fixtures/libprov.so");
auto img = loader::elf_image::parse(*f);
```

## 1.10 Buffer sizing and lifetime

| Loader | Size source |
|---|---|
| `scl::reloc` | fixed arena; sections packed |
| `scl::static` | `scl::Module::image_size(path)`, or `exec_buf` size |
| `scl::optimal` | fixed `_sxip_text_*`/`_sxip_data_*` windows |
| `scl::xip-static` | text in image/flash; data in a RAM arena |
| `scl::dynamic-nolib` | `elf_image::image_size()` (highest `PT_LOAD` end) |
| `scl::dynamic-newlib` | `image_size()` + journal capacity |
| `scl::xip-dynamic-*` | data in a RAM arena; text in the image |
| `scl::overlay` | `SIZEOF(.ovN)` |

`SYS_FLEN` or `file_size()` gives the file size before the read.  The buffer must
outlive the load.  `memory_byte_source` does not copy; it points at the caller's
bytes.

## 1.11 Failures in the arrival phase

| Symptom | Cause |
|---|---|
| `SHORT_READ` | truncated file, fd ended early, wrong length |
| `FILE_NOT_FOUND` | wrong host path, missing SD file, FAT name not 8.3 |
| semihosting hangs | debugger not attached or semihosting not enabled |
| `fs_find` returns null | archive not formatted, name mismatch, module not provisioned |
| SPI `fs_blob` null | the flash is not memory-mapped; read it into RAM |
| live-lock | card I/O from two tasks/cores at once |

---

# Part 2 — The module is loaded

"Loaded" here means the bytes are now at their runtime addresses and `.bss` is
zeroed.  Relocations have **not** run yet.

## 2.1 `loader::byte_source`

The loader has no `byte()` call.  The interface is:

```cpp
class byte_source {
  virtual std::size_t size() const noexcept = 0;
  virtual result<void> read_at(std::size_t offset,
                               std::span<std::byte> out) noexcept = 0;
};
```

`read_at()` fills exactly `out.size()` bytes or returns `SHORT_READ`.  Two
implementations ship with the parser:

* `loader::memory_byte_source` — wraps a `std::span<const std::byte>`;
* `loader::fd_byte_source` — wraps a file descriptor (POSIX/semihosting).

A custom source is two methods:

```cpp
class my_source final : public loader::byte_source {
public:
    explicit my_source(const my_storage& s) : s_(s) {}
    std::size_t size() const noexcept override { return s_.count(); }
    loader::result<void>
    read_at(std::size_t off, std::span<std::byte> out) noexcept override {
        if (off > s_.count() || out.size() > s_.count() - off)
            return loader::unexpected(loader::error::SHORT_READ);
        return s_.copy(off, out);
    }
private:
    const my_storage& s_;
};
```

Rules: exact size; all-or-nothing read; keep the backing storage alive; random
access is allowed (the parser may ask for the same header repeatedly).

## 2.2 The universal load sequence

```text
1. obtain the bytes          semihosting / SD / flash / linked-in
2. wrap them                 span or byte_source
3. parse the ELF header      elf_image::parse (or the ET_REL parser)
4. enumerate what to copy    PT_LOAD segments / SHF_ALLOC sections
5. validate the ranges       offset + filesz, vaddr + memsz, destination
6. reserve the destination   arena / exec buffer / fixed window / heap
7. copy                      memcpy filesz bytes
8. zero the tail             memset memsz - filesz (bss)
9. (then) relocate           Part 4
```

## 2.3 Where each method puts the image

| Method | Code placement | Data placement | What is copied |
|---|---|---|---|
| `reloc` | arena slot | arena slot | every `SHF_ALLOC` section |
| `static` | `exec_buf` | `exec_buf` | every `PT_LOAD` segment |
| `xip-static` | flash/image VA | RAM arena | text stays in flash; RW copied |
| `optimal` | `_sxip_text_*` window | `_sxip_data_*` window | text and data, 0-bias |
| `dynamic-nolib` | image arena | image arena | every `PT_LOAD` segment |
| `dynamic-newlib` | heap image | heap image | every `PT_LOAD` segment |
| `xip-dynamic-*` | image/flash | RAM arena | text stays; RW copied (or whole image on fallback) |
| `overlay` | shared window | shared window | LMA → VMA copy |

## 2.4 Exact pre-relocation call chains

### `scl::reloc`

```text
read_host_file("name.rel.o")
  → reloc::load_set({images}, arena, resolve)
      → place_module(image, arena, used, m)
          - ELF magic / EM_ARM / ET_REL
          - e_shoff / e_shentsize / e_shnum
          - each SHF_ALLOC: align, record m.sec_base, copy or zero
      → apply_relocs(...)                // relocation starts
```

No `elf_image::parse`; it reads the section headers with
`loader/detail/bytes.hpp`.

### `scl::static`

```text
Module::load_file(path, exec_buf, exec_sz, syms)
  → elf_reader::open_elf(path)
      → fd_byte_source::open_file(path)   // open + seek-end
      → verify_elf() → elf_image::parse
  → scl_loader_lock()
  → Module::build()
      → shlib_loader::load_bin(exec_buf, exec_sz)
          - needed = efr_.get_binary_size()  // image_size
          - each program header:
              efr_.load_segments({exec_buf, needed}, ph)
                → read_at_offset → src_->read_at(p_offset, ...)
                → memset bss
      → patch_shlib()                    // relocation starts
```

`load_buf()` is the same with a `memory_byte_source`; `Module::image_size(path)`
opens the file only to size the buffer.

### `scl::xip-static`

```text
provision: bind_host_imports(image, flash_base)
  → elf_image::parse
  → set_extab_thumb_bits / prelink_flash_data
  → patch external JUMP_SLOTs

load: xip::stat::load(blob, m)
  → elf_image::parse
  → classify_segments / text_is_immutable
  → alloc(data_memsz) in image_arena
  → memset + memcpy data segment
  → text_base = blob + text_off      // text is already there
  → apply_rel_xip(...)               // relocation starts
```

### `scl::optimal`

```text
xip::nopic::load(blob, m)
  → elf_image::parse
  → program_header: find R-X and RW
  → check the fixed windows
  → memcpy text to _sxip_text_*, data to _sxip_data_*
  → memset data bss
  → relocation loop                  // relocation starts
```

### `scl::dynamic-nolib`

```text
fw::dlopen_source(src, name)
  → elf_image::parse
  → image_size = highest PT_LOAD end
  → image_arena().alloc + memset
  → for each PT_LOAD: src.read_at(p_offset, mem + p_vaddr, p_filesz)
  → section-header cache (shdrs + shstrtab once)
  → reloc_applier::apply_rel(...)    // relocation starts
```

### `scl::dynamic-newlib`

```text
dynamic_linker::load_from_source(src)
  → elf_image::parse
  → image_base / image_size
  → image_memory::load_segments: alloc, read_at per PT_LOAD, zero bss
  → collect_relocations()
  → read .dynsym/.dynstr
  → relocator::compute / apply_plan  // relocation starts
```

### `scl::xip-dynamic-*`

```text
xip::nolib::dlopen(so_image, name)   // or xip::newlib
  → elf_image::parse
  → classify_segments / text_is_immutable
  → XIP: RW to RAM, text at so_image + text_off
    or fallback: whole image to RAM arena
  → apply_rel_xip(...)               // relocation starts
```

### `scl::overlay`

```text
scl_overlay_load(&region, index)
  → memcpy(vma, lma, size)
  → cache clean/invalidate
  → DSB/ISB
```

No file, no parser, no relocation.

## 2.5 What is not done yet

At the end of this phase:

* the ELF is validated and the bytes are in place;
* `.bss` is zeroed;
* section/segment metadata is still available (source span, `elf_image`, or the
  loader's own tables).

No relocation, no symbol resolution, no constructor, no cache publication.

## 2.6 Load-phase failures

| Message | Meaning |
|---|---|
| `short read` | file truncated or fd ended early |
| `not an ELF` | wrong file / bad magic |
| `not ARM32` | wrong class, endianness or machine |
| `ELF parse failed` | bad header sizes or EABI |
| `segment exceeds buffer` | `exec_buf`/arena too small |
| `R-X VA outside the text window` | wrong linker script or loader |
| `need a split R-X / RW image` | non-split `.so` given to an XIP loader |

---

# Part 3 — The module is parsed

## 3.1 What the parser is

`common-elf-reader` is a small ARM ELF32 little-endian reader, vendored as the
`elf32_read_parse-new.git` submodule (`f650889`).  It validates the file and
gives typed access to the headers, segments, sections, symbols and relocations.
It does **not** allocate, relocate, resolve or run anything.

## 3.2 The ELF structures that matter

| Structure | What the loader asks it |
|---|---|
| `Ehdr` | magic, class, data, machine, EABI, entry, header sizes |
| `Phdr` (`PT_LOAD`) | `p_vaddr`, `p_offset`, `p_filesz`, `p_memsz`, `p_flags` (R/W/X) |
| `Shdr` | section type/flags/address/offset/size, `.dynsym`, `.dynstr`, `.hash`, `.rel.*`, `.init_array`, `.ARM.exidx` |
| `Sym` | name, value (bit 0 = Thumb), size, binding, type, section index |
| `Rel` | `r_offset`, `r_info` (symbol index + type) |
| `Dyn` | `_DYNAMIC`: `DT_SYMTAB`, `DT_STRTAB`, `DT_REL`, `DT_JMPREL`, `DT_PLTGOT`, `DT_INIT_ARRAY` |

The detailed ARM relocation formulas (`CALL`, `JUMP24`, `MOVW/MOVT`, `THM_*`,
`PREL31`, ...) are in the `ARM A32 support` document and in
`loader/arm-reloc.hpp`.

## 3.3 The parser API, with examples

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
}
```

### Find a section and read its bytes

```cpp
if (auto text = img->find_section(".text"); text && text->sh_size) {
    std::byte buf[256];
    const std::size_t n = text->sh_size < sizeof buf ? text->sh_size : sizeof buf;
    if (img->source().read_at(text->sh_offset, {buf, n})) {
        // buf[0..n) is the start of .text
    }
}
```

### Read directly from a file descriptor

```cpp
#include "loader/sources/fd-byte-source.hpp"

auto f = loader::fd_byte_source::open_file("libprov.pic.so");
if (f) {
    auto img = loader::elf_image::parse(*f);
    // ...
}
```

### Image size and base

```cpp
auto size = img->image_size();   // highest PT_LOAD end
auto base = img->image_base();   // lowest PT_LOAD vaddr
```

### Collect all dynamic relocations (hosted only)

```cpp
auto rel = img->collect_relocations();   // std::vector<std::byte>
auto plan = loader::relocator::compute(*rel, dynsym, dynstr);
```

`collect_relocations()` concatenates every allocated `SHT_REL` section
(`.rel.dyn`, `.rel.plt`, ...) into one buffer.  It is the only parser API that
needs `SCL_HOSTED=ON`; `scl::dynamic-newlib` uses it.

### Build main's symbol table over semihosting, one read per section

```cpp
#include "scl/symtab-runtime.hpp"

std::byte symtab_buf[4096];
char      strtab_buf[4096];
scl::symtab::entry entries[64];

std::size_t n = scl::symtab::collect_buffered(
    *img, entries, 64, symtab_buf, sizeof symtab_buf,
    strtab_buf, sizeof strtab_buf);
ctx.set_symtab(reinterpret_cast<const firmware::sym_entry*>(entries), n);
```

### Compile-time registration

```cpp
#include "scl/symtab-register.h"
#include "scl/symtab-runtime.hpp"

extern "C" int host_f(int);
SCL_REGISTER_SYM(host_f);

loader::symbol_source main_syms = scl::symtab::source_registered();
```

### The A32 relocation engine

```cpp
#include "loader/arm-reloc.hpp"

std::uint32_t addend = 0;
if (loader::arm::decode_addend(type, p, addend)) {
    loader::arm::context c{S, addend, P, loader::arm::isa::arm};
    if (loader::arm::apply(type, p, c) != loader::error::OK) {
        // RELOC_OVERFLOW or RELOC_INTERWORK
    }
}
```

### ARM attributes and ABI

```cpp
#include "loader/arm-attributes.hpp"
#include "loader/arm-abi.hpp"

auto a = loader::read_arm_attributes(*img);
if (a.valid && a.has_vfp_args && a.vfp_args == 1) {
    // the module passes arguments in VFP registers
}
if (auto r = loader::check_arm_abi(*img, true); !r) {
    // module and firmware disagree on hard/soft float
}
```

## 3.4 Where the parser is used, method by method

Legend:

* **P** `elf_image::parse`
* **H** `header()`
* **PH** `program_header()` (or `SegmentMap`)
* **SH** `section_header()`
* **FS** `find_section()`
* **RA** `source().read_at()`
* **CR** `collect_relocations()`
* **SZ** `image_size()` / `image_base()`
* **BS** byte source
* **AR** the A32 relocation engine

| Method | P | H | PH | SH | FS | RA | CR | SZ | BS | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| `reloc` | — | — | — | — | — | — | — | — | span | manual ET_REL; `elf-types`/`detail`/`arm-reloc` |
| `static` | ✓ | ✓ | ✓ | ✓ | ✓* | ✓ | — | ✓ | fd/memory | `elf_reader` wrapper; `*` own `find_section_by_name` |
| `xip-static` | ✓ | ✓ | ✓ | — | ✓ | ✓ | — | — | memory | `SegmentMap`; `bind_host_imports` re-parses |
| `optimal` | ✓ | ✓ | ✓ | — | ✓ | —** | — | — | memory | `**` reads section bytes from the blob |
| `dynamic-nolib` | ✓ | ✓ | ✓ | — | —*** | ✓ | — | ✓ | any source | `***` section-header cache |
| `dynamic-newlib` | ✓ | ✓ | ✓ | — | ✓ | ✓ | ✓ | ✓ | any source | `CR` builds the plan |
| `xip-dynamic-nolib` | ✓ | ✓ | ✓ | — | ✓ | —** | — | — | memory | adapter-base re-parses for imports |
| `xip-dynamic-newlib` | ✓ | ✓ | ✓ | — | ✓ | —** | — | — | memory | adapter-base re-parses for imports |
| `overlay` | — | — | — | — | — | — | — | — | none | linked-in |

The one exception worth remembering: `scl::reloc` parses `ET_REL` itself.  It
uses the parser's types and bounded byte helpers, but not `elf_image::parse`.

## 3.5 What the parser does not do

It does not:

* allocate memory or choose where the image runs;
* copy segments/sections or zero `.bss`;
* apply relocations or resolve symbols;
* run constructors, register `.ARM.exidx`, clean caches, or publish.

Those belong to the loaders in Part 4.

---

# Part 4 — The module is used

## 4.0 Linking a loader and supplying main's symbols

```cmake
add_subdirectory(SCL-shared scl)
target_link_libraries(my_firmware PRIVATE scl::reloc)
# or scl::static, scl::optimal, scl::xip-static, scl::dynamic-nolib,
#    scl::dynamic-newlib, scl::xip-dynamic-nolib, scl::xip-dynamic-newlib,
#    scl::overlay
```

Or `-DSCL_LOADER=optimal` and link `scl::loader`.  `SCL_HOSTED=ON` is required
for `scl::dynamic-newlib`.

Main's symbols can come from:

| Source | When |
|---|---|
| main's `.dynsym` (`-pie -Wl,-E`) | `link_context::from_self()` |
| a `scl_sym_entry` table | non-PIE firmware, boot or build time |
| `SCL_REGISTER_SYM` | no file, no parser |
| a sidecar/firmware ELF read over semihosting | `collect_buffered()` |
| a build-time `gen-symtab` table | hosted `dynamic-newlib` |

The loader's own exports come from the module's `.dynsym`/`.hash` or a linear
scan and are available to later loads.

## 4.1 `scl::reloc` — `ET_REL` in RAM

**Loads:** every `SHF_ALLOC` section into an arena, packed and aligned.
**Parses:** manually (ET_REL), with `elf-types`/`detail/bytes`.
**Relocates:** `ABS32`, `TARGET1`, `TARGET2`, `REL32`, `NONE`, `PREL31`,
`THM_CALL`, and the A32 set via `loader::arm::apply`.
**Uses:** `reloc::sym()` returns the exported address; the app calls it.

```cpp
#include "reloc/reloc-loader.hpp"

alignas(8) static std::byte arena[96 * 1024];

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
    float v = add(2.5f, 3.5f);        // 6.0
    (void)v;
    return true;
}
```

For mutually dependent modules, pass several spans to `load_set()`; it places
all of them before resolving.  For one extra module, use `reloc::load` and
`reloc::sym(module, name)`.

## 4.2 `scl::static` — a PIC `.so` copied whole into RAM

**Loads:** the whole image into `exec_buf`.
**Parses:** `elf_reader` → `elf_image::parse`.
**Relocates:** `RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS32`, `REL32`, `COPY`,
TLS; patches main's GOT.
**Uses:** `Module::sym<T>()`; `unload()` reverts main byte-exactly.

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
    int v = prov(5);                  // 105
    (void)v;
    mod.unload();
    return true;
}
```

From memory: `mod.load_buf(reinterpret_cast<const uint8_t*>(bytes.data()),
bytes.size(), exec_buf, sizeof exec_buf, &syms)`.  To size the buffer:
`auto need = scl::Module::image_size("libprov.pic.so")`.

## 4.3 `scl::xip-static` — split PIC, code in place, data in RAM

**Loads:** text stays at its flash/image VA; the RW segment is copied to RAM.
**Parses:** `elf_image::parse` + `SegmentMap`.
**Relocates:** split-base applier; external function imports pre-linked before
flashing.
**Uses:** `xip::stat::resolve()`; r10 is the module's `got_base`.

```cpp
#include "xip/static/adapter.hpp"

bool provision_and_load() {
    std::span<std::byte> writable{module_image, module_size};
    xip::stat::bind_host_imports(writable);   // before flashing

    xip::stat::SModule m;
    if (!xip::stat::load(std::span<const std::byte>{module_image, module_size}, m))
        return false;

    auto prov = reinterpret_cast<int (*)(int)>(xip::stat::resolve(m, "prov_value"));
    if (!prov) return false;
    int v = prov(5);                  // 105
    (void)v;
    return true;
}
```

`bind_host_imports()` is required for a normal `.so` with a PLT before the image
is written to write-once flash.  On a DRAM "XIP" region it can be done at boot.

## 4.4 `scl::optimal` — no-pic, fixed windows, no r10

**Loads:** text and data to the fixed `_sxip_text_*`/`_sxip_data_*` windows,
0-bias; bss zeroed.
**Parses:** `elf_image::parse`, `program_header`, `find_section`.
**Relocates:** `RELATIVE` skipped; `ABS32`/`REL32` and A32/T32 branches via the
shared engine.
**Uses:** plain function pointers; `resolve_all()` for cycles.

```cpp
#include "xip/static/nopic.hpp"
#include "scl/symtab-runtime.hpp"

bool load_prov_optimal(std::span<const std::byte> bytes) {
    xip::nopic::set_main_symbols(scl::symtab::source_registered());

    xip::nopic::PModule m;
    if (!xip::nopic::load(bytes, m)) return false;
    xip::nopic::resolve_all();

    auto prov = reinterpret_cast<int (*)(int)>(xip::nopic::resolve(m, "prov_value"));
    if (!prov) return false;
    int v = prov(5);                  // 105
    (void)v;
    return true;
}
```

`patch_main(m)` after load lets the app call the module by name through main's
GOT.  The image must be linked at the windows.

## 4.5 `scl::dynamic-nolib` — freestanding `dlopen`

**Loads:** image in `fw::image_arena()`; section-header cache.
**Parses:** `elf_image::parse`, `image_size`, `program_header`.
**Relocates:** `RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS32`, `REL32`, `COPY`;
optional lazy/cross trampolines.
**Uses:** `fw::dlsym`; `dlclose` frees the image.

```cpp
#include "dynamic-nolib/dl.hpp"

bool load_prov_nolib(std::span<const std::byte> bytes) {
    fw::dl_set_symtab(kSyms, sizeof kSyms / sizeof kSyms[0]);
    fw::dl_set_lazy_binding(false);    // no loader code after publish

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

`h` is `result<handle*>`; `*h` is `handle*`; `**h` is `handle&`.  For an fd:
`auto f = loader::fd_byte_source::open_file("libprov.so"); auto h =
fw::dlopen_source(*f, "libprov");`.

## 4.6 `scl::dynamic-newlib` — hosted POSIX `dlopen`

**Loads:** segments into a heap image plus journal capacity.
**Parses:** `elf_image::parse`, `collect_relocations`, `find_section`.
**Relocates:** a `relocator` plan; pending imports re-patched later.
**Uses:** `linker.lookup`; `unload` reverts the journal.

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

With `-pie -Wl,-E`, leave `ctx` empty; `link_context` reads main's own
`_DYNAMIC`/`.dynsym`.

## 4.7 `scl::xip-dynamic-nolib` and `scl::xip-dynamic-newlib`

**Loads:** if the image is a clean split, text stays in the image/flash and RW
goes to RAM; otherwise the whole image is copied to the arena.
**Parses:** `elf_image::parse`, `SegmentMap`, `text_is_immutable`.
**Relocates:** split-base applier; `patch_main_imports` at provision time.
**Uses:** `dlopen`/`dlsym`/`dlclose`.

```cpp
#include "xip/dynamic-nolib/adapter.hpp"

void* load_prov_xip(std::span<std::byte> writable, std::span<const std::byte> image) {
    xip::nolib::set_symtab(kSyms, sizeof kSyms / sizeof kSyms[0]);
    xip::nolib::patch_main_imports(writable);   // before flashing

    void* h = xip::nolib::dlopen(image, "libprov");
    if (!h) return nullptr;
    auto prov = reinterpret_cast<int (*)(int)>(xip::nolib::dlsym(h, "prov_value"));
    (void)prov;
    // xip::nolib::dlclose(h) when done
    return h;
}
```

For newlib, replace `xip::nolib` with `xip::newlib` and cast the table to
`const firmware::sym_entry*`.

## 4.8 `scl::overlay` — one window, several modules

**Loads:** `memcpy(LMA → VMA)`, cache clean/invalidate, `DSB`/`ISB`.
**Parses/relocates:** nothing; the host linker did it.
**Uses:** the linked-in function names; only one module is resident.

```cpp
#include "overlay/overlay.h"

extern "C" {
extern std::uint8_t _ovl_vma[], _ov1_lma[], _ov1_size[], _ov2_lma[], _ov2_size[];
int ov1_use_fm(void);
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
    int v = ov1_use_fm();
    (void)v;
    return true;
}
```

## 4.9 Calling, constructors, exceptions, unload

### Calling a module

* ARM state: a plain function pointer; the compiler uses `blx`.
* Thumb state: the function pointer carries bit 0; do not clear it.
* r10 models: enter through `pic_call_*` or the cross-call trampoline with the
  module's `got_base`; a plain call would leave r10 wrong.
* `scl::reloc`/`optimal`: no GOT, plain pointers.

### Constructors and destructors

`.init_array` runs after relocation; `.fini_array` runs on unload (where the
loader supports unload).  `scl::overlay` constructors are part of the firmware's
boot-time `.init_array`, not the swap.

### Exceptions

`.ARM.exidx`/`.ARM.extab` must be registered.  The dynamic and XIP adapters do
this at load.  `libsupc++` is needed for a full C++ exception runtime; use the
loader's `--wrap=malloc/free` when the firmware's allocator is not thread-safe.

### Missing imports

A load can succeed with an unresolved import; calling it faults.  Check after
the whole set:

```cpp
if (const char* missing = lib_first_missing()) { /* configuration error */ }
```

Each loader has its own: `reloc` (a load failure), `xip::stat::first_missing_import`,
`xip::nopic::first_missing_import`, `fw::dl_first_missing_import`,
`linker.first_missing_import`, `xip::nolib::first_missing_import`.

### Unload

`static`, `dynamic-*`, `xip-static`, and `xip-dynamic-*` support unload;
`reloc`, `optimal`, and `overlay` do not (arena/window reuse or fixed windows).
Quiesce all callers first; the loader lock does not protect against a stale
function pointer.

---

# Part 5 — Choosing, architecture, pitfalls

## 5.1 Choosing a method

| Requirement | Method |
|---|---|
| smallest code; one core; no unload | `reloc` |
| unload/reload, byte-exact revert | `static` or `dynamic-newlib` |
| execute from real flash, save RAM | `xip-static`, `optimal`, `xip-dynamic-*` |
| no r10, plain pointers | `reloc`, `optimal` |
| no libc | `reloc`, `optimal`, `dynamic-nolib` |
| hosted libc/POSIX | `dynamic-newlib` |
| several modules share one window | `overlay` |
| cyclic module imports | `reloc` (`load_set`), `optimal` (`resolve_all`), dynamic (later loads) |
| C++ exceptions | add `libsupc++`; use a loader that registers `.ARM.exidx` |
| hard real-time | eager binding, load before releasing cores |

## 5.2 Architectures and boards

### Cortex-M (M0/M0+/M3/M4/M7)

* Thumb-only; every function has bit 0 set.
* No MMU; M7 has a data cache (cache hooks matter).
* All methods work; r10 models need `-msingle-pic-base -mpic-register=r10`.
* Semihosting: `BKPT 0xAB`.
* RP2350: XIP flash, two cores; use `scl::rp2350-flash` if one core writes flash.

### Cortex-A (A7/A53, AArch32)

* ARM and Thumb coexist; preserve the symbol's bit 0.
* MMU and caches on; clean/invalidate after writing code; publish before use.
* Semihosting: `SVC 0x123456`.
* ARM modules need the A32 relocation engine (`CALL`/`BLX`, `MOVW/MOVT`).
* Luckfox Lyra: no application flash; XIP runs from a DRAM image region; storage
  is the SD card; three cores make the lock/publication rules mandatory.

### Hard/soft float

The module and firmware must agree.  `check_arm_abi()` compares
`.ARM.attributes`/`e_flags`; a mismatch is an argument-passing bug.

### Boards

| Board | CPU | Memory | Notes |
|---|---|---|---|
| WeAct F411CE | M4F, 1 core | 512K flash, 128K RAM | simple; W25Q64 for storage |
| Pi-Zero RP2350B | 2x M33 | XIP flash + PSRAM | XIP loaders; flash writes with both cores |
| Luckfox Lyra B | 3x A7 | DRAM only; SD | all loaders; A32/T32; MMU/caches/SMP |

## 5.3 Loader lock, cache, publication

* `scl_loader_lock()` must be recursive (a load can allocate and take it again).
* Wrap load/unload, not ordinary calls.
* After writing code: clean data + invalidate instruction cache; on Cortex-A/M7
  the default barrier-only hooks are not enough.
* `scl_publish_release()` before another core/thread uses the module.
* For RT: `dl_set_lazy_binding(false)`; no loader code after publication.

## 5.4 r10, text relocations, ABI

* Every entry into an r10 module must set r10 to its `got_base`.
* `R_ARM_CALL`/`JUMP24` range ±32 MiB; `MOVW/MOVT` is the far-target escape.
* A relocation that writes into `.text` prevents true flash XIP; use a GOT in
  data or resolve at link time.
* Do not force bit 0; ARM even, Thumb odd.

## 5.5 Semihosting checklist

* Read once into RAM; parse from RAM.
* Keep the buffer alive.
* Do it before the scheduler/other cores.
* No semihosting from an ISR or after publishing a module.
* AArch32 `SVC 0x123456`; Cortex-M `BKPT 0xAB`.
* Use `collect_buffered()` for a sidecar symbol table.
* The debugger must be attached and semihosting enabled.

---

# Part 6 — Configure, build, and test

## 6.1 Prerequisites

| Tool | Version used here | Purpose |
|---|---|---|
| `arm-none-eabi-gcc/g++` | xPack 15.2.1 | firmware and modules |
| CMake | 3.20 or newer | every suite |
| OpenOCD | xPack 0.12.0 | flashing and semihosting |
| a port working copy | `micro-os-plus-iii-cortexm` or `-aarch32` | board support, linker scripts, `hw.sh` |
| a probe | CMSIS-DAP / WCH-Link / XV-Link | one per board |

The suites default to the port paths under `/home/dan/Desktop/...`; override with
`-DUOS_PORT_DIR=...` (or the `UOS_PORT_DIR` environment variable for the run
scripts).

```sh
export PATH="$HOME/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin:$PATH"
```

## 6.2 Configure and build a firmware with a loader

The loader is a CMake INTERFACE source library: its `.cpp` compile into your
firmware.

```cmake
add_subdirectory(SCL-shared scl)

add_executable(my_firmware ...)
target_link_libraries(my_firmware PRIVATE
    micro-os-plus::cortexm        # or micro-os-plus::aarch32
    scl::dynamic-nolib)           # the chosen loader
```

Or choose one case and link the alias:

```sh
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/micro-os-plus-iii/cmake/toolchains/arm-none-eabi.cmake \
  -DSCL_LOADER=optimal \
  -DSCL_HOSTED=OFF
cmake --build build -j8
```

`SCL_LOADER` accepts: `relocatable`, `static-full-ram`, `static`, `optimal`,
`dynamic-nolib`, `dynamic-newlib`, `overlay`.  `SCL_HOSTED=ON` is required for
`scl::dynamic-newlib` (and for `collect_relocations()` / the hosted relocation
planner).

For a XIP loader the firmware's linker script must reserve the windows and the
module must be linked at them; see `xip-split.ld` and `nopic-split.ld` in the
test suites.

Provide the hooks your system needs:

```cpp
extern "C" void scl_loader_lock(void)   { /* recursive mutex/spinlock */ }
extern "C" void scl_loader_unlock(void) { /* ... */ }
extern "C" void scl_cache_clean(const void* a, std::size_t n) { /* DCCMVAC */ }
extern "C" void scl_cache_invalidate(const void* a, std::size_t n) { /* ICIMVAU */ }
extern "C" void scl_publish_release(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
extern "C" void scl_log_puts(const char* s) { /* UART or semihosting */ }
extern "C" void scl_log_putc(char c) { /* ... */ }
```

The weak defaults are a no-op lock, barrier-only cache hooks and a silent log.
They are fine for a single-core bare-metal bring-up, not for an RTOS or SMP.

## 6.3 Build a module

The exact flags matter more than the file name.  The four forms:

```sh
# 1. ET_REL for scl::reloc  (Cortex-A7 example)
arm-none-eabi-g++ -mcpu=cortex-a7 -mthumb -mfloat-abi=hard -mfpu=neon-vfpv4 \
  -mlong-calls -mword-relocations -fno-pic -Os -r -nostdlib \
  -Iinclude -x c++ lib.cpp -o lib.rel.o

# 2. r10 PIC .so for dynamic-nolib / xip-dynamic-*  (split script)
arm-none-eabi-g++ -mcpu=cortex-a7 -mthumb -mfloat-abi=hard -mfpu=neon-vfpv4 \
  -fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative \
  -O0 -shared -nostartfiles -nostdlib -nodefaultlibs -Wl,-E -Wl,-Bsymbolic \
  -T xip-split.ld -Iinclude -x c++ lib.cpp -o lib.so

# 3. Plain -fPIC .so for static / dynamic-newlib
arm-none-eabi-g++ -mcpu=cortex-a7 -mthumb -mfloat-abi=hard -mfpu=neon-vfpv4 \
  -fPIC -O0 -shared -nostartfiles -nostdlib -nodefaultlibs -Wl,-E \
  -Iinclude -x c++ lib.cpp -o lib.pic.so

# 4. No-pic fixed-window .so for optimal
arm-none-eabi-g++ -mcpu=cortex-a7 -mthumb -mfloat-abi=hard -mfpu=neon-vfpv4 \
  -fno-pic -mword-relocations -mlong-calls -O0 -shared -nostartfiles -nostdlib \
  -nodefaultlibs -Wl,-E -Wl,-z,max-page-size=0x100 -Wl,--unresolved-symbols=ignore-all \
  -T nopic-split.ld -Iinclude -x c++ lib.cpp -o lib.opt.so
```

Use `-marm` instead of `-mthumb` to build an ARM-state module (A32); the parser
and relocation engine handle either.  Do not mix hard- and soft-float modules:
`check_arm_abi()` catches it.

The reusable version of these commands is the `fixture()` / `fixture_arm()`
function in the test-suite `CMakeLists.txt`; copy that rather than retyping the
flags.

## 6.4 Add a host test

Host tests live in `SCL-shared-new/tests/` and are compiled by
`tests/run.sh` with the host `g++`.

1. Add `tests/my_test.cpp`.  Include the headers you need, for example:

```cpp
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include <cstdio>

int main() {
    std::byte image[4] = {std::byte{0x7f}, std::byte{'E'},
                          std::byte{'L'}, std::byte{'F'}};
    loader::memory_byte_source src{{image, sizeof image}};
    auto img = loader::elf_image::parse(src);
    if (img) { std::puts("unexpected parse"); return 1; }
    std::puts("short header rejected");
    return 0;
}
```

2. Add it to `tests/run.sh` next to the existing tests:

```sh
echo "=== build my test ==="
"$CXX" -std=c++20 -O1 -g "${INC[@]}" \
    "$HERE/my_test.cpp" "$ROOT/common-elf-reader/src/elf-image.cpp" \
    -o "$OUT/my-test"
"$OUT/my-test"
```

`INC` already contains `include`, `include/scl` and
`common-elf-reader/include`.  `run.sh` uses `set -e`; a non-zero exit fails the
run.

3. Run:

```sh
cd SCL-shared-new
./tests/run.sh
```

The parser's own host tests live in `common-elf-reader/test/` (`elf-reader-test`
and `arm-reloc-test`); they need the `411ce-100Mhz.elf` fixture, which is not
tracked in the parser repository (copy it beside the test binary if missing).

## 6.5 Add an on-target test

An on-target test is one `uos_add_app` in a suite under `SCL-TESTS-shared-new/`.
The suite owns the board, linker script, fixtures and the `run-all-*.sh` entry.

A minimal test source:

```cpp
#include <cmsis-plus/rtos/os.h>
#include <hw_result.hpp>
#include <uart.hpp>

using namespace os::rtos;

int os_main(int, char*[]) {
    (void)interrupts::uncritical_section::enter();
    uart::uart1 << "my-test: starting\n";

    // ... load the module, call it, check the result ...

    const bool ok = true;
    uart::uart1 << (ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (ok) hw_result::ok();
    hw_result::fail();
    return 0;
}
```

The suite's CMake entry:

```cmake
uos_add_app("my-test-hwd"
    SOURCES src/my-test.cpp src/embed-arena.S ${BOARD_SUPPORT}
    NCPU 3                                  # 1 on WeAct, 2 on Pi-Zero, 3 on Lyra
    LINKER_SCRIPT "${BOARD_LD}"
    DEFINES TRACE SEMIHOST HW_BUILD
    INCLUDES "${BOARD_INC}" ...
    LIBRARIES micro-os-plus::aarch32 scl::dynamic-nolib)
add_dependencies("my-test-hwd" fixtures)
set_target_properties("my-test-hwd" PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
list(APPEND SCL_TESTS "my-test-hwd")
```

Then add `my-test` to the `TESTS=(...)` array in the board's
`run-all-*.sh` (or run it directly with `hw.sh`).

Rules:

* load before releasing other cores; constructors and relocations must finish
  before the SMP release;
* use `hw_result::ok()`/`fail()` so OpenOCD sees a semihosted exit; print
  `RESULT: PASS`/`FAIL` for the log parser;
* keep the fixture/arena lifetime alive until the test ends;
* for a register-level test, use the port's `BOARD_SUPPORT` and `BOARD_INC`;
* if the test needs a module file, add it to the suite's fixture build and to
  the `add_dependencies` line.

## 6.6 Add an example app

Examples under `SCL-EXAMPLES-shared-new/<board>/` follow a small shape:

```text
apps/myapp.cpp        kAppName, app_load(), app_start(), app_on_core()
loaders/<loader>.cpp  lib_load(), lib_sym(), lib_unload()
common/               boot, semihosting, hooks, SD, arena
CMakeLists.txt        fixture matrix + lyra_app()/pizero_app() calls
run-all-*.sh          the test list
```

Add the app to the CMake matrix (one call per loader) and to the runner's list;
the CMake already builds the fixture forms and the ELF.  Use an existing app
(`float`, `sd`, `combo`) as the template.

## 6.7 Run the tests

### Host

```sh
cd SCL-shared-new
./tests/run.sh
```

No board, no probe.

### WeAct F411CE

```sh
cd SCL-TESTS-shared-new
./run-all-weact.sh                 # all 22
./run-all-weact.sh static          # regex filter
SKIP_SPI=1 ./run-all-weact.sh      # skip the two W25Q64 tests
DRY_RUN=1 ./run-all-weact.sh       # print commands, do not flash
```

Tunables: `RUN_MS`, `WALL_MS`, `GAP`, `LOGDIR`.  Logs:
`logs/weact-<timestamp>/`.

### Pi-Zero RP2350B

```sh
cd SCL-TESTS-shared-new
./pico2-pizero-shared/run-all-pizero.sh
./pico2-pizero-shared/run-all-pizero.sh psram
GAP=5 ./pico2-pizero-shared/run-all-pizero.sh
POWER_CYCLE=1 ./pico2-pizero-shared/run-all-pizero.sh
```

Tunables: `SECS` (default 240), `GAP`, `BUILD`, `UOS_PORT_DIR`.  Logs:
`pico2-pizero-shared/logs/pizero-<timestamp>/`.

### Luckfox Lyra B

```sh
cd SCL-TESTS-shared-new
./luckfox-lyra-shared/run-all-lyra.sh        # 48
./luckfox-lyra-shared/run-all-lyra.sh fix6   # subset
./luckfox-lyra-shared/run-all-lyra.sh        # SECS/GAP/KHZ are env vars
```

The ARM-state suite is separate:

```sh
cd luckfox-lyra-arm-shared
cmake -S . -B build
cmake --build build --target arm-tests -j8
for t in arm-reloc arm-static arm-optimal arm-dynamic-nolib \
         arm-dynamic-newlib arm-xip-static arm-xip-dynamic-nolib \
         arm-xip-dynamic-newlib arm-overlay; do
  BUILD=$PWD/build OPENOCD=... \
    /path/to/micro-os-plus-iii-aarch32/test/boards/luckfox-lyra/hw.sh "$t" 180
done
```

### One test, directly

Every board has the same interface:

```sh
BUILD=/path/to/build \
OPENOCD=/path/to/openocd-linebuf.sh \
  /path/to/port/test/boards/<board>/hw.sh <test> [seconds]
```

`hw.sh list` shows the tests in that build.  The runner never opens the UART;
the firmware prints the same results there for a terminal.

### Probe selection

* **Pi-Zero**: XV-Link `0416:5951`, unique.
* **WeAct / Lyra**: both use WCH-Link `1a86:8011`.  Some units share the USB
  serial; OpenOCD then picks the first.  Select by USB bus/port with the
  `hide_libusb.so` shim (`HIDE_BUS`, `HIDE_PORTS`), by the pyusb hold helper, or
  simply unplug the other WCH-Link for the run.
* **Logs**: each run tees the semihosting console and writes a per-test log; the
  summary is `PASS=<n> FAIL=<n>`.

### What a pass means

The test prints `RESULT: PASS` and calls `hw_result::ok()`, which issues a
semihosting `SYS_EXIT`.  The runner treats a missing marker or a timeout as a
failure.  A missing-import load can succeed; call `first_missing_import()` after
the set to catch it.

## 6.8 Clean-build checklist

```sh
# 1. toolchain on PATH
# 2. correct port working copy
# 3. correct board/probe
# 4. configure with the right loader / hosted flag
# 5. build the fixtures before the firmware (add_dependencies)
# 6. flash the firmware, not the module
# 7. read the semihosting log, not only the UART
```

Common mistakes: building a `-fPIC` module for `reloc` (it expects ET_REL);
using a no-pic module with a GOT loader (or the reverse); forgetting
`SCL_HOSTED=ON`; forgetting `bind_host_imports()` before flashing an XIP PLT;
forgetting the loader lock/cache hooks on an SMP board; and running a test whose
fixture file has the wrong extension for the loader.

---

# Appendix A — Where the full source is

| What | File |
|---|---|
| All loader branches in one app | `SCL-TESTS-shared-new/luckfox-lyra-shared/src/fixtures-main.cpp` |
| Lyra boot, semihosting, SD | `SCL-EXAMPLES-shared-new/luckfox-lyra/common/lyra.cpp` |
| Lyra loader adapters | `SCL-EXAMPLES-shared-new/luckfox-lyra/loaders/*.cpp` |
| WeAct examples | `SCL-EXAMPLES-shared-new/float-*/`, `spifs-*/`, `combo-*/` |
| Pi-Zero examples | `SCL-EXAMPLES-shared-new/pizero/` |
| Pi-Zero tests | `SCL-TESTS-shared-new/pico2-pizero-shared/src/` |
| Optimal raw test | `SCL-TESTS-shared-new/optimal-shared/test-ram/src/main.cpp` |
| Dynamic raw test | `SCL-TESTS-shared-new/dynamic-shared/test-dynamic/src/` |
| Static raw test | `SCL-TESTS-shared-new/static-shared/test-static/src/` |
| Overlay test | `SCL-TESTS-shared-new/overlay-shared/test-overlay/test1/src/` |
| Parser | `common-elf-reader/` (`elf32_read_parse-new.git`) |

CMake targets: `scl::reloc`, `scl::static`, `scl::xip-static`, `scl::optimal`,
`scl::dynamic-nolib`, `scl::dynamic-newlib`, `scl::xip-dynamic-nolib`,
`scl::xip-dynamic-newlib`, `scl::overlay`.

---

# Appendix B — Glossary

* **main / firmware** — the program containing the loader.
* **module / library** — the ELF file being loaded.
* **ET_REL** — relocatable object; section-based.
* **ET_DYN** — shared object; segment-based, usually PIC.
* **PIC / GOT / PLT / r10** — position-independent code and its tables/base
  register.
* **LMA / VMA** — load address in storage / run address in memory.
* **XIP** — execute in place; code runs from flash (or a DRAM image region).
* **Overlay** — several modules at one VMA, swapped by copying.
* **Relocation** — a fix-up applied after the copy.
* **Symbol source** — how imported names are resolved.
* **Semihosting** — debugger-mediated I/O (`BKPT 0xAB` on Cortex-M,
  `SVC 0x123456` on AArch32).
* **`.ARM.exidx` / `.ARM.extab`** — ARM unwind tables.
* **`loader::byte_source`** — random-access source of ELF bytes (`size`,
  `read_at`); the seam between storage and parser.

---

*End of document.*
