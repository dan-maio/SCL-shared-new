/* scl-syms.ld — keep the compile-time SCL_REGISTER_SYM() table.
 *
 * Include this fragment from the application's SECTIONS block (or merge it),
 * after .rodata:
 *
 *   INCLUDE cmake/ldscripts/scl-syms.ld
 *
 * GNU ld also auto-defines __start_scl_syms/__stop_scl_syms for the orphan
 * section, but an explicit KEEP() is required once --gc-sections is enabled.
 */
.scl_syms : ALIGN(4)
{
    __start_scl_syms = .;
    KEEP(*(scl_syms))
    __stop_scl_syms = .;
} > FLASH
