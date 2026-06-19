#!/bin/bash
# Validacion ELF + valgrind del ejemplo aot/54_index_set.vex.
# Emite el .o AOT, linka con gcc -no-pie (libc para malloc/free de los
# value-strings HEAP), ejecuta y valgrindea (0 leaks / 0 errores).
# Se corre desde WSL:  wsl bash ./tests/aot/idxset_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
VEX="$ROOT/examples_codes_vex/aot/54_index_set.vex"
WORK="$(mktemp -d)"
cd "$WORK" || exit 2
rc=0

"$VM" --vex "$VEX" -m aot --format elf --emit obj -o "$WORK/idxset.o" >/dev/null 2>&1
[ -f "$WORK/idxset.o" ] || { echo "FALLO: no se genero el .o ELF"; exit 1; }

gcc -no-pie -o "$WORK/idxset.elf" "$WORK/idxset.o" || { echo "gcc fallo"; exit 1; }
"$WORK/idxset.elf"; got=$?
echo "ELF run exit=$got (esperado 42)"
[ "$got" = "42" ] || { echo "EXIT MISMATCH"; rc=1; }

valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=all -q "$WORK/idxset.elf"
vg=$?
if [ "$vg" = "99" ]; then
  echo "VALGRIND FALLO (errores/leaks)"; rc=1
else
  echo "VALGRIND OK (0 leaks, 0 errores; prog exit=$vg)"
fi

rm -rf "$WORK"
exit $rc
