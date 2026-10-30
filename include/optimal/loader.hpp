/**
 * @file optimal/loader.hpp
 * @brief The reusable optimal-shared loader API (SCL glue over xip::nopic).
 *
 * This is the only public surface a consuming application needs. It is loader
 * ("scl") code — no test scaffolding, no firmware ABI, no fixture knowledge:
 *
 *   - optimal::Module   RAII over one loaded no-pic module (fixed-address windows);
 *   - optimal::load_and_link()  loads a set of module blobs, resolves cyclic
 *                               module<->module imports, and patches MAIN's GOT so
 *                               the application can call every export BY NAME.
 *
 * The blobs may live anywhere the caller likes — a RAM buffer (pure-RAM use) or a
 * flash-resident image (XIP use); the loader treats them identically. That is why
 * the two test apps (test/test-ram, test/test-xip) share this code unchanged and
 * differ only in where they get each blob from.
 */
#pragma once
#include <cstddef>
#include <span>
#include "xip/static/nopic.hpp"

namespace optimal {

/// RAII wrapper over a loaded no-pic module.
class Module {
public:
    Module() = default;
    ~Module() { if (m_.ok) xip::nopic::unload(m_); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&& o) noexcept : m_(o.m_) { o.m_ = xip::nopic::PModule{}; }
    Module& operator=(Module&& o) noexcept {
        if (m_.ok) xip::nopic::unload(m_);
        m_ = o.m_; o.m_ = xip::nopic::PModule{}; return *this;
    }

    /// Load one module image (segments copied into the reserved SRAM windows).
    bool load(std::span<const std::byte> blob) noexcept { return xip::nopic::load(blob, m_); }

    /// Absolute runtime address of an exported symbol (0 if absent).
    void* sym(const char* name) const noexcept { return xip::nopic::resolve(m_, name); }

    bool ok() const noexcept { return m_.ok; }
    const xip::nopic::PModule& get() const noexcept { return m_; }

private:
    xip::nopic::PModule m_{};
};

/**
 * @brief Load @p n module blobs, link them, and enable by-name calls from main.
 *
 * Steps (the whole loader flow in one call):
 *   1. load each blob into its fixed windows (ctors run);
 *   2. resolve_all() — fill still-unresolved imports so CYCLIC module groups work;
 *   3. patch_main() for every module — point main's own GOT/PLT slots at the
 *      modules' exports, so `name(args)` in the app is a direct call.
 *
 * @param blobs  array of @p n module images (any memory: RAM or flash).
 * @param out    array of @p n Modules to populate (kept alive by the caller).
 * @return number of modules that loaded successfully.
 */
int load_and_link(const std::span<const std::byte>* blobs, int n, Module* out) noexcept;

}  // namespace optimal
