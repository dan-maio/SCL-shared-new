#pragma once
#include "xip/xip-handle.hpp"
#include "scl/code-addr.h"
#include "xip/segment-map.hpp"
#include "loader/elf-image.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/symbol-resolver.hpp"
#include "loader/detail/bytes.hpp"
#include "loader/reloc-applier.hpp"
#include <cstring>

namespace xip::detail {

template <typename handle_t>
inline void* sym_addr(const handle_t* h, std::uint32_t st) noexcept {
    const bool in_d = st >= h->data_vaddr && st < h->data_vaddr + h->data_memsz;
    const bool in_t = st >= h->text_vaddr && st < h->text_vaddr + h->text_size && !in_d;
    return reinterpret_cast<void*>(in_t ? h->text_base + (st - h->text_vaddr)
                                        : h->data_base + (st - h->data_vaddr));
}

template <typename handle_t>
inline void* dlsym_impl(void* handle, const char* name) noexcept {
    auto* h = static_cast<handle_t*>(handle);
    if (!h || !h->in_use) return nullptr;
    auto v = h->resolver.address_of(name);
    return (v && *v != 0) ? sym_addr(h, *v) : nullptr;
}

template <typename handle_t>
inline std::uint32_t get_got_base_impl(void* handle) noexcept {
    auto* h = static_cast<handle_t*>(handle);
    return (h && h->in_use) ? static_cast<std::uint32_t>(h->got_base) : 0;
}

template <typename handle_t>
inline bool is_xip_impl(void* handle) noexcept {
    auto* h = static_cast<handle_t*>(handle);
    return h && h->in_use && h->xip;
}

template <typename handle_t, typename map_t>
inline bool get_xip_map_impl(void* handle, map_t& out) noexcept {
    auto* h = static_cast<handle_t*>(handle);
    if (!h || !h->in_use) return false;
    out.is_xip     = h->xip;
    out.text_base  = h->text_base;
    out.text_size  = h->text_size;
    out.data_base  = h->data_base;
    out.got_base   = h->got_base;
    out.data_memsz = h->data_memsz;
    return true;
}

/// Set the Thumb bit on each .ARM.extab personality pointer in a writable image. The
/// personality is imported, so the linker points it at a PLT veneer without that bit,
/// and on Cortex-M the unwinder's branch to it faults. Flash cannot be patched at load
/// (dl.cpp does this in RAM), so do it at provision time. Requires VA == file offset.
inline void set_extab_thumb_bits(std::span<std::byte> image, const loader::elf_image& img) noexcept {
    using loader::detail::load_word;
    using loader::detail::store_word;
    auto ex = img.find_section(".ARM.exidx");
    if (!ex || ex->sh_size == 0) return;
    auto sx31 = [](std::uint32_t v) -> std::int32_t {
        return (v & 0x40000000u) ? static_cast<std::int32_t>(v | 0x80000000u)
                                 : static_cast<std::int32_t>(v & 0x7fffffffu);
    };
    for (std::uint32_t i = 0; i < ex->sh_size / 8u; ++i) {
        const std::uint32_t w1_off = ex->sh_offset + i * 8u + 4u;
        if (w1_off + 4u > image.size()) break;
        const std::uint32_t w1 = load_word(image, w1_off);
        if (w1 == 1u || (w1 & 0x80000000u)) continue;       // cantunwind or inline
        const std::int64_t tab = static_cast<std::int64_t>(w1_off) + sx31(w1);
        if (tab < 0 || tab + 4 > static_cast<std::int64_t>(image.size())) continue;
        const std::uint32_t w0 = load_word(image, static_cast<std::size_t>(tab));
        if (w0 & 0x80000000u) continue;                     // inline-compact extab
        if (!(w0 & 1u)) store_word(image, static_cast<std::size_t>(tab), scl_code_addr(w0));
    }
}

/// Resolve the data segment's flash copy, for an image that will live at @p flash_base.
/// A catch table in flash reaches its typeinfo PC-relatively, so it reads this copy,
/// which the loader otherwise never relocates: its vptr and name would be raw, and
/// matching a catch would jump through them. apply_rel_xip() recognises these words.
inline void prelink_flash_data(std::span<std::byte> image, const loader::elf_image& img,
                               std::uintptr_t flash_base, loader::symbol_source resolve) noexcept {
    using loader::detail::load_word;
    using loader::detail::store_word;
    using loader::detail::for_each_rel;
    using loader::detail::name_at;
    auto sec = [&](const char* nm) -> std::span<const std::byte> {
        auto sh = img.find_section(nm);
        if (!sh || sh->sh_size == 0) return {};
        return std::span<const std::byte>{ image.data() + sh->sh_offset, sh->sh_size };
    };
    auto dynsym = sec(".dynsym");
    auto dynstr = sec(".dynstr");
    const auto base = static_cast<std::uint32_t>(flash_base);
    for (const char* s : { ".rel.dyn", ".rel.plt" }) {
        for_each_rel(sec(s), [&](std::uint32_t r_off, std::uint32_t r_info) {
            if (r_off + 4 > image.size()) return;
            const auto type = static_cast<loader::elf::rel_type>(loader::elf::r_type(r_info));
            if (type == loader::elf::rel_type::RELATIVE) {
                store_word(image, r_off, load_word(image, r_off) + base);
            } else if (type == loader::elf::rel_type::ABS32 && !dynsym.empty()) {
                const std::size_t so = loader::elf::r_sym(r_info) * sizeof(loader::elf::Sym);
                const std::uint32_t st_value = load_word(dynsym, so + 4);
                std::uint32_t target = 0;
                if (st_value != 0) {
                    target = base + st_value;
                } else if (auto r = resolve(name_at(dynstr, load_word(dynsym, so + 0)))) {
                    target = *r;
                }
                if (target != 0) store_word(image, r_off, load_word(image, r_off) + target);
            }
        });
    }
}

inline int patch_main_imports_impl(std::span<std::byte> image,
                                    std::uintptr_t trampoline_addr,
                                    std::uintptr_t flash_base = 0,
                                    loader::symbol_source resolve = {}) noexcept {
    using loader::detail::load_word;
    using loader::detail::store_word;
    using loader::detail::for_each_rel;

    loader::memory_byte_source src(image);
    auto parsed = loader::elf_image::parse(src);
    if (!parsed) return -1;
    auto img = *parsed;
    set_extab_thumb_bits(image, img);
    if (flash_base != 0) prelink_flash_data(image, img, flash_base, resolve);

    auto sec = [&](const char* nm) -> std::span<const std::byte> {
        auto sh = img.find_section(nm);
        if (!sh || sh->sh_size == 0) return {};
        return std::span<const std::byte>{ image.data() + sh->sh_offset, sh->sh_size };
    };
    auto dynsym = sec(".dynsym");
    if (dynsym.empty()) return 0;

    const std::uint32_t tramp = scl_code_addr(static_cast<std::uint32_t>(trampoline_addr));
    int n = 0;

    auto patcher = [&](std::uint32_t r_offset, std::uint32_t r_info) {
        if (loader::elf::r_type(r_info) !=
            static_cast<std::uint8_t>(loader::elf::rel_type::JUMP_SLOT)) return;
        const std::uint32_t sym = loader::elf::r_sym(r_info);
        const std::uint32_t st_value = load_word(dynsym, sym * sizeof(loader::elf::Sym) + 4);
        if (st_value != 0) return;
        if (r_offset + 4 > image.size()) return;
        store_word(image, r_offset, tramp);
        ++n;
    };

    for (const char* s : { ".rel.plt", ".rel.dyn" }) {
        auto rel = sec(s);
        for_each_rel(rel, patcher);
    }
    return n;
}

} // namespace xip::detail
