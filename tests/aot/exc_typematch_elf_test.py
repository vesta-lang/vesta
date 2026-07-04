#!/usr/bin/env python3
"""Validacion ELF del type matching de excepciones AOT (multi-catch + subtipo +
propagacion).  aot/62_exc_typematch.vx: dispatch por tipo lanzado, subtipo
(catch Base captura Derived) y re-throw al try externo cuando ningun catch
matchea.  main devuelve 147.  El runtime se auto-bundlea.

    wsl python3 tests/aot/exc_typematch_elf_test.py <build_dir>
"""
from aot_harness import AotTest

t = AotTest()
obj = t.wpath("prog.o")
if not t.compile_aot("examples_codes_vx/aot/62_exc_typematch.vx", obj, arch="x86-64"):
    t.fail("FALLO: no se genero el .o")
    t.finish()

elf = t.wpath("tm.elf")
if not t.gcc([obj], elf):
    t.fail("FALLO: gcc no enlazo")
    t.finish()

got = t.run_exit(elf)
if got == 147:
    t.ok("TYPE-MATCH ELF: run exit=%d OK (multi-catch + subtipo + propagacion)" % got)
else:
    t.fail("TYPE-MATCH ELF: EXIT MISMATCH got=%d exp=147" % got)

t.finish()
