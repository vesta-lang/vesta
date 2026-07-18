#!/usr/bin/env python3
"""Validacion del path arm64 arrancando un binario bare-metal en QEMU.
Requiere qemu-system-aarch64.

De-risk del backend arm64 (Phase H): confirma que la cadena
  Keystone (ensamblador arm64) -> raw .bin -> qemu-system-aarch64 (machine virt,
  bare-metal via -device loader, semihosting)
funciona end-to-end en este host x86, ANTES de tener el emisor IR->arm64.

El programa arm64 escribe 'V' por semihosting (SYS_WRITEC) y sale con codigo 42
(SYS_EXIT extendido).  Validamos AMBAS senales: la 'V' en stdout y el exit 42.

    python tests/aot/qemu_arm64_test.py <build_dir>
"""
import os
import subprocess

from aot_harness import AotTest

# Programa arm64 minimo: escribe 'V' + '\n' por semihosting y exit(42).  Se
# ensambla con Keystone (vm --asm-file --arch AArch64).  sp/codigo por encima del
# DTB de la machine virt (0x40000000-0x40100000); se carga en 0x40200000.
ASM = """\
movz x4, #0x4030, lsl #16
mov sp, x4
sub sp, sp, #16
movz w5, #0x56
strb w5, [sp]
mov x1, sp
movz x0, #0x3
hlt #0xf000
movz w5, #0x0a
strb w5, [sp]
mov x1, sp
movz x0, #0x3
hlt #0xf000
movz x2, #0x26
movk x2, #0x2, lsl #16
movz x3, #42
stp x2, x3, [sp]
mov x1, sp
movz x0, #0x18
hlt #0xf000
"""

LOAD_ADDR = "0x40200000"


def qpath(p):
    """Ruta absoluta con barras normales (qemu-system-aarch64.exe es un binario
    Windows y acepta rutas con '/')."""
    return os.path.abspath(p).replace("\\", "/")


t = AotTest()
if not t.have("qemu-system-aarch64"):
    print("SKIP: qemu-system-aarch64 no instalado")
    t.finish()

asm_path = t.wpath("arm64_hello.s")
with open(asm_path, "w", encoding="ascii") as fh:
    fh.write(ASM)

# Ensamblar arm64 -> <prefix>_assembled.bin (requiere --save-output).
prefix = t.wpath("arm64_hello")
rc = subprocess.run(
    [t.vm, "--asm-file", asm_path, "--arch", "AArch64", "-o", prefix,
     "--save-output"],
    capture_output=True, timeout=60).returncode
binf = prefix + "_assembled.bin"
if rc != 0 or not os.path.isfile(binf):
    t.fail("FALLO: no se ensamblo el .bin arm64 (Keystone AArch64)")
    t.finish()

# Ejecutar bare-metal en qemu-system-aarch64: cargar el raw en LOAD_ADDR y fijar
# el PC de la CPU 0 ahi.  Semihosting habilitado para WRITEC/EXIT.
proc = subprocess.run(
    ["qemu-system-aarch64", "-M", "virt", "-cpu", "max", "-m", "128",
     "-nographic", "-semihosting",
     "-device", "loader,file=%s,addr=%s,force-raw=on" % (qpath(binf), LOAD_ADDR),
     "-device", "loader,addr=%s,cpu-num=0" % LOAD_ADDR],
    capture_output=True, timeout=40)
# La salida de semihosting (WRITEC) puede ir por stdout o stderr segun el mux de
# -nographic; miramos ambos.
out = ((proc.stdout or b"") + (proc.stderr or b"")).decode("latin-1", "replace")

print("qemu_exit=%d  salida=%r  (esperado: 42 y 'V')" % (proc.returncode, out.strip()))
if proc.returncode == 42 and "V" in out:
    t.ok("OK: arm64 bare-metal arranca en QEMU (Keystone AArch64), ejecuta codigo "
         "(semihosting 'V') y sale con exit 42")
else:
    t.fail("FALLO: la validacion arm64 en QEMU no dio las senales esperadas")
t.finish()
