#!/usr/bin/env python3
"""Valida el emisor ELF64 AArch64 (Phase H.4a) ejecutando un ELF con qemu -kernel.

A diferencia de qemu_arm64_codegen_test.py (binario plano + -device loader), aqui
el codigo del selector se envuelve en un ejecutable ELF64 ET_EXEC (e_machine=
EM_AARCH64, un PT_LOAD) que qemu-system-aarch64 carga directamente con -kernel
(parsea el ELF y salta a e_entry).  Si sale con codigo 7 (add(3,4)), el contenedor
ELF es correcto.

    python tests/aot/qemu_arm64_elf_test.py <build_dir>
"""
import os
import subprocess

from aot_harness import AotTest

BASE = "40200000"  # por encima del DTB de la machine virt.


def qpath(p):
    return os.path.abspath(p).replace("\\", "/")


t = AotTest()
if not t.have("qemu-system-aarch64"):
    print("SKIP: qemu-system-aarch64 no instalado")
    t.finish()

emitter = os.path.join(t.build, "test_arm64_select")
if not os.path.isfile(emitter):
    emitter += ".exe"
if not os.path.isfile(emitter):
    t.fail("FALLO: test_arm64_select no compilado")
    t.finish()

# 1) emitir el .s del `add` del selector envuelto en el harness bare-metal.
s = t.wpath("arm64_elf.s")
if subprocess.run([emitter, "boot", s], capture_output=True,
                  timeout=30).returncode != 0:
    t.fail("FALLO: el selector no emitio el .s")
    t.finish()

# 2) ensamblar con Keystone.
prefix = t.wpath("arm64_elf")
if subprocess.run(
        [t.vm, "--asm-file", s, "--arch", "AArch64", "-o", prefix,
         "--save-output"], capture_output=True, timeout=60).returncode != 0:
    t.fail("FALLO: no se ensamblo el .s (Keystone)")
    t.finish()
binf = prefix + "_assembled.bin"
if not os.path.isfile(binf):
    t.fail("FALLO: no se genero el .bin")
    t.finish()

# 3) envolver los bytes en un ELF64 AArch64 ejecutable.
elf = t.wpath("arm64_elf.elf")
if subprocess.run([emitter, "elf", binf, elf, BASE], capture_output=True,
                  timeout=30).returncode != 0 or not os.path.isfile(elf):
    t.fail("FALLO: no se emitio el ELF64 AArch64")
    t.finish()

# Validacion estructural del ELF: magic + clase 64 + LE + e_machine=EM_AARCH64.
with open(elf, "rb") as fh:
    hdr = fh.read(20)
mach = hdr[18] | (hdr[19] << 8)
ok_hdr = (hdr[:4] == b"\x7fELF" and hdr[4] == 2 and hdr[5] == 1 and mach == 183)
print("ELF: magic/clase/LE ok=%s  e_machine=%d (esperado 183=EM_AARCH64)" %
      (ok_hdr, mach))

# 4) ejecutar el ELF con qemu -kernel; exit esperado 7 (=add(3,4)).
proc = subprocess.run(
    ["qemu-system-aarch64", "-M", "virt", "-cpu", "max", "-m", "128",
     "-nographic", "-semihosting", "-kernel", qpath(elf)],
    capture_output=True, timeout=40)
print("qemu_exit=%d (esperado 7)" % proc.returncode)

if ok_hdr and proc.returncode == 7:
    t.ok("OK: el ELF64 AArch64 emitido (e_machine=EM_AARCH64) lo carga "
         "qemu-system-aarch64 con -kernel y ejecuta add(3,4)=7")
else:
    t.fail("FALLO: el ELF no tenia la cabecera esperada o no ejecuto (exit != 7)")
t.finish()
