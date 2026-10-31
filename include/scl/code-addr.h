/**
 * @file code-addr.h
 * @brief A code address, as a branch to it needs it.
 *
 * Cortex-M runs only Thumb, so bit 0 must be set, whatever the address came from. A Cortex-A runs
 * both: main may be ARM and a module Thumb, so bit 0 must stay as the compiler or linker set it.
 */
#ifndef SCL_CODE_ADDR_H
#define SCL_CODE_ADDR_H

#include <stdint.h>

static inline uint32_t scl_code_addr(uint32_t addr) {
#if defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'A'
    return addr;
#else
    return addr | 1u;
#endif
}

#endif
