#!/bin/bash
# AUTO multiversion (--float-isa auto): el MAIN con un hot loop vectorizado se
# despacha por cpuid.  La lowering renombra el main del usuario a
# __vex_main_body (helper VEC: el driver lo compila 3x sse2/avx2/avx512) y
# sintetiza un main fino que (a) corre __vex_cpu_init + __vex_auto_init y (b)
# hace CALLIND a traves de __vex_main_body$fp (la variante que el cpuid eligio).
#
# Verifica que aot/182 compilado con --float-isa auto:
#   1. Contiene las TRES anchuras (xmm 128b + ymm 256b + zmm 512b) -> las 3
#      variantes coexisten en el binario (multiversion real, no baseline unico).
#   2. main hace el dispatch: call __vex_auto_init + una llamada INDIRECTA
#      (call rNN) a la variante elegida.
#   3. Corre correcto en el host (180960 & 0xFF = 224), eligiendo la variante
#      soportada por la CPU (no crashea como un binario de ISA fija superior).
#
# Se corre desde WSL:  wsl bash ./tests/aot/auto_multiversion_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

PROG="examples_codes_vex/182_vectorize_elementwise.vex"
"$VM" --vex "$PROG" -m aot --float-isa auto --format elf --emit obj \
      --aot-arch x86-64 -o "$BUILD/amv.o" >/dev/null 2>&1
[ -f "$BUILD/amv.o" ] || { echo "FALLO: no se genero el .o auto"; rm -rf "$WORK"; exit 1; }
cp "$BUILD/amv.o" "$WORK/amv.o"; rm -f "$BUILD/amv.o"

# 1. Las 3 anchuras presentes (xmm + ymm + zmm) = multiversion real.
dis=$(objdump -d -M intel "$WORK/amv.o" 2>/dev/null)
xmm=$(echo "$dis" | grep -cE '\b(addpd|subpd|mulpd|movupd)\b')
ymm=$(echo "$dis" | grep -E 'v(add|sub|mul)pd|vmovupd' | grep -c ymm)
zmm=$(echo "$dis" | grep -E 'v(add|sub|mul)pd|vmovupd' | grep -c zmm)
if [ "$xmm" -gt 0 ] && [ "$ymm" -gt 0 ] && [ "$zmm" -gt 0 ]; then
  echo "WIDTHS: 3 variantes (xmm=$xmm ymm=$ymm zmm=$zmm) OK"
else
  echo "WIDTHS: FALLO -- esperaba las 3 anchuras (xmm=$xmm ymm=$ymm zmm=$zmm)"; rc=1
fi

# 2. main hace el dispatch (call __vex_auto_init + call indirecto).
mainfn=$(echo "$dis" | sed -n '/<main>:/,/ret/p')
if echo "$mainfn" | grep -qE 'call +[er][0-9a-z]+$|call +r1[0-5]'; then
  echo "DISPATCH: main hace CALLIND a la variante OK"
else
  echo "DISPATCH: FALLO -- main no hace llamada indirecta"; rc=1
fi

# 3. ejecucion correcta (auto elige la variante soportada por la CPU).
gcc -no-pie -o "$WORK/amv" "$WORK/amv.o" 2>/dev/null || { echo "FALLO: gcc"; rm -rf "$WORK"; exit 1; }
"$WORK/amv" 2>/dev/null; got=$?
if [ "$got" = "224" ]; then
  echo "RUN: 182 AUTO exit=$got OK (cpuid eligio la variante correcta)"
else
  echo "RUN: EXIT MISMATCH got=$got exp=224"; rc=1
fi

rm -rf "$WORK"
exit $rc
