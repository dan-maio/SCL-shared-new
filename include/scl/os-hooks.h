/**
 * @file os-hooks.h
 * @brief Optional RTOS/multi-core serialization hooks for every loader.
 *
 * The default implementations are weak no-ops. An RTOS application defines
 * strong versions backed by a mutex (and, on SMP, a hardware spinlock). The
 * hooks are held around load/unload entry points, including while module
 * constructors and destructors run. They are not held while module code is
 * called normally after loading.
 *
 * Reentrancy: the lock must be recursive. Loaders nest (optimal::load_and_link()
 * calls xip::nopic::load()), dependency loading can call back into the loader
 * (Module::dep_loader), and constructors may call the loader or a lazy slot.
 *
 * Lazy and cross-call resolvers also take the lock at call time, so a module
 * called from an ISR must not use them: bind eagerly and leave cross-call off.
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
