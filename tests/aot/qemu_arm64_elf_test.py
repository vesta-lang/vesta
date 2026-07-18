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

def qpath(p):
    return os.path.abspath(p).replace("\\", "/")


t = AotTest()
if not t.have("qemu-system-aarch64"):
    print("SKIP: qemu-system-aarch64 no instalado")
    t.finish()


def build_and_run(name, source, expect):
    """Compila un .vx a ELF arm64 (AOT) y lo ejecuta con qemu; comprueba el
    exit-code y que el contenedor sea EM_AARCH64."""
    src = t.wpath(name + ".vx")
    with open(src, "w") as fh:
        fh.write(source)
    elf = t.wpath(name + ".elf")
    r = subprocess.run(
        [t.vm, "-m", "aot", "--vesta", src, "--aot-arch", "aarch64",
         "--format", "elf", "--emit", "exe", "-o", elf],
        capture_output=True, timeout=90)
    if r.returncode != 0 or not os.path.isfile(elf):
        t.fail("FALLO [%s]: el AOT no emitio el ELF arm64\n%s" %
               (name, r.stderr.decode(errors="replace")))
        return False
    with open(elf, "rb") as fh:
        hdr = fh.read(20)
    mach = hdr[18] | (hdr[19] << 8)
    if not (hdr[:4] == b"\x7fELF" and hdr[4] == 2 and hdr[5] == 1 and
            mach == 183):
        t.fail("FALLO [%s]: cabecera ELF inesperada (e_machine=%d)" %
               (name, mach))
        return False
    proc = subprocess.run(
        ["qemu-system-aarch64", "-M", "virt", "-cpu", "max", "-m", "2G",
         "-nographic", "-semihosting", "-kernel", qpath(elf)],
        capture_output=True, timeout=40)
    print("[%s] e_machine=183  qemu_exit=%d (esperado %d)" %
          (name, proc.returncode, expect))
    return proc.returncode == expect


# 1) single-function: main devuelve una constante.
ok1 = build_and_run("arm64_main", "i32 main() {\n    return 42;\n}\n", 42)

# 2) multi-funcion + recursion: ejercita las llamadas cross-funcion arm64 (bl +
#    reloc R_AARCH64_CALL26).  main hace tail-call a suma; suma se llama a si
#    misma.  suma(8) = 0+1+..+8 = 36.
ok2 = build_and_run(
    "arm64_rec",
    "i32 suma(i32 n) {\n"
    "    if (n <= 0) {\n        return 0;\n    }\n"
    "    return n + suma(n - 1);\n}\n"
    "i32 main() {\n    return suma(8);\n}\n",
    36)

if ok1 and ok2:
    t.ok("OK: .vx compilado a ELF64 AArch64 (EM_AARCH64, emision via LibPEparse) "
         "ejecuta en qemu-system-aarch64: single-fn=42 y recursion cross-fn "
         "(CALL26)=36")
else:
    t.fail("FALLO: algun caso arm64 no ejecuto con el exit esperado")
t.finish()
