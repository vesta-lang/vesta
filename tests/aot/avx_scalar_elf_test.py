#!/usr/bin/env python3
"""AVX escalar 3-operandos no-destructivo.  Con --float-isa avx las binarias
f64/f32 escalares se emiten en VX (VADDSD/VSUBSD/VMULSD/VDIVSD + SS) en vez de
legacy SSE -> sin el mov de coalescing 2-address y sin mezclar legacy con VX.
El default (sse2) sigue legacy.

    wsl python3 tests/aot/avx_scalar_elf_test.py <build_dir>
"""
from aot_harness import AotTest

t = AotTest()
PROG = "examples_codes_vx/aot/63_avx_scalar.vx"

# AVX -> VX scalar.
avx_o = t.wpath("a.o")
if not t.compile_aot(PROG, avx_o, float_isa="avx"):
    t.fail("FALLO: no se genero el .o avx")
    t.finish()
dis = t.disasm(avx_o)
vx = t.count_lines(dis, r"vaddsd|vsubsd|vmulsd|vdivsd")
leg = t.count_lines(dis, r"\baddsd|\bsubsd|\bmulsd|\bdivsd")
if vx >= 1 and leg == 0:
    t.ok("AVX: %d VX-scalar, 0 legacy OK (sin mezcla)" % vx)
else:
    t.fail("AVX: FALLO vx=%d legacy=%d (esperado vx>=1, legacy=0)" % (vx, leg))

# SSE2 default -> legacy, 0 VX.
sse_o = t.wpath("s.o")
t.compile_aot(PROG, sse_o)
svex = t.count_lines(t.disasm(sse_o), r"vaddsd|vmulsd")
if svex == 0:
    t.ok("SSE2: 0 VX-scalar OK (legacy, sin regresion)")
else:
    t.fail("SSE2: FALLO %d VX (deberia ser legacy)" % svex)

# Ejecucion (el C aporta f64 runtime, sin conversiones Vesta).
csrc = t.write("m.c",
               "double fma2(double,double,double); double poly(double);\n"
               "int main(){ return (int)(fma2(3.0,4.0,5.0) + poly(6.0)); } /* 17+30 = 47 */\n")
elf = t.wpath("m")
if not t.gcc([csrc, avx_o], elf):
    t.fail("FALLO: gcc")
    t.finish()
got = t.run_exit(elf)
if got == 47:
    t.ok("RUN: avx-scalar exit=%d OK" % got)
else:
    t.fail("RUN: EXIT MISMATCH got=%d exp=47" % got)

t.finish()
