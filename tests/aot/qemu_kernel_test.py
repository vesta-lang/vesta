#!/usr/bin/env python3
"""Validacion del codegen AOT x86-32 corriendo BARE-METAL en QEMU (modo
protegido) via un kernel multiboot.  Requiere qemu-system-i386.

El kernel (aot/31_x86_32_multiboot_qemu.vx): QEMU -kernel lo carga en modo
protegido 32-bit y salta a _kstart (asm), que llama a compute() (codigo Vesta
de 32-bit, suma 1..9 = 45 con un while) y senala por debugcon (0xE9 -> 0x2d) +
isa-debug-exit (0xF4 -> QEMU exit (45<<1)|1 = 91).  Validamos AMBAS senales.

    python tests/aot/qemu_kernel_test.py <build_dir>
"""
import os
import subprocess

from aot_harness import AotTest

t = AotTest()
if not t.have("qemu-system-i386"):
    print("SKIP: qemu-system-i386 no instalado")
    t.finish()

vx = "examples_codes_vx/aot/31_x86_32_multiboot_qemu.vx"
binf = t.wpath("kernel.bin")
dbg = t.wpath("debugcon.txt")

if not t.compile_aot(vx, binf, fmt=None, emit="bin", arch="x86-32", bin_base="0x100000"):
    t.fail("FALLO: no se genero el .bin")
    t.finish()

rc = subprocess.run(
    ["qemu-system-i386", "-accel", "tcg", "-kernel", binf,
     "-display", "none", "-debugcon", "file:%s" % dbg,
     "-device", "isa-debug-exit,iobase=0xf4,iosize=1", "-no-reboot"],
    timeout=25).returncode

first = ""
if os.path.isfile(dbg):
    with open(dbg, "rb") as fh:
        b = fh.read(1)
        if b:
            first = "%02x" % b[0]

print("qemu_exit=%d  debugcon[0]=0x%s  (esperado: 91 y 0x2d)" % (rc, first))
if rc == 91 and first == "2d":
    t.ok("OK: el codigo Vesta de 32-bit corre bare-metal en QEMU (modo protegido, multiboot)")
else:
    t.fail("FALLO: senales inesperadas")
t.finish()
