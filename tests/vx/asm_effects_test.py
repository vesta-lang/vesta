#!/usr/bin/env python3
"""Resumen de efectos de un bloque de inline asm (lectura que el compilador
hace de un asm inline).

Maneja el binario real via `vm --asm-effects <fichero> --arch <arch>
--asm-effects-json` y comprueba el modelo de efecto que reporta sobre cuerpos
NASM/ARM REALES (los de las primitivas atomicas y patrones tipicos).  Es el
flujo de verdad: el mismo camino que consumira el LSP.

    python3 tests/vx/asm_effects_test.py <build_dir>

Convencion del proyecto: los scripts de automatizacion van en Python (no .sh).
"""
import json
import os
import subprocess
import sys
import tempfile

if len(sys.argv) < 2:
    sys.exit("uso: python %s <build_dir>" % os.path.basename(sys.argv[0]))

BUILD = sys.argv[1]
VM = os.path.join(BUILD, "vm.exe")
if not os.path.exists(VM):
    VM = os.path.join(BUILD, "vm")
if not os.path.exists(VM):
    sys.exit("no encuentro vm(.exe) en %s" % BUILD)

TMP = tempfile.mkdtemp(prefix="asmfx_")
n_ok = 0
n_fail = 0


def effects(body, arch):
    """Escribe el cuerpo a un fichero temporal y devuelve el JSON de efectos."""
    p = os.path.join(TMP, "blk.s")
    with open(p, "w", encoding="ascii") as f:
        f.write(body)
    cmd = [VM, "--asm-effects", p, "--arch", arch, "--asm-effects-json"]
    out = subprocess.run(cmd, capture_output=True, text=True)
    # stdout es la unica linea JSON; el exit code refleja known().
    line = out.stdout.strip().splitlines()[-1] if out.stdout.strip() else "{}"
    return json.loads(line), out.returncode


def check(cond, msg):
    global n_ok, n_fail
    if cond:
        n_ok += 1
    else:
        n_fail += 1
        print("  FAIL: %s" % msg)


# --- x86: lock cmpxchg (CAS atomico) -------------------------------------
e, rc = effects("  mov rax, rsi\n  lock cmpxchg [rdi], rdx\n", "x86_64")
check(e["known"] and rc == 0, "x86 cas: conocido (rc=0)")
check(e["has_atomic"], "x86 cas: lock -> atomica")
check(e["touches_mem"], "x86 cas: toca memoria")
check(not e["is_call"], "x86 cas: sin call")
check(e["explicit_stack_bytes"] == 0, "x86 cas: sin marco explicito")

# --- x86: prologo con marco explicito (pico, no neto) --------------------
e, rc = effects(
    "  push rbp\n  sub rsp, 32\n  mov [rsp], rax\n  add rsp, 32\n  pop rbp\n",
    "x86_64",
)
check(e["known"], "x86 prologo: conocido")
check(e["explicit_stack_bytes"] == 40, "x86 prologo: marco pico = 40")
check(e["touches_mem"], "x86 prologo: toca memoria")

# --- x86: rama + flags ---------------------------------------------------
e, rc = effects("  cmp rax, rbx\n  jne .otro\n.otro:\n", "x86_64")
check(e["known"], "x86 rama: conocido")
check(e["has_branch"], "x86 rama: jne -> rama")
check(e["touches_flags"], "x86 rama: cmp -> flags")
check(not e["touches_mem"], "x86 rama: sin memoria")

# --- arm64: bucle LL/SC (ldaxr/stlxr) ------------------------------------
e, rc = effects(
    ".retry:\n  ldaxr x3, [x0]\n  cmp x3, x1\n  b.ne .done\n"
    "  stlxr w4, x2, [x0]\n  cbnz w4, .retry\n.done:\n",
    "arm64",
)
check(e["has_atomic"], "arm64 cas: ldaxr/stlxr -> atomica")
check(e["touches_mem"], "arm64 cas: [x0] -> memoria")
check(e["has_branch"], "arm64 cas: b.ne/cbnz -> rama")
check("ldaxr" not in e["unknown_mnemonics"], "arm64 cas: ldaxr reconocida")
check("stlxr" not in e["unknown_mnemonics"], "arm64 cas: stlxr reconocida")

# --- desconocido: error claro (rc != 0 + nombra el mnemonico) ------------
e, rc = effects("  chorradadesconocida rax, rbx\n", "x86_64")
check(not e["known"] and rc != 0, "desconocido: known=false + rc!=0")
check(e["unknown_mnemonics"] == ["chorradadesconocida"],
      "desconocido: nombra el mnemonico exacto")

if n_fail == 0:
    print("=== asm_effects_test: %d checks OK, 0 fallidos ===" % n_ok)
    sys.exit(0)
else:
    print("=== asm_effects_test: %d checks, %d FALLIDOS ===" % (n_ok, n_fail))
    sys.exit(1)
