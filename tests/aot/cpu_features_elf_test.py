#!/usr/bin/env python3
"""Validacion ELF + valgrind del cimiento CPU dispatch (aot/55_cpu_features.vx).

Emite el .o AOT ELF, enlaza con gcc -no-pie, ejecuta (exit 42 -> SSE2 detectado)
y valgrindea (cpuid no toca heap -> 0 leaks / 0 errores).

    wsl python3 tests/aot/cpu_features_elf_test.py <build_dir>
"""
from aot_harness import AotTest

t = AotTest()
obj = t.wpath("cpu.o")
if not t.compile_aot("examples_codes_vx/aot/55_cpu_features.vx", obj):
    t.fail("FALLO: no se genero el .o ELF")
    t.finish()

elf = t.wpath("cpu.elf")
if not t.gcc([obj], elf):
    t.fail("gcc fallo")
    t.finish()

t.expect_exit(elf, 42, "ELF (esperado 42 = SSE2 detectado)")
t.valgrind(elf, "", errcode=99)
t.finish()
