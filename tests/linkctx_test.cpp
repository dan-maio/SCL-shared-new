/** @file linkctx_test.cpp @brief External-table lookup in fw::link_context. */
#include "scl/link-context.hpp"

/* The host executable already provides _DYNAMIC; this test never calls from_self(). */

int main() {
    scl_sym_entry table[] = {
        {"alpha", 0x20001001u},
        {"beta",  0x20002005u},
    };
    fw::link_context ctx;
    ctx.set_symtab(table, 2);

    auto a = ctx.resolve("alpha");
    auto b = ctx.resolve("beta");
    auto z = ctx.resolve("missing");
    if (!a || *a != 0x20001001u) return 1;
    if (!b || *b != 0x20002005u) return 2;
    if (z) return 3;
    return 0;
}
