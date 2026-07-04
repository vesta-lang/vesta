#!/usr/bin/env python3
"""Validacion del linker propio (`vesta --link`, sin ld/gcc para el enlace).
Escenarios A-I: un .o Vesta hosted, cross-file con C, --entry custom, multi-.o,
.bss, strings sin main (init cross-.o), link-script en Vesta, ELF32,
place_section.  Mas valgrind sobre (A).

    wsl python3 tests/aot/link_test.py <build_dir>
"""
from aot_harness import AotTest

t = AotTest()


def w(name, content):
    return t.write(name, content)


# --- A) un solo .o Vesta -> exec (hosted) ---
a_vx = w("a.vx", "i64 triple(i64 x) { return x * 3; }\ni64 main() { return triple(14); }\n")
a_o, a_elf = t.wpath("a.o"), t.wpath("a.elf")
t.compile_aot(a_vx, a_o)
if t.vm_link([a_o], a_elf):
    t.expect_exit(a_elf, 42, "LINK A (un .o)")
    t.valgrind(a_elf, "A", errcode=88)
else:
    t.fail("LINK A: no se genero el ejecutable")

# --- B) .o Vesta (extern) + .o de C (gcc) -> cross-file ---
kern_vx = w("kern.vx", 'extern "rt" { fn rt_value() -> i64; }\ni64 main() { return rt_value() + 2; }\n')
rt_c = w("rt.c", "long rt_value(void) { return 40; }\n")
kern_o, rt_o, prog_elf = t.wpath("kern.o"), t.wpath("rt.o"), t.wpath("prog.elf")
t.compile_aot(kern_vx, kern_o)
t.gcc_c(rt_c, rt_o, flags=("-fno-pic",))
if t.vm_link([kern_o, rt_o], prog_elf):
    t.expect_exit(prog_elf, 42, "LINK B (Vesta .o + C .o cross-file)")
else:
    t.fail("LINK B: no se genero el ejecutable")

# --- C) --entry custom (dev-OS: sin main, sin stub) ---
boot_c = w("boot.c", 'void _kstart(void) {\n    __asm__ volatile("mov $60, %%rax; mov $42, %%rdi; syscall" ::: "rax","rdi");\n}\n')
boot_o, boot_elf = t.wpath("boot.o"), t.wpath("boot.elf")
t.gcc_c(boot_c, boot_o, flags=("-ffreestanding", "-fno-pic"))
if t.vm_link([boot_o], boot_elf, entry="_kstart"):
    t.expect_exit(boot_elf, 42, "LINK C (--entry _kstart, sin main/stub)")
else:
    t.fail("LINK C: no se genero el ejecutable")

# --- D) multi-.o Vesta: libreria sin main + app que la referencia (extern) ---
lib_vx = w("lib.vx", "i64 quad(i64 x) { return x * 4; }\ni64 dec(i64 x) { return x - 1; }\n")
app_vx = w("app.vx", 'extern "lib" { fn quad(i64 x) -> i64; fn dec(i64 x) -> i64; }\ni64 main() { return dec(quad(11)) - 1; }\n')
lib_o, app_o, app_elf = t.wpath("lib.o"), t.wpath("app.o"), t.wpath("app.elf")
t.compile_aot(lib_vx, lib_o)
t.compile_aot(app_vx, app_o)
if t.vm_link([app_o, lib_o], app_elf):
    t.expect_exit(app_elf, 42, "LINK D (multi-.o Vesta: lib sin main + app)")
else:
    t.fail("LINK D: no se genero el ejecutable")

# --- E) .bss: global sin inicializar (NOBITS) escrito/leido ---
bss_c = w("bss.c", "static long g;   /* .bss */\nvoid _kstart(void) {\n    g = 42; long r = g;\n    __asm__ volatile(\"mov %0,%%rdi; mov $60,%%rax; syscall\" :: \"r\"(r) : \"rax\",\"rdi\");\n}\n")
bss_o, bss_elf = t.wpath("bss.o"), t.wpath("bss.elf")
t.gcc_c(bss_c, bss_o, flags=("-ffreestanding", "-fno-pic", "-fno-asynchronous-unwind-tables"))
if t.vm_link([bss_o], bss_elf, entry="_kstart"):
    t.expect_exit(bss_elf, 42, "LINK E (.bss global escrito/leido)")
else:
    t.fail("LINK E: no se genero el ejecutable")

# --- F) strings en un .o Vesta SIN main (CPU-dispatch init cross-.o) ---
slib_vx = w("slib.vx", 'i64 helper_a(i64 n) { string s = "x"; return s.length() + n; }\n')
sapp_vx = w("sapp.vx", 'extern "x" { fn helper_a(i64 n) -> i64; }\ni64 main() { string t = "y"; return helper_a(40) + t.length(); }\n')
srt_c = w("srt.c", "static char heap[1 << 20];\nstatic unsigned long off;\nvoid *malloc(unsigned long n) { void *p = &heap[off]; off += (n + 15) & ~15UL; return p; }\nvoid *calloc(unsigned long a, unsigned long b) { return malloc(a * b); }\nvoid free(void *p) { (void)p; }\n")
slib_o, sapp_o, srt_o, sapp_elf = t.wpath("slib.o"), t.wpath("sapp.o"), t.wpath("srt.o"), t.wpath("sapp.elf")
t.compile_aot(slib_vx, slib_o)
t.compile_aot(sapp_vx, sapp_o)
t.gcc_c(srt_c, srt_o, flags=("-fno-pic",))
if t.vm_link([sapp_o, slib_o, srt_o], sapp_elf):
    t.expect_exit(sapp_elf, 42, "LINK F (strings en .o sin main, init cross-.o)")
else:
    t.fail("LINK F: no se genero el ejecutable")

# --- G) link-script EN VESTA (configurable): fn link() fija base/entry ---
k_c = w("k.c", 'void _kstart(void) {\n    __asm__ volatile("mov $60,%%rax; mov $42,%%rdi; syscall" ::: "rax","rdi");\n}\n')
layout_vx = w("layout.vx", 'void link() {\n    u64 b = 0x800000;\n    if (debug_build()) { b = 0x900000; }\n    base(b);\n    entry("_kstart");\n    stack_size(align_up(40000, 4096));\n}\n')
k_o, k_elf = t.wpath("k.o"), t.wpath("k.elf")
t.gcc_c(k_c, k_o, flags=("-ffreestanding", "-fno-pic", "-fno-asynchronous-unwind-tables"))
if t.vm_link([k_o], k_elf, link_script=layout_vx):
    base_ok = t.count_lines(t.readelf(["-l"], k_elf), "0x0000000000800000")
    got = t.run_exit(k_elf)
    if got == 42 and base_ok >= 1:
        t.ok("LINK G (link-script Vesta: base 0x800000 + entry): exit=42 OK")
    else:
        t.fail("LINK G: got=%d base_ok=%d (esp 42 / base 0x800000)" % (got, base_ok))
else:
    t.fail("LINK G: no se genero el ejecutable")

# --- H) linkado de objetos de 32-bit (ELF32) con NUESTRO linker ---
f32_vx = w("f32.vx", "i32 fib(i32 n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\ni32 main() { return fib(10); }\n")
f32_o, f32_elf = t.wpath("f32.o"), t.wpath("f32.elf")
t.compile_aot(f32_vx, f32_o, arch="x86-32")
if t.vm_link([f32_o], f32_elf):
    cls = t.count_lines(t.readelf(["-h"], f32_elf), "ELF32")
    got = t.run_exit(f32_elf)
    if got == 55 and cls >= 1:
        t.ok("LINK H (objeto 32-bit ELF32 -> exec): exit=55 OK")
    else:
        t.fail("LINK H: got=%d cls32=%d (esp 55 / ELF32)" % (got, cls))
else:
    t.fail("LINK H: no se genero el ejecutable")

# --- I) place_section: VA fija por seccion desde el link-script Vesta ---
kb_c = w("kb.c", '__attribute__((section(".boot"))) void _kstart(void) {\n    __asm__ volatile("mov $60,%%rax; mov $42,%%rdi; syscall" ::: "rax","rdi");\n}\n')
kl_vx = w("kl.vx", 'void link() {\n    base(0x400000);\n    entry("_kstart");\n    place_section(".boot", 0x410000);\n}\n')
kb_o, kb_elf = t.wpath("kb.o"), t.wpath("kb.elf")
t.gcc_c(kb_c, kb_o, flags=("-ffreestanding", "-fno-pic", "-fno-asynchronous-unwind-tables"))
if t.vm_link([kb_o], kb_elf, link_script=kl_vx):
    at_ok = t.count_lines(t.readelf(["-SW"], kb_elf), "0000000000410000")
    got = t.run_exit(kb_elf)
    if got == 42 and at_ok >= 1:
        t.ok("LINK I (place_section .boot @0x410000): exit=42 OK")
    else:
        t.fail("LINK I: got=%d at_ok=%d (esp 42 / .boot @0x410000)" % (got, at_ok))
else:
    t.fail("LINK I: no se genero el ejecutable")

print("AOT.5 linker: TODOS OK" if t.rc == 0 else "AOT.5 linker: FALLOS")
t.finish()
