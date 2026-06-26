#!/bin/bash
# Validacion ELF del type matching de excepciones AOT (multi-catch + subtipo +
# propagacion).  aot/62_exc_typematch.vex ejercita: dispatch por tipo lanzado
# (EA->handler EA, EB->handler EB), subtipo (catch Base captura Derived) y
# re-throw al try externo cuando ningun catch matchea.  main devuelve 147.
# El runtime de excepciones se auto-bundlea en el objeto (no se enlaza a mano).
#
# Se corre desde WSL:  wsl bash ./tests/aot/exc_typematch_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

PROG="examples_codes_vex/aot/62_exc_typematch.vex"
"$VM" --vex "$PROG" -m aot --format elf --emit obj --aot-arch x86-64 \
      -o "$BUILD/tm_prog.o" >/dev/null 2>&1
[ -f "$BUILD/tm_prog.o" ] || { echo "FALLO: no se genero el .o"; rm -rf "$WORK"; exit 1; }
cp "$BUILD/tm_prog.o" "$WORK/prog.o"; rm -f "$BUILD/tm_prog.o"

gcc -no-pie -o "$WORK/tm.elf" "$WORK/prog.o" \
    || { echo "FALLO: gcc no enlazo"; rm -rf "$WORK"; exit 1; }

"$WORK/tm.elf"; got=$?
if [ "$got" = "147" ]; then
  echo "TYPE-MATCH ELF: run exit=$got OK (multi-catch + subtipo + propagacion)"
else
  echo "TYPE-MATCH ELF: EXIT MISMATCH got=$got exp=147"; rc=1
fi

rm -rf "$WORK"
exit $rc
