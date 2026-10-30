/**
 * @file rp2350-flash.h
 * @brief Erase and program the RP2350's QSPI flash, including with both cores running.
 *
 * Writing stops execute-in-place, so nothing may run from flash during a write. With two cores
 * running, the writer forces a TIMER1 interrupt on the other core, which waits in SRAM until the
 * write ends. An erase or a program stops only for that one sector or page, so the other core
 * carries on in between.
 *
 * Opt in by linking scl::rp2350-flash. Offsets are from the start of flash, not XIP addresses.
 *
 * Before core 0 starts core 1, call scl_rp2350_flash_smp_init(). On each core, before any core
 * writes while the other is running, call scl_rp2350_flash_park_enable(). Single-core code needs
 * neither.
 *
 * Rules:
 * - This takes TIMER1_IRQ_0 and TIMER1_IRQ_1, SIO spinlock 28, and each core's VTOR, which it points
 *   at a table in SRAM copied from the current one. Set your own vectors before the init call.
 * - TIMER1's alarm interrupts are not available to the application. The rest of TIMER1 is.
 * - The interrupt keeps its default priority, 0, so nothing may run at that priority from flash
 *   and expect to preempt it. Interrupts stop on both cores for each operation: a sector erase
 *   takes tens of milliseconds, so this does not suit hard real-time work.
 * - Do not call these with a spinlock held, or with interrupts off: the other core could be
 *   spinning on it with interrupts off, and so could never be parked.
 * - Whatever keeps its data in the flash being written, such as a filesystem, needs its own lock.
 *
 * Call scl_rp2350_flash_map() to read: reads go through XIP, and need nothing else.
 */
#ifndef SCL_RP2350_FLASH_H
#define SCL_RP2350_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCL_RP2350_FLASH_SECTOR 4096u

/** Core 0, before core 1 starts: prepares the vector table and the parking interrupt. */
void scl_rp2350_flash_smp_init(void);
/** On each core: this core can now be parked. Also enables coprocessor 7, which the bootrom uses. */
void scl_rp2350_flash_park_enable(void);

/** Erase whole sectors. offset and len must be multiples of SCL_RP2350_FLASH_SECTOR. */
bool scl_rp2350_flash_erase(uint32_t offset, size_t len);
/** Program any bytes, at any offset, over erased flash. Reads them back to check. */
bool scl_rp2350_flash_program(uint32_t offset, const void* data, size_t len);
/** Read the JEDEC ID: manufacturer, type, capacity. */
void scl_rp2350_flash_read_id(uint8_t id[3]);
/** Where offset is in the XIP window. */
const void* scl_rp2350_flash_map(uint32_t offset);
/** How many times core was parked for the other core's write. */
unsigned scl_rp2350_flash_parks(unsigned core);

#ifdef __cplusplus
}
#endif
#endif
