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

def run_case(mode, want, desc):
    """Emite el .s del selector (modo), lo ensambla y lo ejecuta en QEMU;
    devuelve True si el exit == want."""
    asm_path = t.wpath("arm64_%s.s" % mode)
    if subprocess.run([emitter, mode, asm_path], capture_output=True,
                      timeout=30).returncode != 0 or not os.path.isfile(asm_path):
        print("FALLO: el selector no emitio el .s de %s" % mode)
        return False
    prefix = t.wpath("arm64_%s" % mode)
    if subprocess.run(
            [t.vm, "--asm-file", asm_path, "--arch", "AArch64", "-o", prefix,
             "--save-output"], capture_output=True, timeout=60).returncode != 0:
        print("FALLO: no se ensamblo %s (Keystone)" % mode)
        return False
    binf = prefix + "_assembled.bin"
    if not os.path.isfile(binf):
        print("FALLO: no se genero el .bin de %s" % mode)
        return False
    proc = subprocess.run(
        ["qemu-system-aarch64", "-M", "virt", "-cpu", "max", "-m", "128",
         "-nographic", "-semihosting",
         "-device",
         "loader,file=%s,addr=%s,force-raw=on" % (qpath(binf), LOAD_ADDR),
         "-device", "loader,addr=%s,cpu-num=0" % LOAD_ADDR],
        capture_output=True, timeout=40)
    print("  %s: qemu_exit=%d (esperado %d) -- %s" %
          (mode, proc.returncode, want, desc))
    return proc.returncode == want


ok_add = run_case("boot", 7, "add(3,4)=7 (linea recta)")
ok_sum = run_case("bootsum", 10, "sum(4)=10 (bucle: PHI + CMP + BR_COND)")

if ok_add and ok_sum:
    t.ok("OK: el selector IR->AArch64 genera codigo CORRECTO -- add(3,4)=7 y "
         "sum(4)=10 ejecutan en qemu-system-aarch64")
else:
    t.fail("FALLO: el codigo generado no dio los resultados esperados")
t.finish()
