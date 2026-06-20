#!/bin/bash
# Validacion ELF + valgrind del cimiento CPU dispatch (aot/55_cpu_features.vex).
# Emite el .o AOT ELF, linka con gcc -no-pie, ejecuta (exit 42 -> SSE2 detectado)
# y valgrindea (cpuid no toca heap -> 0 leaks / 0 errores).
# Se corre desde WSL:  wsl bash ./tests/aot/cpu_features_elf_test.sh <build_dir>
#
# El vm.exe de Windows (via interop WSL) resuelve rutas RELATIVAS a su cwd; por
# eso entramos al repo y le pasamos rutas relativas.  El .o sale bajo el build
# (accesible a ambos mundos); el linkado + valgrind viven en $WORK (WSL).
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
VEX="examples_codes_vex/aot/55_cpu_features.vex"
OBJ="$BUILD/cpu_features_elf_test.o"
WORK="$(mktemp -d)"
rc=0

"$VM" --vex "$VEX" -m aot --format elf --emit obj -o "$OBJ" >/dev/null 2>&1
[ -f "$OBJ" ] || { echo "FALLO: no se genero el .o ELF"; rm -rf "$WORK"; exit 1; }
cp "$OBJ" "$WORK/cpu.o"; rm -f "$OBJ"

gcc -no-pie -o "$WORK/cpu.elf" "$WORK/cpu.o" || { echo "gcc fallo"; rm -rf "$WORK"; exit 1; }
"$WORK/cpu.elf"; got=$?
echo "ELF run exit=$got (esperado 42 = SSE2 detectado)"
[ "$got" = "42" ] || { echo "EXIT MISMATCH"; rc=1; }

valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=all -q "$WORK/cpu.elf"
vg=$?
if [ "$vg" = "99" ]; then
  echo "VALGRIND FALLO (errores/leaks)"; rc=1
else
  echo "VALGRIND OK (0 leaks, 0 errores; prog exit=$vg)"
fi

rm -rf "$WORK"
exit $rc
