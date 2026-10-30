# SCL Project Family — Plain-English Analysis

*Scope: the three top-level projects `SCL-shared`, `SCL-TESTS-shared` and `SCL-EXAMPLES-shared` under `/home/dan/Documents`. Written 2026-10-30.*

## 0. The one-paragraph summary

**SCL** ("Shared-Code Loader") is a source-only C++ toolkit that lets ARM Cortex-M firmware load, relocate and call code that was built separately — without an operating system, without `mmap`, and without a prebuilt library file. The same core supports several **loader models**: relocatable objects copied into RAM, PIC shared objects copied into RAM, execute-in-place (XIP) code running from flash, `dlopen`-style dynamic loading, and linker overlays that swap modules through one RAM window. The workspace is split into three repositories: the **engine** (`SCL-shared`), the **test benches** (`SCL-TESTS-shared`), and **worked board applications** (`SCL-EXAMPLES-shared`). Tests and examples pull the engine in as a git submodule and as CMake **INTERFACE** ("source-code") libraries: the engine's `.cpp` files are compiled into *their* firmware with *their* flags; there are no `.a` archives.

```text
                     +---------------------------------------------+
                     |                 SCL-shared                  |
                     |  src/ + include/   loader + patcher         |
                     |  tools/            gen-symtab.cpp (host)    |
                     |  common-elf-reader/  git submodule          |
                     +----------------------+----------------------+
                                            |
                         git submodule      |      git submodule
             +------------------------------+------------------------------+
             |                                                             |
             v                                                             v
+--------------------------------+                  +--------------------------------+
|        SCL-TESTS-shared        |                  |       SCL-EXAMPLES-shared      |
|  one suite per loader model    |                  |  float-* / spifs-* / combo-*   |
|  platforms, fixtures, runners  |                  |  complete WeAct applications   |
+--------------------------------+                  +--------------------------------+
```

---

## 1. `SCL-shared` — the engine

### What it is

`SCL-shared` contains only the loader/patcher code, its headers, the ELF parser submodule (`common-elf-reader`), and one host-side tool (`tools/gen-symtab.cpp`). It defines a set of CMake alias targets. A consumer adds the repo with `add_subdirectory(SCL-shared scl)` and links one of the `scl::*` targets. Alternatively, `-DSCL_LOADER=<case>` aliases the generic `scl::loader` to the chosen model.

### The loader models

| Model | CMake target | Module format | Code runs from | How `main` calls it | `r10` |
|---|---|---|---|---|---|
| relocatable | `scl::reloc` | `ET_REL` (`-fno-pic -r`) | RAM arena | function pointer from `reloc::sym()` | free |
| static-full-ram | `scl::static` | `-fPIC .so` | RAM buffer (whole image) | bare direct call (`patch_main`) | free |
| static (XIP) | `scl::xip-static` | `-fPIC` split `.so` | flash (XIP); RW copied to RAM | bare direct call | used (PIC base) |
| optimal (XIP) | `scl::optimal` | `-fno-pic -mlong-calls`, fixed windows | flash window; RW in RAM | bare direct call | free |
| dynamic (no libc) | `scl::dynamic-nolib` | `-fPIC .so` | RAM buffer | `dlsym`-style lookup, then pointer | used |
| dynamic (newlib) | `scl::dynamic-newlib` | `-fPIC .so` | RAM buffer | POSIX `dlopen`/`dlsym` | used |
| XIP dynamic | `scl::xip-dynamic-nolib` / `-newlib` | split `R-X \+ RW` `.so` | flash (XIP); RW to RAM | `dlsym` / POSIX `dlopen` | used |
| overlay | `scl::overlay` | compiled *with* the firmware | one shared SRAM window | ordinary linked call | free |

Quick decision schema:

```text
Where must the module's code live?
|
+-- RAM (copied in)
|    +-- by name, no dlopen ....... scl::static
|    |                               (plain -fPIC .so, patch_main)
|    +-- function pointer ......... scl::reloc
|    |                               (ET_REL .rel.o in caller arena)
|    +-- dlopen / dlsym ........... scl::dynamic-nolib
|                                    scl::dynamic-newlib
|
+-- Flash (execute-in-place)
|    +-- rebuild without PIC ...... scl::optimal
|    |                               (-fno-pic, fixed windows, no r10)
|    +-- must stay a PIC .so ...... scl::xip-static
|                                    scl::xip-dynamic-*
|                                    (split R-X|RW, r10)
|
+-- Linked with the firmware ...... scl::overlay
                                     (one SRAM window, no resolver)
```

### How each one works, in plain English

- **relocatable** (`scl::reloc`) — Loads ordinary relocatable objects (`.rel.o`) into a caller-provided RAM arena, like a Linux kernel module. It applies `R_ARM_ABS32`, `TARGET1`, `REL32` and `THM_CALL` relocations eagerly, resolves imports against `main`, and returns addresses to `main`, which calls them through typed function pointers. No GOT, PLT, `r10` or `_DYNAMIC`. `load_set()` handles mutually dependent module groups (e.g. the cyclic `c1` ↔ `c2` ↔ `cpp3` cluster).
- **static-full-ram** (`scl::static`) — The classic "patcher": a plain `-fPIC` shared object is copied whole into a RAM buffer, its GOT/PLT and `main`'s GOT are patched, constructors run, and imports are resolved against an explicit `elf_loader::symbol_table` that `main` fills with `add_global(name, addr)`. It supports byte-exact unload via a patch journal, TLS (`__tls_get_addr`), `RTLD_LOCAL`/`RTLD_GLOBAL`, reference counting and `DT_NEEDED` auto-load. No `r10`, no `-pie`.
- **static / XIP + `r10`** (`scl::xip-static`) — A split `-fPIC` `.so` whose read-execute segment stays in flash and executes in place; only its read-write data, GOT and BSS are copied to RAM. The module uses the PIC base register `r10`, set once by the firmware, so `main` can call exports by name without a handle or table.
- **optimal** (`scl::optimal`) — Modules are built `-fno-pic -mlong-calls` and laid out at fixed addresses inside reserved RAM windows, so the loader has no GOT/PLT and no `r10` to manage. A host script auto-packs the fixture addresses and generates the module load list; the firmware loops over it. This is the simplest model that still reads like ordinary linked code.
- **dynamic-nolib** (`scl::dynamic-nolib`) — A freestanding `dlopen` over any byte source: lazy GOT/PLT binding, cross-library `r10` routing, EHABI exidx unwinding, no libc dependency.
- **dynamic-newlib** (`scl::dynamic-newlib`) — The same idea behind a standard POSIX `dlopen`/`dlsym`/`dlclose`/`dlerror` facade, using link-time binding plus a journaled revert for unload. Needs a hosted C++ runtime, so consumers configure `-DSCL_HOSTED=ON`. This is the model that uses `gen-symtab.cpp`.
- **overlay** (`scl::overlay`) — Not a runtime loader at all: modules are compiled and linked **with** the firmware into a shared SRAM window, and the application swaps them in with a `memcpy` under the overlay manager. The linker binds every import to a fixed address, so there is no resolver, no `.dynsym`, no symbol table.

### Supporting targets and tools

- `scl::common` — shared core: ELF image handling, dynamic info, lazy resolution, region allocator, relocation applier, symbol resolver, logging.
- `scl::self-resolve` — `link_context::from_self()`, which resolves a module's imports against `main`'s own `.dynsym` (requires a `-pie` firmware). Kept separate so non-PIE loaders don't pull it in.
- `scl::xip-common` — XIP primitives: segment map, relocation, compaction.
- `scl::loader-nopic` — the fixed-window no-PIC loader underneath `scl::optimal`.
- `common-elf-reader` — the ELF32 parser (git submodule), used by the loaders *and* by the host-side `gen-symtab` tool.
- `gen-symtab-host` — a **host** build of `tools/gen-symtab.cpp`, not a firmware library. See §4.

```cmake
add_subdirectory(SCL-shared scl)
target_link_libraries(my_fw PRIVATE scl::optimal)  # or scl::reloc / scl::static / scl::xip-static / ...
```

---

## 2. `SCL-TESTS-shared` — the test benches

### What it is

A workspace of independent test suites, one per loader model, plus shared platforms, fixtures, runners and documentation. The root `CMakeLists.txt` only re-exports the `scl::*` targets from the `SCL-shared` submodule; each suite can also be configured standalone. Boards supported across the suites are **WeAct F411CE (BlackPill)**, **NUCLEO-F411RE** and **qemu-system-arm** (`netduinoplus2` machine), selected with `SCL_BOARD` / `SCL_PLATFORM`.

### The sub-projects

| Suite | What it tests | Notable content |
|---|---|---|
| `dynamic-shared` | `scl::dynamic-nolib` and `scl::dynamic-newlib`, RAM and XIP | POSIX-only `dlopen` app (`test-dynamic/test1`) over 12 fixtures; XIP apps `test-xip-dynamic-nolib/test1` (`-pie -Wl,-E`) and `test-xip-dynamic-newlib/test1` (embedded symtab) |
| `static-shared` | `scl::static` and `scl::xip-static`, plus the no-PIC fixed-address flavour | `test-static/*` apps (static, static-cpp, static-direct, SPI driver) and `test-xip/*` apps (semihosting, direct, no-pic, no-pic-simple, interrupt variants, SPI flash); on-chip flat-fs |
| `relocatable-shared` | `scl::reloc` (ET_REL) | 9 fixtures including the cyclic cluster loaded with `load_set()` and cross-module calls; plain C-style function-pointer test |
| `optimal-shared` | `scl::optimal` (no-PIC XIP windows) | 11 fixtures, one generic fixture recipe, generated load list (`tools/gen-imports.py`), separate RAM and XIP `main.cpp`; the "simplest complete" by-name model |
| `overlay-shared` | `scl::overlay` | `test-overlay/test1` compares memory overhead of a linker overlay against `scl::dynamic-nolib`; modules share one SRAM window |
| `SCL-shared/` | engine submodule | symlink/git submodule consumed by the suites |
| `docs/` | cross-cutting guides | `manual.md`, `LOADER-CHOICE.md`, `import_in_project_full.md`, `resolving-module-imports.md`, `COMPARISON.md`, and their PDFs; `make-pdfs.py` renders markdown to print-quality PDF |

### How they are used

Each suite exposes a one-shot script, for example:

```sh
cd optimal-shared && ./build-and-run.sh xip qemu
cd static-shared  && ./test-static/build-and-run.sh qemu static
cd dynamic-shared && ./test-dynamic/build-and-run.sh qemu test1
```

The script builds the fixtures, configures CMake with the ARM toolchain, builds the firmware, and runs it on QEMU or flashes it with OpenOCD. Suites also register CTest tests. The docs directory is the place to start when choosing a loader model; `resolving-module-imports.md` explains why a runtime symbol table is needed at all — the background for §4.

---

## 3. `SCL-EXAMPLES-shared` — the worked applications

### What it is

A set of complete WeAct F411CE applications, each built by following the import guide verbatim, that double as live verification of the documentation. Unlike the tests, they are written as a normal application ("`main` + board support + one loadable library").

### The three families

- **`float-*`** — the minimal shape: a tiny `float_add`/`float_mul` library with no imports. Good for seeing the mechanics of each model.
- **`spifs-*`** — the real thing: the W25Q64 SPI-flash driver plus a flat filesystem, loaded as a library. `main` calls it by name to initialise, format, list, read and delete files. The module imports the STM32 HAL from `main`, so these also demonstrate the `.dynsym`/anchor resolution path.
- **`combo-*`** — the load/unload cycle: load the float library, compute and keep results in `main`, unload it, then load the SPI library and write those results to `calc.txt` on the on-board filesystem. Shows that the memory window is reused and that unloading differs per model.

### The model suffixes

| Suffix | Loader | Call style |
|---|---|---|
| `reloc` | `scl::reloc` | `sym` → function pointer |
| `static` | `scl::xip-static` | bare by-name, `r10` set once |
| `static-full-ram` | `scl::static` | bare by-name via `patch_main`, no `-pie` |
| `optimal` | `scl::optimal` | bare by-name, no `r10` |
| `dynamic` | `scl::dynamic-newlib` (hosted) | POSIX `dlopen`/`dlsym` |
| `dynamic-xip` | `scl::xip-dynamic-newlib` | POSIX `dlopen`, XIP from flash |
| `overlay` | `scl::overlay` | ordinary linked call, swapped by `memcpy` |

Each app's `run.sh` clones `SCL-shared` from git on first run (override with `GIT_REMOTE=…`), builds the fixture and firmware, then flashes/runs it. The `dynamic*` apps are the ones that need the embedded symbol table from §4; the `static`, `optimal`, `reloc` and `overlay` apps do not.

---

## 4. `SCL-TESTS-shared/SCL-shared/tools/gen-symtab.cpp` — what, why, when

> The file is a copy (submodule) of `SCL-shared/tools/gen-symtab.cpp`. The canonical source lives in the engine repository; the tests and examples see it through the submodule.

### 4.1 What it is

A small **host-side C++20 program**. It is compiled for the build machine (not for the Cortex-M), linked against `common-elf-reader`, and run *after* the firmware link. It takes the firmware ELF and produces a compact binary **symbol table** that is then patched into a reserved flash section. It is built by `SCL-shared/CMakeLists.txt` as the custom target `gen-symtab-host`.

```text
usage: gen-symtab <firmware.elf> <out.bin>
```

### 4.2 Why it exists

When a firmware loads a library at runtime, two name → address bindings can no longer be done by the linker:

1. The module's **undefined imports** (e.g. `spifs.so` calling `HAL_SPI_Init`, which lives in `main`).
2. **`dlsym(handle, "name")`** lookups from application code.

Both need a *string-to-address map that is present in flash at runtime*. But a normal, non-`-pie` firmware does not have one:

- `.symtab` is **not `SHF_ALLOC`**, so the linker never puts it in a loadable segment. It is stripped from the `.bin`/`.hex` and is simply not on the device.
- `.dynsym` exists only in a **`-pie` or shared** image — a plain static link has no dynamic section to attach it to.
- Forcing `.symtab` into flash is possible, but it carries every local and section symbol, requires a full `Elf32_Sym` parser on the MCU, has no hash for lookup, and nothing tells the firmware where it landed.

`gen-symtab` is the clean answer: **the useful subset of `.symtab`, small enough to keep in flash and trivial to parse in place.** The documentation reports roughly 87 KB of raw symbols shrinking to about 7.5 KB after filtering.

### 4.3 What it does, step by step

1. Opens the ELF with `loader::fd_byte_source` and parses it with `loader::elf_image` from `common-elf-reader`.
2. Finds three sections: the placeholder `.scl_symtab`, plus `.symtab` and `.strtab`. If any is missing, it fails.
3. Walks the symbol table starting at `sh_info` (i.e. skipping local symbols) and keeps a symbol only if:
   - `st_value != 0` and it is defined in a real section (`st_shndx != 0` and `< 0xFFF1`);
   - its binding is **GLOBAL** or **WEAK**;
   - the name is non-empty;
   - the name does **not** start with `_`, *unless* it matches the hard-coded whitelist (`_impure_ptr`, `_Znw`, `_Zdl`, `__cxa`, `__errno`, `__exidx`, `__gnu_Unwind`, `__register_frame_info`, `__deregister_frame_info`, `__gxx_personality`, `__dynamic_cast`, `_ZTVN10__cxxabiv`, `__aeabi_unwind`, `_ZSt7nothrow`, `_ZTI`, `__aeabi_`).
4. De-duplicates by name (first occurrence wins).
5. Emits the blob; if it is larger than the placeholder section, it errors out; otherwise it zero-pads to the placeholder size.

```text
.symtab  (all symbols, ~87 KB)
   |
   +-- skip [0, sh_info) ................ local symbols
   +-- drop st_value == 0 ............... undefined
   +-- drop st_shndx == 0 or >= 0xFFF1 .. section / ABS
   +-- keep bind == GLOBAL or WEAK
   +-- drop empty names
   +-- drop names starting '_' .......... unless whitelisted
   +-- de-duplicate by name
   v
SCLS table (~7.5 KB)
```

The `_` filter removes compiler/runtime internals that loaded code never calls by name, but the whitelist keeps the C++ ABI and libc helpers that **do** get imported (`operator new`, `__cxa_*`, `__aeabi_*`, RTTI/exception tables, etc.).

### 4.4 The blob format

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | magic `0x534C4353` — ASCII `SCLS`, little-endian |
| 4 | 4 | entry count `N` |
| 8 | 8 × `N` | `N` pairs: `u32 name_ptr`, `u32 address` |
| 8 + 8N | … | concatenated NUL-terminated symbol names |

`name_ptr` is an **absolute flash address** (`section address + offset`), and `address` is the symbol's absolute runtime address. Because the firmware is linked at a fixed address, both remain valid at runtime and the table can be read **in place from flash** — no RAM copy, no relocation, no pointer fixups.

### 4.5 How it is wired into the build

1. **Placeholder.** A C file (`symtab-blob.c`) defines a zero-filled `const` array in a dedicated section:

   ```c
   __attribute__((section(".scl_symtab"), used, aligned(4)))
   const uint8_t scl_symtab_blob[SCL_SYMTAB_BLOB_KB * 1024] = { 0 };
   ```

   The linker script keeps the section and gives it its own output section. `SCL_SYMTAB_BLOB_KB` defaults to 88 KB; the XIP newlib test uses 12 KB.

2. **Host tool.** `SCL-shared/CMakeLists.txt` compiles `tools/gen-symtab.cpp` + `common-elf-reader/src/elf-image.cpp` with the host `c++ -std=c++20` into `${CMAKE_BINARY_DIR}/tools/gen-symtab`, exposed as `gen-symtab-host`. It is built only if the source file exists.

3. **Post-link rewrite.** `scl_embed_symtab(<target>)` (defined in the hosted platform's `platform-library.cmake`) adds a POST_BUILD step:

   ```cmake
   "${SCL_HOST_GEN_SYMTAB}" $<TARGET_FILE:target> $<TARGET_FILE:target>.symtab.bin
   ${CMAKE_OBJCOPY} --update-section .scl_symtab=...symtab.bin $<TARGET_FILE:target>
   ```

   `objcopy --update-section` rewrites the section contents **without changing its address or size**, so nothing else in the image moves. This must run before `.hex`/`.bin` conversion (the tests call it exactly there).

4. **Runtime read.** `dl-impl.cpp` (and the XIP newlib `main.cpp`) reads the first 8 bytes, checks the `SCLS` magic and a non-zero count, then hands `&blob[8]` and `N` straight to the loader's `set_symtab()` — no heap, no semihosting, no ELF parsing. If the magic is absent (tool not run), the hosted app falls back to reading its own `.symtab` from the ELF over semihosting; the XIP app reports the table missing.

5. **Keep-alive anchors.** The application link adds `-Wl,-u,<symbol>` for every borrowed libc/libm/libstdc++ symbol the fixtures import. Without these, `--gc-sections` would discard symbols that `main` itself never references, and they would never reach the baked table — resulting in a runtime `HardFault`, not a link error.

As a schema:

```text
BUILD TIME (host)
  firmware.elf
      |
      |  gen-symtab: filter .symtab -> SCLS blob
      v
  blob = "SCLS" | count | (name_ptr, addr)[count] | names
      |
      |  objcopy --update-section .scl_symtab=blob
      v
  patched firmware.elf  --- flash --->  target

TARGET AT BOOT
  .scl_symtab (placeholder filled)
      |
      |  magic == 'SCLS' && count > 0 ?
      v
  link_context::set_symtab(entries, count)
      |
      |  resolve(name): linear strcmp over entries
      v
  module imports receive main's absolute addresses
```

### 4.6 When it is used

**In one sentence:** it is used when the firmware is a normal, non-`-pie`, hosted image that runs the **dynamic (`dlopen`) loader** and must expose `main`'s symbols to the loaded modules or to `dlsym()`.

Concretely, in this workspace:

| Where | Target | Loader |
|---|---|---|
| `SCL-TESTS-shared/dynamic-shared/test-dynamic/test1` | `scl-fw-posix-dl` | `scl::dynamic-newlib` (POSIX `dlopen` in RAM) |
| `SCL-TESTS-shared/dynamic-shared/test-xip/test-xip-dynamic-newlib/test1` | `scl-fw-xip-newlib1` | `scl::xip-dynamic-newlib` (XIP, 12 KB table) |
| `SCL-EXAMPLES-shared/float-dynamic`, `spifs-dynamic`, `combo-dynamic` | app target | `scl::dynamic-newlib` |
| `SCL-EXAMPLES-shared/float-dynamic-xip`, `spifs-dynamic-xip`, `combo-dynamic-xip` | app target | `scl::xip-dynamic-newlib` |

The necessary conditions are:

- the firmware is built with the **hosted toolchain** and `SCL_HOSTED=ON` (the newlib loader uses `std::vector` for its relocation plan);
- the image is **not `-pie`**, so there is no `.dynsym` to discover;
- the firmware must act as the **provider** of symbols (module imports, `dlsym`);
- the `.scl_symtab` placeholder and `--update-section` step are present.

### 4.7 When it is *not* used, and what is used instead

| Loader model | Address resolution instead | Why no embedded table |
|---|---|---|
| `scl::reloc` | `reloc::sym()` returns an address; `main` calls through a pointer; imports resolve against explicitly supplied symbols (or opt into `-pie` + `from_self`) | No GOT/PLT/`r10`; the application already knows the addresses it needs |
| `scl::static` (full RAM) | `elf_loader::symbol_table::add_global(name, addr)`, the loader's native API | The application supplies the map explicitly |
| `scl::xip-static`, `scl::optimal` | `link_context::from_self()` walking `main`'s real `.dynsym`; the firmware is `-pie` | `.dynsym` is already allocated, hashed and discoverable — a second table would be redundant |
| `scl::dynamic-nolib` (XIP test) | `-pie -Wl,-E` firmware: imports resolve against `main`'s `.dynsym` via the link context | Same reason; the freestanding loader can also take an explicit table if the firmware is not `-pie` |
| `scl::overlay` | Ordinary link-time binding | The module is linked with the firmware, so every name is already an address |

A subtlety worth knowing: the `static-shared`, `optimal-shared` and `overlay-shared` suites each carry a copy of `scl_embed_symtab` in their hosted platform, but their current tests never call it. The only suites that actually invoke it are the dynamic ones listed above.

### 4.8 Operational details and caveats

- **Size budget.** The table must fit the reserved section (`SCL_SYMTAB_BLOB_KB`, default 88 KB). The tool reports used bytes and fails the build if the table is larger. The filtered table is typically far smaller than the placeholder.
- **Whitelist maintenance.** If a newly loaded fixture imports a runtime helper whose name begins with `_` and is not whitelisted, resolution fails at runtime. The dynamic tests document extending the whitelist (e.g. `__cxa_*`, `__gxx_personality_v0`, `__dynamic_cast`, `_ZTVN10__cxxabiv*`).
- **Anchors are mandatory.** `-u` keeps borrowed symbols alive through `--gc-sections`; the table can only include symbols that still exist in `.symtab`.
- **Fixed-address assumption.** Absolute `name_ptr` and `addr` values are baked at build time, which is fine for the fixed-address firmware these projects produce. A relocatable firmware would need a different scheme.
- **Host requirements.** Building the tool needs a host C++20 compiler and the `common-elf-reader` sources; running the rewrite needs `objcopy` from the ARM toolchain.
- **Legacy variants.** The workspace also contains Python-era tools: `test/tools/gen-symtab.py` (post-link, `nm`-based), `tools/gen-symtab-objs.py` (pre-link, walks object files and emits `symtab.c`), and `cmake/embed-symtab.cmake` which references that pre-link script. Nothing in the current CMake build includes `embed-symtab.cmake`, and the pre-link script is not present in `SCL-shared/tools`; treat these as historical. The C++ `gen-symtab.cpp` + `scl_embed_symtab` path is the one in use.

### 4.9 When parsing and patching alone cannot resolve all symbols

The common reader and patcher can always **apply** a relocation, but they cannot **invent a symbol source**. They are insufficient whenever the name→address answer does not exist in the image, is not loaded yet, is not representable as a plain address, or cannot be patched in place. The following table maps each situation to the extra mechanism and to the fixtures that exercise it.

```text
Is a name -> address source present at runtime?
|
+-- main is -pie and has .dynsym --------> link_context::from_self()
+-- platform supplies a table -----------> set_symtab() / add_global()
+-- another loaded module defines it ----> handle resolver / load_set()
+-- no -----------------------------------+
        |
        +-- build-time table? -----------> gen-symtab + objcopy   (host)
        +-- boot-time sidecar ELF? ------> parse .symtab at boot  (§4.10)
        +-- can relink main as -pie? ----> .dynsym + from_self
        +-- small fixed export set? -----> compile-time registration
        +-- development/debug only? -----> semihosting ELF read
```

| Case | Why reader + patcher alone fail | What is added | Fixtures that exercise it |
|---|---|---|---|
| Hosted `dlopen` firmware is non-PIE | `main` has no `.dynsym`, and `.symtab` is not in flash; module→main imports and `dlsym` have no runtime map | `gen-symtab.cpp`, the `.scl_symtab` placeholder, `objcopy --update-section`, and `-Wl,-u` anchors | `dynamic-shared/test-dynamic/test1` (all 12: `libprov`, `libcons`, `libsh1/2`, `mylib1/2`, `libcppsh`, `libcppeh`, `ehx_catch`, `cpp3`, `c1`, `c2`), the XIP-newlib test, and the `*-dynamic` / `*-dynamic-xip` examples |
| Cyclic or forward cross-module imports | A single-module load resolves only against modules already placed; a reference to a not-yet-loaded module has no address | `reloc::load_set()` two-phase place-then-resolve; in dynamic loading, load order plus handle dependency refcounts; in optimal, fixed-address packing and load order | `c1` ↔ `c2` ↔ `cpp3` (reloc `load_set`, optimal 11-module set); `libcons`→`libprov`; `ehx_catch`→`libcppeh` |
| Code and GOT live in flash (XIP) | The runtime patcher writes RAM only; a PLT resolves its GOT slot PC-relatively from the flash image, so the RAM copy is never reached | Provisioning-time binding (`bind_host_imports`) before flashing, split `R-X` / `RW` images, and `pic_call_*` entry | All split XIP fixtures: `static-shared/test-xip/*`, `float-static`, `spifs-static`, `*-dynamic-xip` |
| Each library has its own PIC base (`r10`) | The resolver returns a correct address, but the callee needs its own `r10`; a direct call runs it with the caller's base and reads the wrong data | `xip_cross_call` / `dl_cross_call` trampolines that swap `r10` per call, or the no-`r10` models (`reloc`, `optimal`) | `c1`→`c2` and `libcons`→`libprov` in the r10-based dynamic/XIP models |
| TLS symbols | `R_ARM_TLS_*` entries are offsets into a per-thread block, not absolute addresses; `__tls_get_addr` needs a thread pointer and area | `scl::static` TLS runtime: `scl_tls_register/init_area/set_pointer` and a weak `__aeabi_read_tp` | `tls_lib` in `static-shared/test-static/test-static-direct` |
| Optimal model has no runtime name lookup | By design the loader never resolves names at runtime; modules are linked at fixed addresses | Build-time `gen-imports.cpp/.py` (call surface and load list), the `fixtures-src/build.sh` address packer, linker `-l:*.so`, and runtime `patch_main` | All 11 `optimal-shared` fixtures (`slib`, `mylib1/2`, `libprov`, `libcons`, `libcppsh`, `libcppeh`, `ehx_catch`, `cpp3`, `c1`, `c2`) |
| Main must supply an explicit map | A non-PIC `main` has no dynamic symbols to search | `symbol_table::add_global(name, addr)` filled by the application; no host tool | `float-static-full-ram`, `spi-static-full-ram` (HAL imports) |
| Borrowed runtime symbols are GC-able | `--gc-sections` deletes symbols that `main` never references, so they never reach any table | `-Wl,-u,<symbol>` anchors plus the `gen-symtab` underscore whitelist | `libcppsh` (`_Znwj`, `_ZdlPv`, RTTI vtables), `libcppeh`/`ehx` (`__cxa_*`, `__gxx_personality_v0`, `__dynamic_cast`), `cpp3` (`__aeabi_f*`, libm), `spifs` (`HAL_*`, `memcpy`) |

What is **not** a special case:

- **Ordinary relocations** — `ABS32`, `RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `TARGET1`, `REL32`, `THM_CALL` are handled by the common patcher; no extra tool.
- **Weak symbols** — `mylib1` (`foo`, `boo`, `boo2`) and `mylib2` (`mm`) exercise weak binding, but the core resolver already implements GLOBAL/WEAK precedence.
- **Plain C libraries with no imports** — `slib`, `libsh1`, `libsh2`, `float_add`/`float_mul`: parser plus patcher is sufficient end to end.

The rule of thumb:

- **Symbol source missing on target** → `gen-symtab` (plus anchors and whitelist). This is the only case that requires a host ELF tool — unless resolution is moved to boot (§4.10).
- **Symbol source incomplete at load time** → `load_set`, load order, or dependency handling.
- **Symbol cannot be patched in place** (XIP) → pre-bind at provisioning.
- **Symbol needs context, not just an address** (`r10`, TLS) → trampoline or TLS runtime.
- **No runtime lookup by design** (optimal, overlay) → build-time generation or linking.

The genuinely external **tools** are therefore `gen-symtab` (hosted dynamic), `gen-imports` (optimal), and the fixture packers; the rest are loader APIs, linker flags or small runtime helpers. If the build step itself is unacceptable, the next section analyses moving symbol resolution to boot.

### 4.10 Could symbol resolution be done at boot instead?

Short answer: **the logic can move into `common-elf-reader` as API calls; the build-time data preparation cannot disappear by doing so.** `gen-symtab` is already a thin wrapper around the parser. The reason it is a *host* program is not a missing API — it is that `.symtab` exists only in the ELF on the build machine, not in the MCU image. To remove the host step, the symbol bytes themselves must be made reachable at runtime.

#### 4.10.1 What API additions would look like

A useful refactor inside the reader would be callback-based, so it works both hosted and freestanding:

```cpp
// Walk defined GLOBAL/WEAK symbols, skipping locals via sh_info; no allocation.
template <class Fn> void for_each_symbol(elf_image& img, Fn&& fn);

// Serialize the current SCLS blob format into caller-provided storage.
result<std::size_t> build_symtab_blob(elf_image& img, std::span<std::byte> out,
                                      const symtab_filter& filter);

// Runtime view over an in-place table (what dl-impl.cpp hand-rolls today).
class symtab_view {
  std::uint32_t lookup(std::string_view name) const noexcept;
};
```

With these, `gen-symtab.cpp` becomes argument parsing plus file I/O, and the firmware stops duplicating the `magic/count/entries` reader. CMake still has to invoke **some** host executable after the link to produce the blob and run `objcopy --update-section`.

#### 4.10.2 Boot-time pipeline

If the data is available at runtime, the existing pieces already suffice:

1. **Obtain ELF bytes** — a sidecar ELF, an embedded symbol section, or a semihosting file (§4.10.3).
2. **Wrap it** in `loader::memory_byte_source` (already exists) or a file source.
3. **Parse** with `loader::elf_image::parse(src)` and `find_section(".symtab")`, `find_section(".strtab")`.
4. **Filter** exactly as `gen-symtab` does: start at `sh_info`, keep defined GLOBAL/WEAK symbols, apply the underscore whitelist, de-duplicate.
5. **Materialize** `firmware::sym_entry { const char* name; uint32_t addr; }` entries in RAM (or point `name` directly into a memory-mapped flash string table).
6. **Hand the table to the loader**: `xip::newlib::set_symtab(entries, count)` for the hosted loader (its `link_context` already has `set_symtab`), or an equivalent setter for `dynamic-nolib` (whose `link_context` currently only builds from `.dynsym`). For `scl::static`, the equivalent is filling `symbol_table::add_global(name, addr)`.

Sketch:

```cpp
extern "C" const std::byte _fw_elf_start[], _fw_elf_end[];   // linker-provided
std::span<const std::byte> elf{_fw_elf_start,
                               std::size_t(_fw_elf_end - _fw_elf_start)};
loader::memory_byte_source src{elf};
auto img = loader::elf_image::parse(src);
auto sym = img->find_section(".symtab");
auto str = img->find_section(".strtab");

firmware::sym_entry tab[MAX_SYMS];
std::size_t n = 0;
// ... walk sym->sh_info .. count, filter, validate st_name < str->sh_size ...
tab[n++] = { reinterpret_cast<const char*>(strtab + st.st_name), st.st_value };

xip::newlib::set_symtab(tab, n);        // table must stay alive
```

`st_value` is an absolute link-time address. It is valid only if the ELF is from the **exact same link** as the running firmware — a stale sidecar silently produces wrong jump targets. Check a build ID or image hash before use.

```text
power-on / loader init
   |
   v
sidecar ELF in flash --memory_byte_source--> elf_image::parse
   |
   v
find .symtab + .strtab
   |
   v
filter (sh_info, GLOBAL/WEAK, whitelist) --> sym_entry[] in RAM
   |
   v
set_symtab(entries, n)      (or add_global for scl::static)
   |
   v
loader resolves module imports once; table stays read-only
```

#### 4.10.3 Where the ELF/symbol bytes can come from

| Source | Host step | How it works | Main constraint |
|---|---|---|---|
| **Sidecar ELF or symbol-only ELF** in a reserved internal/external flash region | Standard flashing script writes both images; no custom tool | Linker reserves `_fw_elf_start/_end`; boot code parses the sidecar via `memory_byte_source` | Takes flash (full ELF, or `.symtab`+`.strtab` only); must match the firmware build |
| **Embedded `.symtab`/`.strtab` in the same image** | Effectively still a post-link step (`objcopy` flags or section extraction) | Place the symbol sections in a loadable flash region and parse them at boot | Chicken-and-egg: adding data that describes the layout changes the layout; linker scripts cannot reliably place `.symtab`; needs objcopy anyway |
| **Compile-time registration section** | None | Macros put `{name, &symbol}` entries in `.scl_syms`; linker symbols `__start_/__stop_` delimit it | Only symbols you annotate; C++/libc runtime symbols need wrappers; no automatic coverage |
| **`-pie -Wl,-E` and `.dynsym`** | None beyond the link | `.dynsym`/`.dynstr`/`.hash` are allocated; `link_context::from_self()` discovers them via `_DYNAMIC` | PIE/GOT/PLT cost and linker-script work; hash lookup is O(1); already used by the XIP nolib test |
| **Semihosting read of the host ELF** | None | `dl-impl.cpp` opens `FIRMWARE_ELF_PATH`, reads `.symtab`, interns names | Needs a debugger/host attached; very slow at boot; development only |
| **External storage / flat-fs, first-boot cache** | Programmer writes the ELF once | Parse on first boot, write a filtered table to a spare internal flash sector, reuse afterwards | Flash write/erase, power-fail safety, versioning; complex but production-viable |

The **self-embedding trap** is worth stating explicitly: a firmware cannot contain a symbol table describing its own final layout unless something runs *after* the link. Inserting the table changes sizes and possibly addresses; a second link invalidates the first link's `st_value`s. That is why the current design uses a **placeholder section + post-link rewrite**, and why the boot-time variants rely on a **sidecar** (data outside the image), **compile-time registration** (the linker builds the table in one pass), or **`.dynsym`** (the linker already emits it as allocated data).

Compile-time registration looks like this:

```cpp
#define SCL_REGISTER_SYM(sym)                                              \
  extern "C" __attribute__((used, section(".scl_syms")))                   \
  const firmware::sym_entry scl_sym_##sym = { #sym, (uint32_t)(uintptr_t)&sym }

/* .scl_syms : ALIGN(4) { __start_scl_syms = .; KEEP(*(.scl_syms));
                          __stop_scl_syms = .; } > FLASH */
```

It removes both the host tool and the boot parser, at the price of manual annotations and no automatic coverage of library/ABI symbols.

#### 4.10.4 Cost analysis

| Approach | Flash | RAM | Boot/load time | Symbol coverage | Complexity |
|---|---|---|---|---|---|
| Build-time filtered blob (current) | ~7.5 KB table | none (read in place) | zero | Automatic, policy-filtered | Low; needs post-link tool |
| Sidecar ELF + boot parse | Raw `.symtab` ~87 KB (or section-only sidecar) | Entry array: 8 B × N (2 048 → 16 KB) unless using a view | Parse once at boot; linear lookup per resolved import | Automatic, filtered at boot | Medium; build-ID check, lifetime rules |
| Embedded symbol sections | Similar, plus padding | Same | Same | Automatic | High; chicken-and-egg, objcopy still involved |
| Compile-time registration | 8–12 B per annotated symbol | none (table in flash) | zero | Only annotated symbols | Low mechanically, high in discipline |
| `-pie` / `.dynsym` | `.dynsym` + `.dynstr` + `.hash` | none | Hash lookup, O(1) | Exported globals; locals excluded | Medium; PIE code/linker script |
| Semihosting | 0 | Names/table (heap) | Very slow; host round-trips | Automatic | Low, development only |

Notes:

- `fw::link_context::resolve()` currently does a **linear `strcmp` scan** over the supplied table. That is fine for module-load-time resolution (dozens to hundreds of imports), not for per-call `dlsym`; cache resolved pointers.
- Names can point straight into a memory-mapped flash sidecar, so only the 8-byte entries need RAM. If the ELF is copied to a temporary buffer, that buffer (and its strings) must stay alive for the table's lifetime.
- Boot parsing adds code size (`elf_image` and the iterator), startup latency and a failure path. On an RTOS/multi-core system, do it once on the loader context before the scheduler/other cores start, then treat the table as read-only.
- If the sidecar lives in external flash, cache maintenance and memory attributes apply; validate bounds and the magic/count before trusting it.

#### 4.10.5 Recommendation

- **Production, constrained parts:** keep the build-time filtered blob. It is the smallest, fastest and most deterministic option, and the tool is already just a thin wrapper over the shared parser.
- **Cleaner code, same flow:** add `for_each_symbol`, `build_symtab_blob` and `symtab_view` to `common-elf-reader`; `gen-symtab` becomes a ~20-line CLI and the firmware stops hand-parsing the blob.
- **No post-link step allowed:** prefer `-pie -Wl,-E` (no table at all, O(1) lookup) where the toolchain and linker script permit it; otherwise use a **sidecar ELF + boot parser**, or compile-time registration for a small, fixed export surface.
- **Never** embed the firmware's own ELF inside itself without a post-link step; the self-embedding trap makes the addresses wrong.

In short: boot-time resolution is technically feasible with today's reader and `link_context::set_symtab`, but it trades flash, RAM, boot latency and build-mismatch risk for the removal of a small host step. The current design is an optimisation, not a limitation.

### 4.11 Is a fixture `.so` a sufficient ELF?

Short answer: **yes as an ELF image for the RAM-loading models, but no as a self-contained symbol universe — and no for the other loader models.**

A real fixture is a complete ELF32 shared object. For example, `dynamic-shared/test-dynamic/fixtures/c1.so` contains:

- `ELF32`, little-endian, `EM_ARM`, `ET_DYN`, hard-float EABI
- `.hash` (SysV), `.dynsym`, `.dynstr`
- `.rel.dyn`, `.rel.plt`
- `.got`, `.init_array`
- `.symtab`, `.strtab`, `.shstrtab` (not stripped)

```text
c1.so  (ELF32, LSB, EM_ARM, ET_DYN)
  .hash .dynsym .dynstr ... names + exports ....... resolver input
  .rel.dyn .rel.plt ....... relocation requests ... patcher input
  .got .................... slots patched at load
  .init_array ............. constructors, after patching
  .text .data .bss ........ PT_LOAD segments ...... copy / re-base
  .ARM.exidx .............. unwind metadata ....... C++ EH
  .symtab .strtab ......... not loaded ............ gen-symtab / reloc
```

That is sufficient for `scl::static` (RAM copy) and `scl::dynamic-newlib` / `dynamic-nolib` (RAM copy): the parser finds the sections it needs, the resolver uses `.hash`, and the patcher has relocation tables and a GOT. No companion file, archive or source header is required for loading.

#### What the same `.so` is not sufficient for

1. **It is not symbol-complete.** A `.so` defines its exports and leaves imports `UND` by design. It cannot know the addresses of `HAL_SPI_Init`, `printf`, `_Znwj` or another module's exports. Those come from the symbol source at load time: `main`'s `.dynsym`, an embedded symbol table, `add_global`, another loaded handle, or the `gen-symtab` blob. A `.so` alone is an ELF image, not a linked program.
2. **It is not XIP-ready.** `scl::xip-static` / `xip-dynamic-*` need a split `R-X | RW` image built with `-msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative`, `VA == file offset`, no relocations against text, and host imports pre-bound before flashing. A plain `-fPIC .so` has a PC-relative GOT and cannot execute in place.
3. **It is not the right format for `scl::reloc`.** That loader expects `ET_REL` (`.rel.o`, `-fno-pic -mlong-calls -r`), which has a real `.symtab` and no GOT/PLT. An `ET_DYN` `.so` is rejected.
4. **It is not the right shape for `scl::optimal`.** Optimal wants `-fno-pic -mlong-calls` modules linked at fixed, auto-packed addresses plus a load manifest — not a PIC shared object.
5. **It is not an overlay module.** `scl::overlay` builds modules with the firmware; there is no standalone `.so`.
6. **It is not enough if stripped incorrectly.** SCL parses **section headers** (`find_section`, `.symtab`, `.rel.*`). A `.so` stripped of its section table may still run under a Linux dynamic linker but will fail in SCL. Removing `.symtab` is fine for dynamic loading (as long as `.dynsym`/`.strtab` remain), but fatal for `scl::reloc`.
7. **It must have SysV `.hash`.** `symbol_resolver` walks `.hash` chains; a GNU-hash-only `.so` is not sufficient. The fixtures are explicitly built with `--hash-style=sysv`.

#### Per-model sufficiency

| Loader model | Plain `-fPIC .so` sufficient? | Required shape |
|---|---|---|
| `dynamic-nolib` / `dynamic-newlib` (RAM) | Yes | ELF32 LSB ARM `ET_DYN`, `.hash` + `.dynsym` + `.dynstr`, relocation sections, GOT, section headers |
| `static` (full RAM) | Yes | The same plain PIC image; no `r10` required |
| `xip-static` / `xip-dynamic-*` | No | Split `R-X` / `RW`, single-pic-base `r10`, `VA == file offset`, pre-bound host imports |
| `reloc` | No | `ET_REL` (`.rel.o`), `-fno-pic -mlong-calls -r`, real `.symtab` |
| `optimal` | No | `-fno-pic -mlong-calls`, fixed auto-packed addresses, manifest |
| `overlay` | No | Compiled and linked with the firmware; no standalone shared object |

```text
Valid ELF32 / LSB / EM_ARM + section headers + SysV .hash ?
  |
  +-- no --> rebuild / fix flags: --hash-style=sysv, keep section headers
  |
  +-- yes --> Which loader model?
                |
                +-- RAM dynamic / static --> image sufficient; still needs
                |                             a symbol source and load order
                +-- XIP --------------------> rebuild split R-X|RW + r10,
                |                             pre-bind imports, no text relocs
                +-- reloc ------------------> rebuild ET_REL, keep .symtab
                +-- optimal ----------------> rebuild -fno-pic fixed addresses,
                |                             run packer + gen-imports
                +-- overlay ----------------> link with the firmware; no .so
```

#### Case by case: what must be supplied when the `.so` is not enough

Two kinds of gaps exist. **Image-side gaps** (wrong PIC model, wrong ELF type, text relocations, missing sections) can only be fixed by rebuilding. **Loader-side gaps** (missing symbol source, load order, memory, runtime services) can be supplied by the application, the build system, or a loader API. The patcher can only apply what the ELF actually contains.

| Loading case | What the plain `.so` still lacks | What must be added for the loader/patcher to work | Where it comes from |
|---|---|---|---|
| **`dynamic-nolib`, RAM** | Addresses of `main`'s symbols; dependency order; supported hash | Make `main` `-pie -Wl,-E` so `link_context::from_self()` finds `.dynsym` (the nolib path has no external-symtab setter today); load dependencies before callers; keep SysV `.hash`; provide an image arena; grant enough handle slots (the fixed pool is 4) | Link flags, test/application load order, `scl::common` arena |
| **`dynamic-newlib`, RAM** | Same, but `main` is non-PIE | `gen-symtab` blob + `.scl_symtab` placeholder + `objcopy`; `-Wl,-u` anchors; underscore whitelist; load order or a dependency callback; image arena | §4 build wiring; test/application |
| **`static` (full RAM)** | `main`'s addresses; GOT/PLT slots for main→module calls; TLS area if used | Fill `symbol_table::add_global(name, addr)` for module→main imports; link `main` against the library (or call through `Module::sym()` / `operator[]`) so `patch_main()` has slots to fill; `scl_tls_prepare()` or a per-thread area for TLS; `dep_loader` for `DT_NEEDED`; quiesce execution before `unload()` | Application code, link line, `static/tls.h`, `Module` API |
| **`xip-static`, `xip-dynamic-*`** | A PIC `.so` cannot execute in place and its flash GOT cannot be patched at load | Rebuild with `-msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative`, split `R-X` / `RW`, `VA == file offset`, no relocations against text; place windows via the XIP linker script; pre-bind host imports (`bind_host_imports`) **before** flashing; provision the blob through the flat-fs; enter through `pic_call_*`; add `xip_cross_call`/`dl_cross_call` for lib→lib calls using different `r10` bases | `fixtures-src/build-fixtures.sh`, `xip-split.ld`, adapter/provisioning path, port layer |
| **`reloc` (ET_REL)** | A `.so` is the wrong ELF type; dynamic sections are irrelevant | Rebuild as `-fno-pic -mlong-calls -r`; keep `.symtab`/`.strtab` (not stripped); use only the supported types (`R_ARM_ABS32`, `TARGET1`, `REL32`, `THM_CALL`); supply a resolver for `main`; use `load_set()` (up to 4 modules) for forward/cyclic imports; provide the RAM arena; remove TLS/EH relocations the loader does not handle | `relocatable-shared` fixture recipe, `reloc-loader.hpp` |
| **`optimal`** | No runtime name lookup; no PIC/GOT machinery | Rebuild `-fno-pic -mlong-calls` at fixed addresses; run the three-pass packer so windows do not overlap; link `main` against the libraries (`-l:*.so`) so it gets a GOT slot per export; run `patch_main()`; generate the call surface/load list with `gen-imports`; load the whole set before calling across modules; keep exports GLOBAL/WEAK and unstripped | `fixtures-src/build.sh` (`manifest.txt`, `windows.generated.ld`), `gen-imports`, `optimal::load_and_link` |
| **`overlay`** | There is no loadable `.so` at all | Compile the modules **with** the firmware and place them in the shared window via the overlay linker fragment (LMA/VMA pairs); drive `scl_overlay_init/load`; guarantee no task/ISR/core executes the window during the `memcpy` (DSB/ISB only orders the local core) | `src/overlay/ldscripts/overlay-fragment.ld`, `scl::overlay` |

#### What the patcher can and cannot repair

- **Can repair at load time:** apply the supported relocations; fill the module GOT/PLT; rebase relative relocations; patch `main`'s GOT (`patch_main`); run `.init_array`/`.fini_array`; resolve names against any symbol source it is given (main's `.dynsym`, an embedded table, another loaded module); register/relocate `.ARM.exidx` for the dynamic loaders.
- **Cannot repair:** create a missing symbol source; make `main` export symbols it does not have; turn `ET_DYN` into `ET_REL` (or vice versa); make PIC text immutable for XIP; supply `r10` semantics to a plain `-fPIC` image; patch a flash-resident GOT from RAM; recover stripped section headers; replace a missing SysV `.hash`; add TLS support to the dynamic loaders; fix a float-ABI/Thumb mismatch; create `main` GOT slots that were never linked.

#### Cross-cutting ELF repairs

- Keep **section headers and `.shstrtab`** — SCL finds sections by name.
- Keep **`.symtab`/`.strtab`** for `scl::reloc` and for building `gen-symtab` from `main`; dynamic loading needs only `.dynsym`/`.dynstr`.
- Build with **SysV hash** (`--hash-style=sysv`) or extend the resolver.
- Match the **float ABI and Thumb state** of `main`; exports must be Thumb functions and their addresses carry bit 0.
- Avoid unsupported **relocation types** (rebuild with different flags or extend the patcher).
- Do not use **TLS** in modules loaded by the dynamic loaders; use `scl::static` if TLS is required.
- Build XIP images with **no text relocations**; otherwise the whole image must be copied to RAM.
- Keep `.ARM.exidx` when C++ exceptions/RTTI are used, and anchor the personality/runtime symbols.
- Link fixtures with **`-nostartfiles -Wl,-E --unresolved-symbols=ignore-all`** so they can carry `UND` imports.

#### Fixture-specific caveats

- The committed `test/fixtures/*.so` are built for the **r10 single-pic-base** static/XIP family. The dynamic tests **rebuild** them as plain `-fPIC -shared` (`test-dynamic/fixtures-src/build.sh`), because the newlib loader expects the plain-PIC shape. The same source produces different ELFs per model.
- Fixtures are linked with `-nostartfiles -Wl,-E --unresolved-symbols=ignore-all` so they can carry `UND` imports. A normal `-shared` link without that option may refuse to produce a fixture at all.
- C++ fixtures (`libcppsh`, `libcppeh`, `ehx_catch`, `cpp3`) additionally need the C++ runtime symbols from `main` and their `.ARM.exidx` for unwinding; the ELF is valid, but resolution is not self-contained.
- `DT_NEEDED` is not used by the fixtures; the dynamic test loads dependencies explicitly in order. The static loader can auto-load dependencies only through the application-supplied `dep_loader` callback.

#### Rule of thumb

A fixture `.so` is a **sufficient ELF** when the model is “copy the PIC image into RAM and patch it” and the missing imports are supplied by `main` or other modules. It is **not sufficient** when the model requires a different ELF shape (`ET_REL`, split XIP, no-PIC fixed windows), when it must resolve its own imports, or when the ELF has been stripped of the sections SCL reads.

---

## 5. Quick reference

| Question | Short answer |
|---|---|
| What is SCL? | Source-only loaders that let Cortex-M firmware run separately built code at runtime |
| What is `SCL-shared`? | The engine: seven loader models, shared core, ELF reader, host symtab tool |
| What is `SCL-TESTS-shared`? | One test suite per loader model, with platforms, fixtures, runners and docs |
| What is `SCL-EXAMPLES-shared`? | Complete WeAct apps (`float-*`, `spifs-*`, `combo-*`) for each model |
| What is `gen-symtab.cpp`? | A host tool that turns the firmware's ELF `.symtab` into a filtered, compact, flash-resident name→address table |
| Why is it needed? | A non-`-pie` hosted firmware has no runtime symbol table, but `dlopen`/`dlsym` and module imports need one |
| When is it used? | Hosted `scl::dynamic-newlib` / `scl::xip-dynamic-newlib` apps: the two dynamic test suites and the `*-dynamic` / `*-dynamic-xip` examples |
| When is it not used? | reloc, static-full-ram, XIP-static, optimal, dynamic-nolib (`-pie`) and overlay — they use explicit addresses, `.dynsym`/`from_self`, or link-time binding |
| How is it applied? | POST_BUILD: `gen-symtab fw.elf blob.bin` → `objcopy --update-section .scl_symtab=blob.bin fw.elf` |
| What does the blob look like? | `SCLS` magic, `u32` count, `count × (name_ptr, addr)`, NUL-separated names — read in place from flash |

---

## 6. RTOS and multi-core: how they change SCL and its usage

SCL was designed and tested for a **single-core, single-threaded, bare-metal Cortex-M4** (WeAct/NUCLEO F411, QEMU). Nothing in the loaders prevents using them under an RTOS or on a multi-core part, but several assumptions become visible the moment two execution contexts exist. This chapter separates what already helps, what breaks, and what an integrator must add.

### 6.1 The assumptions baked into the current design

| Assumption | Where it shows in the code | What violates it |
|---|---|---|
| One core owns the memory windows | `_simage_arena_start/_end` (`src/image-memory.cpp`), the fixed XIP text/data windows, the overlay window | A second core loading a different module into the same window |
| One loader instance, mutable global state | `g_handles[4]` and `g_sc` in `src/dynamic-nolib/dl.cpp`; `fw::image_arena()`; the TLS module registry `g_mods` in `src/static/tls.c` | Concurrent `dlopen`/`dlclose`, or a second core doing the same |
| Load/unload is serialized by the caller | `scl_loader_lock()`/`scl_loader_unlock()` weak no-ops in `src/static/module.cpp` (only the static loader has them) | An RTOS task and an ISR, or two tasks, entering the loader together |
| No cache maintenance | No D-cache/I-cache operations anywhere in `src/` | Cortex-M7 (or another cached core) executing freshly copied/patched code |
| One PIC base per execution context | `r10` set once by the firmware or swapped by `pic_call_*` / cross-call trampolines | Preemptive tasks that enter different PIC modules; a second core entering the same module |
| No per-thread state in the dynamic loaders | No `R_ARM_TLS_*` handling in `dynamic-nolib`/`dynamic-newlib`; `dlerror` backed by a global in the hosted facade | `thread_local` in a loaded module; per-task `errno`/stdio |
| Nobody executes a module while it is unloaded | `dlclose` refcounts dependencies, but cannot see tasks/ISRs already inside the module | Unload while a lower-priority task or an interrupt is executing module code |
| Fixed addresses everywhere | Arenas, windows and the `SCLS` blob all use absolute addresses baked at link time | Two firmware images (AMP) whose address maps collide |

### 6.2 What an RTOS changes

**(a) Mutual exclusion and reentrancy.** The static loader already exposes weak `scl_loader_lock()`/`scl_loader_unlock()` hooks, intended for exactly this: an RTOS defines strong versions backed by a mutex (or `__disable_irq`). The other loaders have no equivalent and must be wrapped by the application. Two cautions: the lock is held across the **whole** build, including constructors, I/O and symbol resolution — a non-recursive mutex plus a constructor that calls `dlopen` deadlocks; and a mutex held while reading a module over semihosting can block for milliseconds, causing priority inversion. A loader **task** with a request queue is usually better than calling the loader from arbitrary tasks.

**(b) Tasks and the PIC base (`r10`).** The XIP/PIC models reserve `r10` as the module's GOT base. On a preemptive RTOS, `r10` is part of the task context (standard Cortex-M ports save `r4`–`r11`), so switching tasks is safe — but the *value* must be correct for the module being entered. A bare `set_r10()` once at startup only works if one module is active for the whole system. The robust patterns are: call through the `pic_call_*` / cross-call trampolines (they save, swap and restore `r10` per call), or prefer the **no-`r10` models** (`reloc`, `optimal`) for code called from multiple tasks.

**(c) Lazy binding.** `scl::dynamic-nolib` binds `R_ARM_JUMP_SLOT` on first call (`src/port/lazy-resolve.c`, `lazy_resolve_slot`). Under preemption, two tasks can hit the same unresolved PLT slot simultaneously; the aligned 32-bit GOT store is atomic on ARM, so a torn pointer is unlikely, but the resolver path touches shared loader state and can resolve twice. Bind eagerly at load (`RTLD_NOW` semantics) or serialize first calls. Lazy binding also means the first call has unbounded latency — undesirable in a control loop.

**(d) TLS and per-task state.** Only `scl::static` has a TLS implementation: a shared per-thread area, `scl_tls_register/init_area/set_pointer`, and a weak `__aeabi_read_tp()` an RTOS can override (`src/static/tls.c`, `include/static/tls.h`). This is genuinely RTOS-ready — but the RTOS must allocate and activate one area per task, and note that `scl_tls_prepare()` uses `malloc`, so the RTOS heap must be thread-safe. The **dynamic loaders do not implement `R_ARM_TLS_*` at all**, so a module built with `thread_local` will not work through `dlopen`.

**(e) newlib reentrancy and the heap.** The `gen-symtab` whitelist deliberately exports `_impure_ptr` and `__errno`, which are process-global in non-reentrant newlib. In a multi-task system that means `errno` and `stdio` buffers are shared unless the RTOS switches `_impure_ptr` per task. Loaded modules that call `malloc` use the RTOS heap, not the SCL arena, so heap thread-safety, determinism and fragmentation become application concerns. The images themselves come from the fixed linker arena, which is good for determinism.

**(f) Lifecycle and ISR safety.** Loading and unloading must never happen in an ISR. Calling an already-loaded module from an ISR is fine, provided the module is never unloaded or patched while that interrupt can fire. `dlclose` refcounts only library-to-library dependencies; it cannot know that a task or ISR is inside the module. Real adoption therefore needs **quiescence**: unload only at a known safe point, defer it to a loader task, or simply never unload in steady-state operation. Constructors run in the loader's context (inside the lock), so they must not block indefinitely or assume a particular task priority.

**(g) Memory protection.** SCL is MPU-unaware. If tasks run in unprivileged mode, the MPU regions covering the arena and XIP windows must be programmed (execute-never for data, read-only/executable for code), and per-task MPU layouts must all include the shared module window. The overlay window is executable only while the overlay is resident. None of this is automatic.

**(h) Real-time determinism.** `dlopen` is not a bounded-time operation: it scans symbol/section tables, may read over semihosting, applies relocations and runs constructors. It belongs at boot or in a low-priority loader task. The fixed arena allocation is deterministic; the dynamic-newlib loader's use of `std::vector` is not. Keep interrupt paths limited to functions from modules loaded at initialisation.

**(i) POSIX semantics gap.** POSIX expects `dlopen` to be thread-safe and `dlerror` to be thread-local. The provided POSIX facade is neither (its error state is a single global). Applications should treat the loader as a controlled service, not as a drop-in libc replacement.

### 6.3 What multi-core changes

**(a) Shared loader state.** In an SMP setup (one firmware image, both cores) the globals listed in §6.1 are shared without atomics: the dynamic-nolib handle pool and section cache, the region allocator, and the TLS registry. Two cores calling `dlopen` concurrently corrupt them. Options are a hardware spinlock (plus RTOS mutex for tasks) around every loader entry point, or — usually simpler and safer — **one loader core** with the other core requesting loads over an inter-processor channel.

**(b) Fixed memory windows.** The arena and XIP windows are linker-reserved singletons. Two cores cannot load different module sets into the same window. Multi-core deployments must partition RAM per core (separate linker regions and separate `_simage_arena`/`_sxip_*` symbols per image) or serialize all loading on one core. The same applies to the flat-fs: concurrent provisioning of flash blobs must be serialized.

**(c) Caches and memory ordering.** The current targets are Cortex-M4 parts with no data cache, so "copy, relocate, execute" just works. On a Cortex-M7 (or any cached core) the sequence is different: after copying/relocating code in RAM, **clean the D-cache** so the writes reach memory, then **invalidate the I-cache** before executing; after patching a GOT slot, make the write visible and invalidate the stale line. `scl_memory_barrier()` (`include/scl/barrier.h`) emits `dsb`/`isb` but only orders the **local** core. Publishing a loaded module to another core needs a release/acquire pair (DMB or atomics) on a shared flag, plus an IPI/SEV to wake the other core. Sharing a cache line between a module's code and mutable data across cores is a correctness trap best avoided by layout.

**(d) Executing the same module on two cores.** Each core has its own `r10`, so a PIC module can be entered from both — if each entry sets `r10` to that module's GOT base (the trampolines do). But the module's own globals are then shared and unsynchronised; the module must be written reentrantly, or pinned to one core, or duplicated per core. SCL provides no per-core module registry or affinity mechanism.

**(e) SMP versus AMP, and `gen-symtab`.** In SMP (one image), one embedded `SCLS` table in read-only flash is shared safely by both cores. In AMP (two firmware images), each image has its own address map and its own `gen-symtab` output; a module loaded by core A cannot import symbols from core B's image, because the table only contains core A's addresses. Cross-core services must go through shared memory and an IPC protocol (OpenAMP-style), not through SCL symbol resolution. If both cores run SCL, each needs its own linker windows and its own table.

**(f) Flash, XIP and overlays.** Programming or erasing the flash that holds XIP code stalls or invalidates execution on every core using it; the other core must be quiesced. An overlay copy (`memcpy` into the shared window plus DSB/ISB) is inherently exclusive: swapping it while another core executes from that window is fatal. Multi-core overlay use requires strict single-owner semantics and cross-core synchronisation before and after the swap.

**(g) Interrupts and vector tables.** On dual-core MCUs each core has its own NVIC and vector table. SCL does not register interrupt handlers; a module that needs interrupts must have its handler wired by the application into the correct core's table. Deferred unload is often implemented with an IPI, which is again application code.

**(h) Validation gap.** The current test benches are single-core (F411 and QEMU). Nothing in the repository exercises cache maintenance, cross-core barriers or concurrent loading, and the build recipes are tuned for Cortex-M4 (the embed helper even compiles for `cortex-m4`). Any multi-core claim needs to be validated on the real part.

### 6.4 Per-model compatibility at a glance

| Loader model | RTOS fit | Multi-core fit | Main constraint / workaround |
|---|---|---|---|
| `scl::reloc` | Good | Good | Loader is stateless; `module`/`module_set`/arena are caller-owned. Give each task/core its own arena; no `r10`, plain calls |
| `scl::static` | Good | Moderate | Weak lock hooks and per-thread TLS already exist; add a cross-core lock, cache maintenance (M7) and quiescence before unload |
| `scl::xip-static` | Moderate | Moderate | `r10` must be per task/core and set per entry; RW window is shared — partition it or pin the module to one core |
| `scl::optimal` | Good | Moderate | No `r10` after loading; fixed flash/RAM windows must be partitioned per core; calls are ordinary |
| `scl::dynamic-nolib` | Poor | Poor | Global handle pool (4), global section cache, lazy binding; wrap in one loader task/core, bind eagerly, never unload with live users |
| `scl::dynamic-newlib` | Poor | Poor | Hosted heap, blocking semihosting reads, global error state; run only in a loader task; `gen-symtab` itself is unaffected |
| `scl::overlay` | Incompatible with preemption | Incompatible with sharing | One physical window by construction; disable interrupts/tasks and quiesce other cores around the swap |

### 6.5 Recommended adoption pattern

1. **Make loading a service.** One loader task (single core) or one loader core (multi-core), fed by a request queue; application tasks ask for handles, they do not call `dlopen` directly.
2. **Add a real lock.** Provide strong `scl_loader_lock/unlock` for `scl::static`; wrap the other loaders with an RTOS mutex and, for SMP, a hardware spinlock. If constructors may block or call back into the loader, make the lock recursive or move constructor execution outside it.
3. **Partition memory up front.** Separate linker regions and `_simage_arena`/`_sxip_*` symbols per core; separate flat-fs provisioning paths.
4. **Preload at initialisation; avoid `dlclose` in steady state.** If unloading is required, define a quiescence protocol (task barriers, interrupt masking, cross-core handshake) and only then call `dlclose`.
5. **Choose the model for the concurrency, not just the memory layout.** `reloc`/`optimal` avoid `r10` entirely and are the easiest to call from many tasks; PIC/XIP models need disciplined per-call `r10` handling.
6. **Bind eagerly and keep first calls off the real-time path.** Treat lazy binding as a debugging feature, not a production default.
7. **Use TLS only where it exists.** `scl::static` with a per-task TLS area is the supported path; avoid `thread_local` in modules loaded by the dynamic loaders.
8. **Do the cache/MPU/barrier work explicitly.** Clean D-cache and invalidate I-cache after loading on cached cores; configure MPU regions; publish readiness with barriers and IPIs.
9. **Test on the target.** The existing suites prove the mechanisms, not the concurrency; multi-core and preemptive behaviour must be validated with stress tests (parallel load/unload, priority inversion, unload under interrupt load).

### 6.6 Steady-state execution after load

Once the image is loaded and all its slots are resolved and bound, **SCL is no longer in the execution path**. A call into a module is an ordinary call: no loader lock, no loader state, no allocation, no syscall. What remains is a small, model-specific call-path cost, plus correctness constraints imposed by the module and the OS — not by the loader.

| Model | Call path after load | Persistent loader cost |
|---|---|---|
| `scl::reloc` | direct function pointer | none |
| `scl::static` (full RAM) | direct, GOT-indirect | none (eager binding) |
| `scl::xip-static` | `pic_call_*` entry trampoline (push/pop `r10` + `lr`); cross-module calls may route through `xip_cross_call` | a few cycles; a cross-call performs a linear scan per call in `xip_cross_lookup` unless bound |
| `scl::optimal` | direct, no `r10` | none |
| `scl::dynamic-nolib` | first call per slot goes through `dl_runtime_resolve`, then the slot is patched permanently; later calls are direct (one GOT load). With cross-call routing enabled, external lib→lib calls go through `dl_cross_call`, which scans relocation tables every call | resolver once per slot; trampoline permanently if cross mode is on |
| `scl::dynamic-newlib` | eager binding at load; plain GOT-indirect calls afterward | none |
| `scl::overlay` | ordinary linked call | none |

Practical consequence: `dlsym` is expensive per call, so cache the pointer; lazy binding is expensive only on the first call, which is the one place a call path touches loader code; PIC and cross-call trampolines cost cycles but never touch the loader's data structures.

The impacts that do persist are architectural:

- **Module-level concurrency (the dominant issue).** SCL does not serialize calls. A module called from two tasks or two cores runs concurrently, and its globals are shared. It must be reentrant, internally locked, or pinned to one task/core. The loader lock is held only during load/unload, so it never serializes execution.
- **RTOS runtime services.** `malloc`, newlib `errno`/stdio (the `gen-symtab` whitelist exports `_impure_ptr` and `__errno`), and FPU context belong to the OS. `-fno-threadsafe-statics` means magic statics are not guarded, and module calls run on the caller's task stack, so stack depth must fit.
- **`r10` discipline.** Context switches normally preserve `r10`; only the value on entry must be correct. The trampolines make that transparent for PIC models.
- **Lazy first calls.** They are unbounded and racy; resolve them before an interrupt or another task can hit the slot.
- **Lifecycle, not execution.** Unload or later patching requires quiescence (no task, ISR or core executing the module). Calling a loaded export from an ISR is fine unless unload can race it.
- **Multi-core cache effects.** On a cached core, clean D-cache and invalidate I-cache once before first execution (and after any patch). After that, read-only code fetching is normal; shared module data needs the usual barriers and atomics, and publishing a freshly loaded module cross-core needs one release/acquire pair plus an IPI — not per call.
- **XIP and overlay specifics.** Concurrent instruction fetch from the same flash is fine; programming that flash is not. An overlay window cannot be executed by another context during its `memcpy` swap.

In short, execution performance after resolution is essentially unaffected: the loader contributes at most a trampoline, and in cross-call mode a per-call lookup. The real impact is architectural — the OS and multi-core environment change *what may run concurrently and when it is safe to unload or patch*, not the cost of a resolved call.

### 6.7 Load-first, then start the OS/cores

A common and recommended deployment is: do all loading on one core, with the scheduler stopped, and only then start the RTOS or the other cores. The precise claim it supports is narrower than "nothing changes":

> Loading everything on one core before the scheduler/other cores start, and never loading, unloading, patching or `dlsym`-ing again, removes **loader-state races**. It does not remove the constraints that apply to *executing* the module.

```text
LOAD PHASE (one context)                    EXECUTE PHASE (many contexts)
---------------------------                 -----------------------------
load images                                 direct calls into modules
bind every slot eagerly                     module globals are shared
run constructors                            -> reentrant, locked, or pinned
clean D-cache / invalidate I-cache          no loader in the call path
set r10 per core/task on entry              no dlopen / dlclose / dlsym
publish with a release barrier              no overlay swap
        |                                            ^
        +------------ start OS / other cores --------+
                     (quiesce before any unload)
```

**What this ordering neutralizes**

- Concurrent `dlopen`/`dlclose` corrupting `g_handles`, `g_sc`, the arena allocator or the TLS registry — gone, because only one context ever runs the loader.
- Loader lock contention and priority inversion during load — gone.
- Cross-core publication races for a module being built — gone, provided the module is published with a barrier before the other core is released.

**What still applies after the handoff**

| Concern | Why it survives the ordering | What to do |
|---|---|---|
| Lazy binding | `scl::dynamic-nolib` re-points `.rel.dyn` `JUMP_SLOT` slots at `dl_runtime_resolve` after the eager pass, so the first call after start still executes loader code; concurrent first calls race | Pre-trigger every import before the handoff, or use an eager model (`scl::static`, `scl::dynamic-newlib`); `.rel.plt` slots are not re-armed |
| Cross-call routing | With `g_cross_call` on, every external lib→lib call goes through `dl_cross_call`, which scans relocation tables per call | Accept the cost; the registry must remain immutable (no later load/unload) |
| Module reentrancy | Two tasks or cores share the module's globals; SCL never serializes calls | Make the module reentrant, lock internally, or pin it to one task/core |
| `r10` per context | Each core has its own `r10`; a `set_r10()` on the loader core does not propagate, and each task context needs the right value | Enter through `pic_call_*` trampolines, or use the no-`r10` models (`reloc`, `optimal`) |
| Caches and MPU | The loader core may have copied code through a dirty D-cache; the second core needs execute permission and valid instruction cache | Clean D-cache, invalidate I-cache per core before first execution; configure MPU and matching memory attributes on every core; M4 has no data cache, M7 does |
| RTOS runtime services | `malloc`, newlib `errno`/stdio (`_impure_ptr`), FPU context, task stack depth, per-task TLS (only `scl::static`) | Provide thread-safe heap, per-task newlib/TLS setup, FP context and adequate stacks; `-fno-threadsafe-statics` leaves magic statics unguarded |
| Later lifecycle operations | `dlclose`, `dlsym`, overlay swaps — or any new `dlopen` — reintroduce loader execution and patching | Do not perform them after the handoff unless every task, ISR and core using the module is quiesced; refcounts do not observe execution |

**SMP versus AMP.** In SMP (one image, both cores) the module lives in shared RAM and is callable from both cores, subject to the rows above. In AMP (separate image per core) the module loaded by core A is simply not in core B's address map or symbol table; sharing it requires a fixed-address agreement, shared memory, cache/barrier handling and an IPC layer. Loading on one core changes nothing for the other in that case.

**Handoff checklist:** load on one core with the scheduler stopped → bind eagerly or pre-trigger every lazy slot → clean D-cache / invalidate I-cache on every core that will execute the module → set `r10` per core/task on entry → publish the module with a release barrier → start the other cores/tasks → never `dlopen`/`dlclose`/`dlsym`/swap afterwards without quiescence.

### 6.8 Bottom line

An RTOS mainly forces SCL to become **a serialized service with an explicit lifecycle**; multi-core mainly forces it to become **a partitioned service with cache- and barrier-aware publication**. The mechanisms are largely present — weak lock hooks, a stateless `reloc` loader, per-thread TLS in `scl::static`, per-call `r10` trampolines, and a read-only build-time symbol table — but none of the loaders is thread-safe or multi-core-safe out of the box. The practical rule is: **one loader context, partitioned windows, eager binding, no unload while code is live, and explicit cache/MPU management**. Loaded modules should be treated as shared, reentrant code with a clearly documented owner; if they are not, pin them to a single task or core.

---

## 7. Tool inventory: what lives outside the reader/patcher

`gen-symtab.cpp` is the most visible host tool, but it is not the only piece of SCL-adjacent machinery that sits outside `common-elf-reader` and the relocation/loader sources. The core is deliberately free of host dependencies; everything below is either **build-time tooling** or **runtime support** layered around it.

### 7.1 Host-side code generators

| Tool | Canonical location | Type | Input → output | Used by |
|---|---|---|---|---|
| `gen-symtab.cpp` | `SCL-shared/tools/` | C++20, links `common-elf-reader` | firmware `.elf` → `SCLS` binary blob for `.scl_symtab` | Hosted `dynamic-newlib` and XIP-newlib apps (§4) |
| `elf2json.cpp` | `SCL-shared/common-elf-reader/tools/` | C++20 | `.elf` → JSON (header, PHDRs, sections, symbols, dynamic, PLT) | Manual host build; feeds the **AetherELF** visualizer in `common-elf-reader/gui/` (HTML/JS). Analysis/debug only |
| `gen-imports.cpp` | `SCL-TESTS-shared/optimal-shared/tools/` | C++20, parses `.dynsym`/`.dynstr` itself | all fixture `.so` → `imports.generated.h` + `modules.generated.inc` | `optimal-shared`; built by CMake as the `gen-imports` target and run at configure time |
| `gen-imports.py` | same directory | Python, shells out to `arm-none-eabi-readelf` | same outputs | Fallback when no host C++ compiler is found |
| `gen-symtab.py` | `dynamic-shared/tools/` and each suite's `test/tools/` | Python, `nm`-based | `.elf` → **C source** array of `{name, address}` | Legacy; current CMake uses `gen-symtab.cpp` |
| `gen-symtab-objs.py` | `dynamic-shared/tools/` | Python, `nm`-based | `.o` files → `symtab.c`, compiled and linked **pre-link** | Legacy; referenced by `cmake/embed-symtab.cmake`, but nothing includes that file and the script is absent from `SCL-shared/tools` |
| `tools/build.sh` | `dynamic-shared/tools/`, `common-elf-reader/tools/`, `optimal-shared/tools/` | shell | — | Compiles the host tools above; not itself a tool |

Two distinctions explain why the C++ tools replaced the Python ones:

- **Binary blob versus C source.** `gen-symtab.cpp` produces a binary image that `objcopy --update-section` patches into an existing ELF, with no recompile. The Python variants emit C code that must be rebuilt into the firmware.
- **Post-link versus pre-link.** `gen-symtab.cpp` reads the finished ELF; `gen-symtab-objs.py` walks object files before linking. Only the post-link path is wired into current builds.

### 7.2 Build-time generators and runners (not C++ tools)

These shape the fixtures and drive the builds, and are equally outside the reader/patcher:

- **`optimal-shared/fixtures-src/build.sh`** — three-pass fixed-address packer: builds at a placeholder base, measures `PT_LOAD` sizes with `readelf`, packs modules into the reserved windows, re-links, and emits `manifest.txt`, `link.txt` and `windows.generated.ld`.
- **`dynamic-shared/test-dynamic/fixtures-src/build.sh`** — rebuilds the fixtures as plain `-fPIC -shared` for the newlib loader.
- **`dynamic-shared/test-xip/fixtures-src/build-fixtures.sh`** (and `build-xip-libs.sh`) — builds split `R-X | RW`, single-pic-base (`r10`) libraries for XIP.
- **`static-shared/test-xip/fixtures-src/build-*.sh`** and per-app `fixtures-src/build.sh` — XIP, no-PIC and SPI-flash fixtures. The relocatable suite builds its `ET_REL` fixtures from CMake instead.
- **Runners and automation** — `build-and-run.sh` per suite/app, `flash-run*.sh`, `qemu-run.sh`, `qemu-gdb.sh`, `marker-run.sh`, `run-with-tmp-fixtures.sh`, and `tools-config.sh` (toolchain/OpenOCD discovery).
- **`docs/make-pdfs.py`** — Markdown → PDF renderer used to produce this document.

### 7.3 Runtime components outside the reader/patcher

Inside the engine, but outside `common-elf-reader` and the relocation logic:

- `src/port/` — PIC trampolines, lazy resolver, cross-library call, EHABI exidx lookup.
- `src/image-memory.cpp`, `src/region-allocator.cpp` — the RAM arena.
- `src/static/tls.c` — per-thread TLS area and `__tls_get_addr`.
- `src/overlay/overlay.c` — `memcpy` + DSB/ISB overlay manager.
- `src/xip/` — segment classification, relocation, compaction and the XIP adapters.
- `src/log.c`, `src/scl-log-put-hex.c` — logging hooks.
- Test/platform side: flat-fs (`fs-archive.cpp`, `flash-prog.c`), semihosting file I/O, `symtab-blob.c`, the `dl::` POSIX facade, libc shim.

### 7.4 Duplication and legacy notes

Every example vendors a complete `SCL-shared` checkout, and every test suite keeps a copy of `test/tools/gen-symtab.py`, so these filenames appear many times across the workspace. The canonical implementations are `SCL-shared/tools/gen-symtab.cpp`, `SCL-shared/common-elf-reader/tools/elf2json.cpp`, and `optimal-shared/tools/gen-imports.{cpp,py}`. The Python symbol-table tools and `cmake/embed-symtab.cmake` are historical; the active build path is `gen-symtab.cpp` + `scl_embed_symtab` (post-link `objcopy` rewrite).
