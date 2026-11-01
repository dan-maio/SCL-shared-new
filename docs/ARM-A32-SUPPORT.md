---
title: "ARM A32 mode support in SCL-shared-new"
subtitle: "Analysis and modification plan for loading ARM-state modules, not only Thumb"
author: "SCL / Luckfox Lyra B work"
date: "2026-11-01"
---

> **Implementation status (2026-11-01).**  The P0 items in this document are
> implemented in `SCL-shared-new` `eb2ae55`, with `common-elf-reader` now a
> submodule of `elf32_read_parse-new.git` at `f650889` (the A32/T32 relocation
> engine and ARM build attributes).  The ARM-state test suite
> `SCL-TESTS-shared-new/luckfox-lyra-arm-shared/` has 9 tests — one per SCL
> loader — and all 9 pass on the Luckfox Lyra B.  The P1/P2 items below
> (per-ISA trampolines, ARM EH in every adapter, the remaining relocation
> forms, veneers) are still open and are the reference for the next stage.

# 1. Executive summary

`SCL-shared-new` and its `common-elf-reader` were written for Cortex-M:
the loader code, the relocation engine in `scl::reloc`, the trampolines and several
tests assume that **every loaded function is Thumb**.  That is true on Cortex-M, and
it is almost true for the Luckfox Lyra examples because their fixtures are built with
`-mthumb` even though the firmware is ARM.

Loading a genuine **ARM-state (A32)** module does not work today.  The first failures
are:

1. **Symbol addresses are forced to Thumb.**  `reloc::addr_of()`, `reloc::sym()` and
   `run_ctors()` OR bit 0 into every `STT_FUNC` address.  In an ET_REL object GCC
   encodes a Thumb function with `st_value` odd and an ARM function with `st_value`
   even.  The OR turns an ARM function pointer into a bogus Thumb address, so the first
   `blx` switches to Thumb at an ARM address and faults.
2. **The ET_REL relocation engine has no A32 branch or `MOVW/MOVT` support.**
   `scl::reloc` handles `ABS32`, `TARGET1`, `TARGET2`, `REL32`, `NONE`, `PREL31` and
   `THM_CALL` only.  ARM code emits `R_ARM_CALL`, `R_ARM_JUMP24`,
   `R_ARM_MOVW_ABS_NC`, `R_ARM_MOVT_ABS` (and, for PIC objects,
   `R_ARM_GOT_BREL`, `R_ARM_BASE_PREL`, `R_ARM_PLT32`).  The loader stops with
   `error::UNSUPPORTED_RELOC`.
3. **`scl::optimal` assumes ABS32 literal pools.**  ARM `-mlong-calls` does not
   guarantee literal pools: at `-O0` and `-O2` GCC emits `MOVW/MOVT` pairs
   (`R_ARM_MOVW_ABS_NC` / `R_ARM_MOVT_ABS`), and at `-Os` it may use ABS32.  The
   `nopic` loader only implements ABS32/REL32/RELATIVE.
4. **PLT and hand-written trampolines are one instruction set.**  `pic_call_*`,
   `xip_cross_call`, `dl_cross_call` and `dl_runtime_resolve` are compiled in the
   firmware's ISA.  On Cortex-A an ARM firmware can load Thumb modules (the current
   Lyra case), but a module's PLT is in the module's ISA; routing a PLT slot to a
   trampoline of the other ISA only works if the instruction that enters the
   trampoline interworks.  This must be made explicit and tested.
5. **The parser does not check the ARM ABI.**  `elf_image::parse()` validates magic,
   class, data encoding and `EM_ARM`, but not `e_flags` (EABI version, hard/soft
   float), `e_type`, header entry sizes or `.ARM.attributes`.  ET_REL files carry
   **no float-ABI bits in `e_flags`** (`0x05000000`); the information is only in
   `.ARM.attributes` (`Tag_ABI_VFP_args`).  Mixing hard-float and soft-float modules
   silently corrupts the argument registers.

This document is the implementation plan.  It records what exists, what is missing,
the exact relocation formulas and encodings, the required code changes per component,
a test matrix, and a priority order.  The P0 source changes are in `eb2ae55`
and the parser submodule `f650889`; the rest of the document is the reference
for the open P1/P2 work.

The recommended minimum (P0) is: fix bit 0, add the A32 relocation engine
(`CALL`, `JUMP24`, `MOVW/MOVT`, `THM_*` counterparts, `V4BX`, `PREL31`), wire it into
`scl::reloc` and `scl::optimal`, and validate the ARM ABI in the parser.

---

# 2. Scope and repositories

| Item | Value |
|---|---|
| Loader repository | `SCL-shared-new`, commit `a39e0f0` |
| Parser | `common-elf-reader/` submodule, `elf32_read_parse-new.git` `f650889` |
| Parser repository | `/home/dan/Downloads/GIT/elf32_read_parse-new.git` |
| Target board | Luckfox Lyra B, RK3506, 3x Cortex-A7 (ARMv7-A, A32 + T32) |
| Existing consumer | `SCL-TESTS-shared-new/luckfox-lyra-shared`, `SCL-EXAMPLES-shared-new/luckfox-lyra` |
| Existing fixture ISA | Thumb (`-mthumb`) in every Lyra test and example |
| Out of scope here | AArch64, BE8, TLS, POSIX dynamic-linker semantics, MPU mapping |

## 2.1 The `common-elf-reader` integration

In `SCL-shared-new`, `common-elf-reader/` is now a **git submodule** of
`/home/dan/Downloads/GIT/elf32_read_parse-new.git` at `f650889`.  That commit is
the original `elf32_read_parse.git` `ddaa0e6` (the parser API is unchanged) plus
the A32/T32 relocation engine (`include/loader/arm-reloc.hpp`), the ARM build
attributes reader (`include/loader/arm-attributes.hpp`), the ABI check
(`include/loader/arm-abi.hpp`) and the hardened `elf-image.cpp`.

`SCL-TESTS-shared-new` and `SCL-EXAMPLES-shared-new` pin `SCL-shared` at
`aaf2120` (the `eb2ae55` code plus this documentation); the nested parser
submodule is therefore `f650889`.  A loader working tree can be used with
`-DSCL_DIR=/path/to/SCL-shared-new` in the ARM test suite; the committed default
is the `../SCL-shared` submodule.

Further parser changes belong upstream in `elf32_read_parse-new.git`; bump the
submodule pointer in `SCL-shared-new` after each parser commit, rather than
editing the submodule working tree.

---

# 3. A32/T32 facts the loader must encode

## 3.1 The Thumb bit in `st_value`

For ARM ELF:

* An **ARM-state** function has an even `st_value`.
* A **Thumb-state** function has an odd `st_value`.
* A function pointer in data (`R_ARM_ABS32`, `.init_array`, a GOT slot) must carry
  that same bit.
* A branch instruction must switch state with `BLX`/`BX` when the bits differ.

Measured with GCC 15.2.1 for Cortex-A7:

```text
$ arm-none-eabi-readelf -sW lib_arm.o | grep arm_entry
    14: 00000000    48 FUNC    GLOBAL DEFAULT    1 arm_entry

$ arm-none-eabi-readelf -sW t_thumb.o | grep thumb_entry
     8: 00000001     8 FUNC    GLOBAL DEFAULT    1 thumb_entry
```

`arm_entry` is even (ARM), `thumb_entry` is odd (Thumb).  `reloc::addr_of()` currently
does `a |= 1` for every `STT_FUNC`, so it turns `arm_entry` into `arm_entry|1`.  That is
the bug that makes the first ARM module fail even before relocation.

## 3.2 Branch forms

| Form | Instruction | Interworks | Range |
|---|---|---|---|
| ARM `B` | `EA imm24` | no | ±32 MiB |
| ARM `BL` | `EB imm24` | no | ±32 MiB |
| ARM `BLX` immediate | `FA H imm24` | yes (to Thumb) | ±32 MiB |
| Thumb-2 `B.W` | `F000 9000...` | no | ±16 MiB |
| Thumb-2 `BL` | `F000 D000...` | no | ±16 MiB |
| Thumb-2 `BLX` | `F000 C000...` | yes (to ARM) | ±16 MiB |
| `BX`/`BLX` register | `BX Rm` | yes | any |

The existing `apply_thm_call()` always emits Thumb `BL` (`0xD000` second halfword).
It cannot call an ARM function: it would branch to an even address while staying in
Thumb state.  A32 `R_ARM_CALL` must choose `BL` or `BLX` from bit 0 of the target.

## 3.3 `MOVW`/`MOVT`

GCC uses `MOVW`/`MOVT` to materialise a 32-bit address when a literal pool is not
chosen.  For ARM code this is common for **data** imports even without `-mlong-calls`,
and for both code and data with `-mlong-calls` at `-O0`/`-O2`.

Measured relocation types for `-fno-pic -mlong-calls` (GCC 15.2.1, Cortex-A7):

| Optimization | ARM | Thumb |
|---|---|---|
| `-O0` | `R_ARM_MOVW_ABS_NC` + `R_ARM_MOVT_ABS` | `R_ARM_THM_MOVW_ABS_NC` + `R_ARM_THM_MOVT_ABS` |
| `-Os` | `R_ARM_ABS32` | `R_ARM_ABS32` |
| `-O2` | `R_ARM_MOVW_ABS_NC` + `R_ARM_MOVT_ABS` | `R_ARM_THM_MOVW_ABS_NC` + `R_ARM_THM_MOVT_ABS` |

The Lyra fixtures build with `-Os` for ET_REL and `-O0` for the linked `.opt.so`;
that is why the current Thumb-only Lyra suite never exercises `MOVW/MOVT`.  An ARM
module built with the same options can exercise them immediately.

## 3.4 The hard-float ABI

AAPCS-VFP passes the first four single-precision and the first eight double-precision
arguments in `s0-s15`/`d0-d7`.  A soft-float module expects them on the stack.  The
loader must refuse to mix them.

For a **shared object**, `e_flags` carries the answer:

```text
Flags: 0x5000400, Version5 EABI, hard-float ABI
```

For an **ET_REL object**, GCC writes `e_flags = 0x05000000` (EABI5, no float bits) and
puts the ABI in `.ARM.attributes`:

```text
Tag_CPU_arch_profile: Application
Tag_ARM_ISA_use: Yes
Tag_THUMB_ISA_use: Thumb-2
Tag_ABI_VFP_args: VFP registers
```

The parser must therefore read `.ARM.attributes` for ET_REL, not only `e_flags`.

## 3.5 Exception tables

`.ARM.exidx` entries are `(PREL31 function, word2)` pairs.  `word2` is either
`EXIDX_CANTUNWIND` (`1`), an inline compact entry (bit 31 set), or a PREL31 pointer to
`.ARM.extab`.  The extab word 0 is a personality routine pointer or an inline
descriptor.

The existing `set_extab_thumb_bits()` uses `scl_code_addr()`, which is **profile-based**:
on Cortex-A it returns the address unchanged, on Cortex-M it sets bit 0.  For a mixed
ARM/Thumb module the correct bit comes from the **personality symbol's** state, not
from the firmware profile.  This must be reworked before EH is trusted with ARM
modules.

## 3.6 `ldr pc` and interworking

The PLT model used by `scl::dynamic-nolib` and the XIP adapters relies on the linker's
PLTn:

```text
movw/movt/add ip, pc      ; ip = &GOT[n]
ldr pc, [ip]              ; jump to the GOT slot
```

On Cortex-M the PC load stays in Thumb state, and every slot points to Thumb code.
On Cortex-A the instruction set of the target is significant.  The ARM ARM should be
consulted for the exact `LDR PC` interworking rule; regardless of the answer, the
safe design is to make each GOT slot point at a trampoline **of the caller's ISA**,
or at a `BX` veneer, rather than assuming `LDR PC` switches state.  Section 6.6 makes
this concrete.

---

# 4. Current implementation inventory

## 4.1 Parser

| File | What it does | A32/T32 gap |
|---|---|---|
| `include/loader/elf-types.hpp` | ELF32-LE structs, six `rel_type` values | missing all A32 branch/MOVW types, `e_flags` constants, `SHT_ARM_EXIDX`, `SHT_ARM_ATTRIBUTES` |
| `src/elf-image.cpp` | validates magic/class/data/machine; reads headers | no `e_type`, `e_flags`, header-size, `e_shstrndx` or overflow checks |
| `include/loader/detail/bytes.hpp` | bounds-checked loads/stores | fine (uses `memcpy`, unaligned-safe) |
| `include/loader/elf-image.hpp` | `image_size()`, `image_base()`, `find_section()` | `image_size()` can wrap at 4 GiB; no mapping-symbol/attribute API |

`elf_image::parse()` does **not** check `e_type` today, despite the README saying it
validates type.  The loaders check `ET_DYN`/`ET_REL` themselves.

## 4.2 Relocation engines

| Loader | Implementation | Types implemented today |
|---|---|---|
| `scl::reloc` | `src/reloc/reloc-loader.cpp` | ABS32, TARGET1, TARGET2, REL32, NONE, PREL31, THM_CALL |
| `scl::static` | `src/static/shlib-loader.cpp` | RELATIVE, TLS (TPOFF/DTPOFF/DTPMOD), ABS32, REL32, GLOB_DAT/JUMP_SLOT, COPY (non-standard in `patch_main`) |
| `scl::dynamic-nolib` | `src/reloc-applier.cpp` | RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32, REL32, COPY |
| `scl::dynamic-newlib` | `src/dynamic-newlib/dynamic-linker.cpp` | RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32, REL32, COPY |
| `scl::xip-static` | `src/xip/relocate.cpp` | RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32, REL32, COPY |
| `scl::xip-dynamic-nolib` | `src/xip/relocate.cpp` + adapter | same six |
| `scl::xip-dynamic-newlib` | `src/xip/relocate.cpp` + adapter | same six |
| `scl::optimal` | `src/xip/static/nopic.cpp` | RELATIVE (skipped), ABS32, REL32; any other type is written as `value + addend` |

`src/relocator.cpp` (hosted) classifies only the same six dynamic types, so
`dynamic-newlib`'s plan builder rejects anything else before `apply_plan()`.

## 4.3 Bit-0 sites

| File / function | Current code | Effect on ARM |
|---|---|---|
| `src/reloc/reloc-loader.cpp : addr_of()` | `a = sec_base + value; if (STT_FUNC) a |= 1` | ARM function becomes odd |
| `src/reloc/reloc-loader.cpp : sym()` | same `a |= 1` | exported ARM symbol becomes odd |
| `src/reloc/reloc-loader.cpp : run_ctors()` | `reinterpret_cast<void(*)()>(fp | 1)()` | ARM constructor called as Thumb |
| `include/scl/code-addr.h : scl_code_addr()` | profile-based (`'A'` returns unchanged) | correct for symbol addresses, wrong for a trampoline of the *other* ISA |
| `include/loader/exidx-fixup.hpp` | `scl_code_addr(w0)` | profile-based, not module-based |
| `src/xip/adapter-base.hpp` | same | same |
| `src/dynamic-nolib/dl.cpp` | `scl_code_addr(&dl_cross_call)` | frame the trampoline in the firmware ISA only |
| `src/xip/static/adapter.cpp` | `scl_code_addr(&xip_cross_call)` | same |
| `src/xip/dynamic-*/adapter.cpp` | same | same |
| `include/loader/lazy-resolve.hpp` | comment promises `target | 1` | must become `target | (target & 1)` / preserve state |

## 4.4 Trampolines and PLT

| File | ISA today | Problem |
|---|---|---|
| `src/port/pic-trampoline.c` | firmware ISA (ARM on Lyra) | reaches the callee with `blx`, OK; but if a Thumb PLT enters it via `ldr pc`, state is wrong |
| `src/xip/xip-cross-call.c` | firmware ISA | same; `blx r4` reaches both states |
| `src/port/lazy-resolve.c` | firmware ISA (comment says Thumb) | uses `bx r12`; `dl_fixup` returns `scl_code_addr(target)` |
| `include/dynamic-nolib/lazy-plt.hpp` | comment says Thumb | no A32 variant |
| `src/port/dl-cross-call.c` | wrapper | selects only one variant |

## 4.5 Tests

* `common-elf-reader/test-arm/` is **not** an A32 test: it runs on a Cortex-M4
  (WeAct F411CE) and exercises the parser over semihosting.  The name is about
  "ARM ELF", not ARM state.
* `SCL-TESTS-shared-new/tests/` contains host tests for `link_context` and `symtab`
  only; there is no host test that loads an ET_REL or applies A32 relocations.
* The 67 Lyra examples use Thumb fixtures (`-mthumb`) with an ARM firmware.  They
  already prove ARM **main** + Thumb **module** interworking through `call_lib`/`blx`,
  but not ARM modules, ARM PLT entries or ARM EH.

---

# 5. Required changes — parser

## 5.1 `include/loader/elf-types.hpp`

Add the full A32 relocation set (values from `<elf.h>`), not just the six dynamic
types.  Keep the existing enumerator names for compatibility; append new ones:

```cpp
enum class rel_type : std::uint8_t {
    NONE = 0, PC24 = 1, ABS32 = 2, REL32 = 3, ABS16 = 5, ABS12 = 6,
    THM_ABS5 = 7, ABS8 = 8, SBREL32 = 9, THM_CALL = 10, THM_PC8 = 11,
    TLS_DTPMOD32 = 17, TLS_DTPOFF32 = 18, TLS_TPOFF32 = 19,
    COPY = 20, GLOB_DAT = 21, JUMP_SLOT = 22, RELATIVE = 23,
    GOTOFF32 = 24, BASE_PREL = 25, GOT_BREL = 26, PLT32 = 27,
    CALL = 28, JUMP24 = 29, THM_JUMP24 = 30, BASE_ABS = 31,
    TARGET1 = 38, SBREL31 = 39, V4BX = 40, TARGET2 = 41, PREL31 = 42,
    MOVW_ABS_NC = 43, MOVT_ABS = 44, MOVW_PREL_NC = 45, MOVT_PREL = 46,
    THM_MOVW_ABS_NC = 47, THM_MOVT_ABS = 48,
    THM_MOVW_PREL_NC = 49, THM_MOVT_PREL = 50,
    THM_JUMP19 = 51, THM_JUMP6 = 52, THM_ALU_PREL_11_0 = 53, THM_PC12 = 54,
    ABS32_NOI = 55, REL32_NOI = 56,
    GOT_PREL = 96, THM_PC11 = 102, THM_PC9 = 103,
    IRELATIVE = 160
};
```

Add:

```cpp
constexpr word SHT_ARM_EXIDX       = 0x70000001;
constexpr word SHT_ARM_ATTRIBUTES  = 0x70000003;
constexpr word EF_ARM_EABIMASK     = 0xff000000;
constexpr word EF_ARM_EABI_VER5    = 0x05000000;
constexpr word EF_ARM_ABI_FLOAT_SOFT = 0x00000200;
constexpr word EF_ARM_ABI_FLOAT_HARD = 0x00000400;
constexpr word EF_ARM_BE8          = 0x00800000;
constexpr word EF_ARM_LE8          = 0x00400000;

constexpr bool is_thumb(std::uint32_t st_value) { return (st_value & 1u) != 0; }
constexpr std::uint32_t code_addr(std::uint32_t st_value) { return st_value; }
```

Do **not** define `scl_code_addr` here; keep the RTOS/board hook separate.

## 5.2 `src/elf-image.cpp`

Hardening and ABI checks:

1. `parse()`: require `e_ehsize == 52`, `e_phentsize == 32` (when `e_phnum`),
   `e_shentsize == 40` (when `e_shnum`), `e_shoff/e_phoff` inside the file, and
   `e_shstrndx < e_shnum`.
2. `parse()`: require `(e_flags & EF_ARM_EABIMASK) == EF_ARM_EABI_VER5`;
   reject `EF_ARM_BE8` for now.
3. For `ET_DYN`/`ET_EXEC`: if `e_flags & EF_ARM_ABI_FLOAT_HARD` and the firmware is
   soft-float (or vice versa), return a new `error::ABI_MISMATCH`.
4. For `ET_REL`: read `.ARM.attributes` (new helper) and check
   `Tag_ABI_VFP_args == 1` (VFP registers) when the firmware is hard-float.
5. `find_section()`: bound `e_shstrndx` and the name offsets.
6. `image_size()`: compute in `std::uint64_t`, reject > 4 GiB.
7. Add `entry()`/`flags()` accessors and a `section_by_type()` helper for
   `SHT_ARM_EXIDX`/`SHT_ARM_ATTRIBUTES`.

New error codes in `include/loader/error.hpp`:

```cpp
ABI_MISMATCH,      ///< hard/soft float, EABI, or CPU profile mismatch
RELOC_OVERFLOW,    ///< branch/MOVW target out of range
RELOC_INTERWORK,   ///< a B/B.W cannot switch state; a veneer is needed
UNSUPPORTED_RELOC  ///< already exists
```

## 5.3 `.ARM.attributes` reader (new)

Add `include/loader/arm-attributes.hpp` with a small, freestanding reader:

```cpp
struct arm_attributes {
    bool  valid = false;
    bool  has_vfp_args = false;
    std::uint8_t vfp_args = 0;   ///< 0 base, 1 VFP, 2 toolchain, 3 compatible
    char  cpu_profile = 0;       ///< 'A', 'R', 'M', 'S'
    bool  arm_isa = false;
    bool  thumb_isa = false;
};
result<arm_attributes> read_arm_attributes(const elf_image& img) noexcept;
```

The `aeabi` subsection uses ULEB128 tags.  Only `Tag_ABI_VFP_args` (28) and
`Tag_CPU_arch_profile` (7) are required for ABI validation; the rest can be ignored.
Keep the parser bounded: every tag length must fit the subsection.

## 5.4 Mapping symbols (optional, P1)

Local `STT_NOTYPE` symbols named `$a`, `$t`, `$d` mark ARM code, Thumb code and data
ranges.  GCC normally sets the `st_value` bit on Thumb function symbols, but assembly
and linker-generated symbols may not.  A `mapping_at(section, offset)` helper lets the
loader set bit 0 correctly for **local** symbols that are referenced from another
module.  This is a correctness aid, not a substitute for the symbol bit.

---

# 6. Required changes — relocation engine

## 6.1 One shared implementation

Create `include/reloc/arm-reloc.hpp` and `src/reloc/arm-reloc.cpp` (header-only is also
possible, but a `.cpp` keeps the decode tables out of every firmware).  Public surface:

```cpp
namespace loader::arm {

enum class isa { arm, thumb };

struct context {
    std::uint32_t S;   ///< symbol value (bit 0 = Thumb)
    std::uint32_t A;   ///< addend, already decoded from the instruction/word
    std::uint32_t P;   ///< address of the relocation site
    isa           site_isa;  ///< ARM or Thumb, from the relocation type
};

result<void> apply(std::uint8_t type, std::byte* p, const context& c) noexcept;

/// Decode the in-place addend for a REL relocation.
std::uint32_t decode_addend(std::uint8_t type, const std::byte* p, isa site) noexcept;

}
```

`apply()` returns `RELOC_OVERFLOW` or `RELOC_INTERWORK` instead of silently truncating.
Every loader that owns a relocation switch calls this engine; the per-loader switch
only handles its special cases (GOT/PLT/COPY/RELATIVE).

## 6.2 Instruction helpers

The engine needs these primitives (all `noexcept`, bounds-checked by the caller):

| Helper | Purpose |
|---|---|
| `read16/read32/write16/write32` | little-endian instruction words |
| `sext(x, bits)` | sign extension |
| `arm_imm24(p)` / `arm_set_imm24` | ARM `B`/`BL`/`BLX` imm24 |
| `arm_movw_imm16(p)` / `arm_set_movw_imm16` | ARM `MOVW/MOVT` imm4:imm12 |
| `thumb_bl_fields(p)` / `thumb_set_bl_fields` | T32 `S:J1:J2:imm10:imm11` |
| `thumb_bw_fields(p)` / `thumb_set_bw_fields` | T32 `B.W` |
| `thumb_movw_imm16(p)` / `thumb_set_movw_imm16` | T32 `MOVW/MOVT` `i:imm4:imm3:imm8` |
| `arm_ldr_literal_imm12` / `thumb_ldr_literal_imm12` | literal-pool loads (P2) |
| `in_range_arm_branch(delta)` | ±32 MiB, word-aligned |
| `in_range_thumb_bw(delta)` | ±16 MiB, halfword-aligned |

## 6.3 A32 branch encoding

For `R_ARM_CALL` (`BL`/`BLX`):

```text
X = S + A                         // A = SignExtend(imm24 << 2, 26)
if ((X & 1) == 0) {               // ARM target: BL
    if (!in_range_arm_branch(X - P)) return RELOC_OVERFLOW;
    instruction = 0xEB000000 | (((X - P) >> 2) & 0x00FFFFFF);
} else {                          // Thumb target: BLX
    Y = X & ~1u;
    if (!in_range_arm_branch(Y - P)) return RELOC_OVERFLOW;
    H = ((Y - P) >> 1) & 1u;
    instruction = 0xFA000000 | (H << 24) | (((Y - P) >> 2) & 0x00FFFFFF);
}
```

For `R_ARM_JUMP24` / `R_ARM_PC24` (`B`):

```text
X = S + A
if (X & 1) return RELOC_INTERWORK;      // B cannot switch; a veneer is needed
if (!in_range_arm_branch(X - P)) return RELOC_OVERFLOW;
instruction = 0xEA000000 | (((X - P) >> 2) & 0x00FFFFFF);
```

`R_ARM_PLT32` is a `BL` to the PLT: treat it as `R_ARM_CALL` (the ABI's historical
form), or as `BL` if the target is known to be ARM.

For the Thumb counterparts:

```text
R_ARM_THM_CALL:
  X = S + A; off = (X & ~1) - P
  if (off outside ±16 MiB) return RELOC_OVERFLOW
  base = (X & 1) ? 0xD000 : 0xC000;       // BL : BLX
  write S/J1/J2/imm10/imm11 as today, with base

R_ARM_THM_JUMP24:
  X = S + A
  if (X & 1) return RELOC_INTERWORK;
  off = X - P
  base = 0x9000;                          // B.W T4
  write S/J1/J2/imm10/imm11
```

The existing `apply_thm_call()` already computes `S/J1/J2`; factor it out and add the
`base` parameter.  This is a small change but it is what makes Thumb→ARM calls work.

## 6.4 `MOVW`/`MOVT`

ARM encodings:

| Instruction | Encoding | Field |
|---|---|---|
| `MOVW Rd, #imm16` | `cond 0011 0000 imm4 Rd imm12` | `imm16 = imm4:imm12` |
| `MOVT Rd, #imm16` | `cond 0011 0100 imm4 Rd imm12` | `imm16 = imm4:imm12` |

Write `imm4 = (value >> 12) & 0xF`, `imm12 = value & 0xFFF`, preserving `cond` and `Rd`.

Thumb-2 encodings:

| Instruction | Encoding | Field |
|---|---|---|
| `MOVW Rd, #imm16` | `11110 i 10 0100 imm4  0 imm3 Rd imm8` | `imm16 = i:imm4:imm3:imm8` |
| `MOVT Rd, #imm16` | `11110 i 10 1100 imm4  0 imm3 Rd imm8` | `imm16 = i:imm4:imm3:imm8` |

The relocation values:

```text
R_ARM_MOVW_ABS_NC / R_ARM_THM_MOVW_ABS_NC : low16(S + A)
R_ARM_MOVT_ABS     / R_ARM_THM_MOVT_ABS     : high16(S + A)
R_ARM_MOVW_PREL_NC / R_ARM_THM_MOVW_PREL_NC : low16(S + A - P)
R_ARM_MOVT_PREL    / R_ARM_THM_MOVT_PREL     : high16(S + A - P)
```

Because these are **text** relocations, a read-only XIP image cannot apply them at
load time.  Section 6.7 explains the consequence.

## 6.5 Relocation reference

| Type | Value | Notes / encoding |
|---|---|---|
| `R_ARM_NONE` (0) | — | no-op; used for registration-only entries |
| `R_ARM_PC24` (1) | `S + A - P` | legacy ARM `B/BL` |
| `R_ARM_ABS32` (2) | `S + A` | 32-bit store; bit 0 preserved |
| `R_ARM_REL32` (3) | `S + A - P` | 32-bit store |
| `R_ARM_ABS16` (5) | `S + A` | low 16 bits (P2) |
| `R_ARM_ABS12` (6) | `S + A` | low 12 bits (P2) |
| `R_ARM_THM_ABS5` (7) | `S + A` | `LDR/STR` imm5 (P2) |
| `R_ARM_ABS8` (8) | `S + A` | low 8 bits (P2) |
| `R_ARM_THM_CALL` (10) | `S + A - P` | T32 `BL`/`BLX`, bit 0 selects |
| `R_ARM_COPY` (20) | `memcpy(P, S, st_size)` | standard copy; `shlib-loader` currently injects the address instead |
| `R_ARM_GLOB_DAT` (21) | `S` | GOT data slot |
| `R_ARM_JUMP_SLOT` (22) | `S` | GOT/PLT function slot |
| `R_ARM_RELATIVE` (23) | `B + A` | 0-bias XIP loaders leave it alone |
| `R_ARM_GOTOFF32` (24) | `S + A - GOT_ORG` | PIC; `SB`/GOT base |
| `R_ARM_BASE_PREL` (25) | `GOT_ORG + A - P` | PIC |
| `R_ARM_GOT_BREL` (26) | `GOT(S) + A - GOT_ORG` | PIC |
| `R_ARM_PLT32` (27) | `S + A - P` | historical PLT `BL`; treat as `CALL` |
| `R_ARM_CALL` (28) | `S + A - P` | ARM `BL` or `BLX`; bit 0 selects |
| `R_ARM_JUMP24` (29) | `S + A - P` | ARM `B`; no interworking |
| `R_ARM_THM_JUMP24` (30) | `S + A - P` | T32 `B.W`; no interworking |
| `R_ARM_TARGET1` (38) | `S + A` | `ABS32` on bare-metal |
| `R_ARM_V4BX` (40) | — | no-op on ARMv5+ |
| `R_ARM_TARGET2` (41) | `S + A - P` | `REL32` on bare-metal |
| `R_ARM_PREL31` (42) | `(S + A - P) & 0x7fffffff` | keep bit 31; `.ARM.exidx` |
| `R_ARM_MOVW_ABS_NC` (43) | low16(`S + A`) | ARM `MOVW` |
| `R_ARM_MOVT_ABS` (44) | high16(`S + A`) | ARM `MOVT` |
| `R_ARM_MOVW_PREL_NC` (45) | low16(`S + A - P`) | ARM `MOVW` |
| `R_ARM_MOVT_PREL` (46) | high16(`S + A - P`) | ARM `MOVT` |
| `R_ARM_THM_MOVW_ABS_NC` (47) | low16(`S + A`) | T32 `MOVW` |
| `R_ARM_THM_MOVT_ABS` (48) | high16(`S + A`) | T32 `MOVT` |
| `R_ARM_THM_MOVW_PREL_NC` (49) | low16(`S + A - P`) | T32 `MOVW` |
| `R_ARM_THM_MOVT_PREL` (50) | high16(`S + A - P`) | T32 `MOVT` |
| `R_ARM_THM_JUMP19` (51) | `S + A - P` | T32 conditional `B`; P2 |
| `R_ARM_THM_JUMP6` (52) | `S + A - P` | T32 `CBZ/CBNZ`; P2 |
| `R_ARM_THM_ALU_PREL_11_0` (53) | `S + A - P` | T32 `ADR`; P2 |
| `R_ARM_THM_PC12` (54) | `S + A - P` | T32 literal load; P2 |
| `R_ARM_ABS32_NOI` (55) | `S + A` | same as `ABS32` |
| `R_ARM_REL32_NOI` (56) | `S + A - P` | same as `REL32` |
| `R_ARM_GOT_PREL` (96) | `GOT(S) + A - P` | PIC (P2) |
| `R_ARM_THM_PC11` (102) | `S + A - P` | T16 `B`; P2 |
| `R_ARM_THM_PC9` (103) | `S + A - P` | T16 `B`; P2 |
| `R_ARM_IRELATIVE` (160) | — | reject (`UNSUPPORTED_RELOC`) |
| `R_ARM_TLS_*` (17–19, 104–111) | — | reject; no TLS in these loaders |

For every type not implemented, return `UNSUPPORTED_RELOC` with the numeric type in the
log.  Do not fall through to a generic “write `S + A`” — that is the current `nopic`
bug and it corrupts whatever instruction or data is at `P`.

## 6.6 Which loader gets which engine

### `scl::reloc` (`src/reloc/reloc-loader.cpp`)

1. Remove the `|= 1` in `addr_of()`, `sym()` and `run_ctors()`; return `a` unchanged.
   The `st_value` already carries the bit.  Optionally assert
   `((st_value & 1) != 0) == is_thumb_symbol(...)` for diagnostics.
2. Replace `apply_thm_call()` with `loader::arm::apply()`; add `decode_addend()` before
   each call.
3. Add the A32 cases: `CALL`, `JUMP24`, `PC24`, `PLT32`, `MOVW_ABS_NC`, `MOVT_ABS`,
   `MOVW_PREL_NC`, `MOVT_PREL`, `THM_MOVW_*`, `THM_MOVT_*`, `THM_JUMP24`, `V4BX`,
   `ABS32_NOI`, `REL32_NOI`.  Keep `TARGET1` = ABS32 and `TARGET2` = REL32.
4. `register_unwind()`: `PREL31` promotion already keeps the bit; verify that an ARM
   `.ARM.exidx` entry (even function) sorts correctly and that the personality word is
   not forced odd.
5. Range handling: if `CALL`/`JUMP24` is out of range, return `RELOC_OVERFLOW` and log
   the target and site.  The caller must either build with `-mlong-calls` (MOVW/MOVT)
   or provide a veneer.  Do **not** silently truncate.
6. Interworking: if `JUMP24`/`B` targets a Thumb symbol, return `RELOC_INTERWORK` (or
   synthesize a veneer; see 6.8).

### `scl::optimal` (`src/xip/static/nopic.cpp`)

The current loop:

```cpp
const std::uint32_t addend = ...;   // only ABS32/REL32
store_word(..., value + addend - (is_rel32 ? r_off : 0u));
```

must become a switch over the relocation type:

* `RELATIVE`: skip (0-bias load at link VAs).
* `ABS32`/`REL32`: as today.
* `CALL`/`JUMP24`/`MOVW_*`/`MOVT_*`/`THM_*`: call `loader::arm::apply()`; the target
  is in the text or data window, so the helper must accept the 0-bias address.
* `GLOB_DAT`/`JUMP_SLOT`: write `value` (the GOT slot), not `value + addend`.
* `PREL31`: recompute as `(value - P)`, not the generic `value + addend`.
* anything else: log the type and return `UNSUPPORTED_RELOC` (the current code writes
  garbage for, e.g., `R_ARM_CALL`).

`set_extab_thumb_bits()` is called on the text copy; make it module-aware.

### `scl::xip-static` / `xip-dynamic-*` (`src/xip/relocate.cpp`)

Add the same switch to `apply_rel_xip()`.  The function currently patches only the
**data** copy (`ram_data`); a text relocation cannot be applied after the text has been
copied to a read-only region.  Therefore:

* for writable-DRAM XIP (Luckfox Lyra): the text copy is writable before execution;
  the adapter may patch it, but must do so before `scl_cache_clean_invalidate()` and
  before releasing other cores;
* for real flash XIP (RP2350/WeAct): any text relocation must already be resolved by
  the linker; otherwise return `UNSUPPORTED_RELOC` and keep `text_is_immutable()`
  strict.

`prelink_flash_data()` must also handle `MOVW/MOVT` if a catch table or data word is
materialised that way; today it handles `RELATIVE` and `ABS32` only.

### `scl::static` (`src/static/shlib-loader.cpp`)

The `.so` path mostly survives because a `-fPIC` ARM object uses
`R_ARM_RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`/`ABS32`, which are implemented.  Fixes:

* `R_ARM_COPY` in `patch_main()` is **not** standard copy semantics; it injects the
  library symbol's address into main's slot.  For ARM (and for correctness) implement
  `memcpy(dst, S, st_size)` and keep the old behaviour only behind an explicit
  compatibility flag if an example depends on it.
* Add `CALL`/`JUMP24`/`MOVW/MOVT` handling for `-fno-pic -shared` objects that slip
  through; otherwise reject them with a clear `UNSUPPORTED_RELOC` before patching.
* Preserve bit 0 from `find_symbol_address()` for every function slot.

### `scl::dynamic-nolib` and `scl::dynamic-newlib`

They use `reloc_applier` / `dynamic_linker`, which implement the six dynamic types.
For the ARM `.so` case that is usually enough, but:

* `R_ARM_COPY` must be real copy semantics (the `dynamic-newlib` path already is;
  `shlib-loader` is not).
* `R_ARM_PREL31` can appear in `.rel.dyn` for `.ARM.exidx`; add it to `reloc_applier`
  and `dynamic_linker` or reject it explicitly.
* `TLS_*` and `IRELATIVE` should return `UNSUPPORTED_RELOC`, not fall through.
* `classify()` in `src/relocator.cpp` must know every type the plan can contain.

## 6.7 XIP and text relocations (ARM-specific consequence)

The key difference from Thumb is **where** the address materialisation lives:

| Module kind | Imported function call | Imported data | XIP-safe? |
|---|---|---|---|
| Thumb, `-Os -mlong-calls` | ABS32 literal in `.text` | ABS32 literal in `.text` | text must be writable at load |
| ARM, `-Os -mlong-calls` | ABS32 literal in `.text` | ABS32 literal in `.text` | same |
| ARM/Thumb, `-O0/-O2 -mlong-calls` | `MOVW/MOVT` in `.text` | `MOVW/MOVT` in `.text` | same, and the instruction pair must be patched |
| PIC `.so` (`-fPIC`) | PLT/GOT in `.data` | GOT in `.data` | text immutable |

So on a real flash XIP board, an ARM `-fno-pic` module with unresolved imports is
**not** XIP-safe: both ABS32 literal pools and MOVW/MOVT pairs live in `.text`.
Either:

* use `scl::xip-static` with a `-fPIC` module (GOT in data), or
* link the module at its final VAs with every text relocation resolved, or
* load it into RAM (`scl::reloc`/`scl::static`/`dynamic-*`).

On the Luckfox Lyra the “XIP” region is DRAM and is writable before execution, so the
`optimal`/`xip` adapters can patch text **if** they do it before the cache clean and
before other cores run.  The document treats that as a board capability, not a general
XIP guarantee.

## 6.8 Veneers and interworking

For a `B`/`B.W` (`JUMP24`, `THM_JUMP24`, `PC24`) whose target is the other ISA, or for
a `CALL`/`BL` whose target is farther than the branch range, the loader needs a
**veneer**.  A veneer is a small trampoline within range that uses `BX`/`BLX` and a
literal load of the full 32-bit target:

```text
; ARM veneer (8 bytes, word-aligned)
  ldr pc, [pc, #-4]      ; or: ldr r12, [pc]; bx r12
  .word target
```

```text
; Thumb veneer (8 bytes, halfword-aligned)
  ldr.w r12, [pc, #0]
  bx    r12
  .word target
```

The loader must reserve a small veneer pool next to the module (in the arena or a
dedicated section) and keep it executable.  This is P1; the P0 behaviour is a clear
`RELOC_INTERWORK`/`RELOC_OVERFLOW` error with the target and site, so the build can
switch to `-mlong-calls` or PIC.

---

# 7. Required changes — trampolines and PLT

## 7.1 `scl_code_addr()` is not enough

`include/scl/code-addr.h`:

```cpp
static inline uint32_t scl_code_addr(uint32_t addr) {
#if defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'A'
    return addr;
#else
    return addr | 1u;
#endif
}
```

This is correct for **a symbol address the compiler/linker already decorated**:
on Cortex-A the bit stays as the object says; on Cortex-M every function is Thumb, so
setting it is harmless.  It is wrong for a **trampoline that must run in a specific
ISA** when the caller is in the other ISA.

Add:

```cpp
static inline uint32_t scl_thumb_addr(uint32_t addr) { return addr | 1u; }
static inline uint32_t scl_arm_addr(uint32_t addr)   { return addr & ~1u; }
static inline uint32_t scl_code_addr_for(bool thumb, uint32_t addr) {
    return thumb ? scl_thumb_addr(addr) : scl_arm_addr(addr);
}
```

and use `scl_code_addr_for(module_is_thumb, ...)` for GOT slots and trampolines.

## 7.2 Per-ISA trampolines

Each of these needs an ARM and a Thumb variant, or a small `BX` veneer selected by the
module's ISA:

| Trampoline | ARM variant | Thumb variant | Selection |
|---|---|---|---|
| `pic_call_void` / `pic_call_u32` / `pic_call_u32_2` | `-marm` build | `-mthumb` build | firmware call site |
| `xip_cross_call` / `dl_cross_call` | `xip_cross_call_arm` | `xip_cross_call_thumb` | module's PLT ISA |
| `dl_runtime_resolve` | `dl_runtime_resolve_arm` | `dl_runtime_resolve_thumb` | module's PLT ISA |

The GOT slot written by `patch_main_imports()` / `dlopen()` must point at the variant
matching the **module's** state, and the address must be written with that state's bit
so a `BX`/`BLX` keeps it.  A Thumb module's PLT (`ldr pc, [ip]`) must not be sent to an
ARM trampoline without a state-switching veneer.

## 7.3 Lazy binding

`include/loader/lazy-resolve.hpp` says the resolver returns `target | 1`.  Change the
contract to “return the target with its own bit 0 preserved” and let `dl_fixup` use
`scl_code_addr(target)` (which is profile-aware) or, better, the target's own bit.
`dl_runtime_resolve`'s `bx r12` then works for both states.

## 7.4 Stack alignment

The AAPCS requires 8-byte stack alignment at public interfaces.  `dl_runtime_resolve`
pushes 6 words (24 bytes) plus `vpush {d0-d7}` (64 bytes); both keep 8-byte alignment.
The cross-call trampoline pushes 6 words + 4 words + `vpush`; verify the same for the
ARM and Thumb variants.  The ARM `push {r0-r3, r4, lr}` is 24 bytes; fine.  Add a
compile-time or test-time check.

---

# 8. Required changes — exceptions and unwind

1. `include/loader/exidx-fixup.hpp` and `src/xip/adapter-base.hpp`:
   rename `set_extab_thumb_bits()` to `set_extab_personality_bits()` and make the bit
   depend on the personality symbol, not `scl_code_addr()`'s profile.  On Cortex-A,
   leave an ARM personality even and a Thumb personality odd.
2. `src/reloc/reloc-loader.cpp : register_unwind()`: the PREL31 function word is built
   from `addr_of()`, so after the bit-0 fix it preserves ARM/Thumb.  Verify the sort
   and the `tab` conversion for both states.
3. `src/dynamic-nolib/dl.cpp` and the XIP adapters: register `.ARM.exidx` ranges for
   ARM and Thumb modules; `dl_exidx_register()` takes addresses, no ISA, so the entries
   are the only source of state.
4. `libsupc++`'s `__gxx_personality_v0` must be reachable from the module's state.  If
   the firmware is ARM and libsupc++ is ARM, a Thumb module's extab personality entry
   must carry the Thumb bit; if the linker's PLT veneer is ARM, the entry must be even.
   Test both combinations; do not force the bit.
5. Add an ARM EH test: throw in an ARM module, catch in ARM main; throw in a Thumb
   module, catch in ARM main; throw in ARM main, catch in a Thumb module (if the
   module exports a catch helper).

---

# 9. Test plan

## 9.1 Host tests (fast, deterministic)

Add `SCL-shared-new/tests/arm-reloc/`:

* Build fixtures with GCC 15.2.1 for Cortex-A7:
  * `arm-O0`, `arm-Os`, `arm-O2`, with/without `-mlong-calls`;
  * `thumb-O0`, `thumb-Os`, `thumb-O2`, with/without `-mlong-calls`;
  * `arm-pic` (`-fPIC -shared`), `thumb-pic`;
  * mixed `arm+thumb` in one ET_REL, with `$a`/`$t` symbols;
  * EH: `libcppeh`-style ARM/Thumb.
* Use `readelf -rW` to record the expected relocation types; assert the fixture really
  contains the type under test (otherwise the test is vacuous).
* A host harness maps each ET_REL into a `std::vector<std::byte>` at a chosen base,
  applies `reloc::load()`, calls the exported function, and checks the result.
* Decode the patched instruction with `objdump -d` or a small decoder and compare the
  branch target / `MOVW`/`MOVT` immediate.
* Run the existing `tests/run.sh` and add the new binary.

## 9.2 On-target tests (Luckfox Lyra B)

Implemented as `SCL-TESTS-shared-new/luckfox-lyra-arm-shared/`: 9 tests, one per
loader, all passing at `eb2ae55`.  The matrix that was used:

| Case | Firmware | Module | Expected |
|---|---|---|---|
| A1 | ARM | ARM ET_REL, `-O0 -mlong-calls` | load, call, PASS |
| A2 | ARM | ARM ET_REL, `-Os -mlong-calls` | PASS |
| A3 | ARM | ARM ET_REL, `-O2 -mlong-calls` | PASS |
| A4 | ARM | ARM ET_REL, no `-mlong-calls`, near arena | `R_ARM_CALL` in range |
| A5 | ARM | ARM ET_REL, far arena (>32 MiB) | `RELOC_OVERFLOW` (diagnostic) |
| B1 | ARM | Thumb ET_REL (existing) | regression |
| B2 | ARM | ARM `.so` `-fPIC` | static/dynamic/XIP |
| B3 | ARM | Thumb `.so` `-fPIC` | regression + PLT |
| C1 | ARM | mixed ARM+Thumb ET_REL | cross-calls both ways |
| D1 | ARM | ARM module + EH | throw/catch across states |
| D2 | ARM | Thumb module + EH | existing + ARM main catch |
| E1 | ARM | `-march=armv4t` `R_ARM_V4BX` | no-op, call works |
| F1 | ARM | `R_ARM_COPY` data import | standard copy (or documented flag) |

Run with the existing `test/boards/luckfox-lyra/hw.sh`; add the new logs to the
tested-on table.  Keep the 67 existing Lyra examples passing as a regression gate.

## 9.3 QEMU

Use the µOS++ QEMU `-M virt -cpu cortex-a7` build where available for the load/relocate
paths; QEMU is faster than the board for the relocation matrix.  Semihosting and the
SD card are not needed for the loader tests.

## 9.4 Acceptance criteria

* An ARM ET_REL module built at `-O0`, `-Os` and `-O2` loads under `scl::reloc`, its
  `.init_array` runs, and `float_add`/`libprov` calls return the same values as the
  Thumb fixtures.
* An ARM `.so` loads under `scl::static`, `scl::dynamic-nolib`, `scl::dynamic-newlib`,
  `scl::xip-static`, `scl::xip-dynamic-nolib` and `scl::xip-dynamic-newlib`.
* ARM↔Thumb calls and ARM↔Thumb exceptions work.
* The existing 67 Lyra examples and the Cortex-M suites still pass.
* The parser rejects a hard-float module in a soft-float firmware and vice versa, with
  `ABI_MISMATCH`, before any relocation.

---

# 10. Implementation order

## P0 — blocking, small, high value

1. `common-elf-reader`: extend `rel_type`, add `e_flags` constants, `SHT_ARM_EXIDX`,
   `SHT_ARM_ATTRIBUTES`; reject `BE8`, non-EABI5.
2. `common-elf-reader`: `.ARM.attributes` reader + `Tag_ABI_VFP_args` check.
3. `src/reloc/reloc-loader.cpp`: remove the three `|= 1` sites; preserve bit 0.
4. `include/reloc/arm-reloc.hpp` + `src/reloc/arm-reloc.cpp`: `CALL`, `JUMP24`, `PC24`,
   `MOVW/MOVT` (ARM and Thumb), `THM_CALL` interworking, `V4BX`, `PREL31`,
   `ABS32_NOI`/`REL32_NOI`, range/interwork errors.
5. Wire the engine into `scl::reloc` and `scl::optimal`; add a host ARM ET_REL test.

## P1 — needed for a complete A32 product

6. Per-ISA trampolines and GOT-slot selection (7.2); `scl_code_addr_for`.
7. ARM EH: extab personality bits, `.ARM.exidx` in the dynamic/XIP loaders.
8. `R_ARM_COPY` standard semantics; `R_ARM_PREL31` in the dynamic appliers.
9. XIP text-relocation policy (writable DRAM vs read-only flash); `text_is_immutable`
   diagnostics.
10. ARM `.so` coverage for all six loaders; PLT/GOT tests.

## P2 — completeness

11. `R_ARM_THM_JUMP19/JUMP6/ALU_PREL_11_0/PC12`, `GOT_BREL/GOT_PREL/BASE_PREL`,
    `ABS16/ABS12/ABS8/THM_ABS5`, `THM_PC11/PC9`.
12. Veneer pool for out-of-range/interworking `B`; optional.
13. Mapping symbols `$a/$t/$d` for local symbols.
14. Big-endian BE8 (explicitly out of scope today).

---

# 11. Risks and open questions

| Risk | Why it matters | Mitigation |
|---|---|---|
| `LDR PC` interworking on ARMv7-A | The PLT model may jump to the other ISA without `BX` | Per-ISA trampolines, or a `BX` veneer; test on the Lyra |
| `-mlong-calls` is not a stable ABI | It emits ABS32 at `-Os` and MOVW/MOVT at `-O0/-O2` | Support both; tests at all three levels |
| Distance from the arena/window to main | `BL`/`B` range is ±32 MiB; Lyra optimal window is 120 MiB from main | Range check + `-mlong-calls` or veneer |
| Hard/soft float mixing | Silent wrong arguments | Parser ABI check (P0) |
| `R_ARM_COPY` semantics | The static loader's address injection is not ELF `COPY` | Standard copy, compatibility flag if an example depends on it |
| `common-elf-reader` is vendored | Parser changes can diverge | Commit upstream, then sync the vendored copy; or convert to a submodule |
| Thumb-only Cortex-M regression | The bit-0 fix must not break Cortex-M | `scl_code_addr`/`st_value` tests on both profiles |

Open questions to settle before P1:

1. Does the product want **ARM modules at all**, or only ARM **main** + Thumb modules
   (the current Lyra shape)?  The P0 work is still needed for ARM main correctness in
   `scl::reloc` if it ever loads an ARM `.o`, but the priority changes.
2. Is `scl::optimal` required on ARM, or is `scl::xip-static` (GOT in data) the
   supported XIP model?  `optimal`'s no-pic text patching is the hardest to make
   read-only-XIP-safe.
3. Is `common-elf-reader` to become a real submodule?  If yes, the parser changes are
   a separate repository release.

---

# Appendix A — Reproducer

```sh
export PATH="$HOME/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin:$PATH"
mkdir -p /tmp/armrel && cd /tmp/armrel

cat > lib.c <<'EOF'
extern int ext_var;
extern int ext_fn(int);
int lib_var = 7;
static int helper(int x) { return x + lib_var; }
int arm_entry(int x) { return helper(x) + ext_fn(x) + ext_var; }
int (*fp)(int) = arm_entry;
EOF

arm-none-eabi-gcc -mcpu=cortex-a7 -marm -mfloat-abi=hard -mfpu=neon-vfpv4 \
  -fno-pic -O1 -r lib.c -o lib_arm.rel.o
arm-none-eabi-readelf -sW lib_arm.rel.o | grep arm_entry
arm-none-eabi-readelf -rW lib_arm.rel.o
```

Observed (GCC 15.2.1):

```text
    14: 00000000    48 FUNC    GLOBAL DEFAULT    1 arm_entry

Relocation section '.rel.text' ...
00000004  ... R_ARM_MOVW_ABS_NC    .LANCHOR0
00000008  ... R_ARM_MOVT_ABS       .LANCHOR0
00000014  ... R_ARM_CALL           ext_fn
0000001c  ... R_ARM_MOVW_ABS_NC    ext_var
00000020  ... R_ARM_MOVT_ABS       ext_var
```

The current `scl::reloc` returns `UNSUPPORTED_RELOC` at the first `R_ARM_MOVW_ABS_NC`.
The `R_ARM_ABS32` in `.rel.data` would also be patched to `arm_entry | 1` by
`addr_of()`, which is the second failure.

Thumb counterpart:

```text
$ arm-none-eabi-readelf -sW t_thumb.o | grep thumb_entry
     8: 00000001     8 FUNC    GLOBAL DEFAULT    1 thumb_entry
```

## A.1 `e_flags` vs `.ARM.attributes`

```text
$ arm-none-eabi-readelf -hW lib_arm.rel.o | grep Flags
  Flags: 0x5000000, Version5 EABI

$ arm-none-eabi-readelf -hW lib_arm.so | grep Flags
  Flags: 0x5000400, Version5 EABI, hard-float ABI

$ arm-none-eabi-readelf -AW lib_arm.rel.o | grep -E 'Tag_ARM_ISA_use|Tag_THUMB_ISA_use|Tag_ABI_VFP_args'
  Tag_ARM_ISA_use: Yes
  Tag_THUMB_ISA_use: Thumb-2
  Tag_ABI_VFP_args: VFP registers
```

## A.2 `-mlong-calls` relocation matrix

```text
# ARM, -O0/-O2 -mlong-calls: MOVW/MOVT
R_ARM_MOVW_ABS_NC, R_ARM_MOVT_ABS

# ARM, -Os -mlong-calls: ABS32
R_ARM_ABS32

# Thumb, -O0/-O2 -mlong-calls: THM_MOVW/MOVT
R_ARM_THM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS

# Thumb, -Os -mlong-calls: ABS32
R_ARM_ABS32
```

## A.3 ARM `.so`

```text
Relocation section '.rel.dyn'
000012ac  ... R_ARM_GLOB_DAT   lib_var
000012b0  ... R_ARM_GLOB_DAT   ext_var
000012b8  ... R_ARM_ABS32      arm_entry

Relocation section '.rel.plt'
000012a8  ... R_ARM_JUMP_SLOT  ext_fn
```

---

# Appendix B — New function inventory

`common-elf-reader`:

```text
elf_types.hpp : is_thumb(st_value), code_addr(st_value), e_flags helpers
arm-attributes.hpp : read_arm_attributes(img)
elf-image.cpp : validate_header(), validate_flags(), section_by_type()
```

Relocation engine:

```text
arm-reloc.hpp/.cpp :
  decode_addend(type, p, site)
  apply(type, p, context)
  arm_imm24 / arm_set_imm24
  arm_movw_imm16 / arm_set_movw_imm16
  thumb_bl_fields / thumb_set_bl_fields
  thumb_bw_fields / thumb_set_bw_fields
  thumb_movw_imm16 / thumb_set_movw_imm16
  in_range_arm_branch / in_range_thumb_bw
```

Loaders:

```text
reloc-loader.cpp : apply_arm_reloc(), bit0 fix in addr_of/sym/run_ctors
nopic.cpp : dispatch by type, no generic value+addend
relocate.cpp : dispatch + XIP text policy
reloc-applier.cpp : CALL/JUMP24/MOVW/PREL31 or explicit rejection
relocator.cpp : classify() for every supported type
dynamic-linker.cpp : same, plus real COPY
shlib-loader.cpp : real COPY, MOVW/MOVT or rejection
code-addr.h : scl_thumb_addr/scl_arm_addr/scl_code_addr_for
pic-trampoline.c / xip-cross-call.c / lazy-resolve.c : per-ISA variants
exidx-fixup.hpp / adapter-base.hpp : personality-bit selection
```

---

*End of document.*
