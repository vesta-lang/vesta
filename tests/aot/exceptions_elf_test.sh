#!/bin/bash
# Validacion ELF + valgrind de las excepciones nativas AOT (auto-hospedadas
# en Vex, setjmp/longjmp; stdlib/vex/vex_exc.vex).
#
# aot/61_exceptions.vex: try/catch/throw cruzando frontera de funcion, el
# objeto lanzado (heap) sobrevive al longjmp y el catch lee su campo.
# main devuelve 42.  Se enlaza el programa + el runtime vex_exc.vex (ambos
# .o AOT ELF) con gcc -no-pie y se ejecuta.  El objeto lanzado en el camino
# capturado se "fuga" a proposito (v1 no rastrea ownership del throw), por eso
# validamos solo el exit-code (42), sin valgrind del binario completo.
#
# Se corre desde WSL:  wsl bash ./tests/aot/exceptions_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

PROG="examples_codes_vex/aot/61_exceptions.vex"

# Compilar el programa a .o AOT ELF (x86-64).  El driver AUTO-BUNDLEa el runtime
# de excepciones (stdlib/vex/vex_exc.vex) en el MISMO objeto -> no hay que
# enlazar vex_exc.o a mano.  vm.exe (Windows) escribe a $BUILD (path accesible
# desde Windows); luego copiamos a $WORK (que puede vivir en el FS de WSL).
"$VM" --vex "$PROG" -m aot --format elf --emit obj --aot-arch x86-64 \
      -o "$BUILD/exc_prog.o" >/dev/null 2>&1
[ -f "$BUILD/exc_prog.o" ] || { echo "FALLO: no se genero prog.o"; rm -rf "$WORK"; exit 1; }
cp "$BUILD/exc_prog.o" "$WORK/prog.o"
rm -f "$BUILD/exc_prog.o"

# Enlazar SOLO el objeto del programa (autocontenido).  gcc aporta el _start/
# main wrapper + calloc/free para el `new`.
gcc -no-pie -o "$WORK/exc.elf" "$WORK/prog.o" \
    || { echo "FALLO: gcc no enlazo (auto-bundle del runtime fallo?)"; rm -rf "$WORK"; exit 1; }

"$WORK/exc.elf"; got=$?
if [ "$got" = "42" ]; then
  echo "EXCEPCIONES ELF: run exit=$got OK (try/catch/throw cross-fn + e.code)"
else
  echo "EXCEPCIONES ELF: EXIT MISMATCH got=$got exp=42"; rc=1
fi

rm -rf "$WORK"
exit $rc
