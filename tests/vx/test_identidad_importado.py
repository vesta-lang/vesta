#!/usr/bin/env python3
"""Lo que llega de otro modulo tiene UNA identidad, se importe como se importe.

Una plantilla (funcion o struct generico) de un modulo tiene que funcionar y
dar el MISMO simbolo tanto si el modulo declara `namespace` como si no, y se
importe con `only`, llano (`lib.f`) o con alias (`import lib as L` -> `L.f`).
Lo que escribe quien importa sirve para ENCONTRARLA, no para nombrarla.

Los escenarios salen de fallos medidos:
  - de las seis formas, cuatro fallaban: sin namespace el import llano y el
    alias no compilaban, y con `only` la instancia salia sin su modulo
    (`doble_i64`); con namespace, el alias no encontraba el struct;
  - dos librerias sin namespace con una generica del mismo nombre, cada una
    importada con `only` por un modulo distinto, daban el mismo simbolo, y la
    fusion ejecutaba la de la otra: otro resultado y ningun aviso;
  - la misma instancia de un struct generico escrita `lib.Caja<i64>` en un
    modulo y `Caja<i64>` en otro eran dos tipos incompatibles.

Uso:  python tests/vx/test_identidad_importado.py <dir_build>
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

MANIFIESTO = ('[package]\nname = "identidad"\nversion = "1.0.0"\n'
              'edition = 1\n')

LIB = ("{cabecera}public i64 base() => 1;\n\n"
       "public struct Caja<T> {{\n\tpublic T v;\n}}\n\n"
       "public T doble<T>(T x) => x * 2;\n\n"
       # Y una ESPECIALIZACION: se registra con el nombre de su primaria, y
       # buscarla por el nombre escrito dejaba `Par<i64>` en la primaria.
       "public struct Par<T> {{\n\tpublic T a;\n\tpublic i64 cual() => 1;\n}}\n\n"
       "public struct Par<i64> {{\n\tpublic i64 a;\n\tpublic i64 extra;\n"
       "\tpublic i64 cual() => 2;\n}}\n")

# Por forma de import: (linea de import, prefijo con que se escribe).
FORMAS = {
    "only": ("import {m} only Caja, doble, Par;", ""),
    "llano": ("import {m};", "lib."),
    "alias": ("import {m} as L;", "L."),
}

# 2*21 + (2 de la especializacion) - 2 = 42.
MAIN = ("{imp}\n\ni64 main() {{\n\t{p}Caja<i64> c;\n\tc.v = 21;\n"
        "\t{p}Par<i64> q;\n\tq.extra = 0;\n"
        "\treturn {p}doble<i64>(c.v) + q.cual() - 2;\n}}\n")


def compilar_y_correr(vm, dirp):
    """Compila y ejecuta `main.vx`: (R00 o None, log, simbolos del IR)."""
    env = dict(os.environ)
    env["VX_NO_PROJECT_CACHE"] = "1"
    env.pop("VX_CAS_DIR", None)
    env.pop("VX_CACHE_DIR", None)
    subprocess.run([vm, "--vx-emit-ir", "--vesta", "main.vx", "-o", "ir"],
                   cwd=dirp, capture_output=True, env=env)
    simbolos = set()
    for f in os.listdir(dirp):
        if f.startswith("ir") and f.endswith(".ir"):
            with open(os.path.join(dirp, f), encoding="utf-8",
                      errors="replace") as fh:
                simbolos.update(re.findall(r"@function ([A-Za-z0-9_]+)",
                                           fh.read()))
    c = subprocess.run([vm, "--vesta", "main.vx", "-o", "p"], cwd=dirp,
                       capture_output=True, text=True, env=env)
    log = c.stdout + c.stderr
    if not os.path.exists(os.path.join(dirp, "p.velb")):
        return None, log, simbolos
    r = subprocess.run([vm, "--run", "p.velb", "--stats"], cwd=dirp,
                       capture_output=True, text=True, env=env)
    m = re.search(r"R00=0x([0-9a-f]+)", r.stdout + r.stderr)
    return (int(m.group(1), 16) if m else None), log, simbolos


def proyecto(base, nombre, ficheros):
    """Escribe un proyecto en un subdirectorio de @p base y lo devuelve."""
    d = os.path.join(base, nombre)
    os.makedirs(d)
    ficheros = dict(ficheros)
    ficheros["vx.toml"] = MANIFIESTO
    for f, texto in ficheros.items():
        with open(os.path.join(d, f), "w", encoding="utf-8",
                  newline="\n") as fh:
            fh.write(texto)
    return d


def informar(nombre, ok, detalle, log):
    """Una linea por comprobacion; si falla, las lineas de error del log."""
    print("  %s %s%s" % ("ok   " if ok else "FALLA", nombre,
                         (": " + detalle) if detalle else ""))
    if not ok:
        for linea in log.splitlines():
            if re.search(r"error|incompat", linea, re.I):
                print("        | " + linea.strip()[:200])
    return 0 if ok else 1


def matriz(vm, base):
    """Seis formas: con/sin namespace x only/llano/alias."""
    fallos = 0
    for ns in ("con", "sin"):
        cabecera = "namespace lib;\n\n" if ns == "con" else ""
        modulo = "lib" if ns == "con" else '"lib"'   # sin namespace: por ruta
        for forma, (imp, prefijo) in FORMAS.items():
            d = proyecto(base, "m_%s_%s" % (ns, forma), {
                "lib.vx": LIB.format(cabecera=cabecera),
                "main.vx": MAIN.format(imp=imp.format(m=modulo), p=prefijo),
            })
            r, log, sims = compilar_y_correr(vm, d)
            instancia = sorted(s for s in sims if "doble" in s)
            ok = r == 42 and instancia == ["lib__doble_i64"]
            fallos += informar("%s namespace, %s" % (ns, forma), ok,
                               "R00=%s instancia=%s" % (r, instancia), log)
    return fallos


def homonimas(vm, base):
    """Dos librerias sin namespace con la misma generica, via `only`."""
    d = proyecto(base, "homonimas", {
        "l1.vx": "public i64 uno() => 1;\n\npublic T f<T>(T x) => x * 2;\n",
        "l2.vx": "public i64 dos() => 2;\n\npublic T f<T>(T x) => x + 100;\n",
        "m1.vx": ('namespace m1;\n\nimport "l1" only f;\n\n'
                  "public i64 g1(i64 v) => f<i64>(v);\n"),
        "m2.vx": ('namespace m2;\n\nimport "l2" only f;\n\n'
                  "public i64 g2(i64 v) => f<i64>(v);\n"),
        "main.vx": ("import m1 only g1;\nimport m2 only g2;\n\n"
                    "i64 main() => g1(1) + g2(-60);\n"),   # 2 + 40
    })
    r, log, sims = compilar_y_correr(vm, d)
    instancias = sorted(s for s in sims if re.match(r"(l1__|l2__)?f_i64$", s))
    ok = r == 42 and instancias == ["l1__f_i64", "l2__f_i64"]
    return informar("dos librerias con la misma generica", ok,
                    "R00=%s instancias=%s" % (r, instancias), log)


def identidad_struct(vm, base):
    """`lib.Caja<i64>` en un modulo y `Caja<i64>` en otro: el mismo tipo."""
    d = proyecto(base, "identidad_struct", {
        "lib.vx": ("namespace lib;\n\npublic i64 base() => 1;\n\n"
                   "public struct Caja<T> {\n\tpublic T v;\n}\n"),
        "m1.vx": ("namespace m1;\n\nimport lib;\n\n"
                  "public lib.Caja<i64> hazla(i64 x) {\n\tlib.Caja<i64> c;\n"
                  "\tc.v = x;\n\treturn c;\n}\n"),
        "main.vx": ("import lib only Caja;\nimport m1 only hazla;\n\n"
                    "i64 main() {\n\tCaja<i64> c = hazla(42);\n"
                    "\treturn c.v;\n}\n"),
    })
    r, log, _ = compilar_y_correr(vm, d)
    return informar("la misma instancia escrita de dos formas", r == 42,
                    "R00=%s" % r, log)


def main():
    if len(sys.argv) < 2:
        print("uso: test_identidad_importado.py <dir_build>")
        return 2
    build = sys.argv[1]
    vm = os.path.join(build, "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("[identidad importado] no existe %s" % vm)
        return 2
    print("[identidad importado] una plantilla, un simbolo, se importe como "
          "se importe")
    base = tempfile.mkdtemp(prefix="vx_identidad_")
    try:
        fallos = (matriz(vm, base) + homonimas(vm, base) +
                  identidad_struct(vm, base))
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print("[identidad importado] %s" % ("TODO OK" if fallos == 0
                                        else "%d comprobacion(es) FALLAN"
                                        % fallos))
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
