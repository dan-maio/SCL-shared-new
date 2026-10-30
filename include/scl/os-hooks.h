/**
 * @file os-hooks.h
 * @brief Optional RTOS/multi-core serialization hooks for every loader.
 *
 * The default implementations are weak no-ops. An RTOS application defines
 * strong versions backed by a mutex (and, on SMP, a hardware spinlock). The
 * hooks are held around load/unload entry points; they are never held while
 * module code executes.
 *
 * Reentrancy: dependency loading can call back into the loader (Module::
 * dep_loader). Use a recursive mutex, or arrange that dependencies are loaded
 * before taking the hook.
 */
#ifndef SCL_OS_HOOKS_H
#define SCL_OS_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

void scl_loader_lock(void);
void scl_loader_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* SCL_OS_HOOKS_H */
