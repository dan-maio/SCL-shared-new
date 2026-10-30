/**
 * @file adapter.hpp
 * @brief XIP adapter for the newlib (hosted) dlopen loader.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include "xip/xip-handle.hpp"
#include "dynamic-newlib/link-context.hpp"
#include "scl/symtab-register.h"

namespace xip::newlib {

using xip_map = xip::xip_map;

/// Initialize the firmware symbols table for dynamic lookup.
void set_symtab(const firmware::sym_entry* entries, std::size_t count) noexcept;

/// Same, for tables produced by scl::symtab or SCL_REGISTER_SYM (layout-identical).
void set_symtab(const scl_sym_entry* entries, std::size_t count) noexcept;

/// Check after loading the whole set: the name of the first import that neither a loaded
/// module nor main provides, or nullptr. Such an import only faults when it is called.
[[nodiscard]] const char* first_missing_import() noexcept;

/// Load `so_image` (flash-resident bytes). `name` seeds the handle's short name.
/// Returns an opaque handle, or nullptr on failure.
void* dlopen(std::span<const std::byte> so_image, const char* name) noexcept;

/// Resolve `name` in `handle` to its runtime address (flash for code, RAM for
/// data). Returns nullptr if not found.
void* dlsym(void* handle, const char* name) noexcept;

/// The r10 / GOT base to pass to pic_call_* when entering this module.
std::uint32_t get_got_base(void* handle) noexcept;

/// True if the module executes in place from flash (false => copy-whole).
bool is_xip(void* handle) noexcept;

/// Fill `out` with `handle`'s segment placement. Returns false if the handle is
/// invalid (or the module is not loaded).
bool get_xip_map(void* handle, xip_map& out) noexcept;

/// Release the handle (frees the RAM segment for XIP modules).
void dlclose(void* handle) noexcept;

/// Pre-resolve external calls for XIP.
/// Pass @p flash_base (where the image will be programmed) for modules that use C++
/// exceptions: it also resolves the data segment's flash copy, which catch tables read.
int patch_main_imports(std::span<std::byte> image, std::uintptr_t flash_base = 0) noexcept;

}  // namespace xip::newlib

