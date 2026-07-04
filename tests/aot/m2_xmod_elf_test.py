#!/usr/bin/env python3
"""Validacion ELF + valgrind del fix de globals del CPU dispatch cross-module:
m2.vx importa slib2.vx (s.length() -> __vx_strlen_fp despachado desde un DEP,
NO desde el root).  El slot fp es program-global (StaticDataMeta::shared_key) y
su init se prepone a main en el merge cross-module aunque el root no use
dispatch.  main devuelve 5.

    wsl python3 tests/aot/m2_xmod_elf_test.py <build_dir>
"""
from aot_harness import AotTest

t = AotTest()
obj = t.wpath("m2.o")
if not t.compile_aot("examples_codes_vx/aot/xmod_strlen/m2.vx", obj):
    t.fail("FALLO: no se genero el .o ELF de m2")
    t.finish()

elf = t.wpath("m2.elf")
if not t.gcc([obj], elf):
    t.fail("gcc fallo (m2)")
    t.finish()

t.expect_exit(elf, 5, "ELF m2")
t.valgrind(elf, "m2", errcode=88)
t.finish()
