#!/bin/bash
# Valida el kernel multiboot x86-32 (31_x86_32_multiboot_qemu.vex) en QEMU modo
# protegido bare-metal.  El codigo Vex de 32-bit (compute, suma 1..9 = 45) corre
# sin SO; el resultado se observa por debugcon (0x2d) + isa-debug-exit (exit 91).
#
# Uso: tests/aot/qemu_kernel_test.sh <build_dir>   (SKIP si no hay qemu-system-i386)
set -u
BUILD="${1:-cmake-build-release}"
VM="$BUILD/vm.exe"; [ -x "$VM" ] || VM="$BUILD/vm"
SRC="examples_codes_vex/aot/31_x86_32_multiboot_qemu.vex"
BIN="/tmp/k_mb.bin"

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "SKIP: qemu-system-i386 no instalado"; exit 0
fi

"$VM" -m aot --vex "$SRC" -o "$BIN" --aot-arch x86-32 --emit bin --bin-base 0x100000 \
    >/dev/null 2>&1 || { echo "FAIL: compile"; exit 1; }

rm -f /tmp/k_dbg.out
timeout 15 qemu-system-i386 -kernel "$BIN" \
    -debugcon file:/tmp/k_dbg.out \
    -device isa-debug-exit,iobase=0xf4,iosize=1 \
    -display none -no-reboot >/dev/null 2>&1
RC=$?

DBG=$(od -A n -t x1 /tmp/k_dbg.out 2>/dev/null | tr -d ' \n')
if [ "$RC" = "91" ] && [ "$DBG" = "2d" ]; then
    echo "OK: QEMU exit=91 (=(45<<1)|1), debugcon=0x2d (45) -- Vex 32-bit corre bare-metal"
    exit 0
fi
echo "FAIL: QEMU exit=$RC (esperado 91), debugcon=0x$DBG (esperado 2d)"
exit 1
