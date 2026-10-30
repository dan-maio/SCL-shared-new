/** @file rp2350-flash.c @brief See rp2350-flash.h. */
#include "scl/rp2350-flash.h"

#include <string.h>

#define REG(a) (*(volatile uint32_t*)(a))

#define XIP_BASE     0x10000000u
#define SIO_CPUID    0xD0000000u
#define FLASH_LOCK   (0xD0000100u + 28u * 4u)
#define TIMER1_INTE  (0x400B8000u + 0x40u)
#define TIMER1_INTF  (0x400B8000u + 0x44u)
#define RESETS_RESET 0x40020000u
#define RESETS_DONE  0x40020008u
#define RESET_TIMER1 (1u << 24)
#define QMI_CSR      0x400D0000u
#define QMI_TX       (QMI_CSR + 4u)
#define QMI_RX       (QMI_CSR + 8u)
#define VTOR         0xE000ED08u
#define CPACR        0xE000ED88u
#define NVIC_ISER0   0xE000E100u
#define TIMER1_IRQ   4u
#define VECTORS      (16u + 52u)
#define PAGE         256u

#define IN_RAM __attribute__((section(".data.scl_rp2350_flash"), noinline, long_call))

/* ---- the bootrom's flash functions ------------------------------------------------------- */
typedef void (*rom_void)(void);
typedef void (*rom_erase)(uint32_t, size_t, uint32_t, uint8_t);
typedef void (*rom_prog)(uint32_t, const uint8_t*, size_t);
static struct {
    rom_void connect, exit_xip, flush, enter_xip;
    rom_erase erase;
    rom_prog prog;
} g_rom;

static void* rom_lookup(char a, char b) {
    typedef void* (*lookup_t)(uint32_t, uint32_t);
    const lookup_t fn = (lookup_t)(uintptr_t)(*(const uint16_t*)0x16u);
    return fn((uint32_t)a | ((uint32_t)b << 8), 0x0004u);   /* ARM secure */
}

static void rom_init(void) {
    if (g_rom.connect) return;
    g_rom.exit_xip = (rom_void)rom_lookup('E', 'X');
    g_rom.flush = (rom_void)rom_lookup('F', 'C');
    g_rom.enter_xip = (rom_void)rom_lookup('C', 'X');
    g_rom.erase = (rom_erase)rom_lookup('R', 'E');
    g_rom.prog = (rom_prog)rom_lookup('R', 'P');
    g_rom.connect = (rom_void)rom_lookup('I', 'F');
}

/* ---- these run from SRAM: flash is not readable between exit_xip and enter_xip ------------- */
IN_RAM static void ram_erase(uint32_t offset) {
    g_rom.connect();
    g_rom.exit_xip();
    g_rom.erase(offset, SCL_RP2350_FLASH_SECTOR, SCL_RP2350_FLASH_SECTOR, 0x20u);
    g_rom.flush();
    g_rom.enter_xip();
}

IN_RAM static void ram_program(uint32_t offset, const uint8_t* data) {
    g_rom.connect();
    g_rom.exit_xip();
    g_rom.prog(offset, data, PAGE);
    g_rom.flush();
    g_rom.enter_xip();
}

IN_RAM static void ram_read_id(uint8_t* out) {
    g_rom.connect();
    g_rom.exit_xip();
    REG(QMI_CSR) = (30u << 22) | 1u;   /* CLKDIV, EN */
    while (REG(QMI_CSR) & 2u) {
    }
    REG(QMI_CSR) |= 4u;                /* assert CS0 */
    for (unsigned i = 0; i < 4u; ++i) {
        REG(QMI_TX) = i == 0u ? 0x9Fu : 0u;
        for (unsigned t = 0; t < 100000u && (REG(QMI_CSR) & (1u << 16)); ++t) {
        }
        out[i] = (uint8_t)REG(QMI_RX);
    }
    REG(QMI_CSR) &= ~4u;
    REG(QMI_CSR) &= ~1u;
    g_rom.flush();
    g_rom.enter_xip();
}

/* ---- parking the other core ----------------------------------------------------------------- */
__attribute__((aligned(512))) static uint32_t g_ram_vectors[VECTORS];
static volatile bool g_park_ready[2], g_parked[2], g_park_go;
static volatile unsigned g_parks[2];

IN_RAM static void park_here(unsigned me) {
    REG(TIMER1_INTF) = 0;
    g_parks[me] = g_parks[me] + 1u;
    g_parked[me] = true;
    __asm volatile("dsb" ::: "memory");
    while (g_park_go) {
    }
    g_parked[me] = false;
}
IN_RAM static void park_irq0(void) { park_here(0); }
IN_RAM static void park_irq1(void) { park_here(1); }

static inline uint32_t irq_off(void) {
    uint32_t pm;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(pm) :: "memory");
    return pm;
}
static inline void irq_restore(uint32_t pm) { __asm volatile("msr primask, %0" :: "r"(pm) : "memory"); }

/* One writer at a time. Spins with interrupts on, so a core waiting here can itself be parked. */
static void flash_lock(void) {
    while (REG(FLASH_LOCK) == 0u) {
    }
    __asm volatile("dmb" ::: "memory");
}
static void flash_unlock(void) {
    __asm volatile("dmb" ::: "memory");
    REG(FLASH_LOCK) = 1u;
}

/* Returns with interrupts off, and the other core not running from flash. */
static uint32_t exclusive_begin(void) {
    const uint32_t pm = irq_off();
    const unsigned other = 1u - (REG(SIO_CPUID) & 1u);
    if (g_park_ready[other]) {
        g_park_go = true;
        __asm volatile("dsb" ::: "memory");
        REG(TIMER1_INTF) = 1u << other;
        while (!g_parked[other]) {
        }
    }
    return pm;
}
static void exclusive_end(uint32_t pm) {
    const unsigned other = 1u - (REG(SIO_CPUID) & 1u);
    g_park_go = false;
    __asm volatile("dsb" ::: "memory");
    if (g_park_ready[other]) {
        while (g_parked[other]) {
        }
    }
    irq_restore(pm);
}

/* ---- the API --------------------------------------------------------------------------------- */
void scl_rp2350_flash_smp_init(void) {
    REG(RESETS_RESET) &= ~RESET_TIMER1;
    while (!(REG(RESETS_DONE) & RESET_TIMER1)) {
    }
    REG(TIMER1_INTE) |= 3u;
    const uint32_t* current = (const uint32_t*)REG(VTOR);
    for (unsigned i = 0; i < VECTORS; ++i) g_ram_vectors[i] = current[i];
    g_ram_vectors[16u + TIMER1_IRQ] = (uint32_t)&park_irq0;
    g_ram_vectors[16u + TIMER1_IRQ + 1u] = (uint32_t)&park_irq1;
}

void scl_rp2350_flash_park_enable(void) {
    const unsigned me = REG(SIO_CPUID) & 1u;
    const uint32_t pm = irq_off();
    REG(VTOR) = (uint32_t)g_ram_vectors;   /* VTOR is per core */
    REG(CPACR) |= 3u << 14;                /* the bootrom's flash code uses CP7 */
    __asm volatile("dsb\n isb" ::: "memory");
    irq_restore(pm);
    REG(NVIC_ISER0) = 1u << (TIMER1_IRQ + me);
    g_park_ready[me] = true;
}

const void* scl_rp2350_flash_map(uint32_t offset) { return (const void*)(XIP_BASE + offset); }

unsigned scl_rp2350_flash_parks(unsigned core) { return g_parks[core & 1u]; }

bool scl_rp2350_flash_erase(uint32_t offset, size_t len) {
    if ((offset | len) & (SCL_RP2350_FLASH_SECTOR - 1u)) return false;
    rom_init();
    for (size_t done = 0; done < len; done += SCL_RP2350_FLASH_SECTOR) {
        flash_lock();
        const uint32_t pm = exclusive_begin();
        ram_erase(offset + (uint32_t)done);
        exclusive_end(pm);
        flash_unlock();
    }
    return true;
}

bool scl_rp2350_flash_program(uint32_t offset, const void* data, size_t len) {
    static uint8_t page[PAGE] __attribute__((aligned(4)));
    const uint8_t* in = (const uint8_t*)data;
    bool ok = true;
    rom_init();
    flash_lock();
    for (size_t done = 0; done < len && ok;) {
        const uint32_t at = offset + (uint32_t)done, start = at & ~(PAGE - 1u), skip = at - start;
        const size_t left = len - done, n = left < PAGE - skip ? left : PAGE - skip;
        memcpy(page, scl_rp2350_flash_map(start), PAGE);   /* bits already programmed stay */
        memcpy(page + skip, in + done, n);
        const uint32_t pm = exclusive_begin();
        ram_program(start, page);
        exclusive_end(pm);
        ok = memcmp(scl_rp2350_flash_map(at), in + done, n) == 0;
        done += n;
    }
    flash_unlock();
    return ok;
}

void scl_rp2350_flash_read_id(uint8_t id[3]) {
    uint8_t raw[4] = {0, 0, 0, 0};
    rom_init();
    flash_lock();
    const uint32_t pm = exclusive_begin();
    ram_read_id(raw);
    exclusive_end(pm);
    flash_unlock();
    memcpy(id, raw + 1, 3);
}
