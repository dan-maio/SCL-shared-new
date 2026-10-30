/** @file os-hooks.c @brief Weak no-op loader serialization hooks. */
#include "scl/os-hooks.h"

__attribute__((weak)) void scl_loader_lock(void) {}
__attribute__((weak)) void scl_loader_unlock(void) {}
