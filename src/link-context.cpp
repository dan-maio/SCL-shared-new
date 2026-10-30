/** @file link-context.cpp @brief See link-context.hpp. */
#include "scl/link-context.hpp"
#include "loader/dynamic-info.hpp"
#include <cstring>

/// Linker-provided in a -pie image. Weak so non-PIE firmware using an external
/// table still links; from_self() then reports an error.
extern "C" { extern loader::elf::Dyn _DYNAMIC[] __attribute__((weak)); }

namespace fw {

loader::result<link_context> link_context::from_self() noexcept {
    if (!_DYNAMIC) return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
    // _DYNAMIC is an array terminated by DT_NULL; bound it generously for parse().
    std::span<const std::byte> dyn_bytes{
        reinterpret_cast<const std::byte*>(_DYNAMIC), sizeof(loader::elf::Dyn) * 64};
    auto info = loader::dynamic_info::parse(dyn_bytes);
    if (!info) return loader::unexpected(info.err());

    const auto* hash_words = reinterpret_cast<const std::uint32_t*>(info->hash);
    const std::uint32_t nbucket = hash_words[0];
    const std::uint32_t nchain  = hash_words[1];          // == symbol count
    std::span<const std::byte> hash{
        reinterpret_cast<const std::byte*>(info->hash), (2 + nbucket + nchain) * 4};
    std::span<const std::byte> dynsym{
        reinterpret_cast<const std::byte*>(info->symtab), nchain * sizeof(loader::elf::Sym)};
    std::span<const std::byte> dynstr{
        reinterpret_cast<const std::byte*>(info->strtab), info->strsz};
    return link_context(dynsym, dynstr, hash);
}

loader::result<std::uint32_t> link_context::resolve(std::string_view name) const noexcept {
    if (symtab_) {
        for (std::size_t i = 0; i < symtab_count_; ++i) {
            const char* n = symtab_[i].name;
            if (n && std::strlen(n) == name.size() &&
                std::memcmp(n, name.data(), name.size()) == 0)
                return symtab_[i].addr;
        }
    }
    return resolver_.address_of(name);
}

loader::symbol_source link_context::source() const noexcept {
    // Function-pointer thunk + this-context (no std::function on the target). In a
    // running image, st_value is already the absolute address.
    return loader::symbol_source{
        [](void* ctx, std::string_view name) -> loader::result<std::uint32_t> {
            return static_cast<const link_context*>(ctx)->resolve(name);
        },
        const_cast<link_context*>(this)};
}

} // namespace fw
