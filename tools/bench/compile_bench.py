#!/usr/bin/env python3
"""Cuanto tarda cada lenguaje en COMPILAR, y cuanto le sirve su cache.

Es un modulo aparte de `run_all_benches.py` a proposito.  Medir la ejecucion y
medir la compilacion se parecen solo en que ambos cronometran un proceso: el
modelo de ruido, el de cache y hasta lo que significa "en frio" son distintos, y
mezclarlos en el mismo bucle acaba en un arnes que hace mal las dos cosas.

Lo que se mide, y por que son TRES ejes y no uno:

  frio          Sin ninguna cache: ni la del compilador ni la del sistema.  Es
                lo que paga quien clona el repositorio y compila por primera
                vez.  Definirlo bien es la mitad del trabajo, porque cada
                herramienta guarda su cache en un sitio distinto y "frio" para
                una puede ser "caliente" para otra.

  incremental   Con las caches ya calientes y UN fichero tocado.  Es el numero
                que se paga cien veces al dia.  Solo significa algo en un
                proyecto de varios modulos: en un fichero suelto no hay nada
                que reconstruir incrementalmente.

  realimentacion  Cuanto tarda el programador en VER el error.  No es una
                compilacion: no genera codigo ni enlaza.  Cada lenguaje lo hace
                con un comando distinto y hay que decirlo, porque comparar un
                `-fsyntax-only` contra un enlazado completo no compara nada.

Y una advertencia que este modulo existe para no repetir.  Medido en esta
maquina, compilar uno de los benchmarks del corpus -- 40 lineas -- cuesta lo
MISMO que compilar un fichero vacio: 104 ms contra 107 en gcc, 133 contra 135 en
rustc, 313 contra 306 en javac.  Entre el 94% y el 103% de ese tiempo es
arrancar el compilador.  Por eso aqui las fuentes se GENERAN con un tamano
controlado: para que haya algo que compilar.  Y por eso se mide y se resta el
suelo de cada herramienta, igual que en el arnes de ejecucion.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).parent))
from run_all_benches import (  # noqa: E402
    C,
    _stats_summary,
    find_project_root,
    serie_asentada,
    una_medida,
)

# ===========================================================================
# Generador de fuentes: el mismo programa, en siete lenguajes, con el tamano
# como parametro.
#
# La forma es deliberadamente ANODINA -- funciones pequenas, aritmetica entera,
# sin llamadas a biblioteca -- porque lo que se quiere medir es el trabajo de
# base del compilador (analisis lexico, sintactico, de tipos, generacion) y no
# la resolucion de una biblioteca concreta.  Todas generan el mismo numero de
# funciones con la misma forma, asi que "el mismo tamano" significa lo mismo en
# los siete.
# ===========================================================================

def _c_like(n: int, decl: str, ret: str, main_open: str, main_close: str) -> str:
    """Cuerpo comun de los lenguajes con sintaxis de llaves."""
    partes = []
    for i in range(n):
        partes.append(decl.format(i=i, prev=(i - 1) % n))
        partes.append(ret)
    partes.append(main_open)
    for i in range(0, n, 8):   # el main referencia una de cada ocho
        partes.append("    s += calc%d(%d);" % (i, i))
    partes.append(main_close)
    return "\n".join(partes) + "\n"


def gen_c(n: int) -> str:
    return _c_like(
        n,
        "long long calc{i}(long long x) {{\n    long long a = x * 3 + {i};\n"
        "    long long b = a ^ 5;\n    long long c = (a + b) * 7;",
        "    return a + b + c;\n}\n",
        "int main(void) {\n    long long s = 0;",
        "    return (int)(s % 251);\n}\n",
    )


def gen_cpp(n: int) -> str:
    return gen_c(n)


def gen_rs(n: int) -> str:
    partes = []
    for i in range(n):
        partes.append(
            "fn calc%d(x: i64) -> i64 {\n    let a = x.wrapping_mul(3).wrapping_add(%d);\n"
            "    let b = a ^ 5;\n    let c = (a.wrapping_add(b)).wrapping_mul(7);\n"
            "    a.wrapping_add(b).wrapping_add(c)\n}\n" % (i, i))
    partes.append("fn main() {\n    let mut s: i64 = 0;")
    for i in range(0, n, 8):
        partes.append("    s = s.wrapping_add(calc%d(%d));" % (i, i))
    partes.append("    std::process::exit((s % 251) as i32);\n}\n")
    return "\n".join(partes)


def gen_go(n: int) -> str:
    partes = ["package main", "", "import \"os\"", ""]
    for i in range(n):
        partes.append(
            "func calc%d(x int64) int64 {\n\ta := x*3 + %d\n\tb := a ^ 5\n"
            "\tc := (a + b) * 7\n\treturn a + b + c\n}\n" % (i, i))
    partes.append("func main() {\n\tvar s int64 = 0")
    for i in range(0, n, 8):
        partes.append("\ts += calc%d(%d)" % (i, i))
    partes.append("\tos.Exit(int(s % 251))\n}")
    return "\n".join(partes) + "\n"


def gen_java(n: int) -> str:
    partes = ["public class Gen {"]
    for i in range(n):
        partes.append(
            "    static long calc%d(long x) {\n        long a = x * 3 + %d;\n"
            "        long b = a ^ 5;\n        long c = (a + b) * 7;\n"
            "        return a + b + c;\n    }\n" % (i, i))
    partes.append("    public static void main(String[] args) {\n        long s = 0;")
    for i in range(0, n, 8):
        partes.append("        s += calc%d(%d);" % (i, i))
    partes.append("        System.exit((int)(s % 251));\n    }\n}")
    return "\n".join(partes) + "\n"


def gen_py(n: int) -> str:
    partes = ["import sys", ""]
    for i in range(n):
        partes.append(
            "def calc%d(x):\n    a = x * 3 + %d\n    b = a ^ 5\n"
            "    c = (a + b) * 7\n    return a + b + c\n" % (i, i))
    partes.append("def main():\n    s = 0")
    for i in range(0, n, 8):
        partes.append("    s += calc%d(%d)" % (i, i))
    partes.append("    sys.exit(s % 251)\n\nmain()")
    return "\n".join(partes) + "\n"


def gen_vx(n: int) -> str:
    partes = []
    for i in range(n):
        partes.append(
            "i64 calc%d(i64 x) {\n    i64 a = x * 3 + %d;\n    i64 b = a ^ 5;\n"
            "    i64 c = (a + b) * 7;\n    return a + b + c;\n}\n" % (i, i))
    partes.append("i32 main() {\n    i64 s = 0;")
    for i in range(0, n, 8):
        partes.append("    s = s + calc%d(%d);" % (i, i))
    partes.append("    return (i32) (s % 251);\n}")
    return "\n".join(partes) + "\n"


GENERADORES = {
    "c": ("gen.c", gen_c),
    "cpp": ("gen.cpp", gen_cpp),
    "rust": ("gen.rs", gen_rs),
    "go": ("gen.go", gen_go),
    "java": ("Gen.java", gen_java),
    "python": ("gen.py", gen_py),
    # Vesta tiene DOS caminos de compilacion y no cuestan lo mismo: el de la
    # maquina virtual para y en el `.velb`, y el nativo sigue hasta MachineIR,
    # asignacion de registros, codificacion y enlazado propio.  Publicarlos
    # juntos daria un numero que no es ninguno de los dos.
    "vesta": ("gen.vx", gen_vx),
    "vesta_aot": ("gen.vx", gen_vx),
}

# Fuente que no declara NADA.  Lo que tarda en compilarse es el suelo de esa
# herramienta: arrancar el proceso, cargar su runtime y montar sus tablas.  Ese
# tiempo esta dentro de cada medida y hay que poder descontarlo.
VACIAS = {
    "c": ("vacio.c", "int main(void) { return 0; }\n"),
    "cpp": ("vacio.cpp", "int main() { return 0; }\n"),
    "rust": ("vacio.rs", "fn main() {}\n"),
    "go": ("vacio.go", "package main\n\nfunc main() {}\n"),
    "java": ("Vacio.java",
             "public class Vacio { public static void main(String[] a) {} }\n"),
    "python": ("vacio.py", "pass\n"),
    "vesta": ("vacio.vx", "i32 main() { return 0; }\n"),
    "vesta_aot": ("vacio.vx", "i32 main() { return 0; }\n"),
}


# ===========================================================================
# El MISMO programa repartido en varios ficheros.
#
# No es un adorno: como trata un compilador N funciones en un fichero y las
# mismas N repartidas en veinte no tiene por que costar lo mismo, y en unos
# lenguajes ni siquiera es el mismo trabajo.  Uno que compila por unidad de
# traduccion paga N veces el arranque de su frontend y luego enlaza; uno que
# mira el programa entero de una vez puede salir ganando o perdiendo.  Ademas es
# el UNICO escenario donde una cache incremental significa algo: en un fichero
# suelto no hay nada que reutilizar.
#
# El reparto conserva el total: `n` funciones en `k` ficheros son n/k por
# fichero, asi que las dos columnas comparan el mismo trabajo.
# ===========================================================================

def _trozos(n: int, k: int) -> list[range]:
    """Reparte `n` funciones en `k` ficheros, lo mas parejo posible."""
    por = max(1, n // k)
    out = []
    ini = 0
    while ini < n:
        out.append(range(ini, min(ini + por, n)))
        ini += por
    return out


def escribir_multi(lang: str, n: int, k: int, d: Path) -> Optional[list[str]]:
    """Escribe el programa repartido en `k` ficheros.  Devuelve los ficheros
    fuente en orden (el ultimo es siempre el que tiene `main`)."""
    d.mkdir(parents=True, exist_ok=True)
    trozos = _trozos(n, k)
    cabezas = [t.start for t in trozos]   # una funcion por modulo para el main
    ficheros: list[str] = []

    if lang in ("c", "cpp"):
        ext = "c" if lang == "c" else "cpp"
        for j, t in enumerate(trozos):
            cuerpo = "".join(
                "long long calc%d(long long x) {\n    long long a = x * 3 + %d;\n"
                "    long long b = a ^ 5;\n    long long c = (a + b) * 7;\n"
                "    return a + b + c;\n}\n" % (i, i) for i in t)
            (d / ("m%d.%s" % (j, ext))).write_text(cuerpo, encoding="utf-8")
            ficheros.append("m%d.%s" % (j, ext))
        decls = "".join("long long calc%d(long long);\n" % i for i in cabezas)
        llam = "".join("    s += calc%d(%d);\n" % (i, i) for i in cabezas)
        (d / ("main.%s" % ext)).write_text(
            decls + "int main(void) {\n    long long s = 0;\n" + llam +
            "    return (int)(s % 251);\n}\n", encoding="utf-8")
        ficheros.append("main.%s" % ext)
        return ficheros

    if lang == "go":
        for j, t in enumerate(trozos):
            cuerpo = "package main\n\n" + "".join(
                "func calc%d(x int64) int64 {\n\ta := x*3 + %d\n\tb := a ^ 5\n"
                "\tc := (a + b) * 7\n\treturn a + b + c\n}\n" % (i, i) for i in t)
            (d / ("m%d.go" % j)).write_text(cuerpo, encoding="utf-8")
            ficheros.append("m%d.go" % j)
        llam = "".join("\ts += calc%d(%d)\n" % (i, i) for i in cabezas)
        (d / "main.go").write_text(
            "package main\n\nimport \"os\"\n\nfunc main() {\n\tvar s int64 = 0\n"
            + llam + "\tos.Exit(int(s % 251))\n}\n", encoding="utf-8")
        ficheros.append("main.go")
        return ficheros

    if lang == "java":
        for j, t in enumerate(trozos):
            cuerpo = "public class M%d {\n" % j + "".join(
                "    static long calc%d(long x) {\n        long a = x * 3 + %d;\n"
                "        long b = a ^ 5;\n        long c = (a + b) * 7;\n"
                "        return a + b + c;\n    }\n" % (i, i) for i in t) + "}\n"
            (d / ("M%d.java" % j)).write_text(cuerpo, encoding="utf-8")
            ficheros.append("M%d.java" % j)
        llam = "".join("        s += M%d.calc%d(%d);\n" % (j, t.start, t.start)
                       for j, t in enumerate(trozos))
        (d / "Main.java").write_text(
            "public class Main {\n    public static void main(String[] a) {\n"
            "        long s = 0;\n" + llam +
            "        System.exit((int)(s % 251));\n    }\n}\n", encoding="utf-8")
        ficheros.append("Main.java")
        return ficheros

    if lang == "rust":
        for j, t in enumerate(trozos):
            cuerpo = "".join(
                "pub fn calc%d(x: i64) -> i64 {\n    let a = x.wrapping_mul(3)"
                ".wrapping_add(%d);\n    let b = a ^ 5;\n"
                "    let c = (a.wrapping_add(b)).wrapping_mul(7);\n"
                "    a.wrapping_add(b).wrapping_add(c)\n}\n" % (i, i) for i in t)
            (d / ("m%d.rs" % j)).write_text(cuerpo, encoding="utf-8")
            ficheros.append("m%d.rs" % j)
        mods = "".join("mod m%d;\n" % j for j in range(len(trozos)))
        llam = "".join("    s = s.wrapping_add(m%d::calc%d(%d));\n"
                       % (j, t.start, t.start) for j, t in enumerate(trozos))
        (d / "main.rs").write_text(
            mods + "fn main() {\n    let mut s: i64 = 0;\n" + llam +
            "    std::process::exit((s % 251) as i32);\n}\n", encoding="utf-8")
        ficheros.append("main.rs")
        return ficheros

    if lang in ("vesta", "vesta_aot"):
        for j, t in enumerate(trozos):
            cuerpo = "namespace gen.m%d;\n\n" % j + "".join(
                "public i64 calc%d(i64 x) {\n    i64 a = x * 3 + %d;\n"
                "    i64 b = a ^ 5;\n    i64 c = (a + b) * 7;\n"
                "    return a + b + c;\n}\n" % (i, i) for i in t)
            (d / ("m%d.vx" % j)).write_text(cuerpo, encoding="utf-8")
            ficheros.append("m%d.vx" % j)
        imports = "".join('import "m%d" only calc%d;\n' % (j, t.start)
                          for j, t in enumerate(trozos))
        llam = "".join("    s = s + calc%d(%d);\n" % (t.start, t.start)
                       for t in trozos)
        (d / "main.vx").write_text(
            imports + "\ni32 main() {\n    i64 s = 0;\n" + llam +
            "    return (i32) (s % 251);\n}\n", encoding="utf-8")
        ficheros.append("main.vx")
        return ficheros

    if lang == "python":
        for j, t in enumerate(trozos):
            cuerpo = "".join(
                "def calc%d(x):\n    a = x * 3 + %d\n    b = a ^ 5\n"
                "    c = (a + b) * 7\n    return a + b + c\n\n" % (i, i)
                for i in t)
            (d / ("m%d.py" % j)).write_text(cuerpo, encoding="utf-8")
            ficheros.append("m%d.py" % j)
        imports = "".join("import m%d\n" % j for j in range(len(trozos)))
        llam = "".join("    s += m%d.calc%d(%d)\n" % (j, t.start, t.start)
                       for j, t in enumerate(trozos))
        (d / "main.py").write_text(
            "import sys\n" + imports + "\ndef main():\n    s = 0\n" + llam +
            "    sys.exit(s % 251)\n\nmain()\n", encoding="utf-8")
        ficheros.append("main.py")
        return ficheros
    return None


# Como se escribe un comentario y una funcion publica nueva en cada lenguaje.
# Hace falta para fabricar cambios de distinta PROFUNDIDAD sobre un proyecto ya
# construido, que es lo unico que revela la granularidad de invalidacion de
# cada compilador.
_COMENTARIO = {"c": "//", "cpp": "//", "rust": "//", "go": "//", "java": "//",
               "python": "#", "vesta": "//", "vesta_aot": "//"}

_FUNCION_NUEVA = {
    "c": "long long extra%d(long long x) { return x + %d; }\n",
    "cpp": "long long extra%d(long long x) { return x + %d; }\n",
    "rust": "pub fn extra%d(x: i64) -> i64 { x + %d }\n",
    "go": "func extra%d(x int64) int64 { return x + %d }\n",
    "python": "def extra%d(x):\n    return x + %d\n",
    "vesta": "public i64 extra%d(i64 x) { return x + %d; }\n",
    "vesta_aot": "public i64 extra%d(i64 x) { return x + %d; }\n",
}


def mutar(ruta: Path, lang: str, clase: str, vuelta: int) -> bool:
    """Cambia @p ruta con la PROFUNDIDAD pedida.  Devuelve False si no aplica.

    Las tres clases responden a preguntas distintas y por eso se miden aparte:

      comentario  El fichero cambia de fecha y de contenido, pero nada de lo
                  que declara.  Un compilador que compare contenidos con lo que
                  ya sabe puede saltarselo entero; uno que solo mire fechas, no.

      cuerpo      Cambia la implementacion de una funcion, no su firma.  Quien
                  guarde interfaz y cuerpo por separado solo tiene que rehacer
                  ese modulo; quien no, arrastra a todos sus dependientes.

      interfaz    Aparece una funcion publica nueva.  Lo que el modulo OFRECE
                  cambia, asi que revalidar a los dependientes es obligado.  Es
                  el techo: nadie deberia poder saltarselo.
    """
    if not ruta.is_file():
        return False
    texto = ruta.read_text(encoding="utf-8")
    if clase == "comentario":
        ruta.write_text(texto + "%s cambio %d\n" % (_COMENTARIO[lang], vuelta),
                        encoding="utf-8")
        return True
    if clase == "cuerpo":
        # `* 3 +` aparece en el cuerpo de todas las funciones generadas, en los
        # siete lenguajes.  Alternar el factor garantiza que CADA vuelta es un
        # cambio real y no una reescritura identica.
        viejo = "* 3 +" if (vuelta % 2 == 0) else "* 4 +"
        nuevo = "* 4 +" if (vuelta % 2 == 0) else "* 3 +"
        if viejo not in texto:
            return False
        ruta.write_text(texto.replace(viejo, nuevo, 1), encoding="utf-8")
        return True
    if clase == "interfaz":
        plantilla = _FUNCION_NUEVA.get(lang)
        if plantilla is None:
            return False
        ruta.write_text(texto + plantilla % (vuelta, vuelta), encoding="utf-8")
        return True
    return False


def orden_multi(lang: str, ficheros: list[str], salida: Path,
                vm: Path) -> list[str]:
    """Orden que compila el proyecto repartido.  Cada herramienta lo recibe
    como ella lo entiende: unos quieren todas las unidades, otros solo el
    fichero raiz y ya siguen las dependencias."""
    if lang == "c":
        return ["gcc", "-O2", "-std=c11"] + ficheros + ["-o", str(salida)]
    if lang == "cpp":
        return ["g++", "-O2", "-std=c++17"] + ficheros + ["-o", str(salida)]
    if lang == "go":
        return ["go", "build", "-o", str(salida)] + ficheros
    if lang == "java":
        return ["javac", "-d", str(salida.parent / "clases")] + ficheros
    if lang == "rust":
        return ["rustc", "-O", "main.rs", "-o", str(salida)]
    if lang == "vesta":
        return [str(vm), "--vesta", "main.vx", "-o", str(salida)]
    if lang == "vesta_aot":
        fmt = "pe" if sys.platform == "win32" else "elf"
        return [str(vm), "-m", "aot", "--vx", "main.vx", "-o",
                str(salida) + ".exe", "--emit", "exe", "--format", fmt]
    return []


# ===========================================================================
# Ordenes de compilacion, y donde guarda su cache cada herramienta.
#
# Lo segundo es tan importante como lo primero: si "en frio" se mide sin
# vaciar la cache de quien la tenga, se esta comparando el primer arranque de
# unos con el enesimo de otros.
# ===========================================================================

def orden_compilar(lang: str, fuente: Path, salida: Path, vm: Path) -> list[str]:
    """Compilacion COMPLETA hasta binario ejecutable."""
    if lang == "c":
        return ["gcc", "-O2", "-std=c11", str(fuente), "-o", str(salida)]
    if lang == "cpp":
        return ["g++", "-O2", "-std=c++17", str(fuente), "-o", str(salida)]
    if lang == "rust":
        return ["rustc", "-O", str(fuente), "-o", str(salida)]
    if lang == "go":
        return ["go", "build", "-o", str(salida), str(fuente)]
    if lang == "java":
        return ["javac", "-d", str(salida.parent / "clases"), str(fuente)]
    if lang == "vesta":
        return [str(vm), "--vesta", str(fuente), "-o", str(salida)]
    if lang == "vesta_aot":
        # El camino NATIVO: sigue hasta MachineIR, asignacion de registros,
        # codificacion y enlazado propio.  No cuesta lo mismo que parar en el
        # `.velb`, y publicarlos juntos daria un numero que no es ninguno.
        fmt = "pe" if sys.platform == "win32" else "elf"
        return [str(vm), "-m", "aot", "--vx", str(fuente), "-o",
                str(salida) + ".exe", "--emit", "exe", "--format", fmt]
    return []


def orden_comprobar(lang: str, fuente: Path, salida: Path,
                    vm: Path) -> Optional[list[str]]:
    """Solo COMPROBAR: analizar y diagnosticar, sin generar codigo ni enlazar.

    Es lo que hay detras de "cuanto tardo en ver el error".  No todos lo
    ofrecen, y el que no lo tenga se queda fuera de ese eje en vez de
    compararse contra algo que no es lo mismo.
    """
    if lang == "c":
        return ["gcc", "-fsyntax-only", "-std=c11", str(fuente)]
    if lang == "cpp":
        return ["g++", "-fsyntax-only", "-std=c++17", str(fuente)]
    if lang == "rust":
        return ["rustc", "--emit=metadata", "-o", str(salida) + ".rmeta",
                str(fuente)]
    if lang == "python":
        return [sys.executable, "-m", "py_compile", str(fuente)]
    if lang == "vesta":
        # Sin `--vesta ... -o` no hay etapa de check separada todavia; lo mas
        # cercano es volcar el IR sin emitir binario.  Queda anotado como
        # aproximacion en vez de presentarse como equivalente exacto.
        return [str(vm), "--vx-emit-only", "--vesta", str(fuente), "-o",
                str(salida)]
    # go y java no tienen un modo "solo comprobar" separado de compilar.
    return None


def entorno_cache(lang: str, dir_cache: Path, base: dict) -> dict:
    """Entorno con la cache de @p lang apuntando a @p dir_cache.

    Redirigir la cache es lo unico que permite decir "en frio" con propiedad:
    borrar un directorio del repositorio deja frio a Vesta y calientes a Go y
    Rust, y la comparacion resultante mide el estado de la maquina, no los
    compiladores.
    """
    e = dict(base)
    dir_cache.mkdir(parents=True, exist_ok=True)
    if lang == "go":
        e["GOCACHE"] = str(dir_cache / "go")
    elif lang == "rust":
        e["CARGO_HOME"] = str(dir_cache / "cargo")
    elif lang == "vesta":
        # El compilador Vesta cachea en el arbol: `.cache/` junto al proyecto y
        # los `.vxi`/`.vxir` al lado de cada fuente.  No hay variable que lo
        # mueva, asi que en frio se BORRAN (ver `enfriar`).
        pass
    return e


def enfriar(lang: str, dir_trabajo: Path, dir_cache: Path) -> None:
    """Deja a @p lang sin ninguna cache antes de una medida en frio."""
    for sub in (dir_cache / "go", dir_cache / "cargo"):
        shutil.rmtree(sub, ignore_errors=True)
    if lang in ("vesta", "vesta_aot"):
        shutil.rmtree(dir_trabajo / ".cache", ignore_errors=True)
        shutil.rmtree(dir_trabajo / ".vx_cache", ignore_errors=True)
        for p in list(dir_trabajo.glob("*.vxi")) + list(dir_trabajo.glob("*.vxir")):
            try:
                p.unlink()
            except OSError:
                pass


# ===========================================================================
# Medida
# ===========================================================================

def compila_de_verdad(lang: str, cmd: list[str], env: dict, cwd: Path,
                      salida: Path, timeout: float) -> tuple[bool, str]:
    """¿Esta orden COMPILA, o solo falla deprisa?

    Sin esta comprobacion el modulo cronometra fallos y los publica como
    tiempos.  Paso de verdad: el generador nombraba las funciones `f0..f199` y
    en Vesta `f32` y `f64` son palabras reservadas, asi que la compilacion
    moria en el parser -- y 26 ms de error entraban en la tabla como el mejor
    tiempo de compilacion de la tanda.

    Se exige lo mismo que se le exigiria a cualquiera: codigo de salida cero Y
    un artefacto en disco.  Cualquiera de las dos por separado se deja enganar.
    """
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=str(cwd),
                           env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return (False, "se paso del tiempo limite")
    if r.returncode != 0:
        primera = (r.stderr or r.stdout or "").strip().splitlines()
        return (False, primera[0] if primera else "codigo de salida != 0")
    # El artefacto: cada herramienta lo deja con un nombre distinto.
    candidatos = [salida, salida.with_suffix(".exe"), salida.with_suffix(".velb"),
                  salida.parent / "clases"]
    if any(c.exists() for c in candidatos):
        return (True, "")
    # Diagnostico que no cuesta nada y ahorra media hora: si el proceso salio
    # con cero pero no dejo nada, lo mas probable es que haya escrito los
    # errores sin cambiar el codigo de salida.
    primera = (r.stdout or r.stderr or "").strip().splitlines()
    return (False, "no genero artefacto"
            + (": " + primera[0] if primera else ""))


def _calentar(cmd, env, cwd, timeout) -> int:
    """Descarta ejecuciones hasta que la serie deja de bajar (mismo criterio
    que el arnes de ejecucion: no es un numero fijo, se decide midiendo)."""
    traza: list[float] = []
    gastado = 0.0
    while len(traza) < 12 and not serie_asentada(traza) and gastado < 20000.0:
        ms = una_medida(cmd, env, timeout, cwd)
        if ms < 0:
            break
        traza.append(ms)
        gastado += ms
    return len(traza)


def medir_caliente(cmd, env, cwd, repes, timeout) -> dict:
    """Serie con las caches CALIENTES: se calienta y luego se mide."""
    _calentar(cmd, env, cwd, timeout)
    muestras = [una_medida(cmd, env, timeout, cwd) for _ in range(repes)]
    muestras = [m for m in muestras if m >= 0]
    return _stats_summary(muestras) if muestras else {}


def medir_frio(cmd, env, cwd, repes, timeout, lang, dir_cache) -> dict:
    """Serie EN FRIO: se vacia la cache antes de CADA medida.

    No se calienta: calentar seria justo lo contrario de lo que se quiere.  A
    cambio, estas medidas son las mas ruidosas del modulo -- cada una paga
    ademas la paginacion del compilador -- y por eso se publica su dispersion.
    """
    muestras = []
    for _ in range(repes):
        enfriar(lang, cwd, dir_cache)
        ms = una_medida(cmd, env, timeout, cwd)
        if ms >= 0:
            muestras.append(ms)
    return _stats_summary(muestras) if muestras else {}


def _color_ruido(mad_pct: float) -> str:
    if mad_pct < 2.0:
        return C.GREEN
    if mad_pct < 5.0:
        return C.YELLOW
    return C.RED


def imprimir_tabla(titulo: str, filas: list[tuple], suelo: dict,
                   nota: str = "") -> None:
    """Una fila por (lenguaje, tamano) con estimacion, dispersion y neto."""
    print()
    print(f"{C.BOLD}{titulo}{C.RESET}")
    if nota:
        print(f"{C.DIM}  {nota}{C.RESET}")
    cab = (f"{'lenguaje / tamano':<26}{'p50':>10}{'MAD':>8}{'MAD%':>8}"
           f"{'min':>9}{'max':>9}{'neto':>10}")
    print(f"{C.BOLD}{cab}{C.RESET}")
    print("-" * len(cab))
    for lang, etiqueta, s in filas:
        if not s:
            print(f"  {etiqueta:<24}{C.GREY}{'sin dato':>10}{C.RESET}")
            continue
        piso = (suelo.get(lang) or {}).get("p50")
        if piso is None:
            neto = f"{'-':>10}"
        elif s["p50"] - piso <= 0:
            neto = f"{C.DIM}{'~0':>10}{C.RESET}"
        else:
            neto = f"{s['p50'] - piso:>10.0f}"
        print(f"  {etiqueta:<24}{s['p50']:>10.0f}{s['mad']:>8.1f}"
              f"{_color_ruido(s['mad_pct'])}{s['mad_pct']:>7.1f}%{C.RESET}"
              f"{s['min']:>9.0f}{s['max']:>9.0f}{neto}")
    print("-" * len(cab))


def imprimir_ganancia(frio: dict, caliente: dict, langs: list[str]) -> None:
    """Lo que aporta la cache de cada uno: frio dividido por caliente.

    Es la pregunta que motiva el modulo.  Un `1.0x` no significa que la cache
    sea mala: significa que ese lenguaje no tiene nada que cachear en este
    escenario -- compilar un fichero suelto -- y el numero solo empieza a
    decir algo con un proyecto de varios modulos.
    """
    print()
    print(f"{C.BOLD}Lo que aporta la cache (frio / caliente){C.RESET}")
    print(f"{C.DIM}  Mas alto = la cache ahorra mas.  1.0x = no hay nada que "
          f"cachear en este escenario.{C.RESET}")
    pares = []
    for ln in langs:
        f = frio.get(ln)
        c = caliente.get(ln)
        if not f or not c or c.get("p50", 0) <= 0:
            continue
        pares.append((f["p50"] / c["p50"], ln, f["p50"], c["p50"]))
    for g, ln, f, c in sorted(pares, reverse=True):
        col = C.GREEN if g >= 2.0 else (C.YELLOW if g >= 1.2 else C.DIM)
        print(f"  {ln:<12}{col}{g:>7.2f}x{C.RESET}"
              f"   frio {f:>8.0f} ms  ->  caliente {c:>8.0f} ms")


# ===========================================================================
# Programa
# ===========================================================================

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("vm_path", nargs="?", default="")
    p.add_argument("--tamanos", type=str, default="200,2000",
                   help="numero de funciones generadas por fuente "
                        "(~5 lineas cada una).  Default: 200,2000")
    p.add_argument("--ficheros", type=int, default=20,
                   help="en cuantos ficheros se reparte el programa para la "
                        "comparacion mono/multi (default 20)")
    p.add_argument("--repes", type=int, default=5,
                   help="medidas por caso (default 5)")
    p.add_argument("--timeout", type=float, default=600.0)
    p.add_argument("--langs", type=str, default="",
                   help="lista separada por comas; vacio = todos")
    p.add_argument("--out-json", type=str, default="")
    args = p.parse_args()

    raiz = find_project_root(Path(__file__).resolve())
    vm = Path(args.vm_path) if args.vm_path else (
        raiz / "cmake-build-release" / "vm.exe")
    if not vm.is_file():
        print(f"{C.RED}[error]{C.RESET} no encuentro el binario vesta: {vm}")
        return 1

    langs = [l.strip() for l in args.langs.split(",") if l.strip()] or \
        list(GENERADORES.keys())
    tamanos = [int(t) for t in args.tamanos.split(",") if t.strip()]

    base_tmp = Path(os.environ.get("TEMP", "/tmp")) / "vesta_compile_bench"
    shutil.rmtree(base_tmp, ignore_errors=True)
    base_tmp.mkdir(parents=True, exist_ok=True)
    dir_cache = base_tmp / "_cache"
    entorno_base = dict(os.environ)

    print(f"{C.BOLD}Tiempos de compilacion{C.RESET}")
    print(f"{C.DIM}  fuentes generadas en {base_tmp}{C.RESET}")

    # --- 1. Suelo de cada herramienta: compilar un fichero que no declara nada.
    suelo: dict[str, dict] = {}
    for ln in langs:
        nombre, texto = VACIAS[ln]
        d = base_tmp / ("suelo_" + ln)
        d.mkdir(parents=True, exist_ok=True)
        (d / nombre).write_text(texto, encoding="utf-8")
        cmd = orden_compilar(ln, d / nombre, d / "out", vm)
        if not cmd:
            continue
        env = entorno_cache(ln, dir_cache, entorno_base)
        s = medir_caliente(cmd, env, d, args.repes, args.timeout)
        if s:
            suelo[ln] = s
    imprimir_tabla(
        "Suelo del compilador (ms): lo que tarda en compilar un fichero vacio",
        [(ln, ln, suelo.get(ln, {})) for ln in langs], {},
        "Esta DENTRO de cada medida de abajo.  Compilar uno de los benchmarks "
        "del corpus (40 lineas) cuesta lo mismo que esto: por eso aqui las "
        "fuentes se generan con tamano.")

    # --- 2. Compilacion completa, caliente y en frio, por tamano.
    filas_cal: list[tuple] = []
    filas_frio: list[tuple] = []
    frio_por_lang: dict[str, dict] = {}
    cal_por_lang: dict[str, dict] = {}
    resultados: dict = {"suelo": suelo, "casos": []}

    for n in tamanos:
        for ln in langs:
            nombre, gen = GENERADORES[ln]
            d = base_tmp / ("gen_%s_%d" % (ln, n))
            d.mkdir(parents=True, exist_ok=True)
            fuente = d / nombre
            fuente.write_text(gen(n), encoding="utf-8")
            lineas = gen(n).count("\n")
            cmd = orden_compilar(ln, fuente, d / "out", vm)
            if not cmd:
                continue
            env = entorno_cache(ln, dir_cache, entorno_base)
            etiqueta = "%s  %dk lineas" % (ln, round(lineas / 1000))

            # ANTES de cronometrar nada: comprobar que esto compila.  Un fallo
            # rapido parece un compilador rapidisimo.
            ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                           args.timeout)
            if not ok:
                print(f"  {C.RED}[no compila]{C.RESET} {etiqueta}: {motivo}")
                resultados["casos"].append({
                    "lang": ln, "funciones": n, "lineas": lineas,
                    "error": motivo,
                })
                continue

            s_cal = medir_caliente(cmd, env, d, args.repes, args.timeout)
            filas_cal.append((ln, etiqueta, s_cal))

            s_frio = medir_frio(cmd, env, d, args.repes, args.timeout, ln,
                                dir_cache)
            filas_frio.append((ln, etiqueta, s_frio))

            if n == tamanos[-1]:
                cal_por_lang[ln] = s_cal
                frio_por_lang[ln] = s_frio
            resultados["casos"].append({
                "lang": ln, "funciones": n, "lineas": lineas,
                "caliente": s_cal, "frio": s_frio,
            })

    imprimir_tabla("Compilacion completa, con las caches CALIENTES (ms)",
                   filas_cal, suelo,
                   "`neto` descuenta el suelo: es el tiempo que se va en "
                   "compilar de verdad.")
    imprimir_tabla("Compilacion completa EN FRIO (ms)", filas_frio, suelo,
                   "Sin ninguna cache.  Cada medida vacia la cache antes, asi "
                   "que no se calienta y son las mas ruidosas del modulo.")
    imprimir_ganancia(frio_por_lang, cal_por_lang, langs)

    # --- 2b. El MISMO programa repartido en varios ficheros.
    # Se compara contra la fila de un solo fichero del mismo tamano: mismo
    # trabajo, otra forma de presentarselo al compilador.
    filas_multi: list[tuple] = []
    filas_inc: list[tuple] = []
    for n in tamanos:
        for ln in langs:
            d = base_tmp / ("multi_%s_%d" % (ln, n))
            ficheros = escribir_multi(ln, n, args.ficheros, d)
            if not ficheros:
                continue
            cmd = orden_multi(ln, ficheros, d / "out", vm)
            if not cmd:
                continue
            env = entorno_cache(ln, dir_cache, entorno_base)
            etiqueta = "%s  %d ficheros" % (ln, len(ficheros))
            ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                           args.timeout)
            if not ok:
                print(f"  {C.RED}[no compila]{C.RESET} {etiqueta}: {motivo}")
                continue
            # DE CERO: se vacia la cache de artefactos antes de cada medida.
            #
            # Sin esto la comparacion es tramposa y lo comprobe midiendo: con
            # los `.vxi` calientes, Vesta daba 16 ms -- exactamente su suelo,
            # porque no reconstruia NADA -- mientras gcc recompilaba las 21
            # unidades enteras y daba 867.  "Caliente" no significa lo mismo
            # para quien tiene cache de artefactos que para quien no la tiene,
            # asi que la construccion completa se mide siempre de cero.
            s_cero = medir_frio(cmd, env, d, args.repes, args.timeout, ln,
                                dir_cache)
            filas_multi.append((ln, etiqueta, s_cero))

            # Con el proyecto ya construido, cuanto cuesta volver a
            # construirlo segun QUE haya cambiado.  El orden va de menos a mas
            # profundo, y la escalera entre ellos ES la granularidad de
            # invalidacion del compilador: uno que no distinga dara el mismo
            # numero en las cuatro filas.
            una_medida(cmd, env, args.timeout, d)   # dejarlo todo construido
            modulos = [f for f in ficheros if not f.lower().startswith("main")]
            casos_inc = [
                ("sin cambios", None, []),
                ("1 comentario", "comentario", modulos[:1]),
                ("1 cuerpo", "cuerpo", modulos[:1]),
                ("1 interfaz", "interfaz", modulos[:1]),
                ("mitad de los modulos", "cuerpo",
                 modulos[:max(1, len(modulos) // 2)]),
            ]
            for nombre_caso, clase, objetivos in casos_inc:
                serie = []
                for v in range(args.repes):
                    if clase is not None:
                        for f in objetivos:
                            mutar(d / f, ln, clase, v)
                    t = una_medida(cmd, env, args.timeout, d)
                    if t >= 0:
                        serie.append(t)
                s_i = _stats_summary(serie) if serie else {}
                filas_inc.append((ln, "%s  %s" % (ln, nombre_caso), s_i))
                resultados["casos"].append({
                    "lang": ln, "funciones": n, "ficheros": len(ficheros),
                    "regimen": nombre_caso, "stats": s_i,
                })
            resultados["casos"].append({
                "lang": ln, "funciones": n, "ficheros": len(ficheros),
                "regimen": "de cero", "stats": s_cero,
            })
    if filas_multi:
        imprimir_tabla(
            "El MISMO programa repartido en varios ficheros, DE CERO (ms)",
            filas_multi, suelo,
            "Comparar con la tabla de un solo fichero del mismo tamano: mismo "
            "trabajo, otra forma de darselo al compilador.  Se mide de cero "
            "porque 'caliente' no significa lo mismo para quien tiene cache de "
            "artefactos que para quien no la tiene.")
    if filas_inc:
        imprimir_tabla(
            "Reconstruir segun QUE haya cambiado (ms, con las caches puestas)",
            filas_inc, suelo,
            "`sin cambios` NO es un ranking de velocidad de compilacion: es el "
            "coste de REUTILIZAR: lo que tarda en demostrar que lo que ya "
            "tiene sigue valiendo.  Las cuatro filas siguientes van de menos a "
            "mas profundo, y la escalera entre ellas es la granularidad de "
            "invalidacion: quien no distinga dara el mismo numero en todas.")

    # --- 3. Realimentacion: cuanto tarda en salir el diagnostico.
    filas_chk: list[tuple] = []
    for n in tamanos:
        for ln in langs:
            nombre, gen = GENERADORES[ln]
            d = base_tmp / ("gen_%s_%d" % (ln, n))
            fuente = d / nombre
            if not fuente.is_file():
                continue
            cmd = orden_comprobar(ln, fuente, d / "chk", vm)
            if cmd is None:
                continue
            env = entorno_cache(ln, dir_cache, entorno_base)
            lineas = gen(n).count("\n")
            s = medir_caliente(cmd, env, d, args.repes, args.timeout)
            filas_chk.append((ln, "%s  %dk lineas" % (ln, round(lineas / 1000)), s))
    if filas_chk:
        imprimir_tabla(
            "Realimentacion: solo analizar y diagnosticar (ms)", filas_chk, {},
            "NO genera codigo ni enlaza.  go y java no tienen un modo de solo "
            "comprobar separado de compilar, asi que no salen: compararlos "
            "contra su compilacion completa no compararia lo mismo.")
        resultados["realimentacion"] = [
            {"lang": ln, "etiqueta": et, "stats": s} for ln, et, s in filas_chk]

    if args.out_json:
        Path(args.out_json).write_text(json.dumps(resultados, indent=2),
                                       encoding="utf-8")
        print()
        print(f"{C.GREEN}[ok]{C.RESET} JSON: {args.out_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
