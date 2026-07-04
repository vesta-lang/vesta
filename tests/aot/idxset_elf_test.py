#!/usr/bin/env python3
"""Validacion ELF + valgrind del ejemplo aot/54_index_set.vx.

Emite el .o AOT, enlaza con gcc -no-pie (libc para malloc/free de los
value-strings HEAP), ejecuta (exit 42) y valgrindea (0 leaks / 0 errores).

    wsl python3 tests/aot/idxset_elf_test.py <build_dir>
"""
from aot_harness import AotTest

t = AotTest()
obj = t.wpath("idxset.o")
if not t.compile_aot("examples_codes_vx/aot/54_index_set.vx", obj):
    t.fail("FALLO: no se genero el .o ELF")
    t.finish()

elf = t.wpath("idxset.elf")
if not t.gcc([obj], elf):
    t.fail("gcc fallo")
    t.finish()

t.expect_exit(elf, 42, "ELF")
t.valgrind(elf, "", errcode=99)
t.finish()
