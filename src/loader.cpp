/**
 * @file loader.cpp
 * @brief Implementation of optimal::load_and_link — the whole loader flow.
 *
 * SCL code only: it drives xip::nopic (load / resolve_all / patch_main) and knows
 * nothing about the firmware, the fixtures, or where the blobs came from.
 */
#include "optimal/loader.hpp"
#include "scl/os-hooks.h"

namespace optimal {

namespace {
struct loader_guard {
    loader_guard() noexcept  { scl_loader_lock(); }
    ~loader_guard() noexcept { scl_loader_unlock(); }
};
} // namespace

int load_and_link(const std::span<const std::byte>* blobs, int n, Module* out) noexcept {
    loader_guard guard;
    int loaded = 0;
    for (int i = 0; i < n; ++i)
        if (out[i].load(blobs[i])) ++loaded;

    xip::nopic::resolve_all();                       // cyclic module<->module imports

    for (int i = 0; i < n; ++i)
        if (out[i].ok())
            xip::nopic::patch_main(out[i].get());     // fill MAIN's GOT -> by-name calls

    return loaded;
}

}  // namespace optimal
