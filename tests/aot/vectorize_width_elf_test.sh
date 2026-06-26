#!/bin/bash
# Increment 1 (float-AOT target-aware): el ancho SIMD del vectorizador en AOT lo
# fija el TARGET (--float-isa), no el host de build.  Default = sse2 -> 128b
# (cross-compile-safe: corre en cualquier x86-64, no emite ymm/zmm del host).
#
# Verifica que aot/182_vectorize_elementwise compilado a AOT default:
#   1. NO contiene ymm/zmm (128b puro, no hereda el AVX2/AVX512 del host).
#   2. Corre correcto (180960 & 0xFF = 224).
# El runtime de excepciones NO aplica (sin try/catch); enlace gcc directo.
#
# Se corre desde WSL:  wsl bash ./tests/aot/vectorize_width_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

PROG="examples_codes_vex/182_vectorize_elementwise.vex"
"$VM" --vex "$PROG" -m aot --format elf --emit obj --aot-arch x86-64 \
      -o "$BUILD/vw.o" >/dev/null 2>&1
[ -f "$BUILD/vw.o" ] || { echo "FALLO: no se genero el .o"; rm -rf "$WORK"; exit 1; }
cp "$BUILD/vw.o" "$WORK/vw.o"; rm -f "$BUILD/vw.o"

# 1. cross-compile-safe: NADA de ymm/zmm en el default (sse2 -> 128b).
wide=$(objdump -d "$WORK/vw.o" 2>/dev/null | grep -ciE 'ymm|zmm')
if [ "$wide" = "0" ]; then
  echo "WIDTH: default sse2 = 128b puro (0 ymm/zmm) OK"
else
  echo "WIDTH: FALLO -- $wide instrucciones ymm/zmm (deberia ser 128b sse2)"; rc=1
fi

# 2. ejecucion correcta.
gcc -no-pie -o "$WORK/vw" "$WORK/vw.o" || { echo "FALLO: gcc"; rm -rf "$WORK"; exit 1; }
"$WORK/vw"; got=$?
if [ "$got" = "224" ]; then
  echo "RUN: 182 vectorize-AOT exit=$got OK (180960 & 0xFF)"
else
  echo "RUN: EXIT MISMATCH got=$got exp=224"; rc=1
fi

rm -rf "$WORK"
exit $rc
