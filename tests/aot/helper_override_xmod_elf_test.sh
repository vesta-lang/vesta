#!/bin/bash
# Validacion ELF + valgrind del CPU dispatch Inc 5b: @HelperOverride CROSS-MODULE.
# El modulo IMPORTADO (simdlib.vex) declara @HelperOverride(strcmp); el consumidor
# (main5b.vex) HEREDA el override via `import "simdlib";`.  El pre-pase del
# compilador multi-modulo aplica el override al ROOT (que genera __vex_strdisp_init).
# main5b retorna 42 si las comparaciones son correctas Y el contador g_calls de la
# lib quedo >0 (prueba de que el override cross-module se invoco).
#
# Tambien re-valida algunos string previos AOT single-file para asegurar que el
# multi-modulo no rompio el path basico.
#
# Se corre desde WSL:  wsl bash ./tests/aot/helper_override_xmod_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

# --- 1. main5b (cross-module override) -> 42 ---
MAIN="examples_codes_vex/aot/inc5b/main5b.vex"
OBJ="$BUILD/m5b.o"
"$VM" --vex "$MAIN" -m aot --format elf --emit obj -o "$OBJ" >/dev/null 2>&1
if [ ! -f "$OBJ" ]; then
  echo "FALLO: no se genero el .o ELF de main5b"; rc=1
else
  cp "$OBJ" "$WORK/m5b.o"; rm -f "$OBJ"
  gcc -no-pie -o "$WORK/m5b.elf" "$WORK/m5b.o" || { echo "gcc fallo (main5b)"; rc=1; }
  if [ -x "$WORK/m5b.elf" ]; then
    "$WORK/m5b.elf"; got=$?
    if [ "$got" = "42" ]; then echo "ELF main5b: run exit=42 OK"
    else echo "ELF main5b: EXIT MISMATCH got=$got exp=42"; rc=1; fi
    # valgrind del consumidor (usa value-strings con heap).
    valgrind --error-exitcode=88 --leak-check=full --errors-for-leak-kinds=all -q "$WORK/m5b.elf"
    vg=$?
    if [ "$vg" = "88" ]; then echo "  VALGRIND main5b FALLO (errores/leaks)"; rc=1
    else echo "  VALGRIND main5b OK (0 leaks, 0 errores; prog exit=$vg)"; fi
  fi
fi

# --- 2. probe del contador cross-module: main5b modificado para retornar *ctr ---
PROBE="examples_codes_vex/aot/inc5b/probe5b_tmp.vex"
sed 's/    if (cmp_ok == 1 \&\& used_ok == 1) {/    return (i32)(*ctr);\n    if (cmp_ok == 1 \&\& used_ok == 1) {/' "$MAIN" > "$PROBE"
POBJ="$BUILD/p5b.o"
"$VM" --vex "$PROBE" -m aot --format elf --emit obj -o "$POBJ" >/dev/null 2>&1
if [ -f "$POBJ" ]; then
  cp "$POBJ" "$WORK/p5b.o"; rm -f "$POBJ"
  gcc -no-pie -o "$WORK/p5b.elf" "$WORK/p5b.o" 2>/dev/null
  if [ -x "$WORK/p5b.elf" ]; then
    "$WORK/p5b.elf"; gc=$?
    if [ "$gc" = "10" ]; then
      echo "PROBE g_calls=$gc OK (override cross-module invocado 10 veces)"
    else
      echo "PROBE g_calls=$gc INESPERADO (esperado 10)"; rc=1
    fi
  fi
fi
rm -f "$PROBE"

# --- 3. string previos single-file intactos ---
declare -A EXP=( [58]=42 [57]=42 [56]=107 [51]=14 [37]=19 [49]=99 [53]=77 [52]=42 )
for n in 58 57 56 51 37 49 53 52; do
  f=$(ls examples_codes_vex/aot/${n}_*.vex 2>/dev/null | head -1)
  [ -z "$f" ] && { echo "FALLO: no hay .vex para $n"; rc=1; continue; }
  OBJ="$BUILD/sc_${n}.o"
  "$VM" --vex "$f" -m aot --format elf --emit obj -o "$OBJ" >/dev/null 2>&1
  [ -f "$OBJ" ] || { echo "FALLO: no se genero el .o ELF de $n"; rc=1; continue; }
  cp "$OBJ" "$WORK/m$n.o"; rm -f "$OBJ"
  gcc -no-pie -o "$WORK/m$n.elf" "$WORK/m$n.o" || { echo "gcc fallo ($n)"; rc=1; continue; }
  "$WORK/m$n.elf"; got=$?
  exp=${EXP[$n]}
  if [ "$got" = "$exp" ]; then echo "ELF $n: run exit=$got OK"
  else echo "ELF $n: EXIT MISMATCH got=$got exp=$exp"; rc=1; fi
done

rm -rf "$WORK"
exit $rc
