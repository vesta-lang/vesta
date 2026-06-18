#!/bin/bash
# Valgrind de los .o AOT (rep movsb): linka con gcc -no-pie y verifica
# 0 leaks / 0 errores.  Se corre desde WSL.  Los .o se pasan como args
# (rutas dentro del FS de WSL o /mnt/c/...).
WORK=~/vgwork_repmovsb
mkdir -p "$WORK"
cd "$WORK" || exit 2
rc=0
for o in "$@"; do
  base=$(basename "$o" .o)
  echo "=== $base ==="
  cp "$o" "$WORK/$base.o" || { echo "cp fallo"; rc=1; continue; }
  gcc -no-pie -o "$base.elf" "$base.o" || { echo "gcc fallo"; rc=1; continue; }
  ./"$base.elf"; echo "  run exit=$?"
  # valgrind con --error-exitcode=99: SOLO retorna 99 si hubo errores/leaks;
  # en caso limpio propaga el exit-code del programa (19/2/64...).  Por eso
  # comprobamos == 99 explicitamente en vez de != 0.
  valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=all -q ./"$base.elf"
  vg=$?
  if [ "$vg" = "99" ]; then
    echo "  VALGRIND FALLO (errores/leaks detectados)"
    rc=1
  else
    echo "  VALGRIND OK (0 leaks, 0 errores; prog exit=$vg)"
  fi
done
exit $rc
