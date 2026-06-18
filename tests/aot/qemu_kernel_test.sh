#!/usr/bin/env bash
# Validacion del codegen AOT x86-32 corriendo BARE-METAL en QEMU (modo protegido)
# via un kernel multiboot.  Requiere: qemu-system-i386 + el binario `vm`.
#
# Uso:  bash tests/aot/qemu_kernel_test.sh <build_dir>
#   <build_dir> contiene vm/vm.exe.  Asume entorno UNIFORME (vm + qemu nativos);
#   en el dev hibrido Windows+WSL, generar el .bin con vm.exe en Windows y correr
#   solo qemu en WSL (vm.exe no traga rutas /mnt).
#
# El kernel (examples_codes_vex/aot/31_x86_32_multiboot_qemu.vex): QEMU -kernel lo
# carga en modo protegido 32-bit y salta a _kstart (asm), que llama a compute()
# (codigo Vex de 32-bit, suma 1..9 = 45 con un while -> branches+loop reales) y
# senala por debugcon (0xE9 -> 0x2d) + isa-debug-exit (0xF4 -> QEMU exit
# (45<<1)|1 = 91).  Validamos AMBAS senales.
set -u

BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
VEX="$ROOT/examples_codes_vex/aot/31_x86_32_multiboot_qemu.vex"
WORK="$(mktemp -d)"
BIN="$WORK/kernel.bin"
DBG="$WORK/debugcon.txt"

command -v qemu-system-i386 >/dev/null 2>&1 || { echo "SKIP: qemu-system-i386 no instalado"; exit 0; }

"$VM" -m aot --vex "$VEX" --aot-arch x86-32 --emit bin --bin-base 0x100000 -o "$BIN" >/dev/null 2>&1
[ -f "$BIN" ] || { echo "FALLO: no se genero el .bin"; rm -rf "$WORK"; exit 1; }

timeout 25 qemu-system-i386 -accel tcg -kernel "$BIN" \
    -display none -debugcon "file:$DBG" \
    -device isa-debug-exit,iobase=0xf4,iosize=1 -no-reboot
RC=$?

FIRST=$(od -An -tx1 -N 1 "$DBG" 2>/dev/null | tr -d ' ')
rm -rf "$WORK"

echo "qemu_exit=$RC  debugcon[0]=0x$FIRST  (esperado: 91 y 0x2d)"
if [ "$RC" = "91" ] && [ "$FIRST" = "2d" ]; then
    echo "OK: el codigo Vex de 32-bit corre bare-metal en QEMU (modo protegido, multiboot)"
    exit 0
fi
echo "FALLO: senales inesperadas"
exit 1
