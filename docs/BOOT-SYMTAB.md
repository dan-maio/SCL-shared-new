# Boot-time symbol resolution

This fork implements the proposal analysed in `SCL-projects-analysis.md` §4.10:
the firmware can build its name→address table at boot instead of relying on a
post-link host tool. Two mechanisms are provided.

## 1. Parse an ELF that is present at runtime

`include/scl/symtab-runtime.hpp` walks `.symtab`/`.strtab` through the existing
`common-elf-reader` and fills caller-provided storage. No heap, no host tool.

```cpp
#include "scl/symtab-runtime.hpp"
#include "scl/image-memory.hpp"
#include "scl/link-context.hpp"

extern "C" const std::byte _scl_sidecar_start[], _scl_sidecar_end[];

void load_main_symbols() {
    std::span<const std::byte> elf{_scl_sidecar_start,
                                   std::size_t(_scl_sidecar_end - _scl_sidecar_start)};
    loader::memory_byte_source src{elf};
    auto img = loader::elf_image::parse(src);
    if (!img) return;                       // no sidecar / malformed

    static scl::symtab::entry entries[2048];  // 8 B each on ARM32
    static char names[16 * 1024];             // NUL-terminated name arena
    const std::size_t n = scl::symtab::collect(
        *img, entries, 2048, names, sizeof(names));
    if (n == 0) return;

    fw::dl_set_symtab(entries, n);          // scl::dynamic-nolib
    // or: xip::newlib::set_symtab(entries, n);
    // or: symbol_table.add_entries(entries, n);   // scl::static
}
```

The entries are layout-compatible with `firmware::sym_entry`; for a RAM
`dynamic-newlib` platform that owns its own `link_context`:

```cpp
ctx.set_symtab(reinterpret_cast<const firmware::sym_entry*>(entries), n);
```

### Sidecar rules

- The ELF must be from the **exact same link** as the firmware. Store and check
  a build ID/hash; a stale sidecar silently sends calls to wrong addresses.
- A sidecar is data outside the image, so it does not shift the layout it
  describes. Do not try to embed a firmware's own complete ELF inside itself:
  adding the blob changes the addresses the blob contains.
- `cmake/ldscripts/sidecar-elf-example.ld` reserves a `NOLOAD` flash region and
  exposes `_scl_sidecar_start/_end`; the programmer writes the ELF there.
- Names point into the `names` arena, or into a memory-mapped sidecar if you
  build a view instead. Keep that storage alive as long as the loader uses it.

## 1b. Semihosting: read main's own ELF at boot

If a debugger/host is attached, you need neither a sidecar nor a host tool: read
the ELF you already have (`firmware.elf`) over semihosting and parse `.symtab`.
This is the same fallback implemented by `SCL-TESTS-shared`'s `dl-impl.cpp`.

Provide a `loader::byte_source` over your semihosting calls:

```cpp
class semihost_source final : public loader::byte_source {
public:
    semihost_source(int fd, std::size_t size) : fd_(fd), size_(size) {}
    std::size_t size() const noexcept override { return size_; }

    loader::result<void> read_at(std::size_t off,
                                 std::span<std::byte> out) noexcept override {
        if (semihost_seek(fd_, off) != 0) return loader::unexpected(loader::error::SHORT_READ);
        while (!out.empty()) {
            int n = semihost_read(fd_, out.data(), out.size());
            if (n <= 0) return loader::unexpected(loader::error::SHORT_READ);
            out = out.subspan(static_cast<std::size_t>(n));
        }
        return {};
    }
private:
    int fd_; std::size_t size_;
};
```

Then build the table with the buffered collector:

```cpp
int fd = semihost_open(FIRMWARE_ELF_PATH, "rb");   // your SYS_OPEN wrapper
semihost_source src(fd, semihost_file_size(fd));

auto img = loader::elf_image::parse(src);

static scl::symtab::entry tab[2048];
static std::byte symbuf[64 * 1024];   // >= .symtab size
static char      strbuf[32 * 1024];   // >= .strtab size

const std::size_t n = scl::symtab::collect_buffered(
    *img, tab, 2048, symbuf, sizeof(symbuf), strbuf, sizeof(strbuf));

ctx.set_symtab(reinterpret_cast<const firmware::sym_entry*>(tab), n);
// XIP newlib: xip::newlib::set_symtab(tab, n);
semihost_close(fd);
```

`collect_buffered()` reads `.symtab` and `.strtab` with **one read per section**,
so it does not make a semihosting round-trip per symbol name. Keep `strbuf`
alive: `tab[i].name` points into it.

Requirements and caveats:

- The host must be able to open the ELF path (typically `firmware.elf` next to
  the `.hex`).
- The ELF must not be stripped of `.symtab`/`.strtab`; `-O binary`/`.hex`
  conversion does not keep them, but semihosting reads the `.elf`, not the image.
- Keep the `-Wl,-u` anchors so borrowed symbols survive `--gc-sections` and are
  present in `.symtab`.
- Semihosting is a development/debug path (debugger attached, slow). For a
  standalone product use a sidecar, registration, or PIE `.dynsym`.
- `link_context::resolve()` is a linear scan; fine for load-time imports, not
  for per-call `dlsym`.

## 2. Compile-time registration (no ELF, no parser)Annotate the exports you need:

```c
#include "scl/symtab-register.h"

void my_export(int x) { /* ... */ }
int  my_value = 7;

SCL_REGISTER_SYM(my_export);
SCL_REGISTER_SYM(my_value);
```

Include `cmake/ldscripts/scl-syms.ld` in the application's `SECTIONS` block.
Then:

```cpp
const scl_sym_entry* e = nullptr;
const std::size_t n = scl::symtab::registered(&e);
fw::dl_set_symtab(e, n);                  // or set_symtab/add_entries
```

This covers only annotated symbols. C++ overloads, and libc/C++ runtime symbols,
need explicit wrappers or the ELF-based path above.

## 3. SCLS blobs and views

`build_blob()` produces the same format `gen-symtab` emits:

```
magic 'SCLS' | u32 count | { u32 name_ptr, u32 addr }[count] | names
```

- Pass `runtime_addr` = the address where the blob will live, so `name_ptr` is a
  valid absolute pointer. Pass `0` to store blob-relative offsets (host tests,
  relocatable copies).
- `scl::symtab::view` validates and looks up names; `scl::symtab::source(view)`
  adapts it to a `loader::symbol_source` for `scl::reloc`/`scl::optimal`.
- On a cached core, clean the D-cache after writing the blob and invalidate the
  I-cache before executing from it (`scl_cache_*`, below).

## 4. Choosing a path

| Situation | Use |
|---|---|
| Hosted dynamic loader, non-PIE main, no post-link step | Sidecar ELF + `collect()` + `dl_set_symtab`/`set_symtab` |
| A small, fixed export surface | `SCL_REGISTER_SYM` + `registered()` |
| A platform already consumes the binary blob | `gen-symtab` (unchanged) or `build_blob()` |
| `scl::reloc` / `scl::optimal` resolver | Build a `view` and pass `scl::symtab::source(view)` |
| `scl::static` | `symbol_table::add_entries(entries, n)` |
| You can relink as `-pie` | Prefer `.dynsym` + `link_context::from_self()`; no table needed |

## 5. Failure modes to guard

- Missing/empty `.symtab` → `collect()` returns 0; fail loudly or fall back.
- Stale sidecar → wrong addresses; verify a build ID.
- Name longer than 255 bytes → skipped by design.
- Buffer too small → entries/names are dropped; check the returned count against
  the expected export count.
- Duplicate names → first wins.
- Table lifetime → names must outlive every loaded module.
