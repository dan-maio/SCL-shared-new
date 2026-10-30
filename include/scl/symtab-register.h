/**
 * @file symtab-register.h
 * @brief Compile-time symbol registration (no host tool, no ELF parsing).
 *
 * Place SCL_REGISTER_SYM(name) next to an exported function or object. The
 * entry lands in the linker section `scl_syms`; a linker script keeps the
 * section and exposes __start_scl_syms/__stop_scl_syms. scl::symtab::registered()
 * turns that range into a lookup source.
 *
 * Only annotated symbols are covered -- this is not a replacement for
 * gen-symtab when the whole firmware export surface is needed.
 */
#ifndef SCL_SYMTAB_REGISTER_H
#define SCL_SYMTAB_REGISTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scl_sym_entry {
    const char* name;
    uint32_t    addr;
} scl_sym_entry;

#ifdef __cplusplus
}
#endif

/* `static` gives every TU its own descriptor; `used` + KEEP() keeps it. */
#define SCL_REGISTER_SYM(sym)                                                \
    __attribute__((used, section("scl_syms"), aligned(4)))                   \
    static const scl_sym_entry scl_sym_reg_##sym = {                         \
        #sym, (uint32_t)(uintptr_t)&sym                                      \
    }

#endif /* SCL_SYMTAB_REGISTER_H */
