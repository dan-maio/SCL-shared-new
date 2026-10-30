/** @file symtab-runtime.cpp @brief Boot-time symbol-table builder and views. */
#include "scl/symtab-runtime.hpp"

#include "loader/elf-types.hpp"

#include <cstring>

namespace scl::symtab {
namespace {

bool is_whitelisted(std::string_view name) noexcept {
    static constexpr std::string_view wl[] = {
        "_impure_ptr", "_Znw", "_Zdl", "__cxa", "__errno",
        "__exidx", "__gnu_Unwind", "__register_frame_info",
        "__deregister_frame_info", "__gxx_personality", "__dynamic_cast",
        "_ZTVN10__cxxabiv", "__aeabi_unwind", "_ZSt7nothrow", "_ZTI",
        "__aeabi_"
    };
    for (auto w : wl)
        if (name.starts_with(w)) return true;
    return false;
}

/// Read a NUL-terminated name at @p off in a string table of @p limit bytes.
/// Rejects empty and over-long names. Returns false on any failure.
bool read_name(loader::byte_source& src, std::uint32_t off, std::uint32_t limit,
               char* dst, std::size_t cap) noexcept {
    if (off >= limit || cap < 2) return false;
    const std::size_t max = limit - off;
    std::size_t i = 0;
    for (; i < max && i < kMaxNameLen; ++i) {
        char c = 0;
        auto r = src.read_at(off + i,
                    {reinterpret_cast<std::byte*>(&c), sizeof(c)});
        if (!r) return false;
        if (c == '\0') {
            if (i == 0) return false;
            dst[i] = '\0';
            return true;
        }
        if (i + 1 >= cap) return false;
        dst[i] = c;
    }
    return false;   // no terminator, or name longer than kMaxNameLen
}

bool name_equals(const char* p, std::size_t max, std::string_view want) noexcept {
    if (want.size() >= max) return false;
    for (std::size_t i = 0; i < want.size(); ++i)
        if (p[i] != want[i]) return false;
    return p[want.size()] == '\0';
}

bool read_word(const std::byte* p, std::uint32_t& out) noexcept {
    std::memcpy(&out, p, sizeof(out));
    return true;
}

bool keep_symbol(const loader::elf::Sym& s, std::string_view name,
                 const filter& f) noexcept {
    if (s.st_value == 0) return false;
    if (s.st_shndx == 0 || s.st_shndx >= 0xFFF1) return false;
    const std::uint8_t bind = static_cast<std::uint8_t>(s.st_info >> 4);
    if (bind != 1 && !(bind == 2 && f.include_weak)) return false;
    if (name.empty()) return false;
    if (name[0] == '_' && !(f.runtime_whitelist && is_whitelisted(name)))
        return false;
    return true;
}

} // namespace

bool whitelisted(std::string_view name) noexcept { return is_whitelisted(name); }

std::size_t detail::walk(const loader::elf_image& img, const filter& f,
                         visit_fn fn, void* ctx) noexcept {
    auto sym_r = img.find_section(".symtab");
    auto str_r = img.find_section(".strtab");
    if (!sym_r || !str_r) return 0;

    const auto& sym = *sym_r;
    const auto& str = *str_r;
    const std::size_t count = sym.sh_size / sizeof(loader::elf::Sym);
    std::size_t start = sym.sh_info;
    if (start >= count) start = 0;

    char name[kMaxNameLen + 1];
    std::size_t seen = 0;
    for (std::size_t i = start; i < count; ++i) {
        loader::elf::Sym s{};
        auto r = img.source().read_at(
            sym.sh_offset + i * sizeof(loader::elf::Sym),
            {reinterpret_cast<std::byte*>(&s), sizeof(s)});
        if (!r) break;

        if (!read_name(img.source(), str.sh_offset + s.st_name,
                       str.sh_offset + str.sh_size, name, sizeof(name)))
            continue;

        std::string_view sv{name};
        if (!keep_symbol(s, sv, f)) continue;

        ++seen;
        fn(ctx, sv, s.st_value, static_cast<std::uint8_t>(s.st_info >> 4) == 2);
    }
    return seen;
}

std::size_t collect(const loader::elf_image& img,
                    entry* out, std::size_t cap,
                    char* names, std::size_t names_cap,
                    const filter& f) noexcept {
    if (!out || cap == 0 || !names || names_cap == 0) return 0;

    struct ctx {
        entry* out; std::size_t cap; std::size_t n;
        char* names; std::size_t names_cap; std::size_t used;
    } c{out, cap, 0, names, names_cap, 0};

    detail::walk(img, f,
        [](void* p, std::string_view name, std::uint32_t addr, bool) {
            auto* x = static_cast<ctx*>(p);
            if (x->n >= x->cap) return;
            for (std::size_t i = 0; i < x->n; ++i)          // de-duplicate
                if (std::strcmp(x->out[i].name, name.data()) == 0) return;
            const std::size_t len = name.size() + 1;
            if (x->used + len > x->names_cap) return;
            char* dst = x->names + x->used;
            std::memcpy(dst, name.data(), name.size());
            dst[name.size()] = '\0';
            x->out[x->n].name = dst;
            x->out[x->n].addr = addr;
            x->used += len;
            ++x->n;
        }, &c);

    return c.n;
}

std::size_t collect_buffered(const loader::elf_image& img,
                             entry* out, std::size_t cap,
                             std::byte* symtab_buf, std::size_t symtab_cap,
                             char* strtab_buf, std::size_t strtab_cap,
                             const filter& f) noexcept {
    auto sym_r = img.find_section(".symtab");
    auto str_r = img.find_section(".strtab");
    if (!sym_r || !str_r || !out || cap == 0 || !symtab_buf || !strtab_buf)
        return 0;

    const auto& sym = *sym_r;
    const auto& str = *str_r;
    if (sym.sh_size == 0 || sym.sh_size > symtab_cap ||
        str.sh_size == 0 || str.sh_size > strtab_cap)
        return 0;

    if (!img.source().read_at(
            sym.sh_offset, {symtab_buf, static_cast<std::size_t>(sym.sh_size)}))
        return 0;
    if (!img.source().read_at(
            str.sh_offset,
            {reinterpret_cast<std::byte*>(strtab_buf),
             static_cast<std::size_t>(str.sh_size)}))
        return 0;

    const std::size_t count = sym.sh_size / sizeof(loader::elf::Sym);
    std::size_t start = sym.sh_info;
    if (start >= count) start = 0;

    std::size_t n = 0;
    for (std::size_t i = start; i < count && n < cap; ++i) {
        loader::elf::Sym s{};
        std::memcpy(&s, symtab_buf + i * sizeof(loader::elf::Sym), sizeof(s));

        if (s.st_name >= str.sh_size) continue;
        const char* name = strtab_buf + s.st_name;
        const std::size_t max = str.sh_size - s.st_name;
        std::size_t len = 0;
        while (len < max && name[len] != '\0') ++len;
        if (len == 0 || len == max || len > kMaxNameLen) continue;

        std::string_view sv{name, len};
        if (!keep_symbol(s, sv, f)) continue;

        bool dup = false;
        for (std::size_t j = 0; j < n; ++j)
            if (std::strcmp(out[j].name, name) == 0) { dup = true; break; }
        if (dup) continue;

        out[n].name = name;
        out[n].addr = s.st_value;
        ++n;
    }
    return n;
}

std::size_t build_blob(const entry* entries, std::size_t count,
                       std::byte* out, std::size_t cap,
                       std::uint32_t runtime_addr) noexcept {
    if (!entries || count == 0 || !out) return 0;

    std::size_t names_size = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!entries[i].name) return 0;
        names_size += std::strlen(entries[i].name) + 1;
    }

    const std::size_t header = 8;
    const std::size_t table  = count * 8;
    const std::size_t total  = header + table + names_size;
    if (cap < total) return 0;

    const std::uint32_t magic = kMagic;
    const std::uint32_t n32   = static_cast<std::uint32_t>(count);
    std::memcpy(out, &magic, 4);
    std::memcpy(out + 4, &n32, 4);

    const std::size_t names_off = header + table;
    std::size_t used = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t name_off = names_off + used;
        const std::uint32_t name_ptr =
            runtime_addr ? runtime_addr + static_cast<std::uint32_t>(name_off)
                         : static_cast<std::uint32_t>(name_off);
        std::memcpy(out + header + i * 8, &name_ptr, 4);
        std::memcpy(out + header + i * 8 + 4, &entries[i].addr, 4);
        const std::size_t len = std::strlen(entries[i].name) + 1;
        std::memcpy(out + name_off, entries[i].name, len);
        used += len;
    }
    return total;
}

view::view(const std::byte* blob, std::size_t size,
           std::uint32_t blob_addr) noexcept
    : blob_(blob), size_(size), addr_(blob_addr) {
    if (!blob_ || size_ < 8) return;
    std::uint32_t magic = 0, n = 0;
    std::memcpy(&magic, blob_, 4);
    std::memcpy(&n, blob_ + 4, 4);
    if (magic != kMagic || n == 0) return;
    if (static_cast<std::size_t>(n) > (size_ - 8) / 8) return;
    count_ = n;
}

std::uint32_t view::lookup(std::string_view name) const noexcept {
    if (!count_) return 0;
    const char* base = reinterpret_cast<const char*>(blob_);
    for (std::size_t i = 0; i < count_; ++i) {
        std::uint32_t name_ptr = 0, addr = 0;
        read_word(blob_ + 8 + i * 8, name_ptr);
        read_word(blob_ + 8 + i * 8 + 4, addr);

        const char* p = nullptr;
        std::size_t max = kMaxNameLen;
        if (addr_ && name_ptr >= addr_ && (name_ptr - addr_) < size_) {
            const std::size_t off = name_ptr - addr_;
            p = base + off;
            max = size_ - off;
        } else if (name_ptr < size_) {
            p = base + name_ptr;
            max = size_ - name_ptr;
        } else {
            p = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(name_ptr));
        }
        if (p && name_equals(p, max, name)) return addr;
    }
    return 0;
}

extern "C" const scl_sym_entry __start_scl_syms[] __attribute__((weak));
extern "C" const scl_sym_entry __stop_scl_syms[] __attribute__((weak));

std::size_t registered(const entry** out) noexcept {
    if (out) *out = nullptr;
    if (!__start_scl_syms || !__stop_scl_syms) return 0;
    const std::size_t n =
        static_cast<std::size_t>(__stop_scl_syms - __start_scl_syms);
    if (out) *out = __start_scl_syms;
    return n;
}

loader::symbol_source source(const view& v) noexcept {
    return loader::symbol_source{
        [](void* ctx, std::string_view name) -> loader::result<std::uint32_t> {
            const auto a = static_cast<const view*>(ctx)->lookup(name);
            if (a == 0)
                return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
            return a;
        },
        const_cast<view*>(&v)};
}

loader::symbol_source source_registered() noexcept {
    return loader::symbol_source{
        [](void*, std::string_view name) -> loader::result<std::uint32_t> {
            const entry* e = nullptr;
            const std::size_t n = registered(&e);
            for (std::size_t i = 0; i < n; ++i)
                if (e[i].name &&
                    name_equals(e[i].name, std::strlen(e[i].name) + 1, name))
                    return e[i].addr;
            return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
        },
        nullptr};
}

} // namespace scl::symtab
