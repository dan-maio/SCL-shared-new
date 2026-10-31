/**
 * @file exidx-fixup.hpp
 * @brief Make a loaded module's .ARM.extab usable by the unwinder.
 */
#pragma once
#include "loader/detail/bytes.hpp"
#include "scl/code-addr.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace loader {

/// Set the Thumb bit on each .ARM.extab personality pointer. The personality is imported, so
/// the linker points it at a PLT veneer without that bit, and on Cortex-M the unwinder's
/// branch to it faults. @p exidx_off and every entry are offsets into @p image.
inline void set_extab_thumb_bits(std::span<std::byte> image, std::uint32_t exidx_off,
                                 std::uint32_t exidx_size) noexcept {
    using detail::load_word;
    using detail::store_word;
    auto sx31 = [](std::uint32_t v) -> std::int32_t {
        return (v & 0x40000000u) ? static_cast<std::int32_t>(v | 0x80000000u)
                                 : static_cast<std::int32_t>(v & 0x7fffffffu);
    };
    for (std::uint32_t i = 0; i < exidx_size / 8u; ++i) {
        const std::uint32_t w1_off = exidx_off + i * 8u + 4u;
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

}  // namespace loader
