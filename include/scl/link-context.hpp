/**
 * @file link-context.hpp
 * @brief main's own dynamic-symbol surface, used to resolve library imports.
 */
#pragma once
#include "loader/reloc-applier.hpp"   // symbol_source
#include "loader/symbol-resolver.hpp"
#include "scl/symtab-register.h"
#include <cstddef>
#include <string_view>

namespace fw {

class link_context {
public:
    link_context() noexcept = default;

    /** @brief Build from main's _DYNAMIC (parses DT_HASH/SYMTAB/STRTAB/STRSZ). */
    static loader::result<link_context> from_self() noexcept;

    /** @brief Supply a boot-time table (sidecar ELF or SCL_REGISTER_SYM section).
     *         The table must outlive this context and remain immutable. */
    void set_symtab(const scl_sym_entry* entries, std::size_t count) noexcept {
        symtab_ = entries;
        symtab_count_ = count;
    }

    /** @brief A resolver callback: extern name -> absolute address in main. */
    [[nodiscard]] loader::symbol_source source() const noexcept;

    /** @brief Resolve @p name against the external table, then .dynsym. A context
     *         not built by from_self() has only the table. */
    [[nodiscard]] loader::result<std::uint32_t> resolve(std::string_view name) const noexcept;

private:
    /** @brief Construct from main's .dynsym/.dynstr/.hash spans (used by from_self()). */
    link_context(std::span<const std::byte> dynsym,
                std::span<const std::byte> dynstr,
                std::span<const std::byte> hash) noexcept
        : resolver_(dynsym, dynstr, hash) {}
    loader::symbol_resolver resolver_;   ///< ELF-hash resolver over main's dynamic symbols.
    const scl_sym_entry* symtab_{nullptr};
    std::size_t symtab_count_{0};
};

} // namespace fw
