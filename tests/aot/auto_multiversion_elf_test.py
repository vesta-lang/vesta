#!/usr/bin/env python3
"""AUTO multiversion (--float-isa auto): el MAIN con un hot loop vectorizado se
despacha por cpuid.  La lowering renombra el main a __vx_main_body (compilado
3x sse2/avx2/avx512) y sintetiza un main fino que corre __vx_cpu_init +
__vx_auto_init y hace CALLIND a la variante elegida.

Verifica aot/182 con --float-isa auto:
  1. Contiene las TRES anchuras (xmm 128b + ymm 256b + zmm 512b).
  2. main hace el dispatch (call __vx_auto_init + call indirecto).
  3. Corre correcto (180960 & 0xFF = 224).

    wsl python3 tests/aot/auto_multiversion_elf_test.py <build_dir>
"""
import re

from aot_harness import AotTest

t = AotTest()
obj = t.wpath("amv.o")
if not t.compile_aot("examples_codes_vx/182_vectorize_elementwise.vx", obj,
                     float_isa="auto", arch="x86-64"):
    t.fail("FALLO: no se genero el .o auto")
    t.finish()

dis = t.disasm(obj, intel=True)
lines = dis.splitlines()

# 1. Las 3 anchuras presentes (xmm + ymm + zmm) = multiversion real.
xmm = t.count_lines(dis, r"\b(addpd|subpd|mulpd|movupd)\b")
ymm = sum(1 for ln in lines
          if re.search(r"v(add|sub|mul)pd|vmovupd", ln, re.I) and "ymm" in ln)
zmm = sum(1 for ln in lines
          if re.search(r"v(add|sub|mul)pd|vmovupd", ln, re.I) and "zmm" in ln)
if xmm > 0 and ymm > 0 and zmm > 0:
    t.ok("WIDTHS: 3 variantes (xmm=%d ymm=%d zmm=%d) OK" % (xmm, ymm, zmm))
else:
    t.fail("WIDTHS: FALLO -- esperaba las 3 anchuras (xmm=%d ymm=%d zmm=%d)" % (xmm, ymm, zmm))

# 2. main hace el dispatch (call indirecto a la variante).
main_body = []
in_main = False
for ln in lines:
    if "<main>:" in ln:
        in_main = True
    if in_main:
        main_body.append(ln)
        if re.search(r"\bret\b", ln):
            break
mainfn = "\n".join(main_body)
if re.search(r"call +[er][0-9a-z]+$|call +r1[0-5]", mainfn, re.M):
    t.ok("DISPATCH: main hace CALLIND a la variante OK")
else:
    t.fail("DISPATCH: FALLO -- main no hace llamada indirecta")

# 3. ejecucion correcta (auto elige la variante soportada por la CPU).
elf = t.wpath("amv")
if not t.gcc([obj], elf):
    t.fail("FALLO: gcc")
    t.finish()
got = t.run_exit(elf)
if got == 224:
    t.ok("RUN: 182 AUTO exit=%d OK (cpuid eligio la variante correcta)" % got)
else:
    t.fail("RUN: EXIT MISMATCH got=%d exp=224" % got)

t.finish()
