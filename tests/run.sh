#!/usr/bin/env bash
# Build and run the host-side tests for the new SCL APIs.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${OUT:-/tmp/scl-new-tests}"
ELF="${ELF:-$ROOT/common-elf-reader/test/411ce-100Mhz.elf}"
mkdir -p "$OUT"

INC=(-I"$ROOT/include" -I"$ROOT/include/scl" -I"$ROOT/common-elf-reader/include")
CXX="${CXX:-g++}"

echo "=== build symtab test ==="
"$CXX" -std=c++20 -O1 -g "${INC[@]}" \
    "$HERE/symtab_test.cpp" "$ROOT/src/symtab-runtime.cpp" \
    "$ROOT/common-elf-reader/src/elf-image.cpp" \
    -o "$OUT/symtab-test"

echo "=== build link_context test ==="
"$CXX" -std=c++20 -O1 -g "${INC[@]}" \
    "$HERE/linkctx_test.cpp" "$ROOT/src/link-context.cpp" \
    "$ROOT/src/dynamic-info.cpp" "$ROOT/src/symbol-resolver.cpp" \
    -o "$OUT/linkctx-test"

echo "=== run ==="
"$OUT/symtab-test" "$ELF"
"$OUT/linkctx-test"
echo "all host tests: PASS"
