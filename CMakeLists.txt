# SCL — Shared-Code Loader for Cortex-M (ARM32), as INTERFACE source libraries.
#
# This repo is ONLY the loader sources + common-elf-reader (submodule). No tests,
# no platform, no firmware. Every target is an INTERFACE "source-code library":
# its .cpp compile into the CONSUMER's firmware with the consumer's own flags —
# there are no .a archives.
#
# Seven loader cases (pick the one that fits — see LOADER-CHOICE in the tests repo):
#
#   Case             Target              Module built            Runs code from  r10
#   relocatable      scl::reloc          ET_REL  (-fno-pic -r)   RAM (arena)     free
#   static-full-ram  scl::static         -fPIC .so               RAM (buffer)    free
#   static (XIP)     scl::xip-static     -fPIC split .so         flash (XIP)     used
#   optimal (XIP)    scl::optimal        -fno-pic fixed windows  flash-window    free
#   dynamic (nolib)  scl::dynamic-nolib  -fPIC .so, dlopen       RAM (buffer)    used
#   dynamic (newlib) scl::dynamic-newlib -fPIC .so, POSIX dlopen  RAM (buffer)   used
#   overlay          scl::overlay        (app's own modules)     flash overlay   free
#     (dynamic-newlib needs -DSCL_HOSTED=ON; XIP variants: scl::xip-dynamic-{nolib,newlib})
#
# Consume it:
#   add_subdirectory(SCL scl)
#   target_link_libraries(my_fw PRIVATE scl::reloc)     # or scl::static / xip-static / optimal
# Or select one with the option and link the generic alias:
#   cmake -DSCL_LOADER=optimal ...   ->   target_link_libraries(my_fw PRIVATE scl::loader)
cmake_minimum_required(VERSION 3.20)
project(scl LANGUAGES C CXX)

set(SCL ${CMAKE_CURRENT_SOURCE_DIR})

# ---- common-elf-reader (git submodule): common-elf-reader::elf-reader ---------
if(NOT EXISTS "${SCL}/common-elf-reader/CMakeLists.txt")
    message(FATAL_ERROR "common-elf-reader submodule missing — run: git submodule update --init")
endif()
add_subdirectory(common-elf-reader EXCLUDE_FROM_ALL)

# ---- scl::common — the shared core (parser/resolver/relocation/log) -----------
add_library(scl-common INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-common INTERFACE
    ${SCL}/src/dynamic-info.cpp
    ${SCL}/src/lazy-resolve.cpp
    ${SCL}/src/region-allocator.cpp
    ${SCL}/src/reloc-applier.cpp
    ${SCL}/src/symbol-resolver.cpp
    ${SCL}/src/symtab-runtime.cpp
    ${SCL}/src/port/os-hooks.c
    ${SCL}/src/port/cache.c
    ${SCL}/src/log.c
    ${SCL}/src/scl-log-put-hex.c)
target_include_directories(scl-common INTERFACE ${SCL}/include ${SCL}/include/scl)
target_link_libraries(scl-common INTERFACE common-elf-reader::elf-reader)
target_compile_features(scl-common INTERFACE cxx_std_20)
# SCL_HOSTED: parts needing a hosted C++ library (std::vector) — the relocator plan
# builder + elf_image::collect_relocations, required by scl::dynamic-newlib.
option(SCL_HOSTED "Build hosted-only parts (std::vector): relocator, scl::dynamic-newlib" OFF)
if(SCL_HOSTED)
    target_sources(scl-common INTERFACE ${SCL}/src/relocator.cpp)
    target_compile_definitions(scl-common INTERFACE SCL_HOSTED=1)
endif()
add_library(scl::common ALIAS scl-common)

# ---- scl::self-resolve — resolve module imports against main's own .dynsym ----
# link_context::from_self() reads _DYNAMIC, so it only works in a -pie firmware.
# Kept OUT of scl::common so the non-pie loaders (reloc, static) don't pull it.
# Link it ALONGSIDE scl::reloc for a -pie reloc firmware that wants from_self.
add_library(scl-self-resolve INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-self-resolve INTERFACE ${SCL}/src/link-context.cpp)
target_link_libraries(scl-self-resolve INTERFACE scl-common)
add_library(scl::self-resolve ALIAS scl-self-resolve)

# ---- scl::reloc — RAM / ET_REL  (case: relocatable) --------------------------
add_library(scl-reloc INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-reloc INTERFACE ${SCL}/src/reloc/reloc-loader.cpp)
target_include_directories(scl-reloc INTERFACE ${SCL}/include/reloc)
target_link_libraries(scl-reloc INTERFACE scl-common)
add_library(scl::reloc ALIAS scl-reloc)

# ---- scl::static — -fPIC .so copied WHOLE into RAM  (case: static-full-ram) ---
add_library(scl-static INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-static INTERFACE
    ${SCL}/src/static/elf-reader.cpp
    ${SCL}/src/static/shlib-loader.cpp
    ${SCL}/src/static/logger.cpp
    ${SCL}/src/static/module.cpp
    ${SCL}/src/static/tls.c
    ${SCL}/src/port/os-hooks.c
    ${SCL}/src/port/cache.c)
target_include_directories(scl-static INTERFACE ${SCL}/include ${SCL}/include/static)
target_link_libraries(scl-static INTERFACE common-elf-reader::elf-reader)
add_library(scl::static ALIAS scl-static)

# ---- scl::xip-common — execute-in-place primitives ---------------------------
add_library(scl-xip-common INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-xip-common INTERFACE
    ${SCL}/src/xip/segment-map.cpp
    ${SCL}/src/xip/relocate.cpp
    ${SCL}/src/xip/compact.cpp)
target_include_directories(scl-xip-common INTERFACE ${SCL}/include/xip)
target_link_libraries(scl-xip-common INTERFACE scl-common)
add_library(scl::xip-common ALIAS scl-xip-common)

# ---- scl::xip-static — XIP + r10  (case: static / xip-static) ----------------
# -pie firmware: bundles the r10 trampolines + image-memory + self-resolve.
add_library(scl-xip-static INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-xip-static INTERFACE
    ${SCL}/src/xip/static/adapter.cpp
    ${SCL}/src/xip/xip-cross-call.c
    ${SCL}/src/port/pic-trampoline.c
    ${SCL}/src/image-memory.cpp)
target_include_directories(scl-xip-static INTERFACE ${SCL}/include/xip ${SCL}/include/scl)
target_link_libraries(scl-xip-static INTERFACE scl-xip-common scl-self-resolve)
add_library(scl::xip-static ALIAS scl-xip-static)

# ---- scl::loader-nopic / scl::optimal — XIP, no r10  (case: optimal) ---------
add_library(scl-loader-nopic INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-loader-nopic INTERFACE
    ${SCL}/src/xip/static/nopic.cpp
    ${SCL}/src/image-memory.cpp)
target_include_directories(scl-loader-nopic INTERFACE ${SCL}/include/xip ${SCL}/include/scl)
target_link_libraries(scl-loader-nopic INTERFACE scl-xip-common scl-self-resolve)
add_library(scl::loader-nopic ALIAS scl-loader-nopic)

add_library(scl-optimal INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-optimal INTERFACE ${SCL}/src/loader.cpp)
target_include_directories(scl-optimal INTERFACE ${SCL}/include/optimal)
target_link_libraries(scl-optimal INTERFACE scl-loader-nopic)
add_library(scl::optimal ALIAS scl-optimal)

# ---- scl::overlay — linker-overlay window manager (case: overlay) ------------
# Portable C (the DSB/ISB barrier compiles away off-target). The application owns
# the linker script; a template is in src/overlay/ldscripts/overlay-fragment.ld.
add_library(scl-overlay INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-overlay INTERFACE ${SCL}/src/overlay/overlay.c)
target_include_directories(scl-overlay INTERFACE ${SCL}/include/overlay)
target_link_libraries(scl-overlay INTERFACE scl-common)
add_library(scl::overlay ALIAS scl-overlay)

# ---- scl::dynamic-nolib — freestanding dlopen  (case: dynamic, no libc) -------
# dlopen_source/dlsym/dlclose over any byte_source; bundles the Cortex-M port
# layer (VFP asm -> hard-float) + image-memory + link-context.
add_library(scl-dynamic-nolib INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-dynamic-nolib INTERFACE
    ${SCL}/src/dynamic-nolib/dl.cpp
    ${SCL}/src/port/pic-trampoline.c
    ${SCL}/src/port/dl-cross-call.c
    ${SCL}/src/port/lazy-resolve.c
    ${SCL}/src/port/unwind-find-exidx.c
    ${SCL}/src/image-memory.cpp
    ${SCL}/src/link-context.cpp)
target_include_directories(scl-dynamic-nolib INTERFACE ${SCL}/include ${SCL}/include/dynamic-nolib)
target_link_libraries(scl-dynamic-nolib INTERFACE scl-common)
add_library(scl::dynamic-nolib ALIAS scl-dynamic-nolib)

# ---- scl::dynamic-newlib — hosted POSIX dlopen  (case: dynamic, newlib) -------
# link-time binding + runtime GOT patching with a journaled revert; uses
# std::vector, so consumers must configure -DSCL_HOSTED=ON.
add_library(scl-dynamic-newlib INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-dynamic-newlib INTERFACE
    ${SCL}/src/dynamic-newlib/dynamic-linker.cpp
    ${SCL}/src/dynamic-newlib/patch-journal.cpp
    ${SCL}/src/dynamic-newlib/image-memory.cpp
    ${SCL}/src/dynamic-newlib/link-context.cpp)
target_include_directories(scl-dynamic-newlib INTERFACE ${SCL}/include ${SCL}/include/dynamic-newlib)
target_link_libraries(scl-dynamic-newlib INTERFACE scl-common)
add_library(scl::dynamic-newlib ALIAS scl-dynamic-newlib)

# ---- XIP adapters for the two dynamic loaders --------------------------------
add_library(scl-xip-dynamic-nolib INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-xip-dynamic-nolib INTERFACE
    ${SCL}/src/xip/dynamic-nolib/adapter.cpp
    ${SCL}/src/xip/dynamic-nolib/xip-cross-call.c)
target_link_libraries(scl-xip-dynamic-nolib INTERFACE scl-xip-common scl-dynamic-nolib)
add_library(scl::xip-dynamic-nolib ALIAS scl-xip-dynamic-nolib)

add_library(scl-xip-dynamic-newlib INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-xip-dynamic-newlib INTERFACE
    ${SCL}/src/xip/dynamic-newlib/adapter.cpp
    ${SCL}/src/xip/dynamic-nolib/xip-cross-call.c
    ${SCL}/src/port/pic-trampoline.c
    ${SCL}/src/port/unwind-find-exidx.c)
target_link_libraries(scl-xip-dynamic-newlib INTERFACE scl-xip-common scl-dynamic-newlib)
add_library(scl::xip-dynamic-newlib ALIAS scl-xip-dynamic-newlib)

# ---- SCL_LOADER option: alias scl::loader to the chosen case -----------------
set(SCL_LOADER "" CACHE STRING
    "Pick one loader case: relocatable | static-full-ram | static | optimal | dynamic-nolib | dynamic-newlib | overlay (empty = link a scl::<target> directly)")
set_property(CACHE SCL_LOADER PROPERTY STRINGS relocatable static-full-ram static optimal dynamic-nolib dynamic-newlib overlay)

if(SCL_LOADER STREQUAL "relocatable")
    add_library(scl::loader ALIAS scl-reloc)
elseif(SCL_LOADER STREQUAL "static-full-ram")
    add_library(scl::loader ALIAS scl-static)
elseif(SCL_LOADER STREQUAL "static")
    add_library(scl::loader ALIAS scl-xip-static)
elseif(SCL_LOADER STREQUAL "optimal")
    add_library(scl::loader ALIAS scl-optimal)
elseif(SCL_LOADER STREQUAL "dynamic-nolib")
    add_library(scl::loader ALIAS scl-dynamic-nolib)
elseif(SCL_LOADER STREQUAL "dynamic-newlib")
    add_library(scl::loader ALIAS scl-dynamic-newlib)
elseif(SCL_LOADER STREQUAL "overlay")
    add_library(scl::loader ALIAS scl-overlay)
elseif(NOT SCL_LOADER STREQUAL "")
    message(FATAL_ERROR "SCL_LOADER must be one of: relocatable | static-full-ram | static | optimal | dynamic-nolib | dynamic-newlib | overlay")
endif()
if(SCL_LOADER)
    message(STATUS "SCL: scl::loader -> ${SCL_LOADER}")
endif()

# ---- gen-symtab host tool (for the hosted dynamic-newlib symbol-table embed) --
# Compiled for the HOST machine (the ARM toolchain here is for firmware). The
# hosted platform's scl_embed_symtab() bakes main's symbols into flash with it.
set(SCL_HOST_GEN_SYMTAB "${CMAKE_BINARY_DIR}/tools/gen-symtab" CACHE INTERNAL "")
if(EXISTS "${SCL}/tools/gen-symtab.cpp" AND NOT TARGET gen-symtab-host)
    add_custom_command(
        OUTPUT "${SCL_HOST_GEN_SYMTAB}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tools"
        COMMAND c++ -std=c++20
                "${SCL}/tools/gen-symtab.cpp"
                "${SCL}/common-elf-reader/src/elf-image.cpp"
                "-I${SCL}/common-elf-reader/include"
                -o "${SCL_HOST_GEN_SYMTAB}"
        DEPENDS "${SCL}/tools/gen-symtab.cpp"
                "${SCL}/common-elf-reader/src/elf-image.cpp"
                "${SCL}/common-elf-reader/include/loader/elf-image.hpp"
        COMMENT "Building C++20 gen-symtab host tool" VERBATIM)
    add_custom_target(gen-symtab-host DEPENDS "${SCL_HOST_GEN_SYMTAB}")
endif()
