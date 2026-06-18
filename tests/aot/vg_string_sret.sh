#!/bin/bash
# Valgrind de los .o AOT del retorno de string por valor (SRET de 24 bytes).
# Linka con gcc -no-pie y verifica 0 leaks / 0 errores.  Se corre desde WSL.
# Los .o + su exit-code esperado se pasan como pares "ruta:exit".
WORK=~/vgwork_strsret
mkdir -p "$WORK"
cd "$WORK" || exit 2
rc=0
for spec in "$@"; do
  o="${spec%%:*}"
  expect="${spec##*:}"
  base=$(basename "$o" .o)
  echo "=== $base (esperado exit=$expect) ==="
  cp "$o" "$WORK/$base.o" || { echo "cp fallo"; rc=1; continue; }
  gcc -no-pie -o "$base.elf" "$base.o" || { echo "gcc fallo"; rc=1; continue; }
  ./"$base.elf"; got=$?; echo "  run exit=$got"
  if [ "$got" != "$expect" ]; then echo "  EXIT MISMATCH"; rc=1; fi
  valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=all -q ./"$base.elf"
  vg=$?
  if [ "$vg" = "99" ]; then
    echo "  VALGRIND FALLO (errores/leaks detectados)"; rc=1
  else
    echo "  VALGRIND OK (0 leaks, 0 errores; prog exit=$vg)"
  fi
done
exit $rc
