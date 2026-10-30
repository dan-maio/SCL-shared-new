/**
 * @file missing-import.hpp
 * @brief Find an import nothing provides, once a set of modules is loaded.
 *
 * Loaders that allow forward references cannot reject the first module: a later one may
 * provide the symbol. An import left unresolved only faults when it is called, so check
 * after the whole set is loaded.
 */
#pragma once
#include "loader/detail/bytes.hpp"
#include "loader/elf-types.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace loader {

/// First non-weak undefined symbol used by @p rel that @p provided rejects, or nullptr.
/// The name points into @p dynstr. @p provided takes a NUL-terminated std::string_view.
template <class Provided>
const char* find_missing_import(std::span<const std::byte> rel,
                                std::span<const std::byte> dynsym,
                                std::span<const std::byte> dynstr,
                                Provided&& provided) noexcept {
    using detail::load_word;
    constexpr std::size_t ES = sizeof(elf::Sym);
    constexpr std::uint32_t STB_WEAK_BIND = 2;
    const char* missing = nullptr;
    detail::for_each_rel(rel, [&](std::uint32_t, std::uint32_t r_info) {
        if (missing) return;
        const std::size_t so = std::size_t(elf::r_sym(r_info)) * ES;
        if (so == 0 || so + ES > dynsym.size()) return;
        if (load_word(dynsym, so + 4) != 0) return;                  // defined here
        const std::uint32_t st_info = load_word(dynsym, so + 12) & 0xFFu;
        if ((st_info >> 4) == STB_WEAK_BIND) return;
        const std::string_view name = detail::name_at(dynstr, load_word(dynsym, so + 0));
        if (!name.empty() && !provided(name)) missing = name.data();
    });
    return missing;
}

}  // namespace loader
