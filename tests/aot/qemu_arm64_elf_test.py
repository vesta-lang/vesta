#!/usr/bin/env python3
"""Valida la compilacion AOT a ELF64 AArch64 ejecutando el binario con qemu.

Compila un .vx real con `-m aot --aot-arch aarch64` (el codegen arm64 + la
emision ELF via LibPEparse, sin ningun writer hand-rolled) y ejecuta el ELF
resultante con qemu-system-aarch64 -kernel + semihosting.  Si sale con el
codigo que devuelve main(), la cadena completa (codegen arm64 -> e_machine
EM_AARCH64 -> reloc CALL26 del `bl main` -> _start -> semihosting) es correcta.

    python tests/aot/qemu_arm64_elf_test.py <build_dir>
"""
import os
import subprocess

from aot_harness import AotTest

RET = 42  # el valor que devuelve main() en el .vx de prueba.


def qpath(p):
    return os.path.abspath(p).replace("\\", "/")


t = AotTest()
if not t.have("qemu-system-aarch64"):
    print("SKIP: qemu-system-aarch64 no instalado")
    t.finish()

# 1) fuente .vx trivial.
src = t.wpath("arm64_main.vx")
with open(src, "w") as fh:
    fh.write("i32 main() {\n    return %d;\n}\n" % RET)

# 2) compilar a ELF64 AArch64 con el AOT (codegen arm64 + LibPEparse).
elf = t.wpath("arm64_main.elf")
r = subprocess.run(
    [t.vm, "-m", "aot", "--vesta", src, "--aot-arch", "aarch64",
     "--format", "elf", "--emit", "exe", "-o", elf],
    capture_output=True, timeout=90)
if r.returncode != 0 or not os.path.isfile(elf):
    t.fail("FALLO: el AOT no emitio el ELF arm64\n" +
           r.stderr.decode(errors="replace"))
    t.finish()

# 3) validacion estructural: magic + clase 64 + LE + e_machine=EM_AARCH64 (183).
with open(elf, "rb") as fh:
    hdr = fh.read(20)
mach = hdr[18] | (hdr[19] << 8)
ok_hdr = (hdr[:4] == b"\x7fELF" and hdr[4] == 2 and hdr[5] == 1 and mach == 183)
print("ELF: magic/clase/LE ok=%s  e_machine=%d (esperado 183=EM_AARCH64)" %
      (ok_hdr, mach))

# 4) ejecutar con qemu -kernel; exit esperado = RET (retorno de main).
proc = subprocess.run(
    ["qemu-system-aarch64", "-M", "virt", "-cpu", "max", "-m", "2G",
     "-nographic", "-semihosting", "-kernel", qpath(elf)],
    capture_output=True, timeout=40)
print("qemu_exit=%d (esperado %d)" % (proc.returncode, RET))

if ok_hdr and proc.returncode == RET:
    t.ok("OK: .vx compilado a ELF64 AArch64 (e_machine=EM_AARCH64, emision via "
         "LibPEparse) ejecuta en qemu-system-aarch64 y devuelve %d" % RET)
else:
    t.fail("FALLO: cabecera ELF inesperada o exit != %d" % RET)
t.finish()
