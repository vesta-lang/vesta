#!/usr/bin/env python3
"""Regresion de variables GLOBALES de coma flotante (interp + JIT + AOT).

El valor de un `f32`/`f64` son sus BITS IEEE, asi que el camino de globales no
puede asumir i64/f64 e ignorar el tipo declarado: si lo hace, el resultado no es
aproximado, es basura.  Los tres bugs que fija este test:

  1. `g += 1.5` tres veces sobre un `f64` daba -0.75 (suma ENTERA de los bits)
     mientras `g = g + 1.5` daba 4.5.  Esa asimetria entre las dos formas de
     escribir lo mismo es el sintoma que delata el problema.
  2. Un global `f32` valia 0 SIEMPRE: sus bytes iniciales se grababan como los
     de un double, y el LOAD lee 4 -> para 0.5 esos 4 bytes bajos son ceros.
  3. `__module_init` machacaba en runtime los bytes ya correctos con un STORE
     sin convertir al ancho del global.

Se ejecuta en los TRES modos porque el bug vivia en el lowering (comun a todos)
y el JIT/AOT tienen sus propios caminos para float.

    python3 tests/vx/global_float_test.py <build_dir>

Convencion del proyecto: los scripts de automatizacion van en Python (no .sh).
"""
import os
import re
import subprocess
import sys
import tempfile

if len(sys.argv) < 2:
    sys.exit("uso: python %s <build_dir>" % os.path.basename(sys.argv[0]))

BUILD = sys.argv[1]
ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), "..", ".."))
VM = os.path.join(BUILD, "vm.exe")
if not os.path.exists(VM):
    VM = os.path.join(BUILD, "vm")
if not os.path.exists(VM):
    sys.exit("no encuentro vm(.exe) en %s" % BUILD)

TMP = tempfile.mkdtemp(prefix="vxgflt_")

SRC = "examples_codes_vx/312_global_compound_assign.vx"
WANT = 42

# Programa minimo aparte: comprueba los VALORES, no solo el R00 agregado, para
# que un fallo diga QUE tipo se rompio en vez de "el ejemplo devolvio 6".
VALUES_SRC = """
f64 d = 0.5;
f32 f = 0.5;
i64 n = 7;
i32 main() {
    println("d=${d}");
    println("f=${f}");
    println("n=${n}");
    d = 1.25; f = 1.25;
    println("d2=${d}");
    println("f2=${f}");
    d += 0.25; f += 0.25;
    println("d3=${d}");
    println("f3=${f}");
    return 42;
}
"""
VALUES_WANT = {
    "d": "0.5", "f": "0.5", "n": "7",     # init: el f32 valia 0
    "d2": "1.25", "f2": "1.25",           # asignacion: el f32 valia 0
    "d3": "1.5", "f3": "1.5",             # compound: el f64 daba basura
}


def compile_vx(src_abs, out, extra=None):
    """Compila @p src_abs; devuelve (ok, salida)."""
    cmd = [VM, "--vesta", src_abs, "-o", out] + (extra or [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0, r.stdout + r.stderr


def r0_of(velb, mode):
    """R00 tras ejecutar @p velb en @p mode ('vm' o 'jit'), o None si no corrio."""
    r = subprocess.run([VM, "--run", velb, "-m", mode, "--schedulers", "1",
                        "--stats"], capture_output=True, text=True)
    m = re.search(r"R00=0x([0-9a-fA-F]+)", r.stdout + r.stderr)
    return int(m.group(1), 16) if m else None


def check_example():
    """El ejemplo 312 debe dar 42 en interp, JIT y AOT."""
    ok = True
    out = os.path.join(TMP, "e312")
    built, log = compile_vx(os.path.join(ROOT, SRC), out)
    if not built or not os.path.exists(out + ".velb"):
        print("FAIL: %s no compilo\n%s" % (SRC, log))
        return False

    for mode in ("vm", "jit"):
        got = r0_of(out + ".velb", mode)
        if got != WANT:
            print("FAIL: %s (-m %s): R00 == %s, se esperaba %d"
                  % (SRC, mode, got, WANT))
            ok = False
        else:
            print("OK: %s (-m %s) -> R0 = %d" % (SRC, mode, WANT))

    # AOT: el exit-code del binario nativo es el return de main.
    aot = os.path.join(TMP, "e312_aot")
    built, log = compile_vx(os.path.join(ROOT, SRC), aot,
                            ["-m", "aot", "--emit", "exe"])
    exe = aot + (".exe" if os.name == "nt" else "")
    if os.name == "nt" and os.path.exists(aot) and not os.path.exists(exe):
        os.replace(aot, exe)      # el emisor PE escribe sin extension
    if not built or not os.path.exists(exe):
        print("FAIL: %s no compilo a AOT\n%s" % (SRC, log))
        return False
    rc = subprocess.run([exe], capture_output=True, text=True).returncode
    if rc != WANT:
        print("FAIL: %s (-m aot): exit == %d, se esperaba %d" % (SRC, rc, WANT))
        ok = False
    else:
        print("OK: %s (-m aot) -> exit = %d" % (SRC, WANT))
    return ok


def check_values():
    """Los valores impresos, para localizar QUE tipo se rompe si falla."""
    src = os.path.join(TMP, "gflt_values.vx")
    with open(src, "w", encoding="utf-8") as fh:
        fh.write(VALUES_SRC)
    out = os.path.join(TMP, "gflt_values")
    built, log = compile_vx(src, out)
    if not built or not os.path.exists(out + ".velb"):
        print("FAIL: programa de valores no compilo\n%s" % log)
        return False

    ok = True
    for mode in ("vm", "jit"):
        r = subprocess.run([VM, "--run", out + ".velb", "-m", mode],
                           capture_output=True, text=True)
        txt = r.stdout + r.stderr
        for key, want in VALUES_WANT.items():
            m = re.search(r"^%s=(.+)$" % re.escape(key), txt, re.MULTILINE)
            got = m.group(1).strip() if m else None
            if got != want:
                print("FAIL: valores (-m %s): %s == %s, se esperaba %s"
                      % (mode, key, got, want))
                ok = False
        if ok:
            print("OK: valores de globales f64/f32/i64 (-m %s)" % mode)
    return ok


def main():
    fails = 0
    if not check_example():
        fails += 1
    if not check_values():
        fails += 1
    if fails:
        sys.exit("=== globales float: %d bloque(s) fallido(s) ===" % fails)
    print("=== globales float: OK, 0 fallidos ===")


if __name__ == "__main__":
    main()
