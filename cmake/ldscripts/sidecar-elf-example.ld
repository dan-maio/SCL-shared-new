/* sidecar-elf-example.ld — reserve flash for a boot-time symbol source.
 *
 * This is an EXAMPLE, not a fragment to merge blindly: it declares its own
 * MEMORY region. Use it when the programmer writes a sidecar ELF (or a
 * symbol-only ELF) next to the firmware and the boot code parses it.
 *
 *   .scl_sidecar is NOLOAD: it is not part of the firmware image; the flashing
 *   procedure writes the sidecar bytes into that address range separately.
 *
 * Boot code:
 *   extern "C" const std::byte _scl_sidecar_start[], _scl_sidecar_end[];
 *   loader::memory_byte_source src{{_scl_sidecar_start,
 *                                   _scl_sidecar_end - _scl_sidecar_start}};
 *   auto img = loader::elf_image::parse(src);
 *   ...
 */
MEMORY
{
    FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 384K
    SIDECAR (r)   : ORIGIN = 0x08060000, LENGTH = 256K
    RAM     (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS
{
    .text : { *(.text*) *(.rodata*) } > FLASH
    .data : { *(.data*) } > RAM
    .bss  : { *(.bss*) *(COMMON) } > RAM

    .scl_sidecar (NOLOAD) : ALIGN(4)
    {
        _scl_sidecar_start = .;
        . = . + LENGTH(SIDECAR);
        _scl_sidecar_end = .;
    } > SIDECAR
}
