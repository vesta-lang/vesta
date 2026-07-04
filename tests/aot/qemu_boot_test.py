#!/usr/bin/env python3
"""Validacion del backend AOT 16-bit arrancando un boot sector en QEMU.
Requiere qemu-system-i386.

El boot (aot/18_boot_qemu.vx) escribe 'V' al debugcon (0xE9) y por la BIOS
(int 0x10), luego sale via isa-debug-exit (0xF4, codigo 0x42 -> QEMU exit
(0x42<<1)|1 = 133).  Validamos AMBAS senales.

    python tests/aot/qemu_boot_test.py <build_dir>
"""
import os
import subprocess
import sys

from aot_harness import AotTest

t = AotTest()
if not t.have("qemu-system-i386"):
    print("SKIP: qemu-system-i386 no instalado")
    t.finish()

vx = "examples_codes_vx/aot/18_boot_qemu.vx"
binf = t.wpath("boot_qemu.bin")
dbg = t.wpath("debugcon.txt")

if not t.compile_aot(vx, binf, fmt=None, emit="bin", bin_base="0x7C00"):
    t.fail("FALLO: no se genero el .bin")
    t.finish()
sz = os.path.getsize(binf)
if sz != 512:
    t.fail("FALLO: el .bin no mide 512 bytes (mide %d)" % sz)
    t.finish()

rc = subprocess.run(
    ["qemu-system-i386", "-accel", "tcg",
     "-drive", "format=raw,file=%s,if=floppy" % binf,
     "-display", "none", "-debugcon", "file:%s" % dbg,
     "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
    timeout=25).returncode

first = ""
if os.path.isfile(dbg):
    with open(dbg, "rb") as fh:
        b = fh.read(1)
        if b:
            first = "%02x" % b[0]

print("qemu_exit=%d  debugcon[0]=0x%s  (esperado: 133 y 0x56)" % (rc, first))
if rc == 133 and first == "56":
    t.ok("OK: boot sector 16-bit arranca, ejecuta codigo (debugcon 'V' + int 0x10) "
         "y termina por la ruta esperada")
else:
    t.fail("FALLO: la validacion en QEMU no dio las senales esperadas")
t.finish()
