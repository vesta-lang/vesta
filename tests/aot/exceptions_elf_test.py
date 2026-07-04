#!/usr/bin/env python3
"""Validacion ELF de las excepciones nativas AOT (auto-hospedadas en Vesta,
setjmp/longjmp; stdlib/vx/vx_exc.vx).

aot/61_exceptions.vx: try/catch/throw cruzando frontera de funcion; el objeto
lanzado (heap) sobrevive al longjmp y el catch lee su campo.  main devuelve 42.
El runtime de excepciones se AUTO-bundlea en el mismo objeto.  Solo se valida
el exit-code (el objeto capturado se "fuga" a proposito en v1).

    wsl python3 tests/aot/exceptions_elf_test.py <build_dir>
"""
from aot_harness import AotTest

t = AotTest()
obj = t.wpath("prog.o")
if not t.compile_aot("examples_codes_vx/aot/61_exceptions.vx", obj, arch="x86-64"):
    t.fail("FALLO: no se genero prog.o")
    t.finish()

elf = t.wpath("exc.elf")
if not t.gcc([obj], elf):
    t.fail("FALLO: gcc no enlazo (auto-bundle del runtime fallo?)")
    t.finish()

got = t.run_exit(elf)
if got == 42:
    t.ok("EXCEPCIONES ELF: run exit=%d OK (try/catch/throw cross-fn + e.code)" % got)
else:
    t.fail("EXCEPCIONES ELF: EXIT MISMATCH got=%d exp=42" % got)

t.finish()
