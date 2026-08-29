#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_vectorize.py -- Comprueba que los bucles SIGUEN ensanchandose.

Es el complemento de bench_vectorize.py, que mide lo RAPIDO que va; esto mira
si de verdad ocurre.  Hace falta separarlo porque aqui un fallo NO SE VE: un
bucle que deja de reconocerse se baja de uno en uno, da exactamente el mismo
numero y cualquier test que compare resultados pasa igual, solo que el programa
va mas lento.

Tres modos:

  record <dir>    guarda lo generado antes de tocar el vectorizador
  compare <dir>   lo vuelve a generar y dice QUE cambio
  idioms          afirma, contra numeros escritos aqui, que cada forma que el
                  compilador dice reconocer sigue disparando

`record`/`compare` miran lo generado por las tres vias en que se puede mirar --
el IR de bytecode, el del camino nativo (que es otro pipeline) y la salida de
los tres modos de ejecucion -- porque un refactor del vectorizador toca
construccion de grafo de control, y ahi los fallos no dan error: dan una arista
de mas o una entrada de phi equivocada.

`idioms` es una comprobacion ABSOLUTA y no comparativa, y por eso los minimos
estan escritos a mano: comparar contra una grabacion no basta, porque si algo
se rompe y luego se vuelve a grabar, la rotura se convierte en la referencia y
no salta nunca mas.

Lo que comprueba `idioms` son las funciones de `std.numeric`, no bucles
escritos para el test.  La libreria ES la escritura canonica de cada forma y es
lo que un programa de verdad llama, asi que es lo que tiene que seguir
ensanchandose; un bucle escrito aparte solo para el test puede seguir
disparando mientras la libreria deja de hacerlo.

Uso:  python tools/verify_vectorize.py idioms [--build-dir cmake-build-release]
      python tools/verify_vectorize.py record  <dir>
      python tools/verify_vectorize.py compare <dir>
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

# Ejemplos cuyo IR se graba y compara.  La lista sale de MIRAR el IR, no de
# adivinar por el nombre: un fichero puede llamarse "vectorize_algo" y no llegar
# a ensanchar nada.
EXAMPLES = [
    "182_vectorize_elementwise.vx",
    "184_vectorize_reduction.vx",
    "185_vectorize_int.vx",
    "187_vectorize_unary.vx",
    "188_vectorize_f32.vx",
    "189_vectorize_while.vx",
    "190_vectorize_fma.vx",
    "191_vectorize_compound.vx",
    "192_vectorize_scalar.vx",
    "193_vectorize_int_widths.vx",
    "11_arrays_nativos.vx",
    "492_vectorizador_completo.vx",
    "493_std_numeric.vx",
]

# Instrucciones que operan varios carriles a la vez.  OJO: las ETIQUETAS de los
# bloques que monta el vectorizador se llaman igual (vec_main_hdr_3:), asi que
# no vale con buscar la cadena "vec_" -- hay que exigir que sea una
# instruccion, o sea que abra la linea y que el opcode este en esta lista.
WIDE_OPS = ("binop", "binop_s", "unop", "bcast",
            "acc_zero", "acc_add", "acc_combine", "acc_store", "acc_fma")

RE_FUNC = re.compile(r"^(?:@function|function|func)\s+([A-Za-z_][A-Za-z_0-9]*)")
RE_WIDE = re.compile(r"^[ \t]*vec_(?:%s)[ \t]" % "|".join(WIDE_OPS))
# memcpy/memset: cuando el bucle era una copia o un relleno, el mejor resultado
# NO es ensancharlo sino que desaparezca entero y quede una sola instruccion de
# bloque.  Contar eso como cero seria leerlo justo al reves.
RE_BULK = re.compile(r"^[ \t]*(?:memcpy|memset)[ \t]")

# Lo que `idioms` exige, y de donde sale cada cosa.
#
# La columna de la izquierda es el nombre de la instancia monomorfizada tal y
# como aparece en el IR.  Que este el tipo en el nombre no es un detalle: el
# ancho del elemento decide cuantos carriles caben, asi que cada uno toma un
# camino distinto y hay que fijarlos por separado.
LIBRARY_MUST_WIDEN = [
    "std__numeric__add_i64",
    "std__numeric__add_i32",
    "std__numeric__add_f32",
    "std__numeric__add_f64",
    "std__numeric__sub_i64",
    "std__numeric__mul_i32",
    "std__numeric__mul_i16",
    "std__numeric__mul_f64",
    "std__numeric__div_f64",
    "std__numeric__add_scalar_i64",
    "std__numeric__mul_scalar_i32",
    "std__numeric__add_into_i64",
    "std__numeric__sub_into_i64",
    "std__numeric__negate_f64",
    "std__numeric__sum_i64",
    "std__numeric__sum_f64",
]

# Lo que la libreria NO cubre y comprueba el ejemplo 492 con bucles a mano.
#
# La copia no se ensancha: se reconoce que el bucle entero es un movimiento de
# bloque y queda UNA instruccion, que es mejor.  Su sitio en la libreria es
# std.memory, no std.numeric.
RAW_MUST_BULK = [
    "ejemplos__vectorizador_completo__copy_while",
    "ejemplos__vectorizador_completo__copy_for",
]

# Y las formas que HOY no se reconocen.  Se fijan igual que las demas: si
# alguna empieza a ensancharse sera porque alguien lo hizo a proposito, y
# entonces esta linea es la que hay que mover.
RAW_MUST_NOT_WIDEN = [
    # Dos operaciones en el cuerpo; el reconocedor exige una.
    "ejemplos__vectorizador_completo__chain",
    # Producto de 64 bits: no existe empaquetado en ninguna de las maquinas.
    "ejemplos__vectorizador_completo__scalar_i64",
]


def run(cmd, env=None, timeout=900):
    """Ejecuta un comando y devuelve (codigo, salida combinada)."""
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        p = subprocess.run(cmd, env=e, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -1, "(agotado el tiempo)"


def count_by_function(ir_path):
    """Cuenta, por funcion, las instrucciones anchas y las de bloque.

    @param ir_path Fichero con el volcado del IR.
    @return dict nombre -> (anchas, bloque).  Vacio si el fichero no existe.
    """
    out = {}
    if not os.path.exists(ir_path):
        return out
    current = ""
    with open(ir_path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = RE_FUNC.match(line)
            if m:
                current = m.group(1)
                out.setdefault(current, [0, 0])
                continue
            if not current:
                continue
            if RE_WIDE.match(line):
                out[current][0] += 1
            elif RE_BULK.match(line):
                out[current][1] += 1
    return {k: tuple(v) for k, v in out.items()}


def find_vm(build_dir):
    """Localiza el binario del compilador."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for name in ("vm.exe", "vm"):
        p = os.path.join(root, build_dir, name)
        if os.path.exists(p):
            return root, p
    sys.stderr.write("no encuentro el binario en %s -- compila primero\n"
                     % build_dir)
    sys.exit(2)


def drop_caches(root):
    """Tira las caches, para que lo que se mide se genere de verdad."""
    for d in (".cache", ".vx_cache"):
        shutil.rmtree(os.path.join(root, d), ignore_errors=True)


def emit_ir(vm, root, src, out_base, native=False):
    """Compila pidiendo el volcado del IR; devuelve la ruta del .ir o None."""
    cmd = [vm]
    if native:
        cmd += ["-m", "aot", "--format", "pe", "--emit", "exe"]
    cmd += ["--vx-emit-ir", "--vesta", src, "-o", out_base]
    drop_caches(root)
    run(cmd)
    ir = out_base + ".ir"
    return ir if os.path.exists(ir) else None


def capture_one(vm, root, src, dst_dir, work_dir):
    """Guarda en @p dst_dir todo lo observable de un ejemplo.

    Son cuatro cosas y hacen falta las cuatro: el IR de bytecode, el del camino
    nativo (otro pipeline, puede diverger), la cuenta por funcion y la salida de
    los tres modos de ejecucion, que es lo unico que el usuario ve.
    """
    base = os.path.splitext(os.path.basename(src))[0]
    os.makedirs(work_dir, exist_ok=True)

    ir = emit_ir(vm, root, src, os.path.join(work_dir, base))
    if ir:
        shutil.copy(ir, os.path.join(dst_dir, base + ".ir"))
    nat = emit_ir(vm, root, src, os.path.join(work_dir, base + "_n"),
                  native=True)
    if nat:
        shutil.copy(nat, os.path.join(dst_dir, base + ".native.ir"))

    for tag, path in (("bytecode", os.path.join(dst_dir, base + ".ir")),
                      ("nativo", os.path.join(dst_dir, base + ".native.ir"))):
        counts = count_by_function(path)
        with open(os.path.join(dst_dir, "%s.%s.count" % (base, tag)),
                  "w", encoding="utf-8") as fh:
            for name in sorted(counts):
                wide, bulk = counts[name]
                fh.write("%-56s anchas=%d bloque=%d\n" % (name, wide, bulk))

    drop_caches(root)
    run([vm, "--vesta", src, "-o", os.path.join(work_dir, base)])
    velb = os.path.join(work_dir, base + ".velb")
    lines = []
    for tag, extra in (("interprete", ["-m", "vm"]), ("jit", [])):
        lines.append("--- " + tag)
        if os.path.exists(velb):
            _, out = run([vm, "--run", velb] + extra)
            lines.extend(l for l in out.splitlines()
                         if not re.search(r"Tiempo|VESTA_TIMES|^\[vx\]", l))
        else:
            lines.append("(no compilo)")
    with open(os.path.join(dst_dir, base + ".output"), "w",
              encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def cmd_record(vm, root, ref_dir, work_dir):
    """Graba el estado actual de todos los ejemplos."""
    shutil.rmtree(ref_dir, ignore_errors=True)
    os.makedirs(ref_dir, exist_ok=True)
    n = 0
    for name in EXAMPLES:
        src = os.path.join(root, "examples_codes_vx", name)
        if not os.path.exists(src):
            continue
        capture_one(vm, root, src, ref_dir, work_dir)
        n += 1
    print("grabados %d ejemplos en %s" % (n, ref_dir))
    return 0


def cmd_compare(vm, root, ref_dir, work_dir):
    """Vuelve a generarlo todo y dice que cambio."""
    if not os.path.isdir(ref_dir):
        sys.stderr.write("no existe %s -- graba primero\n" % ref_dir)
        return 2
    new_dir = os.path.join(work_dir, "new")
    shutil.rmtree(new_dir, ignore_errors=True)
    os.makedirs(new_dir, exist_ok=True)
    for name in EXAMPLES:
        src = os.path.join(root, "examples_codes_vx", name)
        if os.path.exists(src):
            capture_one(vm, root, src, new_dir, work_dir)

    bad = 0
    for fname in sorted(os.listdir(ref_dir)):
        old = os.path.join(ref_dir, fname)
        new = os.path.join(new_dir, fname)
        if not os.path.exists(new):
            print("FALTA: %s (antes se generaba y ahora no)" % fname)
            bad += 1
            continue
        with open(old, encoding="utf-8", errors="replace") as f1, \
             open(new, encoding="utf-8", errors="replace") as f2:
            a, b = f1.read(), f2.read()
        if a != b:
            print("CAMBIA: %s" % fname)
            bad += 1

    if bad == 0:
        print("todo igual: el IR, el del camino nativo, la cuenta de")
        print("instrucciones anchas y la salida de los dos modos.")
        return 0
    print("")
    print("%d ficheros distintos." % bad)
    print("Cambiar no es romper: mira cada uno y decide si es el cambio que")
    print("buscabas.  Pero si lo que cambio es la CUENTA, un bucle dejo de")
    print("ensancharse -- y eso no se ve en la salida, porque bajarlo de uno en")
    print("uno da el mismo numero.")
    return 1


def cmd_idioms(vm, root, work_dir):
    """Afirma que cada forma reconocida sigue disparando."""
    os.makedirs(work_dir, exist_ok=True)
    counts = {}
    for name in ("493_std_numeric.vx", "492_vectorizador_completo.vx"):
        src = os.path.join(root, "examples_codes_vx", name)
        if not os.path.exists(src):
            sys.stderr.write("falta %s\n" % src)
            return 2
        ir = emit_ir(vm, root, src,
                     os.path.join(work_dir, os.path.splitext(name)[0]))
        if not ir:
            sys.stderr.write("no se genero el IR de %s\n" % name)
            return 2
        counts.update(count_by_function(ir))

    bad = 0

    def check(fn_name, field, want_min, label):
        nonlocal bad
        wide, bulk = counts.get(fn_name, (0, 0))
        got = wide if field == "anchas" else bulk
        short = fn_name.split("__")[-1] if "__" in fn_name else fn_name
        if got < want_min:
            print("DEJO DE FUNCIONAR: %s -- %s=%d, se esperaba >= %d"
                  % (fn_name, field, got, want_min))
            bad += 1
        else:
            print("  ok  %-16s %-22s %s=%d" % (label, short, field, got))

    for fn_name in LIBRARY_MUST_WIDEN:
        check(fn_name, "anchas", 1, "std.numeric")
    for fn_name in RAW_MUST_BULK:
        check(fn_name, "bloque", 1, "copia")
    for fn_name in RAW_MUST_NOT_WIDEN:
        wide, _ = counts.get(fn_name, (0, 0))
        short = fn_name.split("__")[-1]
        if wide > 0:
            print("EMPEZO A ENSANCHARSE: %s -- anchas=%d.  Si es a proposito,"
                  " muevelo de RAW_MUST_NOT_WIDEN a la lista de arriba."
                  % (fn_name, wide))
            bad += 1
        else:
            print("  ok  %-16s %-22s anchas=0 (documentado)" % ("limite", short))

    if bad == 0:
        print("todas las formas siguen como estaban.")
        return 0
    print("")
    print("%d formas cambiaron.  El programa sigue dando el mismo numero -- por"
          " eso hace falta mirar esto y no la salida." % bad)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=("record", "compare", "idioms"))
    ap.add_argument("ref_dir", nargs="?", default=None,
                    help="directorio de la grabacion (record/compare)")
    ap.add_argument("--build-dir", default="cmake-build-release")
    args = ap.parse_args()

    root, vm = find_vm(args.build_dir)
    work_dir = os.path.join(os.environ.get("TEMP", "/tmp"), "vecverify")

    if args.mode == "idioms":
        return cmd_idioms(vm, root, work_dir)
    if not args.ref_dir:
        sys.stderr.write("%s necesita un directorio\n" % args.mode)
        return 2
    if args.mode == "record":
        return cmd_record(vm, root, args.ref_dir, work_dir)
    return cmd_compare(vm, root, args.ref_dir, work_dir)


if __name__ == "__main__":
    sys.exit(main())
