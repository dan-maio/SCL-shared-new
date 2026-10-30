/**
 * @file symtab-runtime.hpp
 * @brief Build and consume a runtime symbol table at boot (no host tool).
 *
 * Two paths are provided over the existing common-elf-reader:
 *
 *   1. collect()/build_blob() read .symtab/.strtab from an ELF that is present
 *      at runtime (a sidecar image in flash, an external blob, etc.) and build
 *      either an entry array or the same `SCLS` blob format gen-symtab emits.
 *   2. registered() exposes the compile-time SCL_REGISTER_SYM() section, which
 *      needs neither an ELF nor a parser.
 *
 * The resulting entries are layout-compatible with firmware::sym_entry, so they
 * can be handed to the hosted link_context::set_symtab(); nolib has its own
 * dl_set_symtab().
 */
#pragma once

#include "loader/elf-image.hpp"
#include "loader/reloc-applier.hpp"   // loader::symbol_source
#include "scl/symtab-register.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace scl::symtab {

inline constexpr std::uint32_t kMagic = 0x534C4353u;  ///< 'SCLS'
inline constexpr std::size_t kMaxNameLen = 255;       ///< Longer names are skipped.

using entry = scl_sym_entry;

struct filter {
    bool include_weak      = true;   ///< Keep STB_WEAK in addition to STB_GLOBAL.
    bool runtime_whitelist = true;   ///< Keep _-prefixed C++/libc runtime symbols.
};

/// True for the C++ ABI / libc runtime prefixes gen-symtab keeps.
bool whitelisted(std::string_view name) noexcept;

namespace detail {
using visit_fn = void (*)(void* ctx, std::string_view name,
                          std::uint32_t addr, bool is_weak);
std::size_t walk(const loader::elf_image& img, const filter& f,
                 visit_fn fn, void* ctx) noexcept;
} // namespace detail

/// Visit every defined GLOBAL (and optionally WEAK) symbol. `name` is valid
/// only for the duration of the callback.
template <class Fn>
void for_each(const loader::elf_image& img, Fn&& fn,
              const filter& f = filter{}) noexcept {
    struct ctx { Fn* fn; };
    ctx c{&fn};
    detail::walk(img, f,
        [](void* p, std::string_view n, std::uint32_t a, bool) {
            (*static_cast<ctx*>(p)->fn)(n, a);
        }, &c);
}

/// Copy filtered symbols into caller storage. `out[i].name` points into
/// `names`; no allocation. Duplicate names are dropped (first wins).
/// Returns the number of entries written.
std::size_t collect(const loader::elf_image& img,
                    entry* out, std::size_t cap,
                    char* names, std::size_t names_cap,
                    const filter& f = filter{}) noexcept;

/// Buffered variant: reads .symtab and .strtab into caller-provided buffers
/// with one read per section. Suitable for slow sources such as semihosting.
/// `out[i].name` points into `strtab_buf`, which must stay alive as long as the
/// table is used. Returns the number of entries written.
std::size_t collect_buffered(const loader::elf_image& img,
                             entry* out, std::size_t cap,
                             std::byte* symtab_buf, std::size_t symtab_cap,
                             char* strtab_buf, std::size_t strtab_cap,
                             const filter& f = filter{}) noexcept;

/// Serialize entries into the `SCLS` blob format:
///   magic | count | { name_ptr, addr }[count] | NUL-terminated names
/// `runtime_addr` is where `out` will live (used for name_ptr). Pass 0 to
/// store blob-relative offsets (handy for host tests and relocatable copies).
/// Returns bytes written, 0 on error/insufficient space.
std::size_t build_blob(const entry* entries, std::size_t count,
                       std::byte* out, std::size_t cap,
                       std::uint32_t runtime_addr = 0) noexcept;

/// Read-only view over an `SCLS` blob, compatible with gen-symtab output.
class view {
public:
    view() noexcept = default;
    view(const std::byte* blob, std::size_t size,
         std::uint32_t blob_addr = 0) noexcept;

    bool valid() const noexcept { return count_ != 0; }
    std::size_t count() const noexcept { return count_; }

    /// Absolute address for @p name, or 0 if absent.
    std::uint32_t lookup(std::string_view name) const noexcept;

private:
    const std::byte* blob_ = nullptr;
    std::size_t      size_ = 0;
    std::uint32_t    addr_ = 0;
    std::size_t      count_ = 0;
};

/// Access the compile-time SCL_REGISTER_SYM() table. Returns 0 if none.
std::size_t registered(const entry** out) noexcept;

/// Resolver adapters for loaders that accept a loader::symbol_source.
loader::symbol_source source(const view& v) noexcept;
loader::symbol_source source_registered() noexcept;

} // namespace scl::symtab
