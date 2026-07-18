#!/usr/bin/env python3
"""Valida el SELECTOR IR->AArch64 (Phase H.2) ejecutando su salida en QEMU.

Cadena completa: el selector (jit/arm64/arm64_select) baja una IrFunction
`add(a,b)=a+b` a texto AArch64; se envuelve en un harness bare-metal que llama
add(3,4) y sale por semihosting con el resultado (7); Keystone lo ensambla; y
qemu-system-aarch64 lo ejecuta.  Si el proceso sale con codigo 7, el codegen
arm64 produce codigo CORRECTO.

    python tests/aot/qemu_arm64_codegen_test.py <build_dir>

Requiere qemu-system-aarch64 (bare-metal, como qemu_arm64_test.py).
"""
import os
import subprocess

from aot_harness import AotTest

LOAD_ADDR = "0x40200000"


def qpath(p):
    return os.path.abspath(p).replace("\\", "/")


t = AotTest()
if not t.have("qemu-system-aarch64"):
    print("SKIP: qemu-system-aarch64 no instalado")
    t.finish()

# El binario de test que emite el .s a partir del selector.
emitter = os.path.join(t.build, "test_arm64_select")
if not os.path.isfile(emitter):
    emitter += ".exe"
if not os.path.isfile(emitter):
    t.fail("FALLO: test_arm64_select no compilado (build primero)")
    t.finish()

asm_path = t.wpath("arm64_codegen.s")
rc = subprocess.run([emitter, "boot", asm_path], capture_output=True,
                    timeout=30).returncode
if rc != 0 or not os.path.isfile(asm_path):
    t.fail("FALLO: el selector no emitio el .s (op no soportada?)")
    t.finish()

# Ensamblar el .s con Keystone (vm --asm-file --arch AArch64).
prefix = t.wpath("arm64_codegen")
rc = subprocess.run(
    [t.vm, "--asm-file", asm_path, "--arch", "AArch64", "-o", prefix,
     "--save-output"],
    capture_output=True, timeout=60).returncode
binf = prefix + "_assembled.bin"
if rc != 0 or not os.path.isfile(binf):
    t.fail("FALLO: no se ensamblo el .s arm64 (Keystone)")
    t.finish()

# Ejecutar bare-metal en qemu-system-aarch64; el exit debe ser 7 (=3+4).
proc = subprocess.run(
    ["qemu-system-aarch64", "-M", "virt", "-cpu", "max", "-m", "128",
     "-nographic", "-semihosting",
     "-device", "loader,file=%s,addr=%s,force-raw=on" % (qpath(binf), LOAD_ADDR),
     "-device", "loader,addr=%s,cpu-num=0" % LOAD_ADDR],
    capture_output=True, timeout=40)

print("qemu_exit=%d  (esperado: 7 = add(3,4) del codigo generado por el selector)"
      % proc.returncode)
if proc.returncode == 7:
    t.ok("OK: el selector IR->AArch64 genera codigo correcto -- add(3,4)=7 "
         "ejecuta en qemu-system-aarch64")
else:
    t.fail("FALLO: el codigo generado no dio el resultado esperado (7)")
t.finish()
