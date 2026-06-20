#!/bin/bash
# Validacion ELF + valgrind del fix de globals del CPU dispatch cross-module:
# m2.vex importa slib2.vex (s.length() -> __vex_strlen_fp dispatched desde un
# DEP, NO desde el root).  Antes del fix, el slot fp del dep quedaba sin init
# (cada modulo tenia su propio slot NON_DEDUP y el init solo se preponia al
# main del modulo que lo emitia) -> call a fp nulo -> SEGV.  Ahora el slot es
# program-global (StaticDataMeta::shared_key) y el init se prepone a main en
# el merge cross-module aunque el root no use dispatch.
#
# Se corre desde WSL:  wsl bash ./tests/aot/m2_xmod_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

MAIN="examples_codes_vex/aot/xmod_strlen/m2.vex"
OBJ="$BUILD/m2x.o"
"$VM" --vex "$MAIN" -m aot --format elf --emit obj -o "$OBJ" >/dev/null 2>&1
if [ ! -f "$OBJ" ]; then
  echo "FALLO: no se genero el .o ELF de m2"; rc=1
else
  cp "$OBJ" "$WORK/m2.o"; rm -f "$OBJ"
  gcc -no-pie -o "$WORK/m2.elf" "$WORK/m2.o" || { echo "gcc fallo (m2)"; rc=1; }
  if [ -x "$WORK/m2.elf" ]; then
    "$WORK/m2.elf"; got=$?
    if [ "$got" = "5" ]; then echo "ELF m2: run exit=5 OK"
    else echo "ELF m2: EXIT MISMATCH got=$got exp=5"; rc=1; fi
    valgrind --error-exitcode=88 --leak-check=full --errors-for-leak-kinds=all -q "$WORK/m2.elf"
    vg=$?
    if [ "$vg" = "88" ]; then echo "  VALGRIND m2 FALLO (errores/leaks)"; rc=1
    else echo "  VALGRIND m2 OK (0 leaks, 0 errores; prog exit=$vg)"; fi
  fi
fi

rm -rf "$WORK"
exit $rc
