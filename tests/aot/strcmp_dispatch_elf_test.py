#!/usr/bin/env python3
"""Validacion ELF + valgrind del CPU dispatch Inc 5a: strcmp/strlen despachados
por tabla de punteros + @HelperOverride(strcmp).  aot/58_strcmp_dispatch.vx (el
override SE invoca -> 42) + los string previos sin tocar.

    wsl python3 tests/aot/strcmp_dispatch_elf_test.py <build_dir>
"""
import glob

from aot_harness import AotTest

t = AotTest()
# nombre:esperado (exit-code).  58 = strcmp dispatch + override (42);
# resto = string previos intactos (usan strcmp/strlen despachados al baseline).
EXP = {58: 42, 51: 14, 37: 19, 49: 99, 53: 77, 52: 42}
VG = {58, 49, 53}  # los que alocan heap (concat / SRET / value-strings con heap)

for n in [58, 51, 37, 49, 53, 52]:
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
