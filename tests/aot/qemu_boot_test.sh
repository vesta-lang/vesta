#!/usr/bin/env bash
# Validacion del backend AOT 16-bit arrancando un boot sector en QEMU.
# Requiere: qemu-system-i386 (apt install qemu-system-x86) y el binario `vm`.
#
# Uso:  bash tests/aot/qemu_boot_test.sh <build_dir>
#   <build_dir> contiene vm/vm.exe.
#
# El boot (examples_codes_vex/aot/18_boot_qemu.vex) escribe 'V' al debugcon
# de QEMU (puerto 0xE9) y por la BIOS (int 0x10), luego sale via
# isa-debug-exit (puerto 0xF4, codigo 0x42 -> QEMU exit (0x42<<1)|1 = 133).
# Validamos AMBAS senales: el codigo de salida y el byte del debugcon.
set -u

BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
VEX="$ROOT/examples_codes_vex/aot/18_boot_qemu.vex"
WORK="$(mktemp -d)"
BIN="$WORK/boot_qemu.bin"
DBG="$WORK/debugcon.txt"

command -v qemu-system-i386 >/dev/null 2>&1 || { echo "SKIP: qemu-system-i386 no instalado"; exit 0; }

"$VM" -m aot --vex "$VEX" --emit bin --bin-base 0x7C00 -o "$BIN" >/dev/null 2>&1
[ -f "$BIN" ] || { echo "FALLO: no se genero el .bin"; exit 1; }
SZ=$(stat -c %s "$BIN" 2>/dev/null || wc -c < "$BIN")
[ "$SZ" = "512" ] || { echo "FALLO: el .bin no mide 512 bytes (mide $SZ)"; exit 1; }

timeout 25 qemu-system-i386 -accel tcg \
    -drive format=raw,file="$BIN",if=floppy \
    -display none -debugcon "file:$DBG" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
RC=$?

FIRST=$(od -An -tx1 -N 1 "$DBG" 2>/dev/null | tr -d ' ')
rm -rf "$WORK"

echo "qemu_exit=$RC  debugcon[0]=0x$FIRST  (esperado: 133 y 0x56)"
if [ "$RC" = "133" ] && [ "$FIRST" = "56" ]; then
    echo "OK: boot sector 16-bit arranca, ejecuta codigo (debugcon 'V' + int 0x10) y termina por la ruta esperada"
    exit 0
fi
echo "FALLO: la validacion en QEMU no dio las senales esperadas"
exit 1
