#!/bin/bash
# POO nativa AOT (clases + herencia + override + interfaces + dtor polimorfico):
# compila los ejemplos aot/2x a .o, los linka y verifica el EXIT-CODE real.
#
# Cierra el gap que dejo pasar la regresion de devirt nativa (ssa_concrete_class_
# cross-fn): el e2e principal solo valida interp/jit (R0); los ejemplos aot/ se
# compilaban a mano sin verificar la EJECUCION nativa.  Aqui si.
#
# Se corre desde WSL:  wsl bash ./tests/aot/poo_exec_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
command -v gcc >/dev/null || { echo "SKIP: gcc no disponible"; exit 0; }
WORK="$(mktemp -d)"
rc=0

# Runtime minimo para los ejemplos con `extern` (note/get_note del 26).
cat > "$WORK/rt.c" <<'EOF'
static long g_note;
void note(long x) { g_note = x; }
long get_note(void) { return g_note; }
EOF
gcc -c -O2 -o "$WORK/rt.o" "$WORK/rt.c" 2>/dev/null

# bench_one <ejemplo> <exit_esperado> <extra_obj>
poo_one() {
  local ex="$1" exp="$2" extra="${3:-}"
  local src="examples_codes_vex/aot/$ex.vex"
  [ -f "$src" ] || { echo "SKIP $ex: no existe"; return; }
  "$VM" --vex "$src" -m aot --format elf --emit obj -o "$BUILD/poo.o" \
        >/dev/null 2>&1
  [ -f "$BUILD/poo.o" ] || { echo "FALLO $ex: no se genero el .o"; rc=1; return; }
  cp "$BUILD/poo.o" "$WORK/poo.o"; rm -f "$BUILD/poo.o"
  if ! gcc -no-pie -o "$WORK/poo" "$WORK/poo.o" $extra 2>/dev/null; then
    echo "FALLO $ex: link gcc"; rc=1; return
  fi
  "$WORK/poo"; local got=$?
  if [ "$got" = "$exp" ]; then
    echo "OK $ex -> exit=$got"
  else
    echo "FALLO $ex -> exit=$got (esperado $exp)"; rc=1
  fi
}

# clases no-virtuales + devirt de hoja.
poo_one 20_class_native 42
poo_one 21_devirt_native 42
# polimorfismo via vtable (la regresion daba 107).
poo_one 22_poly_native 47
# interfaces via vtable (la regresion daba 20).
poo_one 23_interface_native 42
# dtor polimorfico (~Dog via vtable de la instancia); usa extern note/get_note.
poo_one 26_dtor_polimorfico 2 "$WORK/rt.o"

rm -rf "$WORK"
exit $rc
