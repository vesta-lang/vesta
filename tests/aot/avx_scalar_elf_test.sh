#!/bin/bash
# Increment 2a (float-AOT): AVX escalar 3-operandos no-destructivo.  Con
# --float-isa avx, las binarias f64/f32 escalares se emiten en VEX (VADDSD/
# VSUBSD/VMULSD/VDIVSD + SS) en vez de legacy SSE (ADDSD...) -> sin el `mov` de
# coalescing 2-address y sin mezclar legacy-SSE con VEX (penalizacion de
# transicion).  El default (sse2) sigue legacy.
#
# Valida: una funcion f64 pura (params f64, sin conversiones) compilada a AOT
# --float-isa avx contiene VEX scalar y CERO legacy scalar; corre correcto
# (el C main aporta los f64 runtime).  Default sse2 = legacy, 0 VEX.
#
# Se corre desde WSL:  wsl bash ./tests/aot/avx_scalar_elf_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

# El .vex es un fichero del repo (Windows-accesible; vm.exe no lee paths /tmp
# de WSL).  Libreria sin main -> se enlaza con el C harness de abajo.
PROG="examples_codes_vex/aot/63_avx_scalar.vex"

# AVX -> VEX scalar.
"$VM" --vex "$PROG" -m aot --emit obj --format elf --float-isa avx \
      -o "$BUILD/avxsc.o" >/dev/null 2>&1
[ -f "$BUILD/avxsc.o" ] || { echo "FALLO: no se genero el .o avx"; rm -rf "$WORK"; exit 1; }
cp "$BUILD/avxsc.o" "$WORK/a.o"; rm -f "$BUILD/avxsc.o"
vex=$(objdump -d "$WORK/a.o" 2>/dev/null | grep -coiE 'vaddsd|vsubsd|vmulsd|vdivsd')
leg=$(objdump -d "$WORK/a.o" 2>/dev/null | grep -coiE '\baddsd|\bsubsd|\bmulsd|\bdivsd')
if [ "$vex" -ge 1 ] && [ "$leg" = "0" ]; then
  echo "AVX: $vex VEX-scalar, 0 legacy OK (sin mezcla)"
else
  echo "AVX: FALLO vex=$vex legacy=$leg (esperado vex>=1, legacy=0)"; rc=1
fi

# SSE2 default -> legacy, 0 VEX.
"$VM" --vex "$PROG" -m aot --emit obj --format elf \
      -o "$BUILD/ssesc.o" >/dev/null 2>&1
cp "$BUILD/ssesc.o" "$WORK/s.o"; rm -f "$BUILD/ssesc.o"
svex=$(objdump -d "$WORK/s.o" 2>/dev/null | grep -coiE 'vaddsd|vmulsd')
if [ "$svex" = "0" ]; then
  echo "SSE2: 0 VEX-scalar OK (legacy, sin regresion)"
else
  echo "SSE2: FALLO $svex VEX (deberia ser legacy)"; rc=1
fi

# Ejecucion (el C aporta f64 runtime, sin conversiones Vex).
cat > "$WORK/m.c" <<'CEOF'
double fma2(double,double,double); double poly(double);
int main(){ return (int)(fma2(3.0,4.0,5.0) + poly(6.0)); } /* 17+30 = 47 */
CEOF
gcc -no-pie -o "$WORK/m" "$WORK/m.c" "$WORK/a.o" || { echo "FALLO: gcc"; rm -rf "$WORK"; exit 1; }
"$WORK/m"; got=$?
if [ "$got" = "47" ]; then
  echo "RUN: avx-scalar exit=$got OK"
else
  echo "RUN: EXIT MISMATCH got=$got exp=47"; rc=1
fi

rm -rf "$WORK"
exit $rc
