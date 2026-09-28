#!/usr/bin/env python3
"""Compilar EN CALIENTE tiene que dar lo mismo que compilar EN FRIO.

Cada escenario es un proyecto de varios modulos y una lista de pasos.  En cada
paso se aplican unos cambios al fuente, se compila con la cache que dejo el paso
anterior (caliente) y se compila una copia sin cache (frio); los dos programas
tienen que devolver lo mismo, y lo esperado.

Una diferencia entre caliente y frio es una cache que sirve algo que ya no
corresponde al fuente.  Ese fallo no da error: da OTRO programa, y por eso hace
falta una prueba que lo busque.  `test_cache_puro.py` compara frio contra frio
y no lo ve.

Los escenarios salen de fallos reales:
  - un modulo intermedio importa con `only` y cambia el cuerpo de una plantilla
    (o una `const` que usa) de su dependencia: la cache servia la instancia
    VIEJA;
  - dos modulos con una lambda cada uno: las dos se llamaban `__lambda_0` y la
    fusion se quedaba con una, en frio y en caliente.

Uso:  python tests/vx/test_cache_caliente.py <dir_build>
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

MANIFIESTO = ('[package]\nname = "cache_caliente"\nversion = "1.0.0"\n'
              'edition = 1\n')


def compilar_y_correr(vm, dirp, tag):
    """Compila `main.vx` de @p dirp y lo ejecuta.

    Devuelve (R00 o None si no se produjo el programa, log).  La cache de
    PROYECTO se apaga: serviria el programa entero y taparia a la de modulos,
    que es la que se prueba.
    """
    env = dict(os.environ)
    env["VX_NO_PROJECT_CACHE"] = "1"
    env.pop("VX_CAS_DIR", None)
    env.pop("VX_CACHE_DIR", None)
    out = os.path.join(dirp, tag)
    c = subprocess.run([vm, "--vesta", "main.vx", "-o", out], cwd=dirp,
                       capture_output=True, text=True, env=env)
    log = c.stdout + c.stderr
    if not os.path.exists(out + ".velb"):
        return None, log
    r = subprocess.run([vm, "--run", out + ".velb", "--stats"], cwd=dirp,
                       capture_output=True, text=True, env=env)
    m = re.search(r"R00=0x([0-9a-f]+)", r.stdout + r.stderr)
    return (int(m.group(1), 16) if m else None), log + r.stdout + r.stderr


def escribir(dirp, ficheros):
    """Escribe los ficheros del proyecto en @p dirp."""
    for nombre, texto in ficheros.items():
        with open(os.path.join(dirp, nombre), "w", encoding="utf-8",
                  newline="\n") as fh:
            fh.write(texto)


def correr(vm, nombre, inicial, pasos):
    """Ejecuta un escenario.

    @p pasos es una lista de (cambios, esperado, descripcion).  Devuelve el
    numero de pasos que fallan.
    """
    base = tempfile.mkdtemp(prefix="vx_cache_caliente_")
    caliente = os.path.join(base, "caliente")
    os.makedirs(caliente)
    ficheros = dict(inicial)
    ficheros["vx.toml"] = MANIFIESTO
    fallos = 0
    try:
        for n, (cambios, esperado, desc) in enumerate(pasos):
            ficheros.update(cambios)
            escribir(caliente, ficheros)
            frio = os.path.join(base, "frio%d" % n)
            os.makedirs(frio)
            escribir(frio, ficheros)
            rc, logc = compilar_y_correr(vm, caliente, "c%d" % n)
            rf, logf = compilar_y_correr(vm, frio, "f%d" % n)
            if rc == rf == esperado:
                print("  ok    %s, paso %d (%s) -> %d" % (nombre, n, desc,
                                                          esperado))
                continue
            fallos += 1
            print("  FALLA %s, paso %d (%s): caliente=%s frio=%s esperado=%s"
                  % (nombre, n, desc, rc, rf, esperado))
            for linea in (logc if rc != esperado else logf).splitlines():
                if re.search(r"error|no resuelto|unresolved", linea, re.I):
                    print("        | " + linea.strip()[:200])
    finally:
        shutil.rmtree(base, ignore_errors=True)
    return fallos


def only_cuerpo_plantilla(vm):
    """Intermedio con `only`: cambia el cuerpo de la plantilla que instancia."""
    lib = ("namespace lib;\n\npublic i64 base() => 1;\n\n"
           "public T doble<T>(T x) => x * %d;\n")
    mid = ("namespace mid;\n\nimport lib only doble;\n\n"
           "public i64 calcula(i64 v) => doble<i64>(v);\n")
    main = "import mid only calcula;\n\ni64 main() => calcula(21);\n"
    return correr(vm, "only_cuerpo_plantilla",
                  {"lib.vx": lib % 2, "mid.vx": mid, "main.vx": main},
                  [({}, 42, "inicial"),
                   ({"lib.vx": lib % 3}, 63, "el cuerpo pasa a x*3")])


def only_const_de_plantilla(vm):
    """Intermedio con `only`: cambia una `const` privada que usa la plantilla."""
    lib = ("namespace lib;\n\nconst i64 FACTOR = %d;\n\n"
           "public T por<T>(T x) => x * FACTOR;\n")
    mid = ("namespace mid;\n\nimport lib only por;\n\n"
           "public i64 calcula(i64 v) => por<i64>(v);\n")
    main = "import mid only calcula;\n\ni64 main() => calcula(21);\n"
    return correr(vm, "only_const_de_plantilla",
                  {"lib.vx": lib % 2, "mid.vx": mid, "main.vx": main},
                  [({}, 42, "inicial"),
                   ({"lib.vx": lib % 3}, 63, "FACTOR pasa a 3")])


def lambdas_en_dos_modulos(vm):
    """Una lambda en cada modulo: no pueden compartir simbolo."""
    a = ("namespace a;\n\npublic i64 fa(i64 v) {\n"
         "\tfn(i64) -> i64 f = (i64 x) => x * %d;\n\treturn f(v);\n}\n")
    b = ("namespace b;\n\npublic i64 fb(i64 v) {\n"
         "\tfn(i64) -> i64 f = (i64 x) => x + 100;\n\treturn f(v);\n}\n")
    main = ("import a only fa;\nimport b only fb;\n\n"
            "i64 main() => fa(1) + fb(-60);\n")
    return correr(vm, "lambdas_en_dos_modulos",
                  {"a.vx": a % 2, "b.vx": b, "main.vx": main},
                  [({}, 42, "inicial"),                  # 2 + 40
                   ({"a.vx": a % 3}, 43, "la de a pasa a x*3")])  # 3 + 40


ESCENARIOS = [only_cuerpo_plantilla, only_const_de_plantilla,
              lambdas_en_dos_modulos]


def main():
    if len(sys.argv) < 2:
        print("uso: test_cache_caliente.py <dir_build>")
        return 2
    build = sys.argv[1]
    vm = os.path.join(build, "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("[cache caliente] no existe %s" % vm)
        return 2
    print("[cache caliente] compilar en caliente == compilar en frio")
    fallos = sum(e(vm) for e in ESCENARIOS)
    print("[cache caliente] %s" % ("TODO OK" if fallos == 0
                                   else "%d paso(s) FALLAN" % fallos))
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
