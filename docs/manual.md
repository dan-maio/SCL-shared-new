# SCL-shared & common-elf-reader — API Manual

**Version:** documents the current `SCL-shared-new` tree: the loader sources plus a
vendored copy of `common-elf-reader`. Examples still say `SCL-shared` where the
text applies to both trees. The APIs added by this fork are summarised in
[§6.8](#68-apis-added-by-scl-shared-new).

**Audience:** embedded developers who want to *use* the loaders, and maintainers
who want to understand the *internal* API. Every section is written twice: a
plain-English explanation first, then the technical contract.

**Code examples** are taken verbatim from the companion test repository
`SCL-TESTS-shared` (paths shown as `tests/…`).

---

## Table of contents

1. [Introduction — what these projects are](#1-introduction--what-these-projects-are)
2. [Design principles](#2-design-principles)
3. [The loader models at a glance](#3-the-loader-models-at-a-glance)
4. [common-elf-reader — the shared ELF32 parser (internal API)](#4-common-elf-reader--the-shared-elf32-parser-internal-api)
5. [The SCL shared core — parser/resolver/relocation services (internal API)](#5-the-scl-shared-core--parserresolverrelocation-services-internal-api)
6. [Firmware-level services (`scl/`)](#6-firmware-level-services-scl)
7. [External (user) API per loader case](#7-external-user-api-per-loader-case)
8. [Cross-cutting runtime services](#8-cross-cutting-runtime-services)
9. [Host tools](#9-host-tools)
10. [Building and consuming with CMake](#10-building-and-consuming-with-cmake)
11. [Error-code reference](#11-error-code-reference)
12. [Glossary](#12-glossary)

---

## 1. Introduction — what these projects are

**SCL-shared** ("Shared-Code Loader") is a collection of *runtime loaders* that let a
bare-metal ARM Cortex-M firmware **load and run shared libraries at run time** — the
embedded analogue of a desktop `dlopen`. It is **sources only**: there are no `.a`
archives, every loader `.cpp` file is compiled into *your* firmware with *your* flags.
It targets **ARM Cortex-M, ELF32 little-endian, hard-float** (`-mfloat-abi=hard
-mfpu=fpv4-sp-d16`).

**common-elf-reader** is a small, standalone **ARM ELF32 little-endian parser** that
SCL-shared-new vendors (a plain copy, not a submodule) so that *all* loader cases parse ELF objects
through **one API**. It is deliberately only a reader/parser: relocation application,
symbol resolution and GOT patching stay in each consuming loader.

```
SCL-shared
├── common-elf-reader/          vendored — the ELF32 parser (one API for all loaders)
├── include/                    public headers (loader/, scl/, static/, reloc/,
│                               dynamic-*, xip/, optimal/, overlay/)
├── src/                        the loader implementations (compile into your firmware)
├── tools/gen-symtab.cpp        host tool: bake main's exported symbols into flash
└── CMakeLists.txt              INTERFACE CMake targets (scl::reloc, scl::static, ...)
```

### 1.1 The seven loader cases

One codebase, seven loaders. They differ in *what module format they accept*, *where the
code runs* (RAM vs flash — XIP), and *whether the `r10` register is reserved*.

| # | CMake target | Module you must build | Runs code from | `r10` | Call style |
|---|---|---|---|---|---|
| 1 | `scl::reloc` | `ET_REL` relocatable `.o` (`-mlong-calls -fno-pic -r`) | RAM (arena) | free | direct |
| 2 | `scl::static` | `-fPIC -shared` `.so` | RAM (buffer) | free | direct |
| 3 | `scl::xip-static` | `-fPIC -shared` split R-X\|RW `.so` | flash (XIP) | **used** | direct |
| 4 | `scl::optimal` | `-fno-pic -mlong-calls` fixed-window module | flash window (XIP) | free | direct |
| 5 | `scl::dynamic-nolib` | `-fPIC -shared` `.so` | RAM (arena) | used | `dlopen`/`dlsym` |
| 6 | `scl::dynamic-newlib` | `-fPIC -shared` `.so` | RAM (heap) | used | POSIX `dlopen` |
| 7 | `scl::overlay` | your own modules, bound at link time | shared SRAM window | free | direct |

Supporting targets: `scl::common` (the shared core), `scl::self-resolve` (resolve a
module's imports against `main`'s own `.dynsym` via `link_context::from_self()`),
`scl::xip-common` (XIP primitives), `scl::loader-nopic` (the no-`r10` XIP loader under
`scl::optimal`), `scl::xip-dynamic-nolib` and `scl::xip-dynamic-newlib` (XIP adapters for
the two dynamic loaders).

### 1.2 The fundamental trade-off (why there are seven)

Three things everyone wants are **not simultaneously satisfiable**:

> **{ execute from flash (XIP) · call by name directly · keep `r10` free }**
> — you can have any two, never all three.

- **Execute from flash** → code must be correct at a fixed address before it runs.
- **No `r10`** → code cannot compute its data addresses from a base register at run
  time, so those addresses must be *baked in* at fixed locations.
- Something has to *decide* those fixed locations — either the toolchain (`r10` PIC),
  or a reserved memory window plus a build step that places the library there.

If RAM is acceptable, the problem disappears entirely: copy into RAM and relocate.

---

## 2. Design principles

Everything in both projects follows the same rules:

- **No global state, no exceptions, no RTTI.** Every fallible call returns a
  `result<T>` and is `noexcept` / `[[nodiscard]]`. This compiles for freestanding
  bare-metal builds (the default mode).
- **The core never assumes the whole file is resident.** All ELF I/O goes through
  `byte_source::read_at`, so the *same* parser works over semihosting on the target and
  over an in-memory buffer in host tests.
- **Little-endian ARM ELF32 as a data format.** `elf-types.hpp` mirrors the ABI layout
  exactly and `static_assert`s a little-endian host/target.
- **Bounds-safe everywhere.** Every byte/word/string read out of an ELF span is
  bounds-checked; malformed offsets yield empty views or zeroes, never out-of-range
  reads. `std::string_view` (not `char*`) is used for all section/symbol names.
- **Fixed-capacity containers, no heap** in the freestanding loaders (fixed `std::array`
  handle pools, a free-list `region_allocator`, journal buffers). The *hosted* loader
  (`scl::dynamic-newlib`) uses `std::vector` and is gated behind `-DSCL_HOSTED=ON`.
- **Weak no-op hooks** for logging, reentrancy locking and the thread pointer — the
  application overrides them to enable tracing / RTOS serialisation / TLS.
- **The loader owns no linker scripts.** Where fixed windows or overlay regions are
  needed, the *application* owns the linker script; SCL ships a documented template
  (`src/overlay/ldscripts/overlay-fragment.ld`) and consumes linker symbols.

### 2.1 How the pieces fit together

```
                     ┌───────────────────────────────┐
                     │ common-elf-reader (parser)    │
                     │ loader::elf_image, byte_source │
                     └───────────────┬───────────────┘
                                     │ consumed by
                     ┌───────────────▼───────────────┐
                     │ scl::common (shared core)     │
                     │ dynamic_info, symbol_resolver, │
                     │ reloc_applier, relocator,      │
                     │ lazy_resolve, region_allocator │
                     └───────────────┬───────────────┘
     ┌───────────┬───────────┬───────┴────────┬──────────────┬───────────────┐
     ▼           ▼           ▼                ▼              ▼               ▼
 scl::reloc  scl::static scl::xip-common   dynamic-nolib  dynamic-newlib  scl::overlay
 (ET_REL)    (RAM .so)   (XIP primitives)  (freestanding  (POSIX dlopen)  (link-time)
                          │                dlopen)
               ┌──────────┴───────────┐        │               │
               ▼                      ▼        ▼               ▼
        scl::xip-static          scl::optimal  xip-dynamic-   xip-dynamic-
        (flash, r10)             (flash, no r10) nolib/newlib  nolib/newlib
```

**Internal API** = `common-elf-reader` + `scl::common` + the `scl/` firmware services.
**External (user) API** = the public surface of each case loader (`scl::Module`,
`dl::dlopen`, `optimal::load_and_link`, `reloc::load`, `xip::stat::load`,
`scl_overlay_load`, ...).

---

## 3. The loader models at a glance

The five "real" models plus the two special cases, one sentence each:

| Model | One-line description |
|---|---|
| **reloc** | Load a compiled-but-unlinked `.o` (`ET_REL`), copy its sections into a RAM arena, patch absolute addresses, call. Nothing cheaper exists. |
| **static-full-ram** | Load a plain `-fPIC` `.so` *whole* into a RAM buffer, patch the library's GOT *and main's GOT*, call exports by name. Classic "static patcher", no `-pie` needed. |
| **static (XIP)** | Split R-X\|RW `-fPIC` `.so`: R-X stays in flash and executes in place, only RW goes to RAM, `r10` = RAM GOT base. |
| **optimal (XIP)** | `-fno-pic` module linked at a *fixed, build-derived* address in reserved windows; code runs from a flash window with no `r10` and no per-call cost. |
| **dynamic (nolib/newlib)** | The POSIX `dlopen`/`dlsym`/`dlclose`/`dlerror` contract over `-fPIC` `.so`s, with lazy binding and (newlib) a journaled, byte-exact unload. |
| **overlay** | Modules built *with* the firmware and bound at link time to one shared SRAM window; "load" is a `memcpy` LMA→VMA + `DSB`/`ISB`. |

The rest of this manual describes the **API** of each, starting with the internal
parser that all of them share.

---

## 4. common-elf-reader — the shared ELF32 parser (internal API)

### 4.1 Plain English

This library knows how to read an ARM ELF32 file: validate the magic bytes and machine
type, give you typed access to the ELF header, the program headers (segments), the
section headers, and compute a few derived quantities (`image_size`, `image_base`). It
does **not** relocate anything, resolve anything or touch memory — that is the job of the
loader sitting above it. Everything is read through a `byte_source` interface so the
parser does not care *where* the ELF bytes come from: a file descriptor (semihosting on
the target, POSIX on the host) or a plain in-memory buffer.

The whole library is one translation unit (`src/elf-image.cpp`); everything else is
header-only.

### 4.2 Technical overview

| Header | Contents |
|---|---|
| `loader/error.hpp` | `error` enum + `result<T>` (a C++20 `std::expected` stand-in) |
| `loader/byte-source.hpp` | `byte_source` — abstract random-access reader (the I/O seam) |
| `loader/elf-types.hpp` | ARM ELF32-LE on-disk structs, constants, `enum class rel_type/bind`, decoders |
| `loader/elf-image.hpp` | `elf_image` — validated, typed access to an ELF object |
| `loader/sources/fd-byte-source.hpp` | *(optional)* `byte_source` over a POSIX / semihosting fd |
| `loader/sources/memory-byte-source.hpp` | *(optional)* `byte_source` over an in-memory span |
| `loader/detail/bytes.hpp` | bounds-checked little-endian word/byte/string accessors |
| `tools/elf2json.cpp` | host tool: export an ELF as JSON for the GUI visualizer |

All types live in namespace `loader` (raw ELF structs in `loader::elf`).

### 4.3 `error.hpp` — the error vocabulary

```cpp
namespace loader {
enum class error {
    OK,                // no error
    FILE_NOT_FOUND,    // byte source could not be opened
    SHORT_READ,        // byte source returned fewer bytes than requested
    NOT_ELF,           // magic number is not 0x7f 'E' 'L' 'F'
    NOT_ARM32,         // not a little-endian 32-bit ARM object
    NOT_SHARED_OBJECT, // e_type is not ET_DYN
    BAD_SECTION,       // required section missing or malformed
    SYMBOL_NOT_FOUND,  // hash lookup did not find the requested name
    UNSUPPORTED_RELOC, // relocation type the loader does not implement
    OUT_OF_MEMORY,     // a fixed-capacity container overflowed
};
}
```

### 4.4 `result<T>` — error handling without exceptions

`result<T>` is a minimal replacement for C++23 `std::expected<T, error>`, exposing only
what the loaders use. A function that can fail returns `result<T>`; you test it with
`operator bool`, dereference with `operator*`, and read the failure with `.err()`.

```cpp
template <class T> class result {
public:
    bool has_value() const noexcept;
    explicit operator bool() const noexcept;      // true == success
    T&       operator*() & noexcept;
    T*       operator->() noexcept;
    loader::error err() const noexcept;           // only meaningful when !has_value()
    T&       value() & noexcept;                  // must only be called when has_value()
    T        value_or(U&& fallback) const&;       // success value or fallback
};
// result<void> specialisation carries no value.
```

Construct from a value (`result<T>(value)`) or from failure
(`loader::unexpected(loader::error::X)`). See the `reloc_applier` example in §5.4 for a
real call site.

### 4.5 `byte-source.hpp` — the I/O seam

Abstract random-access reader. A loader/core never knows or cares whether the object is
on disk or in RAM.

```cpp
namespace loader {
class byte_source {
public:
    virtual ~byte_source() = default;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    // Reads exactly out.size() bytes at byte offset. Fills out fully on success.
    // Returns error::SHORT_READ if the range exceeds the object.
    [[nodiscard]] virtual result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept = 0;
};
}
```

**Implement your own** if you have a custom backend (flash archive, SPI, QSPI, DMA): just
subclass `byte_source`. Two ready-made implementations are provided.

### 4.6 `sources/fd-byte-source.hpp`

`byte_source` over a POSIX / semihosting file descriptor. RAII (closes on destruction),
random-access (`lseek` + `read`), never loads the whole file.

```cpp
namespace loader {
class fd_byte_source final : public byte_source {
public:
    static result<fd_byte_source> open_file(const char* path) noexcept;
    // Returns error::FILE_NOT_FOUND if the file cannot be opened.
    bool is_open() const noexcept;
    std::size_t size() const noexcept override;
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept override;
};
}
```

### 4.7 `sources/memory-byte-source.hpp`

`byte_source` over a caller-owned in-memory buffer. **Non-owning** — the buffer must
outlive the source. Useful when the object is already resident (host tests, or firmware
that mmaps/DMAs an image into RAM).

```cpp
namespace loader {
class memory_byte_source final : public byte_source {
public:
    explicit memory_byte_source(std::span<const std::byte> data) noexcept;
    std::size_t size() const noexcept override;
    result<void> read_at(std::size_t offset, std::span<std::byte> out) noexcept override;
};
}
```

### 4.8 `elf-types.hpp` — the on-disk structures

The raw ABI structs, `memcpy`'d straight out of byte buffers (safe because a
little-endian host/target is `static_assert`ed):

```cpp
namespace loader::elf {
using addr = std::uint32_t;  using off = std::uint32_t;
using word = std::uint32_t;  using half = std::uint16_t;

struct Ehdr { unsigned char e_ident[16]; half e_type; half e_machine;
              word e_version; addr e_entry; off e_phoff; off e_shoff;
              word e_flags; half e_ehsize, e_phentsize, e_phnum;
              half e_shentsize, e_shnum, e_shstrndx; };   // 52 bytes
struct Phdr { word p_type; off p_offset; addr p_vaddr; addr p_paddr;
              word p_filesz, p_memsz, p_flags, p_align; }; // 32 bytes
struct Shdr { word sh_name, sh_type, sh_flags; addr sh_addr;
              off sh_offset; word sh_size, sh_link, sh_info,
              sh_addralign, sh_entsize; };                 // 40 bytes
struct Sym  { word st_name; addr st_value; word st_size;
              unsigned char st_info, st_other; half st_shndx; }; // 16 bytes
struct Rel  { addr r_offset; word r_info; };               // 8 bytes  (ARM uses REL, not RELA)
struct Dyn  { std::int32_t d_tag; word d_val; };           // 8 bytes

// constants: DT_NULL/PLTGOT/HASH/STRTAB/SYMTAB/STRSZ/SYMENT/INIT_ARRAY/...
// ELFMAG0..3, ELFCLASS32, ELFDATA2LSB, EI_CLASS, EI_DATA
// ET_REL/EXEC/DYN, EM_ARM(40), PT_LOAD, SHT_REL, SHF_ALLOC

enum class rel_type : std::uint8_t {   // ARM REL relocation types
    ABS32=2, REL32=3, COPY=20, GLOB_DAT=21, JUMP_SLOT=22, RELATIVE=23 };
enum class bind : std::uint8_t { LOCAL=0, GLOBAL=1, WEAK=2 };

constexpr word        r_sym (word info);      // symbol index  = info >> 8
constexpr std::uint8_t r_type(word info);      // reloc type    = info & 0xff
constexpr std::uint8_t st_bind(std::uint8_t info);  // binding = info >> 4
constexpr std::uint8_t st_type(std::uint8_t info);  // type    = info & 0xf
}
```

### 4.9 `elf-image.hpp` — the validated view

`elf_image::parse()` validates magic/class/data/machine and keeps a validated header.
The `byte_source` is held by **pointer** — the caller must keep it alive.

```cpp
namespace loader {
class elf_image {
public:
    static result<elf_image> parse(byte_source& src) noexcept;
    // errors: NOT_ELF (bad magic), NOT_ARM32 (not LE 32-bit ARM)

    [[nodiscard]] const elf::Ehdr& header() const noexcept;
    [[nodiscard]] result<void> program_header(elf::half index, elf::Phdr& out) const noexcept;
    [[nodiscard]] result<void> section_header(elf::half index, elf::Shdr& out) const noexcept;
    [[nodiscard]] result<elf::Shdr> find_section(std::string_view name) const noexcept;
    //   error::BAD_SECTION if the section is absent

    [[nodiscard]] result<std::uint32_t> image_size() const noexcept;
    //   highest PT_LOAD end address (vaddr + memsz), NOT the sum of segments.
    //   "how many bytes the loaded image occupies in memory (file + bss)".

    [[nodiscard]] result<std::uint32_t> image_base() const noexcept;
    //   lowest PT_LOAD p_vaddr

#if defined(SCL_HOSTED)
    [[nodiscard]] result<std::vector<std::byte>> collect_relocations() const noexcept;
    //   concatenated contents of every allocated SHT_REL section (.rel.dyn, .rel.plt, ...)
    //   hosted-only (std::vector); freestanding uses reloc_applier over raw spans instead
#endif

    [[nodiscard]] byte_source& source() const noexcept;
    //   the underlying byte source (for reading raw section contents)
};
}
```

**Plain English notes on `image_size()`/`image_base()`:** the loaded image is
*vaddr-addressed* (memory slot 0 == vaddr 0), so its footprint is the **highest PT_LOAD
end address**, not the sum of segment sizes — segments may be page-aligned with gaps
between them. Summing sizes under-sizes the buffer and corrupts vaddr-based relocation.

### 4.10 `detail/bytes.hpp` — bounds-checked low-level accessors

Shared by `reloc_applier`, `relocator`, `symbol_resolver` and the ET_REL loader. Header
only, freestanding (no POSIX `strnlen`):

```cpp
namespace loader::detail {
std::uint32_t load_word(std::span<const std::byte> s, std::size_t off); // out-of-range -> 0
std::uint8_t  load_byte(std::span<const std::byte> s, std::size_t off); // out-of-range -> 0
void          store_word(std::span<std::byte> s, std::size_t off, std::uint32_t v); // OOR -> no-op
std::string_view name_at(std::span<const std::byte> tab, std::uint32_t off); // NUL-terminated
//   string bounded to the span; empty if off is out of range
void for_each_rel(std::span<const std::byte> rel, auto fn);   // fn(r_offset, r_info) per entry
}
```

### 4.11 Example — parsing with common-elf-reader

This is the canonical "read an ELF and ask about it" snippet
(from `common-elf-reader/README.md`):

```cpp
#include "loader/elf-image.hpp"
#include "loader/sources/fd-byte-source.hpp"
using namespace loader;

auto src = fd_byte_source::open_file("libsh1.so");   // result<fd_byte_source>
if (!src) { /* src.err() == error::FILE_NOT_FOUND */ }

auto img = elf_image::parse(*src);   // validates magic/class/machine/type
if (!img) { /* img.err(): NOT_ELF | NOT_ARM32 | NOT_SHARED_OBJECT | SHORT_READ */ }

const elf::Ehdr& eh = img->header();
uint32_t bytes      = *img->image_size();   // highest PT_LOAD end (file+bss)
uint32_t base       = *img->image_base();   // lowest PT_LOAD vaddr

elf::Phdr ph;  img->program_header(0, ph);
elf::Shdr sh;  img->section_header(0, sh);
auto dynsym = img->find_section(".dynsym"); // result<elf::Shdr>, BAD_SECTION if absent
```

To read raw section bytes, use the `elf::Shdr` fields with `img->source().read_at()` —
see §5.4 for the pattern.

### 4.12 Consuming common-elf-reader

```cmake
# sibling repo (path):
add_subdirectory(../common-elf-reader common-elf-reader-build)
target_link_libraries(my_loader PRIVATE common-elf-reader::elf-reader)

# git submodule:
#   git submodule add <url> third_party/common-elf-reader
#   add_subdirectory(third_party/common-elf-reader)
```

Set `-DSCL_HOSTED=ON` to enable the `std::vector`-based host helper
(`elf_image::collect_relocations`); leave it off for firmware.

---

## 5. The SCL shared core — parser/resolver/relocation services (internal API)

### 5.1 Plain English

On top of the parser sits `scl::common`, a small set of **pure, reusable services** that
all the loaders share:

- **`dynamic_info`** — reads the `.dynamic` array (`_DYNAMIC`) and pulls out the
  addresses of `.hash`, `.dynsym`, `.dynstr` and the string-table size.
- **`symbol_resolver`** — given raw `.dynsym` + `.dynstr` + `.hash` bytes, looks up a
  symbol *name* and returns its image-relative value, using the ELF SysV hash table.
- **`reloc_applier`** — streams a raw `.rel(.dyn/.plt)` table and patches it **in place**
  into a loaded, writable image. Allocation-free; this is the freestanding engine.
- **`relocator`** — the *hosted* twin of `reloc_applier`: computes a
  `relocation_plan` (a `std::vector<reloc_entry>`) without writing memory.
- **`lazy_resolve`** — maps a GOT.PLT slot (image-relative offset) back to its
  `R_ARM_JUMP_SLOT` symbol and resolves it; the host-testable core of lazy binding.
- **`region_allocator`** — a fixed-capacity, first-fit, 8-byte-aligned free-list
  allocator over an external region. Used for the image arena and main's C++ heap.

`symbol_source` (in `reloc-applier.hpp`) is the universal "resolve a name to an address"
callback: a function pointer + context, deliberately *not* `std::function`, so it stays
usable on the bare-metal target.

### 5.2 `loader/dynamic-info.hpp` — reading `.dynamic`

```cpp
namespace loader {
struct dynamic_info {
    std::uint32_t hash   = 0;  // DT_HASH   (address of .hash)
    std::uint32_t symtab = 0;  // DT_SYMTAB (address of .dynsym)
    std::uint32_t strtab = 0;  // DT_STRTAB (address of .dynstr)
    std::uint32_t strsz  = 0;  // DT_STRSZ  (bytes of .dynstr)
    static result<dynamic_info> parse(std::span<const std::byte> bytes) noexcept;
    //   scans until DT_NULL; error::BAD_SECTION if hash/symtab/strtab are missing
};
}
```

### 5.3 `loader/symbol-resolver.hpp` — hash-based name lookup

```cpp
namespace loader {
class symbol_resolver {
public:
    symbol_resolver(std::span<const std::byte> dynsym,   // .dynsym (array of elf::Sym)
                    std::span<const std::byte> dynstr,   // .dynstr (NUL-separated names)
                    std::span<const std::byte> hash) noexcept;  // .hash (nbucket,nchain,bucket[],chain[])

    // Find the value (st_value) of the named symbol.
    // Returns the image-relative value; 0 for external symbols (st_value == 0).
    // error::SYMBOL_NOT_FOUND if no matching name in the hash chain.
    [[nodiscard]] result<std::uint32_t> address_of(std::string_view name) const noexcept;
};
}
```

The chain walk is capped at `nchain` steps so a malformed cyclic hash chain cannot hang
the target.

### 5.4 `loader/reloc-applier.hpp` — the freestanding relocation engine

This is the workhorse of the non-hosted loaders. It differs from `relocator` in two
ways: it is **allocation-free**, and its external-symbol resolver is a plain function
pointer + context.

```cpp
namespace loader {

// Resolve an external symbol name to an absolute address (e.g. in main).
struct symbol_source {
    result<std::uint32_t> (*fn)(void* ctx, std::string_view name);
    void* ctx;
    result<std::uint32_t> operator()(std::string_view n) const { return fn(ctx, n); }
};

class reloc_applier {
public:
    // Apply a raw .rel(.dyn/.plt) table into `image`.
    //   image   : writable bytes of the loaded image (vaddr 0 at image[0])
    //   base    : runtime address of image[0]  (== load bias)
    //   reldyn  : bytes of the .rel table (array of elf::Rel)
    //   dynsym  : bytes of .dynsym (array of elf::Sym)
    //   dynstr  : bytes of .dynstr (NUL-separated names)
    //   resolve : resolver for external symbols (st_value == 0)
    // Defined symbols (st_value != 0) bind to base+st_value.
    // Unresolved externals are written as 0 (matches --unresolved-symbols=ignore).
    // errors: UNSUPPORTED_RELOC, BAD_SECTION (target offset outside the image)
    static result<void> apply_rel(std::span<std::byte> image, std::uint32_t base,
                                  std::span<const std::byte> reldyn,
                                  std::span<const std::byte> dynsym,
                                  std::span<const std::byte> dynstr,
                                  symbol_source resolve) noexcept;
};
}
```

**Handled relocation types** (ARM REL):

| `rel_type` | What it does |
|---|---|
| `RELATIVE` | `*slot += base` (self-relative local data) |
| `GLOB_DAT` | `*slot = sym_target()` (global-variable GOT slot) |
| `JUMP_SLOT` | `*slot = sym_target()` (function GOT/PLT slot) |
| `ABS32` | `*slot += sym_target()` (e.g. constructor pointers) |
| `REL32` | `*slot += sym_target() - (base + r_offset)` (PC-relative data) |
| `COPY` | `memcpy(st_size bytes)` from the resolved source into the slot (extern var defined in main) |

`sym_target()` = `base + st_value` when the symbol is defined in the image, otherwise
`resolve(name)`.

### 5.5 `loader/relocator.hpp` — the hosted relocation planner

Pure computation, no memory written. Hosted (`-DSCL_HOSTED=ON`) only, because the plan
is a `std::vector`.

```cpp
namespace loader {
struct reloc_entry {
    std::uint32_t    offset;    // image-relative location to patch (r_offset)
    elf::rel_type     type;     // ARM relocation kind
    elf::bind        bind;      // binding of the associated symbol
    std::string_view name;      // symbol name (VIEW into .dynstr — keep it alive!)
    std::uint32_t    sym_value; // image-relative symbol value; 0 => external
    std::uint32_t    sym_size;  // st_size from .dynsym (used by R_ARM_COPY)
};
using relocation_plan = std::vector<reloc_entry>;

class relocator {
public:
    // Compute the relocation plan for one ELF object.
    // Names in the result are views into `dynstr`; keep it alive as long as the plan.
    // error::UNSUPPORTED_RELOC if a relocation type is not recognised.
    static result<relocation_plan> compute(std::span<const std::byte> reldyn,
                                           std::span<const std::byte> dynsym,
                                           std::span<const std::byte> dynstr) noexcept;
};
}
```

### 5.6 `loader/lazy-resolve.hpp` — lazy JUMP_SLOT resolution (core)

```cpp
namespace loader {
struct lazy_tables {
    std::span<const std::byte> jmprel;  // JUMP_SLOT relocs (array of elf::Rel)
    std::span<const std::byte> dynsym;  // .dynsym
    std::span<const std::byte> dynstr;  // .dynstr
};

// Resolve the JUMP_SLOT whose GOT slot lives at image-relative byte offset got_off.
// Scans t.jmprel for the entry with r_offset == got_off, reads its symbol name, and
// resolves it via `resolve`. Returns the absolute target WITH the Thumb bit (bit 0) set
// so a `bx` stays in Thumb state.
// errors: UNSUPPORTED_RELOC, SYMBOL_NOT_FOUND
[[nodiscard]] result<std::uint32_t>
lazy_resolve_slot(std::uint32_t got_off, const lazy_tables& t, symbol_source resolve) noexcept;
}
```

### 5.7 `loader/region-allocator.hpp` — fixed free-list allocator

No dynamic allocation, no globals, 8-byte aligned, coalesces adjacent free blocks. Used
for the loaded-image arena (`_simage_arena_*`) and for main's C++ heap.

```cpp
namespace loader {
class region_allocator {
public:
    static constexpr std::size_t kMaxBlocks = 16;  // max live spans tracked
    static constexpr std::size_t kAlign     = 8;   // granularity

    region_allocator(void* base, std::size_t size) noexcept;  // one initially-free block
    [[nodiscard]] result<void*> alloc(std::size_t n) noexcept; // rounded up to kAlign
    void free(void* p) noexcept;                              // no-op for nullptr
};
}
```

**Plain English:** a tiny `malloc`-like buddy over a caller-supplied RAM range. It is a
*free-list* (`blocks_[16]`), so at most 16 live+free spans can be tracked — one `alloc`
of a large block "carves" a remainder into a new slot. `free()` marks the block free and
merges adjacent free neighbours.

### 5.8 `loader/detail/elf-hash.hpp` — the SysV ELF hash

```cpp
namespace loader::detail {
std::uint32_t elf_hash(std::string_view name) noexcept;  // classic ELF hash function
}
```

---

## 6. Firmware-level services (`scl/`)

These are the headers that sit between the pure core and the application. They are
*internal* (the loaders use them) but several are meant to be **overridden or called by
the application** to wire the loader to a specific board.

### 6.1 `scl/log.h` — the logging hook (override to see loader traces)

The SCL never talks to a UART or semihosting directly. All diagnostic output goes through
these three functions. **Weak no-op defaults** are compiled in; the application overrides
them (e.g. with `trace_puts`/`trace_putc` over semihosting) to enable loader tracing.

```c
void scl_log_puts(const char* s);            // emit a NUL-terminated string
void scl_log_putc(char c);                   // emit a single character
void scl_log_put_hex(const char* label, uint32_t v); // print "label0xXXXXXXXX"
```

**Example override** (application code):

```c
#include "scl/log.h"
#include "diag/trace.h"
void scl_log_puts(const char* s) { trace_puts(s); }
void scl_log_putc(char c)        { trace_putc(c); }
void scl_log_put_hex(const char* label, uint32_t v) {
    trace_printf("%s0x%08X", label, (unsigned)v);
}
```

### 6.2 `scl/link-context.hpp` — main's own dynamic-symbol surface

Resolves a library's imports against **main itself**. Works by reading main's `_DYNAMIC`
array (the linker emits `_DYNAMIC`), parsing `DT_HASH/SYMTAB/STRTAB/STRSZ`, and building
a `symbol_resolver` over main's dynamic symbols. Requires a **`-pie -Wl,-E`** firmware so
every global main exports is present in its `.dynsym`.

```cpp
namespace fw {
class link_context {
public:
    static loader::result<link_context> from_self() noexcept;  // parse main's _DYNAMIC
    [[nodiscard]] loader::symbol_source source() const noexcept;  // as a resolver callback
    [[nodiscard]] loader::result<std::uint32_t> resolve(std::string_view name) const noexcept;
    //   absolute address of `name` in main, or SYMBOL_NOT_FOUND
};
}
```

This is exactly what lets the `dynamic-nolib`, `xip-static` and `xip::nopic` loaders
resolve lib→main imports **with no hand-maintained symbol table**.

### 6.3 `scl/image-memory.hpp` — the process-wide image arena

```cpp
namespace fw {
// The process-wide image arena allocator (over the linker symbols
// `_simage_arena_start` / `_simage_arena_end`).
loader::region_allocator& image_arena() noexcept;
}
```

The application reserves an arena in its linker script (its *addresses* delimit it) and
the loaders allocate loaded-image memory from it. Example usage from the XIP test
(`tests/dynamic-shared/test-xip/test-xip-dynamic-nolib/test1/src/main.cpp`):

```cpp
auto memp = fw::image_arena().alloc(sz);
if (!memp) { trace_printf("  [prov] OOM: %s\n", name); return; }
auto* buf = static_cast<std::byte*>(*memp);
// ... use buf ...
fw::image_arena().free(buf);
```

### 6.4 `scl/barrier.h` — memory barrier (overlay + post-copy)

```c
// On ARM: `dsb 0xf; isb 0xf` — drains the write buffer and flushes the pipeline.
// Off-target: compiles to nothing.
#define scl_memory_barrier()
```

Used by `scl::overlay` after the `memcpy` from flash to the window so the freshly copied
code is what the CPU actually fetches.

### 6.5 `scl/pic-trampoline.h` — enter a library with `r10` = its GOT base

Libraries built with `-msingle-pic-base -mpic-register=r10` read their globals through
`r10` = *their own* GOT base. These naked trampolines set `r10`, call, and restore it
(`r10` is saved/restored around the call). `r10` is a *scratch* register for `-fPIC`
libraries (`libcppsh`) and main (plain `-fpic`), so setting it is harmless there.

```c
void     pic_call_void(void* fn, void* got);                  // call fn()
uint32_t pic_call_u32(void* fn, void* got, uint32_t a0);      // call fn(a0)
uint32_t pic_call_u32_2(void* fn, void* got, uint32_t a0, uint32_t a1);
```

The trampolines carry EHABI unwind directives (`.fnstart`/`.save {r10,lr}`) so a C++
unwinder can step *through* the lib→trampoline→main frames during an exception.

### 6.6 `scl/unwind-exidx.h` — register loaded libraries with the C++ unwinder

The ARM EHABI unwinder calls `__gnu_Unwind_Find_exidx(pc, &count)` to locate the unwind
index table covering a faulting PC. `scl::dynamic-nolib` (and `xip-*`) register each
loaded image's **relocated** `.ARM.exidx` range here on `dlopen` and remove it on
`dlclose`, so a `throw` inside a loaded library can be unwound back into main.

```c
void dl_exidx_register(uint32_t img_base, uint32_t img_size,
                       uint32_t exidx_base, int exidx_count);  // no-op if full
void dl_exidx_unregister(uint32_t img_base);
```

`__gnu_Unwind_Find_exidx` is provided as a strong override in
`src/port/unwind-find-exidx.c`; PCs outside any registered library fall back to main's
own `__exidx_start`/`__exidx_end`.

### 6.7 `scl/` reentrancy hooks (used by every loader)

```cpp
// Weak no-ops by default. An RTOS application defines strong versions, backed by a
// recursive mutex, to serialise load/unload against calls from other tasks.
void scl_loader_lock();
void scl_loader_unlock();
```

See `RTOS-MULTICORE.md` for what the lock covers.

### 6.8 APIs added by SCL-shared-new

| API | Purpose | Guide |
|---|---|---|
| `scl::symtab::collect()`, `collect_buffered()`, `build_blob()`, `view`, `source()` | Build and read a symbol table at boot | `BOOT-SYMTAB.md` |
| `SCL_REGISTER_SYM()`, `scl::symtab::registered()`, `source_registered()` | Compile-time table | `BOOT-SYMTAB.md` |
| `fw::dl_set_symtab()`, `link_context::set_symtab()` | External main table for `dynamic-nolib` / `fw::link_context` | `BOOT-SYMTAB.md` |
| `xip::newlib::set_symtab(const scl_sym_entry*, ...)` | Same, for XIP newlib | `BOOT-SYMTAB.md` |
| `xip::nopic::set_main_symbols()` | External main symbols for `scl::optimal` | `BOOT-SYMTAB.md` |
| `symbol_table::add_entries()` | Publish a table to `scl::static` | `BOOT-SYMTAB.md` |
| `xip::stat::set_symtab()`, `xip::nolib::set_symtab()` | External main table for those XIP adapters | `BOOT-SYMTAB.md` |
| `first_missing_import()`, `fw::dl_first_missing_import()`, `Module::first_missing_import()` | Name the first import nothing provides; call after loading the set | `RTOS-MULTICORE.md` |
| `flash_base` on `patch_main_imports()`, `bind_host_imports()` | Prelink the data segment's flash copy, for XIP modules that use exceptions | `RTOS-MULTICORE.md` |
| `fw::dl_set_lazy_binding()`, `fw::dl_lazy_binding()` | Eager/lazy binding switch | `RTOS-MULTICORE.md` |
| `scl_cache_clean()`, `scl_cache_invalidate()`, `scl_cache_clean_invalidate()`, `scl_publish_release()` | Weak cache and publication hooks | `RTOS-MULTICORE.md` |

---

## 7. External (user) API per loader case

This is the part application code actually calls. Each subsection documents one loader,
its public headers, and a real example from `SCL-TESTS-shared`.

### 7.1 Case 1 — `scl::reloc` (ET_REL modules into a RAM arena)

**Plain English.** Give the loader a compiled-but-not-linked `.o` (`ET_REL`, built
`-mlong-calls -fno-pic -r`). It copies every section that has the "allocate in memory"
flag (`SHF_ALLOC`) into a RAM arena you provide, resolves undefined symbols (imports)
against a resolver you provide (which can look in *other loaded modules* for cyclic
imports), applies `R_ARM_ABS32`/`REL32`/`THM_CALL`, runs the constructors, and hands you
back a `module`. Export symbols are then plain function pointers — **no GOT, no PLT, no
`r10`**.

**Public header:** `include/reloc/reloc-loader.hpp` (namespace `reloc`).

```cpp
namespace reloc {
inline constexpr int MAX_SECTIONS = 48;   // fixed cap on section-header count
inline constexpr int MAX_SET      = 4;    // max modules linked together by load_set()

struct module {
    std::span<std::byte>       arena{};      // RAM the sections were placed in
    std::span<const std::byte> image{};      // original ET_REL file bytes
    std::uint32_t sec_base[MAX_SECTIONS]{};  // runtime base per section index
    std::uint32_t used{};                    // bytes of arena consumed
    // ... shnum / shoff / shentsize / symtab_idx / valid
    explicit operator bool() const noexcept;  // true == placed+loaded
};

// Load one ET_REL image into `arena`, resolving undefined symbols via `resolve`.
// Applies relocations and runs .init_array.
// errors: BAD_SECTION / UNSUPPORTED_RELOC / OUT_OF_MEMORY / NOT_ELF / NOT_ARM32
loader::result<module> load(std::span<const std::byte> image,
                            std::span<std::byte>       arena,
                            loader::symbol_source      resolve) noexcept;

// Runtime address of an exported (defined, non-UND) symbol, or 0.
std::uint32_t sym(const module& m, std::string_view name) noexcept;

struct module_set { module mods[MAX_SET]{}; int n{0};
                    explicit operator bool() const noexcept; };

// Load a SET of ET_REL modules together (mutual/cyclic imports work):
//   1. place every module's SHF_ALLOC sections in `arena` (contiguous) —
//      now every symbol in every module has an address;
//   2. resolve each module's relocations against the whole set first, then
//      `resolve` (main) for anything no module defines; then run all ctors.
loader::result<module_set> load_set(std::span<const std::span<const std::byte>> images,
                                    std::span<std::byte>                        arena,
                                    loader::symbol_source                       resolve) noexcept;
std::uint32_t sym(const module_set& s, std::string_view name) noexcept;  // searched across all modules
}
```

**Example** (`tests/relocatable-shared/test-relocatable/src/main.cpp`). The resolver is a
free function returning `loader::result<uint32_t>`; the `symbol_source` wraps it:

```cpp
static loader::result<std::uint32_t> main_resolve(void*, std::string_view n) {
    if (n == "trace_event")  return addr((void*)&trace_event);
    if (n == "printf")       return addr((void*)&printf);
    if (n == "AA")           return addr(&AA);
    // ... one entry per symbol main exports ...
    return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
}

// Load ONE module:
static reloc::module load_one(const char* name) {
    reloc::module m;
    std::size_t sz = read_mod(name, 0);                 // read foo.rel.o into g_img[0]
    if (!sz) { trace_puts("  read FAILED\n"); return m; }
    loader::symbol_source rs{ main_resolve, nullptr };
    auto r = reloc::load(std::span<const std::byte>(g_img[0], sz), std::span(g_arena), rs);
    if (!r) { trace_u32("  load FAILED: ", (std::uint32_t)r.err()); return m; }
    return *r;
}

// Load a SET (cross-module / cyclic) as one link unit:
static reloc::module_set load_set(const char* const* names, int n) {
    reloc::module_set set;
    for (int i = 0; i < n; ++i) {
        std::size_t sz = read_mod(names[i], i);
        if (!sz) { trace_puts("  read FAILED\n"); return set; }
        g_spans[i] = std::span<const std::byte>(g_img[i], sz);
    }
    loader::symbol_source rs{ main_resolve, nullptr };
    auto r = reloc::load_set(std::span<const std::span<const std::byte>>(g_spans, n),
                             std::span(g_arena), rs);
    if (!r) { trace_u32("  load_set FAILED: ", (std::uint32_t)r.err()); return set; }
    return *r;
}

template <class F> static F sym(const reloc::module& m, const char* name) {
    return reinterpret_cast<F>(static_cast<std::uintptr_t>(reloc::sym(m, name)));
}

int main() {
    // ...
    static const char* pr[2] = { "relprov", "relmod" };
    reloc::module_set s_pr = load_set(pr, 2);
    if (s_pr) {
        printf("  prov_add(10,20) = %u\n", sym<u32_2>(s_pr, "prov_add")(10, 20));
        printf("  rel_compute(7)  = %u\n", sym<u32_1>(s_pr, "rel_compute")(7));
    }
    reloc::module m1 = load_one("mylib1");
    if (m1)
        printf("  mylib1 library_function(42) = %u\n", sym<u32_1>(m1, "library_function")(42));
    // ...
}
```

### 7.2 Case 2 — `scl::static` (a `-fPIC` `.so` copied whole into RAM)

**Plain English.** Load a normal `-fPIC -shared` `.so` into a RAM buffer you provide.
The loader parses it, copies the `PT_LOAD` segments, patches the *library's* GOT and
**main's** GOT (so `name(args)` becomes a real direct call), runs the constructors, and
returns a `scl::Module`. Imports resolve against an `elf_loader::symbol_table` you
populate with `add_global(name, addr)` — a third resolution style, alongside the resolver
callback and `.dynsym`/`from_self`. `unload()` runs the destructors, restores every word
it wrote into main **byte-exact** (via a patch journal), unpublishes exports and releases
the buffer.

**Public headers:** `include/static/module.h` (namespace `scl`), `include/static/elf-reader.h`,
`include/static/shlib-loader.h`, `include/static/tls.h`, `include/static/logger.h`
(namespace `elf_loader`).

#### The high-level facade: `scl::Module`

```cpp
namespace scl {
enum class module_error { OK, FILE_NOT_FOUND, NOT_AN_ELF, NO_DYNSYM, LOAD_FAILED };
enum class scope { GLOBAL, LOCAL };   // mirrors dlopen RTLD_GLOBAL / RTLD_LOCAL
using dep_loader = bool (*)(const char* soname);   // auto-load DT_NEEDED deps; nullptr disables

class Module {
public:
    ~Module();   // teardown if still loaded

    // Load a .so from a file (semihosting) into exec_buf, resolve against main
    // and `syms`, run ctors, then auto-load any DT_NEEDED deps via `load_dep`.
    bool load_file(const char* path, uint8_t* exec_buf, uint32_t exec_sz,
                   elf_loader::symbol_table* syms,
                   scope s = scope::GLOBAL, dep_loader load_dep = nullptr);

    // Load a .so from an in-memory buffer (no file I/O).
    bool load_buf(const uint8_t* raw, uint32_t rawsz,
                  uint8_t* exec_buf, uint32_t exec_sz,
                  elf_loader::symbol_table* syms,
                  scope s = scope::GLOBAL, dep_loader load_dep = nullptr);

    // Bytes load_file() needs for its buffer (highest PT_LOAD end), or 0 on failure.
    static std::uint32_t image_size(const char* path);

    // Reference counting: after load the refcount is 1.
    void retain();
    void release();                 // tear the module down at refcount 0
    void unload();                  // convenience: release() (idempotent)
    uint32_t refcount() const;

    // Symbol access: image-relative value -> runtime address, or nullptr.
    void* sym(const char* name) const;
    template<typename T> T sym(const char* name) const;   // cast helper

    // Call-by-name helper: mod["name"](args...) invokes the symbol as a void fn.
    Call operator[](const char* name);

    // Status.
    module_error last_error() const;
    int  last_detail() const;               // underlying elf_loader::loader_error as int
    const char* last_error_string() const;  // "FileNotFound", "InvalidSegment", ...
    bool loaded() const;                    // true once a library is loaded
    const std::vector<std::string>& dependencies() const;  // DT_NEEDED sonames
    uint32_t tls_offset() const;            // byte offset within a thread's TLS area
};
}
```

#### The symbol table: `elf_loader::symbol_table`

```cpp
namespace elf_loader {
class symbol_table {
public:
    void add_global(std::string_view name, uint32_t address);
    void set_global(std::string_view name, uint32_t address);
    void add_weak(std::string_view name, uint32_t address);
    uint32_t find(std::string_view name) const;
    bool has(std::string_view name) const;
    bool remove(std::string_view name);  // unpublish on unload; true if removed
    void clear();
};
}
```

#### The lower-level `elf_reader` and `shlib_loader`

`elf_reader` (namespace `elf_loader`) is a thin adapter over `loader::elf_image` plus the
project's own section/segment helpers. `shlib_loader` orchestrates the GOT patching.

```cpp
enum class loader_error { OK, FILE_NOT_FOUND, NOT_AN_ELF, SECTION_NOT_FOUND,
                          INVALID_SEGMENT, INVALID_RELOCATION, INVALID_SYMBOL,
                          DEP_NOT_FOUND, OUT_OF_MEMORY };
inline const char* loader_error_string(loader_error e);  // "Ok", "FileNotFound", ...

class elf_reader {
public:
    loader_error error{loader_error::OK};
    bool open_elf(std::string_view file_path);
    bool open_buf(const uint8_t* data, uint32_t size);
    loader_error verify_elf();
    const Elf32_Ehdr& get_header();
    void read_program_header(uint32_t nbr, Elf32_Phdr& phdr);
    void read_section_header(uint32_t nbr, Elf32_Shdr& shdr);
    loader_error read_section_content(uint32_t nbr, std::span<uint8_t> dest);
    loader_error read_image_data(uint32_t file_offset, std::span<uint8_t> dest); // PT_TLS .tdata
    uint32_t get_binary_size();
    uint32_t find_section_by_name(std::string_view name);
    uint32_t find_program(uint16_t type);
    std::string_view get_elf_name() const;
    uint32_t get_program_header_count() const;
    void get_segment_info(uint32_t nbr, load_segment& seg);
    loader_error load_segments(std::span<uint8_t> ram_buffer, const Elf32_Phdr& phdr);
    void read_helper_sections(const char* name, helper_section& hs);
    loader_error read_relocation_symbols(const pass_args& pa, std::span<symbol_patch> ssp, uint32_t& count_out);
    uint32_t read_dynamic_symbols(const pass_args& pa, std::span<symbol_patch> ssp, uint32_t start_k, uint32_t& count_out);
    uint32_t calculate_patch_array_size(const pass_args& pa);
    uint32_t find_symbol_address(std::string_view name, const section_map& ms);
    loader_error scan_dependencies(std::vector<std::string>& deps_out);
};

class shlib_loader {
public:
    loader_error load_bin(uint8_t* buffer, uint32_t bufsize);   // copy PT_LOAD into RAM
    elf_reader& get_elf();
    loader_error read_elf(std::string_view file);
    loader_error populate_helper_sections();
    uint8_t* get_ram_shlib();
    uint8_t* get_ram_main();
    loader_error find_shlib_symbols_to_patch(std::span<symbol_patch> ssp);
    loader_error find_main_symbols_to_patch(std::span<symbol_patch> msp);
    symbol_patch find_symbol_by_name(lib_type type, std::string_view name);
    uint32_t get_shlib_patch_count();
    uint32_t get_main_patch_count();
    void allocate_patch_arrays(std::span<symbol_patch> ssp_shlib, std::span<symbol_patch> ssp_main);
    uint32_t patch_shlib(symbol_table* global_syms);   // patch the library's own GOT
    uint32_t patch_main(symbol_table* global_syms);    // patch MAIN's GOT (byte-exact journal)
    loader_error execute_constructors();
    const std::vector<patch_record>& main_journal() const;  // for a byte-exact unload
    std::vector<patch_record> take_journal();
    void set_publish(bool p);                             // RTLD_LOCAL: don't publish exports
    loader_error allocate_tls();                          // must be BEFORE patch_shlib()
    uint32_t tls_offset() const;
    unsigned int resolve_weak_function(funcm f);
    loader_error resolve_dependencies(symbol_table* global_syms,
                                      loader_error (*load_dep)(std::string_view soname, shlib_loader& out));
    const std::vector<std::string>& dependencies() const;
    void set_ram_main(uint8_t* addr);
};
```

`patch_record { address; original; }` is the journal entry — every word `patch_main`
wrote into main's memory plus the value it replaced, so `Module::unload` can restore the
firmware byte-for-byte.

#### TLS: `static/tls.h`

Multi-thread-capable TLS for the static loader, using the soft-TP ABI
(`-mtp=soft`, the ARM default): every TLS access calls `__aeabi_read_tp()`, which the
loader overrides to return the per-thread area base.

```c
uint32_t scl_tls_register(const void* tdata, uint32_t tdata_sz, uint32_t tls_size);
//   copy .tdata as the module's template, reserve tls_size in the area, return its offset
//   (UINT32_MAX on failure)
uint32_t scl_tls_module_id(uint32_t offset);     // DTV index for a registered offset
void     scl_tls_unregister(uint32_t offset);    // drop a template (offset stays reserved)
void*    scl_tls_get_addr(const void* tls_index); // __tls_get_addr for global-dynamic access
uint32_t scl_tls_area_size(void);                // total size of a thread's TLS area
void     scl_tls_init_area(void* area);          // copy every template into a thread's area
void*    scl_tls_prepare(void);                  // single-thread convenience: alloc+init+activate
void     scl_tls_set_pointer(void* p);           // set the current thread's TLS area base
void*    scl_tls_get_pointer(void);
```

#### Example (from `tests/static-shared/test-static/test-drv/src/main.cpp`)

```cpp
static elf_loader::symbol_table g_syms;
static void init_syms() {
    g_syms.add_global("trace_puts", (uint32_t)(uintptr_t)(void*)trace_puts);
    g_syms.add_global("printf",     (uint32_t)(uintptr_t)(void*)printf);
    g_syms.add_global("puts",       (uint32_t)(uintptr_t)(void*)puts);
    g_syms.add_global("malloc",     (uint32_t)(uintptr_t)(void*)malloc);
    g_syms.add_global("free",       (uint32_t)(uintptr_t)(void*)free);
    // ... every symbol the .so imports that main provides ...
}

int main() {
    trace_puts("test-drv start\n");
    init_syms();

    // Load from a file (semihosting) into a RAM buffer:
    scl::Module mod;
    if (!mod.load_file("fixtures/libspi.so", g_spi_img, sizeof(g_spi_img), &g_syms))
        { trace_puts("spi load FAIL\n"); for (;;); }

    spi_init(); spi_format();            // direct calls — main's GOT was patched

    // Load from a buffer that came out of a SPI-flash flat-fs:
    const FsEntry* e = flatfs_find("uart.so");
    spi_read(e->off, g_img, (uint16_t)e->size);
    scl::Module m;
    if (!m.load_buf(g_img, e->size, g_exec, sizeof(g_exec), &g_syms))
        { trace_puts("  uart load FAIL\n"); }
    else { uart_init(); uart_puts("UART from flatfs"); uart_discard(); }

    // Two modules side by side in one buffer (offset 16384 for the second):
    scl::Module mu, mc;
    mu.load_buf(g_img, eu->size, g_exec, sizeof(g_exec), &g_syms);
    mc.load_buf(g_img, ec->size, g_exec + 16384, sizeof(g_exec) - 16384, &g_syms);
    uint32_t r0 = cpp_features_run(0, 10);   // direct call into the C++ .so
    uint32_t s  = cpp_features_summary();
    // ...
}
```

### 7.3 Case 3 — `scl::xip-static` (split R-X\|RW `-fPIC` `.so`, executes in flash)

**Plain English.** Same idea as `scl::static`, but the module is built as a **split**
`-fPIC` shared object: the linker produces **two** `PT_LOAD` segments — a read-only,
executable **R-X** segment (code + rodata, `VA == file_offset`) and a **RW** segment
(GOT + `.data` + `.bss` + `.data.rel.ro`). The R-X segment stays in flash and the CPU
executes it **in place**; only the RW segment is copied to RAM and relocated. `r10` =
the RAM GOT base.

Two preparation steps matter:

- **`bind_host_imports()`** must be called on a *writable copy of the blob* **before it
  is flashed** — the flash PLT stubs reach their GOT slot PC-relatively, and once
  programmed, NOR flash can only clear bits. It writes every external `R_ARM_JUMP_SLOT`
  import straight into the flash-resident GOT (main is `r10`-free, so no trampoline).
- Cross-library function imports are pre-linked to the shared `xip_cross_call`
  trampoline, which resolves the symbol in the loaded-module registry at call time and
  swaps `r10` to the callee's GOT base.

**Public header:** `include/xip/static/adapter.hpp` (namespace `xip::stat`).

```cpp
namespace xip::stat {

struct SModule {
    bool           ok = false;
    std::uintptr_t text_base = 0;   // flash address of the R-X segment
    std::uintptr_t data_base = 0;   // RAM address of the RW segment
    std::uintptr_t got_base  = 0;   // RAM address of .got  (== r10 for pic_call_*)
    std::uint32_t  text_vaddr = 0, text_size = 0, data_vaddr = 0, data_memsz = 0;
    std::uintptr_t flash_data_base = 0;  // flash image of the RW segment (for the trampoline)
    std::span<const std::byte> reldyn{}, relplt{}, dynsym{}, dynstr{};
    loader::symbol_resolver resolver{ {}, {}, {} };
    std::byte* ram = nullptr;
};

// Provision-time prelink: bind external FUNCTION imports into the image.
// Resolves every external R_ARM_JUMP_SLOT against main's dynamic symbols and writes
// the address into the writable `image` — call BEFORE the image is flashed.
// Returns the number of slots patched, or -1 if the image could not be parsed.
int bind_host_imports(std::span<std::byte> image) noexcept;

// Load from a flash-resident blob. Keeps R-X in flash, copies RW to RAM, applies
// split-base relocations (imports resolved against main's .dynsym), runs ctors with
// r10 = got_base.
//   patch_host_got=true  also point main's own GOT/PLT slots at this module's exports,
//                        so the app can call in BY NAME with no trampoline.
bool load(std::span<const std::byte> blob, SModule& out, bool patch_host_got = false) noexcept;

// Static symbol resolution over the loaded image (flash for code, RAM for data).
// Not a runtime dynamic-linker service. Returns nullptr if not found.
void* resolve(const SModule& m, const char* name) noexcept;

// Free the RAM segment.
void unload(SModule& m) noexcept;
}
```

### 7.4 Case 4 — `scl::optimal` (fixed-address windows, XIP, no `r10`)

**Plain English.** The module is compiled `-fno-pic -mlong-calls` and **linked at fixed
addresses** that the build script derives from the chip's RAM geometry (no hand-typed
addresses). The firmware reserves two RAM windows (`_sxip_text_*`, `_sxip_data_*`); each
module is linked at its own sub-slot *inside* the windows, so several modules can be
resident at once. `load` is a `memcpy` (0-bias) + bss-zero + the one relocation the
linker did not pre-fill (every `R_ARM_ABS32` = `S + A`) + `.init_array`. Because there is
no `r10` and no GOT, exports are called with **plain function pointers** — `name(args)`.

`optimal::load_and_link()` is the whole flow in one call: load every blob, resolve
cyclic module↔module imports, and patch main's GOT so every export is callable **by name**.

**Public header:** `include/optimal/loader.hpp` (namespace `optimal`), built on
`xip/static/nopic.hpp` (namespace `xip::nopic`).

```cpp
namespace optimal {

// RAII wrapper over one loaded no-pic module.
class Module {
public:
    Module() = default;
    ~Module();                                   // unloads if ok
    Module(const Module&) = delete;              // move-only
    bool load(std::span<const std::byte> blob) noexcept;  // segments -> reserved windows
    void* sym(const char* name) const noexcept;  // absolute runtime address (0 if absent)
    bool ok() const noexcept;
    const xip::nopic::PModule& get() const noexcept;
};

// Load `n` module blobs, link them, and enable by-name calls from main:
//   1. load each blob into its fixed windows (ctors run);
//   2. resolve_all() — fill still-unresolved imports so CYCLIC groups work;
//   3. patch_main() for every module — point main's GOT/PLT at the exports.
// Returns the number of modules that loaded successfully.
int load_and_link(const std::span<const std::byte>* blobs, int n, Module* out) noexcept;
}
```

The underlying `xip::nopic` API is also public (for finer control):

```cpp
namespace xip::nopic {
struct PModule { bool ok; std::uintptr_t text_base, data_base;
                 std::uint32_t text_vaddr, text_size, data_vaddr, data_memsz;
                 std::span<const std::byte> reldyn, dynsym, dynstr;
                 loader::symbol_resolver resolver; };
bool load(std::span<const std::byte> blob, PModule& out) noexcept;
void* resolve(const PModule& m, const char* name) noexcept;
void* resolve_any(const char* name) noexcept;   // loaded-module registry, then main
void resolve_all() noexcept;                    // re-resolve cyclic imports; idempotent
void patch_main(const PModule& m) noexcept;     // fill MAIN's GOT/PLT by name
void unload(PModule& m) noexcept;
}
```

**Example** (`tests/optimal-shared/test-ram/src/main.cpp`): modules come from a RAM
arena, then the *entire* fixture set is loaded and linked with one call, after which every
export is a direct `extern "C"` call:

```cpp
#include "optimal/loader.hpp"

// The generated module list (basenames, load order):
const char* const kModules[] = { "slib.so","mylib1.so","mylib2.so","libprov.so",
                                 "libcons.so","libcppsh.so","libcppeh.so","ehx_catch.so",
                                 "cpp3.so","c2.so","c1.so" };
constexpr int kNumModules = 11;

optimal::Module            g_mods[kNumModules];
std::span<const std::byte> g_blobs[kNumModules];
alignas(8) std::byte g_arena[52 * 1024];   // holds all module images at once

std::span<const std::byte> provide(const char* name) {
    char path[160];
    std::snprintf(path, sizeof(path), "%s/%s", DL_LIB_DIR, name);
    firmware::semihost_file src(path);
    if (!src.is_open()) return {};
    const std::size_t sz = src.size();
    std::byte* buf = g_arena + g_used;
    src.read_at(0, std::span<std::byte>(buf, sz));
    g_used += (sz + 7) & ~std::size_t(7);
    return std::span<const std::byte>(buf, sz);
}

int main(void) {
    setup();
    for (int i = 0; i < kNumModules; ++i) g_blobs[i] = provide(kModules[i]);
    int loaded = optimal::load_and_link(g_blobs, kNumModules, g_mods);
    trace_printf("[boot] %d/%d modules loaded + linked, main GOT patched\n", loaded, kNumModules);

    // Every fixture export is now a DIRECT by-name call (declared extern "C"):
    trace_printf("  vars: s_a=%u (ctor 10->15)  s_b=%u (ctor->30)  s_msg=\"%s\"\n", s_a, s_b, s_msg);
    trace_printf("  funcs: s_add(10,5)=%u  s_scale(3)=%u\n", s_add(10, 5), s_scale(3));
    trace_printf("  mylib1 library_function(39)=%u  g(5)=%u\n", library_function(39), g(5));
    // ... classes (cppsh), exceptions (ehx), hard-float, cyclic c1<->c2<->cpp3 ...
}
```

### 7.5 Case 5 — `scl::dynamic-nolib` (freestanding `dlopen`)

**Plain English.** A tiny POSIX-flavoured dynamic loader with **no libc**. The
application provides a `byte_source` (semihosting file, flat-fs blob, anything), the
loader copies the `PT_LOAD` segments into the image arena, resolves imports against
*other loaded libraries first, then main* (via `link_context::from_self()`), applies
relocations, and runs constructors. It supports **lazy binding** (first call routes
through `dl_runtime_resolve`, which patches the GOT slot), **cross-library `r10`
routing** (optional), and registers each image's `.ARM.exidx` so C++ exceptions in
loaded libraries unwind correctly.

There are two surfaces:

1. **`fw::`** — the core, over any `byte_source`.
2. **`dl::`** — the POSIX-style static class with `dlopen`/`dlsym`/`dlclose`/`dlerror`
   and `RTLD_*` constants.

**Public headers:** `include/dynamic-nolib/dl.hpp`, `include/dynamic-nolib/lazy-plt.hpp`.

```cpp
namespace fw {

struct handle {
    std::uint32_t base = 0;        // image base (load bias)
    std::uint32_t size = 0;        // allocated image size
    std::uint32_t got_base = 0;    // r10 value for entering this library
    std::uint32_t got_off  = 0;    // image offset of .got
    std::uint16_t got_words = 0;   // # of 4-byte .got entries captured
    std::uint32_t got_raw[K_GOT_SNAP_MAX] = {};   // pre-relocation .got snapshot (debug)
    std::uint32_t fini_array = 0;  // absolute address of the fini fn-ptr array
    std::uint32_t fini_count = 0;
    loader::symbol_resolver resolver{ {}, {}, {} };
    bool in_use = false;
    std::uint32_t refcount = 0;    // # of loaded libs importing from this one
    std::uint32_t dep_count = 0;
    std::uint8_t  deps[4] = {};    // indices (into g_handles) this lib imports from
    loader::lazy_tables lazy{};        // .rel.dyn JUMP_SLOT spans for lazy fixup
    loader::lazy_tables lazy_plt{};    // .rel.plt JUMP_SLOT spans
    std::uint32_t exidx_base = 0;      // relocated .ARM.exidx start
    std::uint32_t exidx_count = 0;     // # of 8-byte EHABI entries
    char name[12] = {};                // short provenance name (basename)
};

// Core loader over any byte_source: copy, relocate, resolve against main,
// run constructors. `rawname` seeds the handle's short provenance name.
[[nodiscard]] loader::result<handle*> dlopen_source(loader::byte_source& src, const char* rawname) noexcept;
[[nodiscard]] loader::result<void*> dlsym(handle& h, const char* name) noexcept;
void dlclose(handle& h) noexcept;    // runs destructors, frees the image, invalidates h

// P6: enable/disable cross-library r10 routing (off by default). When on, dlopen
// points every external JUMP_SLOT at dl_cross_call so lib->lib calls run with the
// callee's own got_base.
void dl_enable_cross_call(bool on) noexcept;
void dl_cross_trace(int level) noexcept;   // 0=off, 1=cross-lib hops, 2=verbose

// Debug/didactic dumps (see the header for details):
void dl_dump_image(const handle& h) noexcept;
void dl_dump_main_provides(const char* const* names, int n) noexcept;
void dl_dump_got_walkthrough(const handle& h) noexcept;
}

// POSIX-style facade:
class dl {
public:
    static void* dlopen(const char* filename, int flag) noexcept;
    static void* dlsym(void* handle, const char* symbol) noexcept;
    static int   dlclose(void* handle) noexcept;
    static char* dlerror() noexcept;    // last error, consumed on read
};
// RTLD_LAZY=1, RTLD_NOW=2, RTLD_GLOBAL=256, RTLD_LOCAL=0 are defined if missing.
```

Lazy binding runtime (`include/dynamic-nolib/lazy-plt.hpp`):

```cpp
extern "C" {
void  dl_runtime_resolve(void);   // naked Thumb trampoline, ip = &GOT[n] on entry
void* dl_fixup(std::uint32_t* slot) noexcept;  // resolve + patch slot, return target
}
namespace fw { std::uint32_t dl_fixup_count() noexcept; }  // first-call-only proof
```

**Example** — using the `fw::` core over a `byte_source` (freestanding). This is the real
nolib surface; the resolver chain (other loaded libs → main) is automatic via
`link_context::from_self()`, and calls go through the `pic_call_*` trampolines with
`h->got_base`:

```cpp
#include "loader/sources/fd-byte-source.hpp"   // or memory_byte_source for a RAM blob
#include "dynamic-nolib/dl.hpp"
#include "scl/pic-trampoline.h"

// Storage front-end: any byte_source (semihosting file, flat-fs blob, ...).
auto src = loader::fd_byte_source::open_file("/fixtures/mylib1.so");
if (!src) { /* error::FILE_NOT_FOUND */ }

auto load = fw::dlopen_source(*src, "mylib1");      // copy + relocate + resolve + ctors
if (!load) { /* load.err(): NOT_ELF / OUT_OF_MEMORY / SHORT_READ / ... */ return; }
fw::handle* h = *load;

auto fn = fw::dlsym(*h, "library_function");         // result<void*> (SYMBOL_NOT_FOUND)
if (fn) {
    auto f = reinterpret_cast<std::uint32_t (*)(std::uint32_t)>(*fn);
    std::uint32_t r = pic_call_u32(f, (void*)h->got_base, 42);   // r10 = got_base
}
auto var = fw::dlsym(*h, "aa");                      // exported global -> read directly
if (var) std::uint32_t v = *static_cast<std::uint32_t*>(*var);

fw::dlclose(*h);                                     // refcount-guarded; runs fini_array
```

The freestanding `dl::` class (`dl::dlopen`/`dlsym`/`dlclose`/`dlerror`, §7.5 API above)
is the POSIX-shaped version of this same core over a *path*, routed through the weak
`fw::dlopen` / `fw::dlopen_fs` hooks. Note: the hosted test battery's `dl::` namespace
(`firmware/dl-impl.hpp`, `handle = firmware::library*`) is a **different facade** over
`scl::dynamic-newlib` — see §7.6.

#### What `dlopen_source()` actually does (the full load pipeline)

`src/dynamic-nolib/dl.cpp:190`. Every step below runs inside one `noexcept`
call; any failure returns `loader::unexpected(...)` and frees everything allocated
so far (the arena slot is released, the handle is reset):

| # | Step | Where |
|---|---|---|
| 1 | Parse the ELF (`elf_image::parse`) — validates magic/class/endian/machine | `src/common-elf-reader/src/elf-image.cpp` |
| 2 | `img.image_size()` → total VA range for the arena slot | `elf_image::image_size` |
| 3 | `fw::image_arena().alloc(size)` — one slot from the fixed arena | `src/image-memory.cpp`, `region-allocator.cpp` |
| 4 | Zero-fill the slot | `memset` |
| 5 | Copy every `PT_LOAD` segment to its `p_vaddr` offset in the slot | `img.program_header(i) + src.read_at` |
| 6 | `section_cache::load()` — read the section-header table **and** `.shstrtab` in **two** reads (≈8 per-lookup reads otherwise) | `dl.cpp:84` |
| 7 | Extract `.dynsym` / `.dynstr` / `.hash` as in-image spans (`sec_in_image`) | `dl.cpp:144` |
| 8 | `fw::link_context::from_self()` — main's exported dynamic symbols (`_DYNAMIC`) | `src/link-context.cpp` |
| 9 | `alloc_handle()` — first free slot in the fixed `g_handles[4]` pool | `dl.cpp:60` |
| 10 | Snapshot `.got` **before** relocation into `h->got_raw[]` (didactic walkthrough) | `dl.cpp:246` |
| 11 | Build the `combined_resolver` (other loaded libs first, then main) and record dependency edges | `dl.cpp:158` |
| 12 | `loader::reloc_applier::apply_rel()` on `.rel.dyn` then `.rel.plt` | `src/reloc-applier.cpp` |
| 13 | Bump refcounts of every library this one imports from | — |
| 14 | Lazy binding setup: point every `JUMP_SLOT` GOT slot at `dl_runtime_resolve`, stash the tables in `h->lazy` / `h->lazy_plt` | `dl.cpp:279` |
| 15 | *(optional)* route every *external* `JUMP_SLOT` at `dl_cross_call` for lib→lib `r10` swap | `dl.cpp:312` |
| 16 | `got_base = base + .got_offset` (the `r10` value) | `dl.cpp:331` |
| 17 | Fix the Thumb bit on each generic `.ARM.extab` personality pointer; `dl_exidx_register()` | `dl.cpp:335`, `port/unwind-find-exidx.c` |
| 18 | Run constructors: `.init_array` (found **by section type**, not name — the C libs emit `.inits`) via `pic_call_void(fn, got_base)` | `dl.cpp:375` |
| 19 | Record `.fini_array` (by type) for `dlclose` | `dl.cpp:384` |

`fw::dlsym` (`dl.cpp:392`) is `h->resolver.address_of(name)` → `base + st_value`.
`fw::dlclose` (`dl.cpp:398`) refuses if `refcount != 0`, runs `.fini_array` in reverse,
decrements dependency refcounts, `dl_exidx_unregister`, frees the arena slot and
resets the handle to `{}`.

#### Internal functions (`src/dynamic-nolib/dl.cpp`)

| Function | Line | Purpose |
|---|---|---|
| `put_hex` / `put_dec` | 35 / 38 | no-libc hex/decimal print (via the `scl_log` hook) |
| `reloc_name(t)` | 46 | human-readable name for an ARM REL type |
| `alloc_handle()` | 60 | first free slot in `g_handles[4]` |
| `section_cache::{load,find,find_by_type}` | 84/107/127 | two-read section-header + `.shstrtab` cache; lookup by name or by `sh_type` |
| `sec_in_image(cache,mem,name,addr_out)` | 144 | an allocated section as an in-image span |
| `combined_resolver::{thunk,source}` | 164/184 | resolve other-loaded-libs-first, then main; records dependency edges |
| `log_fixup_miss(h,slot,why)` | 433 | log a lazy-fixup failure with the slot's symbol name |
| `dl_fixup(slot)` | 456 | **C linkage** lazy resolver: GOT slot → JUMP_SLOT → resolve → patch |
| `dl_cross_lookup(got_slot_addr)` | 501 | **C linkage** cross-call resolver: returns `{callee_got, target\|1}` packed in a `uint64_t` |
| `got_slot_reloc(h,off,sym)` | 629 | find the relocation targeting a GOT offset (walkthrough) |

File-scope state: `g_handles[4]` (the handle pool), `g_cross_call` (bool),
`g_cross_trace` (int), `g_fixup_count` (uint32_t), `g_sc` (one section cache,
single-threaded).

#### Port layer (`src/port/`)

- **`pic_call_void/u32/u32_2`** (`pic-trampoline.c`): naked asm, ABI `r0=fn, r1=got,
  r2=a0, r3=a1`. Pattern `push {r10,lr} → mov r10,r1 → blx r0 → pop {r10,pc}`, with
  EHABI unwind directives.
- **`dl_runtime_resolve`** (`lazy-resolve.c`): naked Thumb trampoline entered with
  `r12 = &GOT[n]` (set by `PLTn`). Saves `r0-r3, r4, lr` + `d0-d7`, calls
  `dl_fixup(&GOT[n])`, restores, tail-calls the target.
- **`dl_cross_call`** (`dl-cross-call.c`): naked trampoline; saves `r4-r7, r10, lr` +
  `r0-r3` + `d0-d7`, calls `dl_cross_lookup`, swaps `r10 := callee_got`, `blx target`,
  restores the caller's `r10`.

#### The GOT layout and why PLT0 is bypassed

A Cortex-M PIC library's GOT is laid out with three reserved slots first:
`GOT[0] = &_DYNAMIC`, `GOT[1] = link_map` (unused on bare metal), `GOT[2] = resolver`.
Standard ARM lazy binding routes `PLTn → GOT[n] → PLT0 → GOT[2] → resolver`. The SCL
**bypasses PLT0 entirely**, for two Cortex-M-specific reasons: (1) PLT0's PC-relative
GOT-base arithmetic is off by 4 bytes on Thumb, and (2) PLT0's `ldr pc,[lr,#8]!`
clobbers `lr`, which the resolver needs as the caller's return address. Instead, the
loader writes `&dl_runtime_resolve` **directly into every external `JUMP_SLOT` GOT slot**;
`PLTn` already puts `&GOT[n]` in `r12`, so the resolver always learns which slot to patch.

#### The three binding strategies coexist

| Strategy | JUMP_SLOT slot points at | When |
|---|---|---|
| **Eager** | resolved absolute address | intra-library / resolved-at-load references |
| **Lazy** | `dl_runtime_resolve` | first call resolves + patches the slot; later calls jump directly |
| **Cross-call** | `dl_cross_call` | external lib→lib calls, when `dl_enable_cross_call(true)` — resolves and swaps `r10` at call time |

### 7.6 Case 6 — `scl::dynamic-newlib` (hosted POSIX `dlopen`, journaled revert)

**Plain English.** The *hosted* dynamic loader — requires newlib (a heap) and
`-DSCL_HOSTED=ON`. It builds a `relocation_plan` (`std::vector`), applies it while
recording a **patch journal**, and on `unload` replays the journal in reverse to restore
main **byte-exact**. Symbol resolution is fully automatic: main's symtab → the library's
own `.dynsym` → cross-library search. Unresolved slots are tracked as **pending
relocations** and re-patched when a later `dlopen` provides the missing symbol.

**Public headers:** `include/dynamic-newlib/dynamic-linker.hpp`,
`include/dynamic-newlib/image-memory.hpp`, `include/dynamic-newlib/link-context.hpp`,
`include/dynamic-newlib/patch-journal.hpp` (namespace `firmware`).

```cpp
namespace firmware {

struct reloc_table {       // main's dynamic section addresses/sizes
    std::uint32_t reldyn_addr, reldyn_size, dynsym_addr, dynsym_size,
                  dynstr_addr, dynstr_size, hash_addr, hash_size, inits_addr, inits_size;
};
struct sym_entry { const char* name; std::uint32_t addr; };

class link_context {
public:
    link_context() noexcept;
    void set_symtab(const sym_entry* entries, std::size_t count) noexcept; // bare-metal mode
    const reloc_table& table() const noexcept;
    std::span<const std::byte> reldyn() const noexcept;
    std::span<const std::byte> dynsym() const noexcept;
    std::span<const std::byte> dynstr() const noexcept;
    std::span<const std::byte> hash() const noexcept;
    std::byte* got_base() const noexcept;
    std::size_t got_size() const noexcept;
    std::uint32_t resolve(const char* name) const noexcept;  // 0 if not found
};

class image_memory {
public:
    image_memory() noexcept = default;
    loader::result<void> load_segments(loader::elf_image& image, std::size_t extra_bytes = 0) noexcept;
    std::byte* base() const noexcept;
    std::size_t size() const noexcept;
    bool is_loaded() const noexcept;
    // move-only; owns a std::unique_ptr<std::byte[]>
};

class patch_journal {
public:
    static constexpr std::size_t kJournalDefaultCapacity = 1024;
    struct Entry { std::byte* address{}; std::uint32_t original_value{}; };
    void use_buffer(std::byte* buf, std::size_t cap) noexcept;  // co-allocated with image
    loader::result<void> record(std::byte* address, std::uint32_t original_value) noexcept;
    loader::result<void> record_bytes(std::byte* address, std::size_t nbytes) noexcept; // R_ARM_COPY
    void revert() noexcept;                    // restore in reverse order
    std::size_t count() const noexcept;
    bool empty() const noexcept;
    std::size_t capacity() const noexcept;
};

struct library {
    image_memory   image;
    patch_journal  journal;
    std::uint32_t base_va{}, dynsym_va{}, dynstr_va{}, hash_va{};
    std::uint32_t dynsym_sz{}, dynstr_sz{}, hash_sz{};
    void*         init_array{};
    std::size_t   init_count{};
    bool          constructed{false};
};

class dynamic_linker {
public:
    dynamic_linker(const link_context& ctx) noexcept;
    [[nodiscard]] loader::result<library*>
    load_from_source(loader::byte_source& src) noexcept;   // parse+load+relocate+construct
    void unload(library* lib) noexcept;                     // revert journal, free, remove
    [[nodiscard]] loader::result<void*> lookup(library* lib, const char* name) noexcept;
    [[nodiscard]] auto& libs() noexcept;                    // all loaded libraries
};
}
```

#### How main's symbols reach the newlib loader (`_reloc` table vs `.dynsym`)

Unlike the nolib loader (`fw::link_context::from_self()`, which parses `_DYNAMIC` from a
`-pie -Wl,-E` firmware), the newlib `firmware::link_context` is built for a **plain
(non-PIC) firmware with no `_DYNAMIC`**. Two inputs are supported:

1. **The `_reloc` table** (`src/dynamic-newlib/link-context.cpp`). The application's
   linker script emits a fixed-format table of `(address, size)` pairs for main's
   `.rel.dyn`, `.dynsym`, `.dynstr`, `.hash` and `.inits`:

   ```ld
   _reloc = .;
   LONG(LOADADDR(.rel.dyn));  LONG(SIZEOF(.rel.dyn));
   LONG(LOADADDR(.dynsym));   LONG(SIZEOF(.dynsym));
   LONG(LOADADDR(.dynstr));   LONG(SIZEOF(.dynstr));
   LONG(LOADADDR(.hash));     LONG(SIZEOF(.hash));
   LONG(LOADADDR(.inits));    LONG(SIZEOF(.inits));
   ```

   The `link_context()` constructor reads it and sanity-checks each address against the
   flash (`0x0800_0000`–`0x0808_0000`) and RAM (`0x2000_0000`–`0x2002_0000`) windows,
   zeroing any entry that fails. `reldyn()/dynsym()/dynstr()/hash()` expose the spans;
   `got_base()`/`got_size()` expose main's `_sgot`/`_esgot` bounds.

2. **An external `.symtab`** (bare-metal mode): `link_context::set_symtab(entries, count)`
   hands the loader a `{name, addr}[]` array read from the firmware ELF by platform code;
   `resolve()` does a linear scan over it. This is the automatic path used by the XIP
   newlib adapter (`xip::newlib::set_symtab`).

The `dynamic_linker` (`src/dynamic-newlib/dynamic-linker.cpp`) resolves symbols in the
order **main symtab → the library's own `.dynsym` → cross-library search**, records every
word it patches in the journal (including main's own GOT via `patch_main_got`), and keeps
a **pending-reloc list** for slots left at 0 because no providing library was loaded yet —
each new `load_from_source` re-patches them. `unload()` reverts the journal in reverse,
prunes pending relocs pointing into the freed image, and removes the library.

The journal is **co-allocated**: `load_from_source` asks `image_memory::load_segments` for
`kJournalDefaultCapacity × sizeof(Entry)` extra bytes at the tail of the image buffer and
points `patch_journal::use_buffer` at them — no separate allocation.

**Example** (`tests/dynamic-shared/test-xip/test-xip-dynamic-newlib/test1/src/main.cpp`
pattern): the hosted loader is normally hidden behind the POSIX `dlopen`/`dlsym`/`dlclose`
facade. A direct use looks like:

```cpp
#include "dynamic-newlib/dynamic-linker.hpp"

firmware::link_context ctx;                       // main's dynamic-symbol surface
firmware::dynamic_linker linker(ctx);             // one linker instance per firmware

// application-provided byte_source (here: a semihosting file)
firmware::semihost_file src(path);
if (!src.is_open()) return;

auto lib = linker.load_from_source(src);          // parse + load + relocate + ctors
if (!lib) { /* lib.err() */ }

auto fn = linker.lookup(*lib, "cppsh_compute");   // automatic cross-resolution
// ... call fn ...

linker.unload(*lib);                              // journaled revert, byte-exact
```

**The POSIX facade** (`firmware/dl-impl.hpp`, vendored with the hosted platform): a
`namespace dl` with `using handle = firmware::library*;` and
`dlopen(path, Mode::Now)/dlsym(h,name)/dlclose(h)/dlerror()`, translating the core
`result` errors into the POSIX `nullptr + dlerror()` convention. `tests/dynamic-shared/
test-dynamic/test1/src/main.cpp` exposes it under the plain POSIX names:

```cpp
// the standard POSIX API (backed by the hosted dl:: facade over scl::dynamic-newlib)
extern "C" {
void* dlopen(const char* p, int)            { return dl::dlopen(p); }
void* dlsym(void* h, const char* n)         { return dl::dlsym((dl::handle)h, n); }
int   dlclose(void* h)                      { return dl::dlclose((dl::handle)h); }
const char* dlerror(void)                   { return dl::dlerror(); }
}

static void* open_so(const char* so) {
    char path[160];
    snprintf(path, sizeof path, SCL_SO_DIR "/%s", so);
    void* h = dlopen(path, 0);
    if (!h) { printf("  !dlopen %-10s: %s\n", so, dlerror()); g_fail++; }
    else printf("  dlopen %-10s ok\n", so);
    return h;
}

int main(void) {
    void* a = open_so("libprov.so");
    printf("  libprov: prov_value(5) = %u\n", call1(a, "prov_value", 5));
    void* b = open_so("libcons.so");                 // libcons calls prov_value in libprov
    printf("  libcons: cons_call(5)  = %u\n", call1(b, "cons_call", 5));
    dlclose(a); dlclose(b);
    // ...
}
```

### 7.7 Case 7 — `scl::overlay` (time-multiplex one SRAM window)

**Plain English.** The modules are **built with the firmware** and bound at **link time**
to the *same* run address (a shared SRAM window, `VMA`); their bytes are stored
back-to-back in flash (each at its own `LMA`). "Loading" a module is a `memcpy`
`LMA → VMA` followed by `DSB`/`ISB`. Only one module is resident in a window at a time;
the linker's `NOCROSSREFS` guarantees modules never reference each other. This is the
cheapest possible "load" and gives direct calls with no PIC/GOT/`r10` machinery — use it
only for your *own* build-time modules, never third-party `.so`.

**Public header:** `include/overlay/overlay.h` (C API, namespace-less). The application
owns the linker script (template: `src/overlay/ldscripts/overlay-fragment.ld`).

```c
typedef struct scl_overlay_module {
    void*       vma;    /* shared window run address (same for every module) */
    const void* lma;    /* flash load address of this module's bytes */
    uint32_t    size;   /* module size in bytes (SIZEOF(.ovN)) */
    const char* name;   /* short name for tracing; may be NULL */
} scl_overlay_module;

#define SCL_OVERLAY_NONE (-1)   /* no module resident */

typedef struct scl_overlay_region {
    const scl_overlay_module* modules;   /* descriptor table (application-owned) */
    int  count;                          /* number of entries in `modules` */
    int  resident;                       /* index of the loaded module, or SCL_OVERLAY_NONE */
} scl_overlay_region;

void scl_overlay_init(scl_overlay_region* r,
                      const scl_overlay_module* modules, int count);
/* Make module `index` resident: memcpy its bytes from flash into the window,
 * then DSB+ISB. No-op if already resident.
 * Returns 0 on success (or already resident), -1 if `index` is out of range. */
int  scl_overlay_load(scl_overlay_region* r, int index);
int  scl_overlay_current(const scl_overlay_region* r);  /* resident index or SCL_OVERLAY_NONE */
```

**Example** (from the linker-script template comment): after the application builds its
module table from the linker symbols `ADDR(.ov1)`, `LOADADDR(.ov1)`, `SIZEOF(.ov1)`:

```c
extern uint8_t _ovl_vma[], _ov1_lma[], _ov1_size[], _ov2_lma[], _ov2_size[];
static const scl_overlay_module mods[] = {
    { _ovl_vma, _ov1_lma, (uint32_t)(uintptr_t)_ov1_size, "ov1" },
    { _ovl_vma, _ov2_lma, (uint32_t)(uintptr_t)_ov2_size, "ov2" },
};
scl_overlay_region ovl;
scl_overlay_init(&ovl, mods, 2);
scl_overlay_load(&ovl, 0);        // memcpy flash->window + barrier
ovcpp_fcompute(2, 100);           // direct call — the address was fixed at link time
```

### 7.8 XIP adapters for the dynamic loaders (`scl::xip-dynamic-nolib` / `-newlib`)

These wrap the two dynamic loaders with an execute-in-place path. Given a **flash-resident
module image** (a `std::span` of the raw `.so` bytes, e.g. a flat-fs blob), they check
whether the module can XIP (split R-X\|RW + immutable text); if yes, the R-X segment stays
in flash and only RW is copied to RAM (`r10` = data base); if not, they fall back to the
stock copy-whole loader.

**Public headers:** `include/xip/dynamic-nolib/adapter.hpp` (namespace `xip::nolib`),
`include/xip/dynamic-newlib/adapter.hpp` (namespace `xip::newlib`). Both expose the same
shape:

```cpp
namespace xip::nolib {   // xip::newlib has an identical surface, plus set_symtab()
using xip_map = xip::xip_map;

void* dlopen(std::span<const std::byte> so_image, const char* name) noexcept; // opaque handle
void* dlsym(void* handle, const char* name) noexcept;   // runtime address (flash code / RAM data)
std::uint32_t get_got_base(void* handle) noexcept;      // r10 to pass to pic_call_*
bool is_xip(void* handle) noexcept;                     // true if executing in place
bool get_xip_map(void* handle, xip_map& out) noexcept;  // segment placement
void dlclose(void* handle) noexcept;                    // frees the RAM segment

// Provision-time: patch each external JUMP_SLOT slot in a writable copy of the raw
// .so (VA == file_offset) with the address resolved from main, BEFORE the image is
// flashed. Returns the number of slots patched, or -1 on parse failure.
int patch_main_imports(std::span<std::byte> image) noexcept;
}

// namespace xip::newlib only:
namespace xip::newlib {
// Initialize the firmware symbols table for dynamic lookup (bare-metal mode).
void set_symtab(const firmware::sym_entry* entries, std::size_t count) noexcept;
}
```

`xip_map` (from `include/xip/xip-handle.hpp`) is the segment-placement report:

```cpp
namespace xip {
struct xip_map {
    bool           is_xip     = false;   // true => executes in place from flash
    std::uintptr_t text_base  = 0;
    std::uint32_t  text_size  = 0;
    std::uintptr_t data_base  = 0;
    std::uintptr_t got_base   = 0;
    std::uint32_t  data_memsz = 0;
};
}
```

**Example** (`tests/dynamic-shared/test-xip/test-xip-dynamic-nolib/test1/src/main.cpp`):

```cpp
#include "xip/dynamic-nolib/adapter.hpp"
#include "scl/pic-trampoline.h"

void test_one(const char* so, const char* sym, unsigned arg) {
    void* h = xopen(so);                       // xip::nolib::dlopen(fs_blob, name)
    if (!h) { trace_printf("  !%s dlopen FAILED\n", so); return; }
    const std::uint32_t g = xip::nolib::get_got_base(h);
    trace_printf("  %s: %s %s(%u) = %u\n", so,
                 xip::nolib::is_xip(h) ? "XIP" : "copy-whole", sym, arg,
                 C1(h, g, sym, arg));          // dlsym + pic_call_u32(fn, got, arg)
    xip::nolib::dlclose(h);
}

// C1 / C0 / C2 / RD / CSTR are small test helpers:
//   C1(h,g,s,a)  -> void* fn = xip::nolib::dlsym(h,s); return pic_call_u32(fn,(void*)g,a);
//   RD(h,s)      -> read a global: *(volatile uint32_t*)xip::nolib::dlsym(h,s)
```

### 7.9 Internal XIP helpers (`include/xip/` plumbing)

The two XIP adapters above (and the `xip::static` path) are built on four small
freestanding headers. They are **internal** — a firmware never includes them
directly; they exist here so the "can this `.so` execute in place?" decision and
the split-base relocation are readable in isolation. Each maps to the analysis
in `doc/xip-flash-execution-analysis.md`.

| Header | Namespace | What it provides |
|---|---|---|
| `xip/segment-map.hpp` | `xip::SegmentMap` | *Blocker A* — pure classification of a module's program headers into the R-X and RW segments; `split == false` means a single fused RWX blob (cannot XIP). `in_data(vaddr)` tests which segment an address belongs to. |
| `xip/module.hpp` | `xip::Module` | The two-base model: `text` (a `span` of the flash-resident R-X bytes, executed in place) + `data` (the only RAM allocation) with `text_base` / `data_base` (`data_base == r10`). |
| `xip/relocate.hpp` | `xip::text_is_immutable`, split-base reloc apply | The R-X immutability safety scan (`true` iff no relocation targets the R-X range) plus `R_ARM_RELATIVE`/`JUMP_SLOT` application resolved against the `SegmentMap` with **two** bias values instead of one. |
| `xip/compact.hpp` | `xip::compact(...)` | *Blocker C* — the top-level "classify + safety-scan" procedure over a flash-resident blob (`fw::fs_blob`); on success fills an in-place `Module` view, else reports `xip == false` so the caller falls back to copy-whole. |

The public entry points that use them are the adapters in §7.3 (`xip::static`)
and §7.8 (`xip::nolib` / `xip::newlib`); `xip_map` (§7.8) is the flattened,
firmware-facing report of what these produce.

---

## 8. Cross-cutting runtime services

These are the mechanisms several loaders share. They are *internal* but worth
understanding because they shape the external API's guarantees.

### 8.1 Symbol resolution strategies (three ways to answer "where is `name`?")

| Strategy | Used by | How |
|---|---|---|
| **Resolver callback** (`loader::symbol_source`) | `scl::reloc`, the relocation engines | The application (or the loader) supplies a function that returns `name`'s absolute address. The `reloc` test implements it as a lookup table of main's exports. |
| **Symbol table** (`elf_loader::symbol_table`) | `scl::static` | The application calls `add_global(name, addr)` for every export main provides; the loader resolves the library's UND symbols against it. |
| **Main's `.dynsym`** (`fw::link_context::from_self()`) | `scl::dynamic-nolib`, `scl::xip-static`, `scl::optimal`/`nopic` | The firmware is linked `-pie -Wl,-E`; the loader parses main's own `_DYNAMIC` and resolves against its dynamic symbols — no table to maintain. |

Resolution *order* (dynamic family): **other already-loaded libraries first, then main**
— so lib→lib imports work, and `dlclose` refcounts correctly.

### 8.2 The patch journal (byte-exact unload)

Both `scl::static` (`Module::teardown`) and `scl::dynamic-newlib` (`patch_journal`) record
every word they write into **main's** memory before overwriting it. Unload replays the
journal **in reverse** so main's GOT/PLT is restored byte-for-byte, then unpublishes the
module's exports and (newlib) frees the image.

### 8.3 Lazy binding (dynamic-nolib)

The GOT of a Cortex-M PIC library begins with three reserved slots —
`GOT[0] = &_DYNAMIC`, `GOT[1] = link_map` (unused on bare metal), `GOT[2] = resolver`.
Standard ARM lazy binding routes `PLTn → GOT[n] → PLT0 → GOT[2] → resolver`, but PLT0 is
**broken on Cortex-M**: its PC-relative GOT-base arithmetic is off by 4 on Thumb, and it
clobbers `lr` (which the resolver needs as the caller's return address). The SCL therefore
**bypasses PLT0**: `dlopen` writes `&dl_runtime_resolve` directly into every external
`JUMP_SLOT` GOT slot.

First call flow:

```
LIBRARY CODE → BL PLTn → PLTn: add r12,pc,#off (r12 = &GOT[n])
                       → ldr pc,[r12]        (pc = dl_runtime_resolve)
dl_runtime_resolve   → push {r0-r3,r4,lr} + vpush {d0-d7}
                     → mov r0,r12 ; bl dl_fixup
dl_fixup(&GOT[n])    → find owning handle → resolve (self → other libs → main)
                     → *GOT[n] = target|1 ; g_fixup_count++ ; return target
dl_runtime_resolve   → vpop {d0-d7} ; pop {r0-r3,r4,lr} ; bx r12   (tail-call target)
SECOND CALL          → PLTn → ldr pc,[GOT[n]] = real target (direct)
```

`dl_fixup_count()` (`fw::dl_fixup_count()`) proves first-call-only binding.

### 8.4 The cross-call trampoline (lib→lib `r10` swap)

Libraries are built `-msingle-pic-base -mpic-register=r10`, so each library reads its
globals through *its own* `r10`/GOT. A lib→lib call would otherwise run the callee with the
*caller's* `r10` and read the wrong data. When cross-call mode is enabled
(`dl_enable_cross_call(true)` for nolib; always for XIP), every **external** JUMP_SLOT is
pointed at the shared naked trampoline (`dl_cross_call` / `xip_cross_call`). At call time
it invokes `dl_cross_lookup(&GOT[n])` / `xip_cross_lookup(slot)`, which maps the slot back
to its symbol, finds the defining module (or main), and returns
`{callee_got, target|1}` packed in a `uint64_t`. The trampoline sets `r10 := callee_got`
for the duration of the call, then restores the caller's `r10`. For a lib→main target,
`callee_got = 0` (main is `-fpic`, `r10`-free).

### 8.5 The section cache (dynamic-nolib, I/O optimisation)

`elf_image::find_section()` re-reads the section-header table and each section's name from
the byte source on every call; over semihosting (a halt/resume per read) that is hundreds
of round-trips per `dlopen`. `dlopen_source` instead reads the section-header table and
`.shstrtab` **once** into a file-scope `section_cache` (≈4.6 KB of `.bss`, single-threaded)
and resolves section lookups by name or by `sh_type` in RAM. This is why constructors are
found **by section type** (`SHT_INIT_ARRAY` = 14), not by name — the C libraries' linker
scripts emit `.init_array` input as an output section named `.inits`.

---

## 9. Host tools

Two small host executables live in the tree.

### 9.1 `tools/gen-symtab` — bake main's exported symbols into flash

For the **hosted** dynamic loader, the firmware's own exported symbol table is embedded
into flash as a binary blob. `gen-symtab <firmware.elf> <out.bin>`:

1. opens and parses the firmware ELF with `common-elf-reader`;
2. finds `.scl_symtab` (the reserved flash placeholder), `.symtab`, `.strtab`;
3. filters to GLOBAL/WEAK, defined, non-`_`-prefixed symbols (an internal whitelist keeps
   `_impure_ptr`, `_Znw`, `_Zdl`, `__cxa*`, `__aeabi*`, etc.);
4. writes a `SCLS`-magic blob of `{name_addr, addr}[]` + the name pool, padded to the
   placeholder size.

It is built automatically by CMake as the `gen-symtab-host` target (host `c++`, not the
ARM toolchain) and embedded by the hosted platform with `scl_embed_symtab()`.

### 9.2 `common-elf-reader/tools/elf2json` — ELF → JSON for the visualizer

`elf2json <input.elf> [output.json]` exports an ARM ELF32 binary as a JSON document
`{ header, program_headers, section_headers, symbols, dynamic, plt }`, using the same
`elf_image` API. It feeds the web GUI under `common-elf-reader/gui/`.

---

## 10. Building and consuming with CMake

### 10.1 Add SCL to your firmware

```cmake
# in the firmware's CMakeLists.txt
add_subdirectory(SCL-shared scl)          # or path/to/SCL-shared
target_link_libraries(my_fw PRIVATE scl::optimal)   # pick ONE case loader
```

`common-elf-reader` is vendored in this repository, so a plain clone is enough:

```sh
git clone <url>/SCL-shared-new.git
```

### 10.2 The generic `scl::loader` alias

Select a case with the cache option and link the generic alias:

```sh
cmake -DSCL_LOADER=optimal ...
```

```cmake
target_link_libraries(my_fw PRIVATE scl::loader)
```

Valid `SCL_LOADER` values: `relocatable` → `scl::reloc`, `static-full-ram` → `scl::static`,
`static` → `scl::xip-static`, `optimal` → `scl::optimal`, `dynamic-nolib` → `scl::dynamic-nolib`,
`dynamic-newlib` → `scl::dynamic-newlib`, `overlay` → `scl::overlay`.

### 10.3 Target reference

| Target | Sources | Notes |
|---|---|---|
| `common-elf-reader::elf-reader` | `src/elf-image.cpp` | the parser; everything else header-only |
| `scl::common` | `dynamic-info, lazy-resolve, region-allocator, reloc-applier, symbol-resolver, log.c, scl-log-put-hex` | shared core; `relocator.cpp` + `SCL_HOSTED=1` added when `SCL_HOSTED=ON` |
| `scl::self-resolve` | `link-context.cpp` | `link_context::from_self()`; needs a `-pie` firmware; link alongside `scl::reloc` if wanted |
| `scl::reloc` | `src/reloc/reloc-loader.cpp` | ET_REL loader |
| `scl::static` | `static/{elf-reader,shlib-loader,logger,module}.cpp, tls.c` | RAM `-fPIC` `.so` patcher + TLS |
| `scl::xip-common` | `xip/{segment-map,relocate,compact}.cpp` | XIP primitives (classify, immutability scan, split-base apply) |
| `scl::xip-static` | `xip/static/adapter.cpp, xip/xip-cross-call.c, port/pic-trampoline.c, image-memory.cpp` | XIP + r10; pulls `scl-xip-common` + `scl-self-resolve` |
| `scl::loader-nopic` | `xip/static/nopic.cpp, image-memory.cpp` | the no-pic XIP loader |
| `scl::optimal` | `src/loader.cpp` | `optimal::load_and_link` glue over `scl::loader-nopic` |
| `scl::overlay` | `src/overlay/overlay.c` | portable C; `DSB/ISB` compiles away off-target |
| `scl::dynamic-nolib` | `dynamic-nolib/dl.cpp, port/{pic-trampoline,dl-cross-call,lazy-resolve,unwind-find-exidx}.c, image-memory.cpp, link-context.cpp` | freestanding `dlopen` |
| `scl::dynamic-newlib` | `dynamic-newlib/{dynamic-linker,patch-journal,image-memory,link-context}.cpp` | hosted; requires `-DSCL_HOSTED=ON` |
| `scl::xip-dynamic-nolib` | `xip/dynamic-nolib/adapter.cpp, xip/dynamic-nolib/xip-cross-call.c` | XIP adapter over `scl::dynamic-nolib` |
| `scl::xip-dynamic-newlib` | `xip/dynamic-newlib/adapter.cpp, xip/dynamic-nolib/xip-cross-call.c, port/{pic-trampoline,unwind-find-exidx}.c` | XIP adapter over `scl::dynamic-newlib` |
| `gen-symtab-host` | `tools/gen-symtab.cpp` + `elf-image.cpp` | host tool; built for the HOST compiler |

### 10.4 Options

- **`SCL_HOSTED`** (`OFF` default) — build the hosted-only parts that need a C++ standard
  library: `src/relocator.cpp` (the `std::vector` relocation planner) and
  `elf_image::collect_relocations()`. Required by `scl::dynamic-newlib`.
- **`SCL_LOADER`** — pick one case and alias it to `scl::loader` (see §10.2).

### 10.5 What your firmware must provide

- **Symbols the `.so` imports** (libc/libm/HAL/math/`operator new`…). Either export them
  (`-pie -Wl,-E` + `from_self`), populate an `elf_loader::symbol_table`, or write a
  resolver callback — depending on the case.
- **RAM** for the image arena (`_simage_arena_start/_end`), the static-loader exec buffer,
  the `optimal` fixed windows (`_sxip_text_*`, `_sxip_data_*`), or an overlay region —
  via your linker script.
- **The SCL logging hooks** if you want loader tracing (weak no-ops otherwise).
- **A `byte_source`** for the dynamic loaders if your storage isn't a POSIX file
  (implement `loader::byte_source`).

#### The link-anchor trap (`-Wl,-E` and `--gc-sections`)

A module importing `printf` resolves against main's `.dynsym` — but **`-Wl,-E` only
exports symbols that were actually linked in**, and `--gc-sections` strips anything main
does not itself reference. If a loaded library calls `printf` but main never does, the
symbol is not in `.dynsym`, the module's PLT stub jumps to 0, and you get a **HardFault at
run time — not a link error**. The standard fixes:

```cpp
// 1. Linkage anchor array — reference the symbols so gc-sections keeps them:
[[gnu::used, gnu::retain]]
static void* g_link_anchor[] = {
    (void*)printf, (void*)puts, (void*)malloc, (void*)free,
    (void*)sinf, (void*)cosf, (void*)logf, (void*)sqrtf,
    // ... every symbol only the loaded libs use ...
};
```

```cmake
# 2. Or disable gc-sections / keep specific libraries:
target_link_options(my-fw PRIVATE -Wl,--no-gc-sections)          # keep everything
target_link_options(my-fw PRIVATE -Wl,--no-gc-sections=-lc
                                        -Wl,--no-gc-sections=-lm) # keep libc/libm
```

#### Constructor timing

Constructors run **during load**, before your first call. A fault in a global constructor
looks like a *load failure*, not a call failure. Debug accordingly.

### 10.6 Fixture build recipes (what each model expects the module to be)

The module format is a *hard contract* — a module built for one model cannot be handed to
another (a prebuilt stock `.so` only runs from RAM, via `scl::static` or
`scl::dynamic-newlib`). The recipes below are the build-side of each loader:

| Model | Compile | Link | Notes |
|---|---|---|---|
| **reloc** | `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mlong-calls -fno-pic -Os` | `-r` (partial link, stays `ET_REL`) | absolute refs (`ldr rN,=target`), no GOT/PLT/r10 |
| **static-full-ram** | `-fPIC -shared -fno-common -fno-inline-functions` | `-shared -nostartfiles [-nostdlib]` | plain PIC `.so`; C++ fixtures add `-fno-exceptions -fno-rtti -fno-threadsafe-statics`, their own `operator new/delete` wrapping `malloc/free`, and `-Wl,-S` to strip debug sections |
| **xip-static** | `-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative` | `-shared -nostartfiles` with a **split** linker script (two `PT_LOAD` segments, `VA == file_offset`) | R-X stays in flash, RW copied to RAM |
| **optimal** | `-fno-pic -mlong-calls` | linked at a **fixed, build-derived** address inside the reserved windows | no `r10`, no GOT; exports are plain addresses |
| **dynamic (both)** | `-fpic -shared` (nolib: `-msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative`) | `-shared -nostartfiles -Wl,-E --unresolved-symbols=ignore-all` | GOT/PLT based; XIP needs the split `.so` |
| **overlay** | `-fno-pic -mlong-calls`, sections named `.ovN.*` | with the application's `OVERLAY : NOCROSSREFS` block | not an ELF load at all — a `memcpy` |

**For `scl::static` direct-by-name calls**, the firmware must be linked *against* the
`.so` files so the linker emits the GOT/PLT slots, and the headers shared between firmware
and module must wrap every declaration in `extern "C"`:

```cmake
target_link_libraries(my_fw PRIVATE scl::static
    ${FIXTURES}/libspi.so
    ${FIXTURES}/libuart.so
    ${FIXTURES}/libled.so
    ${FIXTURES}/libcpp_features.so)   # absolute .so paths, not -l flags
```

(CMake for `arm-none-eabi` filters `-l` flags; use absolute `.so` paths.) The firmware's
`Reset_Handler` must copy `.got`/`.got.plt` from flash to RAM before `main()` runs, and the
linker script must place `.got`/`.got.plt` in RAM and `.plt` in flash.

### 10.7 Choosing a model — the two questions

| Question | If yes | If no |
|---|---|---|
| Must the code execute from **flash** (RAM too small)? | XIP — pick `xip-static` (allow `r10`), `optimal` (keep `r10`), or the XIP dynamic adapters | RAM — `reloc` (leanest) or `static` (plain `.so`, direct calls) |
| Do you want the POSIX **`dlopen`** handle contract? | `dynamic-nolib` (freestanding) / `dynamic-newlib` (hosted, journaled unload) | a direct-by-name model above |

Plus the call-style axis: `dlsym` + `pic_call_*` (dynamic), direct `name(args)` via
`patch_main` (static/xip-static/optimal), or `memcpy` + direct call (overlay).

### 10.8 A complete template application (nolib path)

A minimal-but-complete `main()` that uses the whole stack — core toolbox, `link_context`,
the freestanding `dlopen` loader, and the PIC trampoline:

```cpp
#include <cstdint>
#include <span>
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/symbol-resolver.hpp"
#include "loader/reloc-applier.hpp"
#include "loader/region-allocator.hpp"
#include "scl/log.h"
#include "scl/image-memory.hpp"
#include "scl/link-context.hpp"
#include "scl/pic-trampoline.h"
#include "dynamic-nolib/dl.hpp"
#include "dynamic-nolib/lazy-plt.hpp"

// Application-provided byte_source (here: a RAM buffer; use fd_byte_source for files).
static std::byte g_image[16384];

extern "C" int main(void) {
    // 1. Core toolbox, directly usable: parse, size, resolve, apply, allocate.
    loader::memory_byte_source src(g_image);
    auto img = loader::elf_image::parse(src);
    if (!img) return 1;
    auto need = img->image_size();
    auto* mem = static_cast<std::byte*>(*fw::image_arena().alloc(*need));

    // 2. The freestanding dlopen loader over ANY byte_source.
    auto load = fw::dlopen_source(src, "mylib1");
    if (!load) { scl_log_puts("dlopen failed\n"); return 1; }
    fw::handle* h = *load;

    // 3. Resolve a function and a data symbol; call through r10 trampolines.
    auto fn_res = fw::dlsym(*h, "library_function");
    auto* fn = reinterpret_cast<std::uint32_t (*)(std::uint32_t)>(*fn_res);
    std::uint32_t r = pic_call_u32(fn, (void*)h->got_base, 42);
    scl_log_put_hex("result=", r);

    auto var_res = fw::dlsym(*h, "library_global");
    if (var_res) { std::uint32_t v = *static_cast<std::uint32_t*>(*var_res); }

    // 4. Lazy-binding proof + didactic dump.
    std::uint32_t n = fw::dl_fixup_count();
    fw::dl_dump_got_walkthrough(*h);

    // 5. Unload (refcount-guarded): destructors, refcounts, arena free.
    fw::dlclose(*h);

    // 6. POSIX-style facade, identical semantics.
    void* p = dl::dlopen("mylib1.so", RTLD_NOW);
    if (p) { void* s = dl::dlsym(p, "library_function"); dl::dlclose(p); }
    const char* err = dl::dlerror();
    return 0;
}
```

The hosted (`dynamic-newlib`) equivalent replaces steps 2–5 with
`firmware::link_context ctx;` → `firmware::dynamic_linker linker(ctx);` →
`linker.load_from_source(src)` → `linker.lookup(lib, "name")` →
`linker.unload(lib)`, and its `lookup()` result is a **direct** call (no `pic_call_*`,
the GOT was patched eagerly).

---

## 11. Error-code reference

### 11.1 `loader::error` (common-elf-reader + SCL core)

| Code | Meaning |
|---|---|
| `OK` | No error (not normally stored). |
| `FILE_NOT_FOUND` | Byte source could not be opened. |
| `SHORT_READ` | Byte source returned fewer bytes than requested. |
| `NOT_ELF` | Magic number is not `0x7f 'E' 'L' 'F'`. |
| `NOT_ARM32` | Not a little-endian 32-bit ARM object. |
| `NOT_SHARED_OBJECT` | `e_type` is not `ET_DYN`. |
| `BAD_SECTION` | Required section missing or malformed. |
| `SYMBOL_NOT_FOUND` | Hash lookup did not find the requested name. |
| `UNSUPPORTED_RELOC` | Relocation type the loader does not implement. |
| `OUT_OF_MEMORY` | A fixed-capacity container overflowed (or the arena/handle pool is full). |

### 11.2 `elf_loader::loader_error` (scl::static)

| Code | String | Meaning |
|---|---|---|
| `OK` | `"Ok"` | success |
| `FILE_NOT_FOUND` | `"FileNotFound"` | file could not be opened |
| `NOT_AN_ELF` | `"NotAnElf"` | not a valid ELF |
| `SECTION_NOT_FOUND` | `"SectionNotFound"` | required section missing |
| `INVALID_SEGMENT` | `"InvalidSegment"` | a `PT_LOAD` segment is malformed / buffer too small |
| `INVALID_RELOCATION` | `"InvalidRelocation"` | malformed relocation |
| `INVALID_SYMBOL` | `"InvalidSymbol"` | malformed symbol |
| `DEP_NOT_FOUND` | `"DepNotFound"` | a `DT_NEEDED` dependency is missing |
| `OUT_OF_MEMORY` | `"OutOfMemory"` | allocation/pool overflow |

### 11.3 `scl::module_error` (scl::Module)

`OK`, `FILE_NOT_FOUND`, `NOT_AN_ELF`, `NO_DYNSYM` (no `.dynsym`/`.dynstr`), `LOAD_FAILED`
(with `last_detail()` = the underlying `elf_loader::loader_error`).

---

## 12. Glossary

| Term | Plain-English meaning |
|---|---|
| **`byte_source`** | The I/O seam: an abstract random-access reader (`size()`, `read_at(offset, span)`). The parser and loaders never know whether the ELF lives on disk (semihosting) or in RAM. |
| **`result<T>`** | A value-or-error wrapper (a minimal `std::expected`). Fallible API calls return this instead of throwing or using errno. |
| **ELF** | Executable and Linkable Format — the container for the compiled library. SCL reads ARM **ELF32 little-endian** objects. |
| **`Ehdr` / `Phdr` / `Shdr`** | ELF file header / program header (a *segment*) / section header (a *section*). Segments are what get loaded; sections describe contents. |
| **`PT_LOAD`** | A loadable segment: a range of file bytes placed into memory at `p_vaddr`, with `p_memsz` ≥ `p_filesz` (the tail is zero-filled bss). |
| **`ET_REL` / `ET_DYN`** | Relocatable object (`.o`) vs shared object (`.so`). `scl::reloc` loads `ET_REL`; all other loaders load `ET_DYN`. |
| **`.dynsym` / `.dynstr` / `.hash`** | The dynamic symbol table, its string table, and the SysV hash table used to look names up fast. |
| **`.rel.dyn` / `.rel.plt`** | Relocation tables: the list of "patch this word, with this symbol, this way" records. ARM uses **REL** (8-byte entries, no explicit addend field). |
| **GOT** | Global Offset Table — a table of pointers the code reads *through* (PC-relative) to reach data/imports. |
| **PLT** | Procedure Linkage Table — tiny stubs that jump through GOT slots to reach imported functions (often lazily). |
| **`r10`** | On Cortex-M, the register reserved as the PIC base for `-msingle-pic-base -mpic-register=r10` code; it points at the current module's GOT base. |
| **PIC** | Position-Independent Code — code that can run at any address because all references are relative (or via GOT). `-fPIC` is the "big" model (default on desktop); `-fpic` with `r10` is the single-pic-base embedded model. |
| **XIP** | eXecute-In-Place — the code stays in flash and the CPU fetches instructions straight from flash; only writable data goes to RAM. |
| **Load bias** | The runtime address of `image[0]` (the start of the loaded image). Relocations that are `R_ARM_RELATIVE` simply add the bias. |
| **`R_ARM_RELATIVE`** | `*slot += load_bias` — the most common relocation for PIC local data. |
| **`R_ARM_GLOB_DAT`** | `*slot = <symbol address>` — a global-variable GOT slot. |
| **`R_ARM_JUMP_SLOT`** | `*slot = <function address>` — a function GOT/PLT slot. |
| **`R_ARM_ABS32`** | `*slot += <symbol address>` — absolute 32-bit data reference (constructor pointers, literal pools). |
| **`R_ARM_REL32`** | `*slot += <symbol address> - PC` — PC-relative 32-bit data reference. |
| **`R_ARM_COPY`** | Copy the symbol's bytes from its defining module (usually main) into the loading module's copy — used when main exports a variable. |
| **`R_ARM_THM_CALL`** | A Thumb `BL` — a PC-relative branch that must be re-encoded when the target moves (used by the ET_REL loader). |
| **`st_value`** | A symbol's value. For a *defined* symbol it is image-relative; **0 means the symbol is *external* (undefined)** — it must be resolved against another module or main. |
| **`symbol_source`** | The universal "resolve a name to an address" callback: `result<uint32_t> (*)(void* ctx, std::string_view name)` plus its context pointer. |
| **`link_context::from_self()`** | Build a resolver over *main's own* dynamic symbols by parsing the linker-emitted `_DYNAMIC` array. Requires a `-pie -Wl,-E` firmware. |
| **Image arena** | The RAM region (linker symbols `_simage_arena_start`/`_end`) managed by `fw::image_arena()` (`region_allocator`) that the loaders allocate loaded images from. |
| **Patch journal** | The log of every word a loader wrote into main's memory plus its original value; unload replays it in reverse for a byte-exact restore. |
| **Lazy binding** | Deferring the resolution of imported functions until first call; the unbound GOT slot points at `dl_runtime_resolve`, which patches it on demand. |
| **Cross-call trampoline** | A naked routine that swaps `r10` to the *callee* library's GOT base around a lib→lib call, then restores it. |
| **`.ARM.exidx` / `.ARM.extab`** | The ARM EHABI unwind tables; the C++ unwinder uses them to unwind through loaded libraries when an exception crosses a library boundary. |
| **`__gnu_Unwind_Find_exidx`** | The libgcc hook that returns the unwind table covering a PC; SCL overrides it to consult the loaded-library registry first, then main's own table. |
| **`DT_NEEDED`** | A `.dynamic` entry naming a library this one depends on; loaders that support it auto-load dependencies via a caller-provided hook. |
| **TLS / thread pointer** | Thread-Local Storage. Each module gets a byte offset in a shared per-thread area; accesses go through `__aeabi_read_tp()` (soft-TP ABI). |
| **Overlay** | Link-time time-multiplexing: several modules bound to the *same* VMA (a shared SRAM window), stored back-to-back in flash; "load" is a `memcpy` + barrier. |
| **VMA / LMA** | Virtual (run) address / Load (storage) address. In overlays all modules share the VMA but have distinct LMAs in flash. |
| **`scl_overlay_region`** | The runtime handle for one overlay window: the descriptor table, its count, and which module is currently resident. |
| **Flat-fs / blob** | A simple on-chip flash file system; XIP loaders treat a blob's raw `.so` bytes as a `std::span` and classify it for in-place execution. |
| **`gen-symtab`** | A host tool that bakes the firmware's own exported symbol table into a flash placeholder so hosted `dlopen` can resolve lib→main imports. |
| **`_reloc` table** | A fixed-format `(address, size)[]` emitted by the application's linker script for a *non-PIC* firmware's `.rel.dyn`/`.dynsym`/`.dynstr`/`.hash`/`.inits`; the newlib `link_context()` reads it instead of parsing `_DYNAMIC`. |
| **Section cache** | The `dynamic-nolib` internal cache that reads the section-header table + `.shstrtab` once per `dlopen` (2 semihosting reads) so all subsequent section lookups hit RAM. |

---

## Appendix — quick reference of every public function

| Loader | Public entry points |
|---|---|
| `common-elf-reader` | `elf_image::parse/header/program_header/section_header/find_section/image_size/image_base/collect_relocations`, `fd_byte_source::open_file`, `memory_byte_source`, `error`, `result<T>` |
| core | `dynamic_info::parse`, `symbol_resolver::address_of`, `reloc_applier::apply_rel`, `relocator::compute`, `lazy_resolve_slot`, `region_allocator::{alloc,free}`, `elf_hash` |
| `scl::reloc` | `reloc::load`, `reloc::load_set`, `reloc::sym(module)`, `reloc::sym(module_set)` |
| `scl::static` | `scl::Module::{load_file,load_buf,image_size,retain,release,unload,sym,operator[],last_error,...}`, `elf_loader::symbol_table`, `elf_loader::elf_reader`, `elf_loader::shlib_loader`, `scl_tls_*` |
| `scl::xip-static` | `xip::stat::{bind_host_imports, load, resolve, unload}` |
| `scl::optimal` | `optimal::{Module, load_and_link}`, `xip::nopic::{load, resolve, resolve_any, resolve_all, patch_main, unload}` |
| `scl::dynamic-nolib` | `fw::{dlopen_source, dlsym, dlclose, dl_enable_cross_call, dl_cross_trace, dl_dump_image, dl_dump_main_provides, dl_dump_got_walkthrough}`, `dl::{dlopen, dlsym, dlclose, dlerror}`, `dl_runtime_resolve`, `dl_fixup` |
| `scl::dynamic-newlib` | `firmware::{dynamic_linker, link_context, image_memory, patch_journal}` |
| `scl::overlay` | `scl_overlay_init`, `scl_overlay_load`, `scl_overlay_current` |
| XIP dynamic adapters | `xip::nolib::*`, `xip::newlib::*`: `dlopen`, `dlsym`, `get_got_base`, `is_xip`, `get_xip_map`, `dlclose`, `patch_main_imports` |
| firmware services | `fw::link_context::from_self()`, `fw::image_arena()`, `scl_log_puts/putc/put_hex`, `scl_memory_barrier`, `pic_call_void/u32/u32_2`, `dl_exidx_register/unregister`, `scl_loader_lock/unlock` |

---

## Appendix — compile-time constants & POSIX defines

| Constant | Header / file | Value | Purpose |
|---|---|---|---|
| `K_GOT_SNAP_MAX` | `dynamic-nolib/dl.hpp` | 32 | max `.got` entries snapshotted per image (didactic walkthrough) |
| `kMaxBlocks` | `loader/region-allocator.hpp` | 16 | max live spans in the region allocator |
| `kAlign` | `loader/region-allocator.hpp` | 8 | allocation granularity (bytes) |
| `MAX_SECTIONS` | `reloc/reloc-loader.hpp` | 48 | max section headers for one ET_REL module |
| `MAX_SET` | `reloc/reloc-loader.hpp` | 4 | max modules in one `load_set()` |
| `kJournalDefaultCapacity` | `dynamic-newlib/patch-journal.hpp` | 1024 | patch-journal capacity |
| `SCL_OVERLAY_NONE` | `overlay/overlay.h` | -1 | "no module resident" marker |
| `SCL_HOSTED` | CMake option | 0/1 | enable `std::vector` parts (`relocator`, `collect_relocations`) |
| `SCL_LOADER` | CMake option | string | pick the `scl::loader` alias target |

```c
/* dynamic-nolib/dl.hpp defines these if the platform header does not: */
#define RTLD_LAZY    1
#define RTLD_NOW     2
#define RTLD_GLOBAL  256
#define RTLD_LOCAL   0
```

---

*SCL-shared & common-elf-reader — API manual. Covers the internal API (parser + shared
core + firmware services) and the external (user) API of all seven loader cases, with
usage examples drawn from `SCL-TESTS-shared`, build/consumption guidance, an error-code
reference, and a glossary.*
