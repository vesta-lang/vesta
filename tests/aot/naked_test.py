#!/usr/bin/env python3
"""Validacion de @Naked (funciones sin prologo/epilogo/ret).

Corre en el HOST: compila a PE y ejecuta el .exe (exit code = return de main).
Tambien valida el .o ELF por desensamblado (objdump) cuando este disponible:
que el ISR exista, sea GLOBAL y su cuerpo sea EXACTAMENTE el asm del usuario
(cero prologo).

    python tests/aot/naked_test.py <build_dir>
"""
import re

from aot_harness import AotTest

t = AotTest()

# --- 1) @Naked con params (Win64): add_naked(40,2) == 42, via PE en el host ---
nadd = t.write("nadd.vx",
               "@Naked i64 add_naked(i64 a, i64 b) {\n"
               "    asm volatile {\n"
               "        mov rax, rcx\n"
               "        add rax, rdx\n"
               "        ret\n"
               "    }\n"
               "}\n"
               "i64 main() { return add_naked(40, 2); }\n")
exe = t.wpath("nadd.exe")
if t.compile_aot(nadd, exe, fmt="pe", emit="exe"):
    got = t.run_exit(exe)
    if got == 42:
        t.ok("NAKED 1 (add_naked PE host): exit=42 OK")
    else:
        t.fail("NAKED 1: EXIT MISMATCH got=%d exp=42" % got)
else:
    t.fail("NAKED 1: no se genero el .exe")

# --- 2) ISR void @Naked: presente, GLOBAL y cuerpo byte-exacto (objdump) ---
nisr = t.write("nisr.vx",
               "@Naked void isr_timer() {\n"
               "    asm volatile {\n"
               "        push rax\n"
               "        mov al, 0x20\n"
               "        out 0x20, al\n"
               "        pop rax\n"
               "        iretq\n"
               "    }\n"
               "}\n"
               "i64 main() { return 0; }\n")
nisr_o = t.wpath("nisr.o")
t.compile_aot(nisr, nisr_o, fmt="elf", emit="obj")
if t.have("objdump"):
    syms = t.symbols(nisr_o)
    # isr_timer debe existir y ser GLOBAL ('g' en la columna de binding).
    if re.search(r"\bg\b.*\bisr_timer$", syms, re.M):
        t.ok("NAKED 2a (isr_timer GLOBAL en .o): OK")
    else:
        t.fail("NAKED 2a: isr_timer no es GLOBAL o no existe")
    # Su cuerpo NO debe contener push rbp (sin prologo) y SI iretq.
    dis = t.disasm(nisr_o, intel=True).splitlines()
    body, cap = [], False
    for ln in dis:
        if "<isr_timer>:" in ln:
            cap = True
            continue
        if cap and ln.strip() == "":
            break
        if cap:
            body.append(ln)
    body_txt = "\n".join(body)
    if re.search(r"push +rbp", body_txt, re.I):
        t.fail("NAKED 2b: PROLOGO detectado en isr_timer (no debe haberlo)")
    elif re.search(r"iretq", body_txt, re.I):
        t.ok("NAKED 2b (sin prologo, iretq presente): OK")
    else:
        t.fail("NAKED 2b: no se pudo desensamblar isr_timer")
else:
    print("NAKED 2 (objdump no disponible): SKIP")

if t.rc == 0:
    print("=== naked_test: TODO OK ===")
else:
    print("=== naked_test: FALLOS ===")
t.finish()
