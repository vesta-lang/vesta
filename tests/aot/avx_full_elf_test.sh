#!/bin/bash
# Increment 2b (float-AOT): VEX para cvt/cmp/sqrt escalares -> --float-isa avx es
# LIMPIO end-to-end (sin mezclar legacy-SSE con VEX -> sin penalizacion de
# transicion).  Un programa real con conversiones (ITOF/FTOI) + vectorizacion
# compila a AOT avx con CERO ops escalares legacy y corre correcto.
#
# Valida aot/182_vectorize_elementwise --float-isa avx:
#   1. 0 ops escalares legacy (addsd/mulsd/cvtsi2sd/cvttsd2si) -> todo VEX.
#   2. 0 bytes (bad) -> el encoding VEX decodifica bien.
#   3. corre = 224 (180960 & 0xFF).
#
# Se corre desde WSL:  wsl bash ./tests/aot/avx_full_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

PROG="examples_codes_vex/182_vectorize_elementwise.vex"
"$VM" --vex "$PROG" -m aot --format elf --emit obj --float-isa avx \
      -o "$BUILD/avxf.o" >/dev/null 2>&1
[ -f "$BUILD/avxf.o" ] || { echo "FALLO: no se genero el .o avx"; rm -rf "$WORK"; exit 1; }
cp "$BUILD/avxf.o" "$WORK/f.o"; rm -f "$BUILD/avxf.o"

# CERO legacy SSE float escalar: arith + cvt + cmp + sqrt + neg/abs + MOVES
# (movsd/movss/movq) -> todo VEX, sin mezcla -> sin penalizacion de transicion.
# \b evita matchear las VEX (vmovq/vmovsd... empiezan por 'v', sin word-boundary).
leg=$(objdump -d "$WORK/f.o" 2>/dev/null | grep -coiE '\baddsd|\bsubsd|\bmulsd|\bdivsd|\baddss|\bsubss|\bmulss|\bdivss|\bcvtsi2sd|\bcvttsd2si|\bcvtsi2ss|\bcvttss2si|\bcvtss2sd|\bcvtsd2ss|\bucomisd|\bucomiss|\bsqrtsd|\bsqrtss|\bxorps|\bandps|\bmovsd|\bmovss|\bmovq')
bad=$(objdump -d "$WORK/f.o" 2>/dev/null | grep -coiE '\(bad\)')
if [ "$leg" = "0" ] && [ "$bad" = "0" ]; then
  echo "AVX-FULL: 0 escalar legacy (arith/cvt/cmp/sqrt/neg/moves), 0 bad OK"
else
  echo "AVX-FULL: FALLO legacy=$leg bad=$bad (esperado 0/0)"; rc=1
fi

gcc -no-pie -o "$WORK/f" "$WORK/f.o" || { echo "FALLO: gcc"; rm -rf "$WORK"; exit 1; }
"$WORK/f"; got=$?
if [ "$got" = "224" ]; then
  echo "RUN: 182 avx-full exit=$got OK"
else
  echo "RUN: EXIT MISMATCH got=$got exp=224"; rc=1
fi

rm -rf "$WORK"
exit $rc
