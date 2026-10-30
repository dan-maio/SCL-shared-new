/**
 * @file symtab_test.cpp
 * @brief Host test for boot-time symbol-table building, views and registration.
 *
 * Usage: symtab-test <arm-firmware.elf>
 */
#include "scl/symtab-runtime.hpp"
#include "scl/symtab-register.h"
#include "loader/elf-image.hpp"
#include "loader/sources/fd-byte-source.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

/* Compile-time registration test entries. */
int  reg_alpha = 42;
void reg_beta(void) {}

SCL_REGISTER_SYM(reg_alpha);
SCL_REGISTER_SYM(reg_beta);

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-48s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <firmware.elf>\n", argv[0]);
        return 2;
    }
    std::printf("symtab boot-time test: %s\n", argv[1]);

    auto src_r = loader::fd_byte_source::open_file(argv[1]);
    check(static_cast<bool>(src_r), "open ELF");
    if (!src_r) return 1;

    auto img_r = loader::elf_image::parse(*src_r);
    check(static_cast<bool>(img_r), "parse ELF");
    if (!img_r) return 1;

    static scl::symtab::entry entries[4096];
    static char names[512 * 1024];
    const std::size_t n =
        scl::symtab::collect(*img_r, entries, 4096, names, sizeof(names));
    std::printf("  collected %zu symbols\n", n);
    check(n > 0, "collect .symtab");

    bool no_locals = true;
    for (std::size_t i = 0; i < n; ++i)
        if (entries[i].name[0] == '_' &&
            !scl::symtab::whitelisted(entries[i].name))
            no_locals = false;
    check(no_locals, "filter _internals");

    /* Buffered path: one read per section (the semihosting-friendly variant). */
    static scl::symtab::entry entries_buf[4096];
    static std::byte symbuf[64 * 1024];
    static char strbuf[32 * 1024];
    const std::size_t nb = scl::symtab::collect_buffered(
        *img_r, entries_buf, 4096, symbuf, sizeof(symbuf),
        strbuf, sizeof(strbuf));
    check(nb == n, "buffered collect count matches");
    check(nb > 0 && entries_buf[0].name >= strbuf &&
              entries_buf[0].name < strbuf + sizeof(strbuf),
          "buffered names point into strtab buffer");
    check(nb > 0 && std::strcmp(entries_buf[0].name, entries[0].name) == 0 &&
              entries_buf[0].addr == entries[0].addr,
          "buffered entries match streaming entries");

    std::vector<std::byte> blob(8 + n * 8 + 512 * 1024);
    const std::size_t used = scl::symtab::build_blob(
        entries, n, blob.data(), blob.size(), 0);
    check(used > 8 + n * 8, "build SCLS blob");

    scl::symtab::view v(blob.data(), used, 0);
    check(v.valid(), "view validates magic/count");
    check(v.count() == n, "view count matches");

    bool all_found = true;
    for (std::size_t i = 0; i < n; ++i)
        if (v.lookup(entries[i].name) != entries[i].addr) {
            std::printf("    miss: %s\n", entries[i].name);
            all_found = false;
            break;
        }
    check(all_found, "view lookup finds every entry");
    check(v.lookup("definitely_not_a_symbol_xyz") == 0, "unknown name -> 0");

    auto src = scl::symtab::source(v);
    check(src.fn && src(entries[0].name) &&
              *src(entries[0].name) == entries[0].addr,
          "symbol_source adapter");

    const scl_sym_entry* reg = nullptr;
    const std::size_t rn = scl::symtab::registered(&reg);
    std::printf("  registered %zu compile-time symbols\n", rn);
    check(rn >= 2, "SCL_REGISTER_SYM section");

    auto reg_src = scl::symtab::source_registered();
    check(reg_src.fn && reg_src("reg_alpha") &&
              *reg_src("reg_alpha") == (std::uint32_t)(std::uintptr_t)&reg_alpha,
          "registered lookup reg_alpha");
    check(reg_src.fn && reg_src("reg_beta"), "registered lookup reg_beta");

    std::printf("RESULT: %s\n", failures ? "FAILURE" : "SUCCESS");
    return failures ? 1 : 0;
}
