#!/bin/bash
# Validacion ELF + valgrind del @HelperOverride(memcpy) (CPU dispatch Inc 4):
# aot/57_helper_override.vex (el override SE invoca: g_calls>0 y los concats
# quedan byte-exactos -> 42) + 56 (SIN override -> sigue 107 via cpuid) +
# los string previos sin tocar.  Emite el .o AOT ELF, linka con gcc -no-pie,
# ejecuta y valgrindea los que usan heap.
# Se corre desde WSL:  wsl bash ./tests/aot/helper_override_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

# nombre:esperado (exit-code).  57 = override; 56 = dispatch sin override (107);
# resto = string previos intactos.
declare -A EXP=( [57]=42 [56]=107 [37]=19 [49]=99 [53]=77 [52]=42 )
# valgrindea los que alocan heap (concat / SRET con heap).
declare -A VG=( [57]=1 [56]=1 [49]=1 [53]=1 )

for n in 57 56 37 49 53 52; do
  f=$(ls examples_codes_vex/aot/${n}_*.vex 2>/dev/null | head -1)
  [ -z "$f" ] && { echo "FALLO: no hay .vex para $n"; rc=1; continue; }
  OBJ="$BUILD/ho_${n}.o"
  "$VM" --vex "$f" -m aot --format elf --emit obj -o "$OBJ" >/dev/null 2>&1
  [ -f "$OBJ" ] || { echo "FALLO: no se genero el .o ELF de $n"; rc=1; continue; }
  cp "$OBJ" "$WORK/m$n.o"; rm -f "$OBJ"
  gcc -no-pie -o "$WORK/m$n.elf" "$WORK/m$n.o" || { echo "gcc fallo ($n)"; rc=1; continue; }
  "$WORK/m$n.elf"; got=$?
  exp=${EXP[$n]}
  if [ "$got" = "$exp" ]; then
    echo "ELF $n: run exit=$got OK"
  else
    echo "ELF $n: EXIT MISMATCH got=$got exp=$exp"; rc=1
  fi
  if [ "${VG[$n]:-0}" = "1" ]; then
    # error-exitcode = 88: no colisiona con ningun exit-code de programa.
    valgrind --error-exitcode=88 --leak-check=full --errors-for-leak-kinds=all -q "$WORK/m$n.elf"
    vg=$?
    if [ "$vg" = "88" ]; then
      echo "  VALGRIND $n FALLO (errores/leaks)"; rc=1
    else
      echo "  VALGRIND $n OK (0 leaks, 0 errores; prog exit=$vg)"
    fi
  fi
done

rm -rf "$WORK"
exit $rc
