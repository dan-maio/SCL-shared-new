/**
 * @file reloc-loader.cpp
 * @brief scl::reloc — ET_REL (relocatable) module loader implementation.
 * @see reloc/reloc-loader.hpp
 */
#include "reloc/reloc-loader.hpp"
#include "loader/arm-reloc.hpp"
#include "loader/elf-types.hpp"
#include "loader/detail/bytes.hpp"     // load_word / name_at
#include "scl/os-hooks.h"
#include "scl/log.h"
#include "scl/unwind-exidx.h"
#include "scl/cache.h"

#include <cstring>

namespace reloc {
namespace {

using loader::error;

struct loader_guard {
    loader_guard() noexcept  { scl_loader_lock(); }
    ~loader_guard() noexcept { scl_loader_unlock(); }
};
using loader::detail::load_word;
using loader::detail::name_at;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;

// ---- ELF constants we need (ET_REL side; the rest live in elf-types.hpp) ----
constexpr uint16_t ET_REL   = 1;
constexpr uint16_t EM_ARM   = 40;
constexpr uint32_t SHT_PROGBITS   = 1;
constexpr uint32_t SHT_SYMTAB     = 2;
constexpr uint32_t SHT_NOBITS     = 8;
constexpr uint32_t SHT_REL        = 9;
constexpr uint32_t SHT_INIT_ARRAY = 14;
constexpr uint32_t SHF_ALLOC      = 2;
constexpr uint16_t SHN_UNDEF = 0;
constexpr uint16_t SHN_ABS   = 0xfff1;

// Elf32 struct field offsets.
constexpr uint32_t EH_TYPE = 16, EH_MACHINE = 18, EH_SHOFF = 32,
                   EH_SHENTSIZE = 46, EH_SHNUM = 48;
constexpr uint32_t SH_NAME = 0, SH_TYPE = 4, SH_FLAGS = 8, SH_OFFSET = 16,
                   SH_SIZE = 20, SH_LINK = 24, SH_INFO = 28, SH_ADDRALIGN = 32,
                   SH_ENTSIZE = 36;
constexpr uint32_t ST_NAME = 0, ST_VALUE = 4, ST_INFO = 12, ST_SHNDX = 14;
constexpr uint32_t REL_OFFSET = 0, REL_INFO = 4;
constexpr uint32_t SYMSZ = 16, RELSZ = 8;

inline uint16_t load16(std::span<const std::byte> s, uint32_t off) noexcept {
    if (off + 2u > s.size()) return 0;
    return static_cast<uint16_t>(std::to_integer<uint8_t>(s[off]) |
                                 (std::to_integer<uint8_t>(s[off + 1]) << 8));
}
inline uint8_t load8(std::span<const std::byte> s, uint32_t off) noexcept {
    return off < s.size() ? std::to_integer<uint8_t>(s[off]) : 0;
}
inline uint32_t align_up(uint32_t v, uint32_t a) noexcept {
    return (a <= 1) ? v : (v + a - 1u) & ~(a - 1u);
}

/// Read one Shdr word field for section @p i.
struct sh_reader {
    std::span<const std::byte> img;
    uint32_t shoff; uint16_t shentsize;
    uint32_t f(uint16_t i, uint32_t field) const {
        return load_word(img, shoff + uint32_t(i) * shentsize + field);
    }
};

// ---- symbol resolution ------------------------------------------------------
// Returns the runtime address of symbol @p si.  Defined symbols keep their own
// ARM/Thumb bit: ET_REL `st_value` is odd for Thumb and even for ARM.  External
// (UND) symbols come from the resolver, which already returns the correct bit.
struct sym_env {
    const module& m;
    std::span<const std::byte> img;
    uint32_t symtab_off, symtab_sz;
    std::span<const std::byte> strtab;
    loader::symbol_source resolve;

    bool ok = true;

    uint32_t addr_of(uint32_t si) {
        const uint32_t so = symtab_off + si * SYMSZ;
        if (so + SYMSZ > symtab_off + symtab_sz) { ok = false; return 0; }
        const uint16_t shndx = load16(img, so + ST_SHNDX);
        const uint32_t value = load_word(img, so + ST_VALUE);
        if (shndx == SHN_UNDEF) {                     // external -> resolver
            auto nm = name_at(strtab, load_word(img, so + ST_NAME));
            auto r = resolve(nm);
            if (!r) {
                scl_log_puts("[scl] unresolved import: ");
                for (char c : nm) scl_log_putc(c);
                scl_log_putc('\n');
                ok = false;
                return 0;
            }
            return *r;                                // already Thumb-correct
        }
        if (shndx == SHN_ABS) return value;           // absolute (bit 0 preserved)
        if (shndx >= m.shnum) { ok = false; return 0; }
        // `st_value` already carries the ARM/Thumb bit: GCC writes an odd
        // value for a Thumb function and an even value for an ARM function.
        // Do not OR bit 0 here — that turns every ARM function into Thumb.
        return m.sec_base[shndx] + value;
    }
};

// ---- phase helpers (shared by load and load_set) ----------------------------

// Place @p image's SHF_ALLOC sections into @p arena starting at *used (packed,
// so load_set can pack several modules); fill @p m and mark it placed. Does NOT
// apply relocations or run constructors.
error place_module(std::span<const std::byte> image, std::span<std::byte> arena,
                   uint32_t& used, module& m) noexcept {
    if (image.size() < 52) return error::NOT_ELF;
    if (!(load8(image, 0) == 0x7f && load8(image, 1) == 'E' &&
          load8(image, 2) == 'L' && load8(image, 3) == 'F')) return error::NOT_ELF;
    if (load16(image, EH_MACHINE) != EM_ARM) return error::NOT_ARM32;
    if (load16(image, EH_TYPE)    != ET_REL) return error::BAD_SECTION;

    m = module{};
    m.image = image; m.arena = arena;
    m.shoff = load_word(image, EH_SHOFF);
    m.shentsize = load16(image, EH_SHENTSIZE);
    m.shnum = load16(image, EH_SHNUM);
    if (m.shnum == 0 || m.shnum > MAX_SECTIONS) return error::BAD_SECTION;
    sh_reader sh{image, m.shoff, m.shentsize};
    const uint32_t abase = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(arena.data()));
    for (uint16_t i = 0; i < m.shnum; ++i) {
        if (sh.f(i, SH_TYPE) == SHT_SYMTAB) m.symtab_idx = i;
        if (!(sh.f(i, SH_FLAGS) & SHF_ALLOC)) continue;
        const uint32_t sz = sh.f(i, SH_SIZE), align = sh.f(i, SH_ADDRALIGN);
        used = align_up(used, align ? align : 4);
        if (used + sz > arena.size()) return error::OUT_OF_MEMORY;
        m.sec_base[i] = abase + used;
        if (sh.f(i, SH_TYPE) == SHT_NOBITS) {
            std::memset(arena.data() + used, 0, sz);              // .bss
        } else {
            const uint32_t off = sh.f(i, SH_OFFSET);
            if (off + sz > image.size()) return error::BAD_SECTION;
            std::memcpy(arena.data() + used, image.data() + off, sz);
        }
        m.used += sz;
        used   += sz;
    }
    if (m.symtab_idx == 0) return error::BAD_SECTION;
    m.valid = true;   // "placed" — sym() works now (needed for cross-module resolve)
    return error::OK;
}

// Apply @p m's relocations; UND symbols resolved through @p und.
error apply_relocs(module& m, loader::symbol_source und) noexcept {
    sh_reader sh{m.image, m.shoff, m.shentsize};
    const uint32_t symtab_off = sh.f(m.symtab_idx, SH_OFFSET);
    const uint32_t symtab_sz  = sh.f(m.symtab_idx, SH_SIZE);
    const uint16_t strtab_idx = static_cast<uint16_t>(sh.f(m.symtab_idx, SH_LINK));
    auto strtab = m.image.subspan(sh.f(strtab_idx, SH_OFFSET), sh.f(strtab_idx, SH_SIZE));
    sym_env env{m, m.image, symtab_off, symtab_sz, strtab, und};

    for (uint16_t i = 0; i < m.shnum; ++i) {
        if (sh.f(i, SH_TYPE) != SHT_REL) continue;
        const uint16_t tgt = static_cast<uint16_t>(sh.f(i, SH_INFO));
        if (tgt >= m.shnum || m.sec_base[tgt] == 0) continue;   // reloc for a non-loaded section
        const uint32_t tbase = m.sec_base[tgt], roff = sh.f(i, SH_OFFSET), rsz = sh.f(i, SH_SIZE);
        for (uint32_t o = 0; o + RELSZ <= rsz; o += RELSZ) {
            const uint32_t r_offset = load_word(m.image, roff + o + REL_OFFSET);
            const uint32_t r_info   = load_word(m.image, roff + o + REL_INFO);
            const uint8_t  type     = static_cast<uint8_t>(r_info & 0xffu);
            const uint32_t P        = tbase + r_offset;
            std::byte*     pp = reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(P));
            const uint32_t S  = env.addr_of(r_info >> 8);
            if (!env.ok) return error::SYMBOL_NOT_FOUND;

            std::uint32_t A = 0;
            if (!loader::arm::decode_addend(type, pp, A))
                return error::UNSUPPORTED_RELOC;
            const loader::arm::isa site =
                loader::arm::is_thumb_branch(type) ? loader::arm::isa::thumb
                                                   : loader::arm::isa::arm;
            const loader::arm::context ctx{S, A, P, site};
            if (auto e = loader::arm::apply(type, pp, ctx); e != error::OK)
                return e;
        }
    }
    return error::OK;
}

constexpr uint32_t SHT_ARM_EXIDX = 0x70000001u;

inline uint32_t prel31_target(uint32_t where, uint32_t w) noexcept {
    const uint32_t a = (w & 0x40000000u) ? (w | 0x80000000u) : (w & 0x7fffffffu);
    return where + a;
}

// A partial link leaves one .ARM.exidx section per function group, unsorted, but the
// unwinder binary-searches one table. Build that table at the end of the arena and
// register it for the whole arena. Entries hold PREL31 values, so they are recomputed.
error register_unwind(const module* mods, int n, std::span<std::byte> arena, uint32_t& used) noexcept {
    struct entry { uint32_t fn, tab; };   // tab: an inline word, or the extab's address
    used = align_up(used, 8);
    auto* table = reinterpret_cast<entry*>(arena.data() + used);
    const uint32_t cap = static_cast<uint32_t>((arena.size() - used) / sizeof(entry));
    uint32_t count = 0;
    for (int k = 0; k < n; ++k) {
        const module& m = mods[k];
        sh_reader sh{m.image, m.shoff, m.shentsize};
        for (uint16_t i = 0; i < m.shnum; ++i) {
            if (sh.f(i, SH_TYPE) != SHT_ARM_EXIDX || m.sec_base[i] == 0) continue;
            const uint32_t base = m.sec_base[i], sz = sh.f(i, SH_SIZE);
            for (uint32_t o = 0; o + 8 <= sz; o += 8) {
                if (count >= cap) return error::OUT_OF_MEMORY;
                const auto* w = reinterpret_cast<const uint32_t*>(static_cast<std::uintptr_t>(base + o));
                entry e{prel31_target(base + o, w[0]), w[1]};
                if (e.tab != 1u && !(e.tab & 0x80000000u)) e.tab = prel31_target(base + o + 4, e.tab);
                uint32_t j = count++;
                for (; j > 0 && table[j - 1].fn > e.fn; --j) table[j] = table[j - 1];
                table[j] = e;
            }
        }
    }
    const uint32_t tbase = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(table));
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = tbase + i * 8u;
        const uint32_t tab = table[i].tab;
        const uint32_t fn = (table[i].fn - at) & 0x7fffffffu;
        table[i].fn = fn;
        if (tab != 1u && !(tab & 0x80000000u)) table[i].tab = (tab - (at + 4u)) & 0x7fffffffu;
    }
    used += count * sizeof(entry);
    const uint32_t abase = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(arena.data()));
    dl_exidx_unregister(abase);
    if (count) dl_exidx_register(abase, used, tbase, static_cast<int>(count));
    return error::OK;
}

// Run @p m's .init_array constructors (now ABS32-relocated to runtime addresses).
void run_ctors(module& m) noexcept {
    sh_reader sh{m.image, m.shoff, m.shentsize};
    for (uint16_t i = 0; i < m.shnum; ++i) {
        if (sh.f(i, SH_TYPE) != SHT_INIT_ARRAY || m.sec_base[i] == 0) continue;
        const uint32_t base = m.sec_base[i], sz = sh.f(i, SH_SIZE);
        for (uint32_t o = 0; o + 4 <= sz; o += 4) {
            uint32_t fp = *reinterpret_cast<const uint32_t*>(static_cast<std::uintptr_t>(base + o));
            // The relocation stored the constructor's own ARM/Thumb bit.
            if (fp) reinterpret_cast<void (*)()>(static_cast<std::uintptr_t>(fp))();
        }
    }
}

}  // namespace

std::uint32_t sym(const module& m, std::string_view name) noexcept;   // fwd (below)

namespace {
// Cross-module resolver for load_set: each placed module's exports first, then
// the external resolver (main).
struct set_ctx { module* mods; int n; loader::symbol_source ext; };
loader::result<std::uint32_t> set_resolve(void* c, std::string_view nm) noexcept {
    auto* s = static_cast<set_ctx*>(c);
    for (int i = 0; i < s->n; ++i)
        if (std::uint32_t a = sym(s->mods[i], nm)) return a;
    return s->ext(nm);
}
}  // namespace

loader::result<module> load(std::span<const std::byte> image, std::span<std::byte> arena,
                            loader::symbol_source resolve) noexcept {
    loader_guard guard;
    module m{};
    uint32_t used = 0;
    if (auto e = place_module(image, arena, used, m); e != error::OK) return loader::unexpected(e);
    if (auto e = apply_relocs(m, resolve);            e != error::OK) return loader::unexpected(e);
    if (auto e = register_unwind(&m, 1, arena, used); e != error::OK) return loader::unexpected(e);
    scl_cache_clean_invalidate(arena.data(), used);
    run_ctors(m);
    return m;
}

loader::result<module_set> load_set(std::span<const std::span<const std::byte>> images,
                                    std::span<std::byte> arena,
                                    loader::symbol_source resolve) noexcept {
    loader_guard guard;
    if (images.size() > MAX_SET) return loader::unexpected(error::OUT_OF_MEMORY);
    module_set set{};
    uint32_t used = 0;
    for (auto img : images) {                              // phase 1: place all
        if (auto e = place_module(img, arena, used, set.mods[set.n]); e != error::OK)
            return loader::unexpected(e);
        ++set.n;
    }
    set_ctx sc{ set.mods, set.n, resolve };                // phase 2: resolve all
    loader::symbol_source ss{ set_resolve, &sc };
    for (int i = 0; i < set.n; ++i)
        if (auto e = apply_relocs(set.mods[i], ss); e != error::OK) return loader::unexpected(e);
    if (auto e = register_unwind(set.mods, set.n, arena, used); e != error::OK) return loader::unexpected(e);
    scl_cache_clean_invalidate(arena.data(), used);
    for (int i = 0; i < set.n; ++i) run_ctors(set.mods[i]); // phase 3: run all ctors
    return set;
}

std::uint32_t sym(const module& m, std::string_view name) noexcept {
    if (!m.valid || m.symtab_idx == 0) return 0;
    sh_reader sh{m.image, m.shoff, m.shentsize};
    const uint32_t symtab_off = sh.f(m.symtab_idx, SH_OFFSET);
    const uint32_t symtab_sz  = sh.f(m.symtab_idx, SH_SIZE);
    const uint16_t strtab_idx = static_cast<uint16_t>(sh.f(m.symtab_idx, SH_LINK));
    auto strtab = m.image.subspan(sh.f(strtab_idx, SH_OFFSET), sh.f(strtab_idx, SH_SIZE));
    for (uint32_t o = 0; o + SYMSZ <= symtab_sz; o += SYMSZ) {
        const uint32_t so = symtab_off + o;
        const uint16_t shndx = load16(m.image, so + ST_SHNDX);
        if (shndx == SHN_UNDEF || shndx >= m.shnum || m.sec_base[shndx] == 0) continue;
        if (name_at(strtab, load_word(m.image, so + ST_NAME)) != name) continue;
        // Preserve the symbol's own ARM/Thumb bit; do not force Thumb.
        return m.sec_base[shndx] + load_word(m.image, so + ST_VALUE);
    }
    return 0;
}

std::uint32_t sym(const module_set& s, std::string_view name) noexcept {
    for (int i = 0; i < s.n; ++i)
        if (std::uint32_t a = sym(s.mods[i], name)) return a;
    return 0;
}

}  // namespace reloc
