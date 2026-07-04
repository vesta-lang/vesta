#!/usr/bin/env python3
"""Validacion ELF + valgrind del CPU dispatch de memcpy (Inc 2):
aot/56_memcpy_dispatch.vx + los ejemplos de string previos.  Emite el .o AOT
ELF, enlaza con gcc -no-pie, ejecuta (verifica exit-code) y valgrindea los que
usan heap (0 leaks / 0 errores).

    wsl python3 tests/aot/memcpy_dispatch_elf_test.py <build_dir>
"""
import glob

from aot_harness import AotTest

t = AotTest()
# nombre:esperado (exit-code).  56 = dispatch nuevo; resto = string previos.
EXP = {56: 107, 37: 19, 49: 99, 53: 77, 52: 42}
VG = {56, 49, 53}  # los que alocan heap (concat largo, SRET con heap)

for n in [56, 37, 49, 53, 52]:
    matches = sorted(glob.glob("examples_codes_vx/aot/%d_*.vx" % n))
    if not matches:
        t.fail("FALLO: no hay .vx para %d" % n)
        continue
    obj = t.wpath("m%d.o" % n)
    if not t.compile_aot(matches[0], obj):
        t.fail("FALLO: no se genero el .o ELF de %d" % n)
        continue
    elf = t.wpath("m%d.elf" % n)
    if not t.gcc([obj], elf):
        t.fail("gcc fallo (%d)" % n)
        continue
    t.expect_exit(elf, EXP[n], "ELF %d" % n)
    if n in VG:
        t.valgrind(elf, str(n), errcode=88)

t.finish()
