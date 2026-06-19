#!/bin/bash
# Validacion ELF + valgrind del CPU dispatch Inc 5a: strcmp/strlen despachados
# por tabla de punteros + @HelperOverride(strcmp).  aot/58_strcmp_dispatch.vex
# (el override SE invoca: g_calls>0, las comparaciones quedan correctas y las
# longitudes via .length() correctas -> 42) + los string previos sin tocar.
# Emite el .o AOT ELF, linka con gcc -no-pie, ejecuta y valgrindea los que
# usan heap.
# Se corre desde WSL:  wsl bash ./tests/aot/strcmp_dispatch_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

# nombre:esperado (exit-code).  58 = strcmp dispatch + override (42);
# resto = string previos intactos (usan strcmp/strlen despachados al baseline).
declare -A EXP=( [58]=42 [51]=14 [37]=19 [49]=99 [53]=77 [52]=42 )
# valgrindea los que alocan heap (concat / SRET / value-strings con heap).
declare -A VG=( [58]=1 [49]=1 [53]=1 )

for n in 58 51 37 49 53 52; do
  f=$(ls examples_codes_vex/aot/${n}_*.vex 2>/dev/null | head -1)
  [ -z "$f" ] && { echo "FALLO: no hay .vex para $n"; rc=1; continue; }
  OBJ="$BUILD/sc_${n}.o"
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
