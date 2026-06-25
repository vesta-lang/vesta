#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_vectorize.py -- Mide la ganancia de la auto-vectorizacion del JIT.

Para cada kernel vectorizable (element-wise, reduccion, dot-product/FMA) compila
TRES variantes y las ejecuta con el JIT (-m jit), midiendo el wall time:

  * escalar : VESTA_NO_VECTORIZE=1  -> el bucle NO se vectoriza (linea base).
  * sse2    : VESTA_VEC_ISA=sse2    -> chunk de 128b (W=2 f64).
  * avx2    : VESTA_VEC_ISA=avx2    -> chunk de 256b (W=4 f64).

(AVX512 no se mide por ejecucion: esta CPU no tiene avx512f; su codegen se valida
por disassembly aparte.)

El chunk se hornea en COMPILE time (VESTA_VEC_ISA); el JIT emite al ancho del host
(AUTO).  Verifica ademas que el resultado (R00) coincide entre las tres variantes.

Uso:  python tools/bench_vectorize.py [--build-dir cmake-build-release]
                                      [--reps N] [--runs K] [--n ELEMS]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

# Kernels: nombre -> cuerpo del bucle interior vectorizable (sobre f64* HOST).
# Cada uno se envuelve en un bucle externo de `reps` iteraciones (no vectorizable)
# para amplificar el trabajo y dar un wall time medible.
KERNELS = {
    "element-wise (c=a+b)": "for (i32 i = 0; i < n; i++) { c[i] = a[i] + b[i]; }",
    "reduccion (s+=a[i])":  "for (i32 i = 0; i < n; i++) { s = s + a[i]; }",
    "dot-product (s+=a*b)": "for (i32 i = 0; i < n; i++) { s = s + a[i] * b[i]; }",
}

VEX_TEMPLATE = """\
// Generado por tools/bench_vectorize.py -- benchmark de auto-vectorizacion.
f64 kernel(f64* a, f64* b, f64* c, i32 n, i32 reps) {{
    f64 s = 0.0;
    for (i32 r = 0; r < reps; r++) {{
        {inner}
        s = s + c[0] + a[0];   // dependencia: evita que el optimizer borre el bucle
    }}
    return s;
}}

i64 main() {{
    i32 n = {n};
    f64* a = malloc(n * 8);
    f64* b = malloc(n * 8);
    f64* c = malloc(n * 8);
    for (i32 i = 0; i < n; i++) {{ a[i] = 1.0; b[i] = 2.0; c[i] = 0.0; }}
    f64 r = kernel(a, b, c, n, {reps});
    free(a); free(b); free(c);
    return (i64) r;
}}
"""

MODES = [
    ("escalar", {"VESTA_NO_VECTORIZE": "1"}),
    ("sse2",    {"VESTA_VEC_ISA": "sse2"}),
    ("avx2",    {"VESTA_VEC_ISA": "avx2"}),
]


def run(cmd, env=None, timeout=600):
    """Ejecuta un comando capturando salida; devuelve (rc, stdout+stderr)."""
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    p = subprocess.run(cmd, env=full_env, capture_output=True, text=True,
                       timeout=timeout)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def extract_r00(out):
    """Extrae el valor R00=0x... de la salida de --stats (o None)."""
    m = re.search(r"R00=0x([0-9a-fA-F]+)", out)
    return m.group(1).lstrip("0") or "0" if m else None


def compile_variant(vm, vex_path, out_base, env):
    """Compila vex_path -> out_base.velb con el env del modo.  Devuelve el .velb."""
    rc, out = run([vm, "--vex", vex_path, "-o", out_base], env=env)
    velb = out_base + ".velb"
    if rc != 0 or not os.path.exists(velb):
        raise RuntimeError("fallo al compilar (%s):\n%s" % (env, out[-800:]))
    return velb


def time_run(vm, velb, runs):
    """Ejecuta el .velb con -m jit `runs` veces; devuelve (min_segundos, r00)."""
    best = None
    r00 = None
    for _ in range(runs):
        t0 = time.perf_counter()
        rc, out = run([vm, "--run", velb, "-m", "jit", "--stats"])
        dt = time.perf_counter() - t0
        if rc != 0:
            raise RuntimeError("fallo en ejecucion:\n%s" % out[-800:])
        r00 = extract_r00(out)
        best = dt if best is None else min(best, dt)
    return best, r00


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="cmake-build-release")
    ap.add_argument("--n", type=int, default=200000, help="elementos del array")
    ap.add_argument("--reps", type=int, default=2000,
                    help="repeticiones del bucle externo")
    ap.add_argument("--runs", type=int, default=5,
                    help="ejecuciones por variante (se toma el minimo)")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    vm = os.path.join(root, args.build_dir, "vm.exe")
    if not os.path.exists(vm):
        vm = os.path.join(root, args.build_dir, "vm")
    if not os.path.exists(vm):
        sys.exit("no se encuentra el binario vm en %s" % args.build_dir)

    total = args.n * args.reps
    print("== Benchmark auto-vectorizacion (JIT) ==")
    print("vm=%s" % vm)
    print("n=%d  reps=%d  ops_internas=%.1fM  runs=%d (min)\n"
          % (args.n, args.reps, total / 1e6, args.runs))

    hdr = "%-24s %10s %10s %10s   %8s %8s" % (
        "kernel", "escalar", "sse2", "avx2", "sse2x", "avx2x")
    print(hdr)
    print("-" * len(hdr))

    tmp = tempfile.mkdtemp(prefix="vecbench_")
    for name, inner in KERNELS.items():
        src = VEX_TEMPLATE.format(inner=inner, n=args.n, reps=args.reps)
        vex_path = os.path.join(tmp, "k.vex")
        with open(vex_path, "w") as f:
            f.write(src)

        times = {}
        r00s = {}
        for mode, env in MODES:
            out_base = os.path.join(tmp, "k_" + mode)
            velb = compile_variant(vm, vex_path, out_base, env)
            secs, r00 = time_run(vm, velb, args.runs)
            times[mode] = secs
            r00s[mode] = r00

        # correctness: las 3 variantes deben dar el MISMO R00.
        vals = set(v for v in r00s.values() if v is not None)
        ok = "" if len(vals) <= 1 else "  <!> DIVERGE %s" % r00s

        ts, ss, av = times["escalar"], times["sse2"], times["avx2"]
        print("%-24s %9.3fs %9.3fs %9.3fs   %7.2fx %7.2fx%s" % (
            name, ts, ss, av, ts / ss, ts / av, ok))

    print("\n(sse2x/avx2x = speedup vs escalar; minimo de %d ejecuciones)" % args.runs)


if __name__ == "__main__":
    main()
