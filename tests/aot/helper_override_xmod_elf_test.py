#!/usr/bin/env python3
"""Validacion ELF + valgrind del CPU dispatch Inc 5b: @HelperOverride
CROSS-MODULE.  El modulo IMPORTADO (simdlib.vx) declara @HelperOverride(strcmp)
y el consumidor (main5b.vx) lo HEREDA via `import "simdlib";`.  main5b retorna
42 si las comparaciones son correctas Y el contador g_calls de la lib quedo >0.

    wsl python3 tests/aot/helper_override_xmod_elf_test.py <build_dir>
"""
import glob

from aot_harness import AotTest

t = AotTest()
MAIN = "examples_codes_vx/aot/inc5b/main5b.vx"

# --- 1. main5b (cross-module override) -> 42 ---
obj = t.wpath("m5b.o")
if not t.compile_aot(MAIN, obj):
    t.fail("FALLO: no se genero el .o ELF de main5b")
else:
    elf = t.wpath("m5b.elf")
    if t.gcc([obj], elf):
        t.expect_exit(elf, 42, "ELF main5b")
        t.valgrind(elf, "main5b", errcode=88)
    else:
        t.fail("gcc fallo (main5b)")

# --- 2. probe del contador cross-module: main5b modificado para retornar *ctr ---
with open(MAIN, encoding="utf-8") as fh:
    src = fh.read()
probe_src = src.replace(
    "    if (cmp_ok == 1 && used_ok == 1) {",
    "    return (i32)(*ctr);\n    if (cmp_ok == 1 && used_ok == 1) {", 1)
probe = t.write("probe5b.vx", probe_src)
pobj = t.wpath("p5b.o")
if t.compile_aot(probe, pobj):
    pelf = t.wpath("p5b.elf")
    if t.gcc([pobj], pelf):
        gc = t.run_exit(pelf)
        if gc == 10:
            t.ok("PROBE g_calls=%d OK (override cross-module invocado 10 veces)" % gc)
        else:
            t.fail("PROBE g_calls=%d INESPERADO (esperado 10)" % gc)

# --- 3. string previos single-file intactos ---
EXP = {58: 42, 57: 42, 56: 107, 51: 14, 37: 19, 49: 99, 53: 77, 52: 42}
for n in [58, 57, 56, 51, 37, 49, 53, 52]:
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

t.finish()
