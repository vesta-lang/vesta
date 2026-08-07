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
import hashlib
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
    buscar_compiladores,
    elegir_compilador,
    find_project_root,
    serie_asentada,
    una_medida,
)

# Cual de los compiladores instalados se usa.  Se decide UNA vez al
# arrancar y todas las ordenes lo consultan: tener gcc y clang a la vez es
# lo normal, y coger a ciegas el primero del PATH etiquetaria como gcc un
# numero que produjo clang.
ELEGIDO = {"c": "gcc", "cpp": "g++"}

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


# ===========================================================================
# QUE codigo, no cuanto.
#
# El generador de arriba emite lo mas anodino que existe: funciones sueltas con
# aritmetica entera.  Eso mide el trabajo de base -- lexico, sintaxis, tipos,
# generacion -- y esta bien para las curvas de tamano, pero deja sin tocar todo
# lo que de verdad puede explotar en un compilador.  Cada una de estas familias
# pega en una parte distinta:
#
#   genericos    Monomorfizacion: una plantilla por N tipos concretos MULTIPLICA
#                el codigo que hay que generar.  Es el sitio clasico donde un
#                compilador pasa de lineal a cuadratico sin avisar.
#   comptime     Ejecutar codigo DURANTE la compilacion.  El coste ya no depende
#                del tamano del fuente sino de lo que ese codigo tarde en correr,
#                que es una dimension que ningun otro banco toca.
#   anidamiento  Una sola expresion con mil niveles.  Pone a prueba la
#                recursion del analizador y del recorrido del arbol; es donde
#                aparecen los desbordamientos de pila y los algoritmos que
#                recorren el arbol una vez por nivel.
#   tipos        Muchas declaraciones distintas en vez de muchas funciones
#                iguales.  Estresa las tablas de simbolos y la resolucion de
#                nombres, no la generacion de codigo.
#
# Lo que un lenguaje no tenga se queda FUERA en vez de sustituirse por algo
# parecido: comptime no tiene equivalente en C, y comparar contra `constexpr`
# de C++ como si fuera lo mismo daria un numero que no significa nada.
# ===========================================================================

def gen_genericos_vx(n: int) -> str:
    """Una plantilla instanciada con `n` tipos distintos de usuario."""
    partes = ["struct Caja<T> {\n    T dato;\n    T leer() { return this.dato; }\n}\n",
              "T identidad<T>(T x) {\n    return x;\n}\n"]
    for i in range(n):
        partes.append("struct S%d { i64 a; }\n" % i)
    partes.append("i32 main() {\n    i64 s = 0;")
    for i in range(n):
        partes.append("    Caja<S%d> c%d;\n    c%d.dato.a = %d;\n"
                      "    s = s + identidad<i64>(c%d.leer().a);"
                      % (i, i, i, i % 100, i))
    partes.append("    return (i32) (s % 251);\n}\n")
    return "\n".join(partes)


def gen_genericos_cpp(n: int) -> str:
    partes = ["template <typename T> struct Caja { T dato; T leer() { return dato; } };",
              "template <typename T> T identidad(T x) { return x; }"]
    for i in range(n):
        partes.append("struct S%d { long long a; };" % i)
    partes.append("int main() {\n    long long s = 0;")
    for i in range(n):
        partes.append("    Caja<S%d> c%d; c%d.dato.a = %d;"
                      " s += identidad<long long>(c%d.leer().a);"
                      % (i, i, i, i % 100, i))
    partes.append("    return (int)(s % 251);\n}")
    return "\n".join(partes) + "\n"


def gen_genericos_rs(n: int) -> str:
    partes = ["struct Caja<T> { dato: T }",
              "impl<T: Copy> Caja<T> { fn leer(&self) -> T { self.dato } }",
              "fn identidad<T>(x: T) -> T { x }"]
    for i in range(n):
        partes.append("#[derive(Clone, Copy)] struct S%d { a: i64 }" % i)
    partes.append("fn main() {\n    let mut s: i64 = 0;")
    for i in range(n):
        partes.append("    let c%d = Caja { dato: S%d { a: %d } };"
                      " s = s.wrapping_add(identidad(c%d.leer().a));"
                      % (i, i, i % 100, i))
    partes.append("    std::process::exit((s % 251) as i32);\n}")
    return "\n".join(partes) + "\n"


def gen_genericos_java(n: int) -> str:
    partes = ["public class Gen {",
              "    static class Caja<T> { T dato; T leer() { return dato; } }",
              "    static <T> T identidad(T x) { return x; }"]
    for i in range(n):
        partes.append("    static class S%d { long a; }" % i)
    partes.append("    public static void main(String[] args) {\n        long s = 0;")
    for i in range(n):
        partes.append("        Caja<S%d> c%d = new Caja<>(); c%d.dato = new S%d();"
                      " c%d.dato.a = %d; s += identidad(c%d.leer().a);"
                      % (i, i, i, i, i, i % 100, i))
    partes.append("        System.exit((int)(s % 251));\n    }\n}")
    return "\n".join(partes) + "\n"


def gen_comptime_vx(n: int) -> str:
    """`n` constantes calculadas EJECUTANDO codigo al compilar.

    El coste no lo pone el tamano del fuente: lo pone lo que ese codigo tarde en
    correr.  Es la unica familia donde compilar mas despacio puede deberse a un
    bucle del programador y no al compilador, y por eso se mide aparte.
    """
    partes = ["comptime i64 fib(i64 n) {\n    if (n <= 1) return n;\n"
              "    return fib(n - 1) + fib(n - 2);\n}\n",
              "comptime i64 fact(i64 n) {\n    if (n <= 1) return 1;\n"
              "    return n * fact(n - 1);\n}\n"]
    for i in range(n):
        partes.append("const i64 K%d = fib(%d) + fact(%d);" % (i, 10 + (i % 8),
                                                               5 + (i % 5)))
    partes.append("i32 main() {\n    i64 s = 0;")
    for i in range(n):
        partes.append("    s = s + K%d;" % i)
    partes.append("    return (i32) (s % 251);\n}\n")
    return "\n".join(partes)


def _anid(n: int, abrir: str, cerrar: str, hoja: str) -> str:
    """Expresion con `n` niveles de parentesis anidados."""
    return abrir * n + hoja + cerrar * n


def gen_anidamiento_vx(n: int) -> str:
    return ("i32 main() {\n    i64 s = %s;\n    return (i32) (s %% 251);\n}\n"
            % _anid(n, "(1 + ", ")", "7"))


def gen_anidamiento_c(n: int) -> str:
    return ("int main(void) {\n    long long s = %s;\n"
            "    return (int)(s %% 251);\n}\n" % _anid(n, "(1 + ", ")", "7"))


def gen_anidamiento_rs(n: int) -> str:
    return ("fn main() {\n    let s: i64 = %s;\n"
            "    std::process::exit((s %% 251) as i32);\n}\n"
            % _anid(n, "(1 + ", ")", "7"))


def gen_anidamiento_go(n: int) -> str:
    return ("package main\n\nimport \"os\"\n\nfunc main() {\n\tvar s int64 = %s\n"
            "\tos.Exit(int(s %% 251))\n}\n" % _anid(n, "(1 + ", ")", "7"))


def gen_anidamiento_java(n: int) -> str:
    return ("public class Gen {\n    public static void main(String[] a) {\n"
            "        long s = %s;\n        System.exit((int)(s %% 251));\n"
            "    }\n}\n" % _anid(n, "(1 + ", ")", "7"))


def gen_tipos_vx(n: int) -> str:
    """`n` structs distintos con varios campos: estresa la tabla de simbolos."""
    partes = []
    for i in range(n):
        partes.append("struct T%d { i64 a; i64 b; i64 c; i64 d; }\n" % i)
    partes.append("i32 main() {\n    i64 s = 0;")
    for i in range(n):
        partes.append("    T%d v%d;\n    v%d.a = %d;\n    s = s + v%d.a;"
                      % (i, i, i, i % 100, i))
    partes.append("    return (i32) (s % 251);\n}\n")
    return "\n".join(partes)


def gen_tipos_c(n: int) -> str:
    partes = []
    for i in range(n):
        partes.append("typedef struct { long long a, b, c, d; } T%d;" % i)
    partes.append("int main(void) {\n    long long s = 0;")
    for i in range(n):
        partes.append("    T%d v%d; v%d.a = %d; s += v%d.a;"
                      % (i, i, i, i % 100, i))
    partes.append("    return (int)(s % 251);\n}")
    return "\n".join(partes) + "\n"


def gen_tipos_rs(n: int) -> str:
    partes = []
    for i in range(n):
        partes.append("struct T%d { a: i64, b: i64, c: i64, d: i64 }" % i)
    partes.append("fn main() {\n    let mut s: i64 = 0;")
    for i in range(n):
        partes.append("    let v%d = T%d { a: %d, b: 0, c: 0, d: 0 };"
                      " s = s.wrapping_add(v%d.a);" % (i, i, i % 100, i))
    partes.append("    std::process::exit((s % 251) as i32);\n}")
    return "\n".join(partes) + "\n"


def gen_tipos_go(n: int) -> str:
    partes = ["package main", "", "import \"os\"", ""]
    for i in range(n):
        partes.append("type T%d struct{ a, b, c, d int64 }" % i)
    partes.append("func main() {\n\tvar s int64 = 0")
    for i in range(n):
        partes.append("\tv%d := T%d{a: %d}\n\ts += v%d.a" % (i, i, i % 100, i))
    partes.append("\tos.Exit(int(s % 251))\n}")
    return "\n".join(partes) + "\n"


def gen_tipos_java(n: int) -> str:
    partes = ["public class Gen {"]
    for i in range(n):
        partes.append("    static class T%d { long a, b, c, d; }" % i)
    partes.append("    public static void main(String[] args) {\n        long s = 0;")
    for i in range(n):
        partes.append("        T%d v%d = new T%d(); v%d.a = %d; s += v%d.a;"
                      % (i, i, i, i, i % 100, i))
    partes.append("        System.exit((int)(s % 251));\n    }\n}")
    return "\n".join(partes) + "\n"


def familia_modular_vx(familia: str, n: int, d: Path) -> Optional[list[str]]:
    """La familia METIDA EN UN MODULO, con un principal que la usa.

    Un numero por familia dice cuanto cuesta compilarla, y nada mas.  Lo util es
    el desglose: si de trescientos milisegundos doscientos ochenta son comptime,
    cambiar el cuerpo de una funcion normal cuesta cuarenta y cambiar su
    interfaz ciento veinte, ya se sabe donde mirar.  Y eso solo se puede
    preguntar si la familia vive en un modulo del que otro depende.

    Solo Vesta por ahora: es donde estan comptime y nuestra monomorfizacion, que
    son los dos subsistemas de los que no habia ni un numero.
    """
    d.mkdir(parents=True, exist_ok=True)
    if familia == "genericos":
        cuerpo = ["struct Caja<T> {\n    T dato;\n    T leer() { return this.dato; }\n}\n",
                  "T identidad<T>(T x) {\n    return x;\n}\n"]
        for i in range(n):
            cuerpo.append("struct S%d { i64 a; }\n" % i)
        # El `* 3 +` de la primera linea es el ancla de la mutacion "cuerpo":
        # sin un patron que cambiar, el cambio no se aplicaba y la fila salia
        # identica a `sin cambios` sin que nada lo dijera.
        cuerpo.append("public i64 punto(i64 x) {\n    i64 s = x * 3 + 1;")
        for i in range(n):
            cuerpo.append("    Caja<S%d> c%d;\n    c%d.dato.a = %d;\n"
                          "    s = s + identidad<i64>(c%d.leer().a);"
                          % (i, i, i, i % 100, i))
        cuerpo.append("    return s;\n}\n")
        texto = "namespace fam.m0;\n\n" + "\n".join(cuerpo)
    elif familia == "comptime":
        cuerpo = ["comptime i64 fib(i64 n) {\n    if (n <= 1) return n;\n"
                  "    return fib(n - 1) + fib(n - 2);\n}\n",
                  "comptime i64 fact(i64 n) {\n    if (n <= 1) return 1;\n"
                  "    return n * fact(n - 1);\n}\n"]
        for i in range(n):
            cuerpo.append("const i64 K%d = fib(%d) + fact(%d);"
                          % (i, 10 + (i % 8), 5 + (i % 5)))
        cuerpo.append("public i64 punto(i64 x) {\n    i64 s = x * 3 + 1;")
        for i in range(n):
            cuerpo.append("    s = s + K%d;" % i)
        cuerpo.append("    return s;\n}\n")
        texto = "namespace fam.m0;\n\n" + "\n".join(cuerpo)
    elif familia == "anidamiento":
        texto = ("namespace fam.m0;\n\npublic i64 punto(i64 x) {\n"
                 "    i64 s = %s;\n    return s + x * 3 + 1;\n}\n"
                 % _anid(n, "(1 + ", ")", "7"))
    elif familia == "tipos":
        cuerpo = []
        for i in range(n):
            cuerpo.append("struct T%d { i64 a; i64 b; i64 c; i64 d; }\n" % i)
        cuerpo.append("public i64 punto(i64 x) {\n    i64 s = x * 3 + 1;")
        for i in range(n):
            cuerpo.append("    T%d v%d;\n    v%d.a = %d;\n    s = s + v%d.a;"
                          % (i, i, i, i % 100, i))
        cuerpo.append("    return s;\n}\n")
        texto = "namespace fam.m0;\n\n" + "\n".join(cuerpo)
    else:
        return None
    (d / "m0.vx").write_text(texto, encoding="utf-8")
    (d / "main.vx").write_text(
        'import "m0" only punto;\n\ni32 main() {\n'
        "    return (i32) (punto(1) % 251);\n}\n", encoding="utf-8")
    return ["m0.vx", "main.vx"]


# familia -> {lang: (nombre de fichero, generador)}.  Lo que un lenguaje no
# tiene simplemente no aparece: sustituirlo por algo parecido daria un numero
# que no significa lo mismo.
FAMILIAS = {
    "genericos": {
        "vesta": ("gen.vx", gen_genericos_vx),
        "vesta_aot": ("gen.vx", gen_genericos_vx),
        "cpp": ("gen.cpp", gen_genericos_cpp),
        "rust": ("gen.rs", gen_genericos_rs),
        "java": ("Gen.java", gen_genericos_java),
    },
    "comptime": {
        "vesta": ("gen.vx", gen_comptime_vx),
        "vesta_aot": ("gen.vx", gen_comptime_vx),
    },
    "anidamiento": {
        "vesta": ("gen.vx", gen_anidamiento_vx),
        "vesta_aot": ("gen.vx", gen_anidamiento_vx),
        "c": ("gen.c", gen_anidamiento_c),
        "cpp": ("gen.cpp", gen_anidamiento_c),
        "rust": ("gen.rs", gen_anidamiento_rs),
        "go": ("gen.go", gen_anidamiento_go),
        "java": ("Gen.java", gen_anidamiento_java),
    },
    "tipos": {
        "vesta": ("gen.vx", gen_tipos_vx),
        "vesta_aot": ("gen.vx", gen_tipos_vx),
        "c": ("gen.c", gen_tipos_c),
        "cpp": ("gen.cpp", gen_tipos_c),
        "rust": ("gen.rs", gen_tipos_rs),
        "go": ("gen.go", gen_tipos_go),
        "java": ("Gen.java", gen_tipos_java),
    },
}


# ===========================================================================
# TOPOLOGIA de dependencias.
#
# El reparto en ficheros de arriba deja a los veintiun modulos como HOJAS: el
# principal los importa a todos y ninguno importa a otro.  Es el caso mas facil
# que existe para invalidar una cache, y por eso no ensena nada -- cambiar
# cualquiera de ellos no puede afectar a nadie mas.
#
# La pregunta que de verdad juzga una cache de interfaces es otra: si cambio el
# CUERPO de un modulo del que dependen otros, ¿se recompilan ellos tambien?  No
# deberian: su interfaz no ha cambiado.  Si se recompilan, la interfaz no esta
# cortando la propagacion y ahi se pierde la mitad de su valor.  Eso solo se ve
# con una cadena o un diamante.
#
#   ancha      main -> m0, m1, ... mk        (todos hojas; el caso de arriba)
#   cadena     main -> mk -> ... -> m1 -> m0 (el cambio en m0 puede propagarse
#                                             hasta el final)
#   diamante   main -> cima -> medios -> base (dos niveles, y los medios
#                                             comparten base: ¿se rehace una vez
#                                             o una por cada uno?)
# ===========================================================================

def dependencias(k: int, forma: str) -> tuple[list[list[int]], list[int]]:
    """Devuelve (deps por modulo, modulos que importa el principal)."""
    if forma == "cadena":
        deps = [[] if j == 0 else [j - 1] for j in range(k)]
        return deps, [k - 1]
    if forma == "diamante":
        # m0 es la base; m1..mk-2 dependen de ella; mk-1 es la cima.
        deps = [[] for _ in range(k)]
        for j in range(1, k - 1):
            deps[j] = [0]
        deps[k - 1] = list(range(1, k - 1))
        return deps, [k - 1]
    return [[] for _ in range(k)], list(range(k))   # ancha


def escribir_topologia(lang: str, n: int, k: int, forma: str,
                       d: Path) -> Optional[list[str]]:
    """Escribe el programa con la forma de dependencias pedida.

    Cada modulo expone UNA funcion publica que llama a las de sus dependencias,
    mas relleno hasta repartir las `n` funciones.  El relleno es lo que da peso;
    la funcion publica es la que crea la arista.
    """
    d.mkdir(parents=True, exist_ok=True)
    deps, raiz = dependencias(k, forma)
    trozos = _trozos(n, k)
    if len(trozos) < k:
        return None
    ficheros: list[str] = []

    def relleno_llaves(t, tipo, decl):
        return "".join(decl % (i, tipo, i) for i in t)

    if lang in ("c", "cpp"):
        ext = "c" if lang == "c" else "cpp"
        for j in range(k):
            inc = "".join('#include "m%d.h"\n' % o for o in deps[j])
            llam = "".join(" + pub%d(x)" % o for o in deps[j])
            filler = "".join(
                "static long long r%d_%d(long long x) { return x * 3 + %d; }\n"
                % (j, i, i) for i in trozos[j])
            (d / ("m%d.h" % j)).write_text(
                "#pragma once\nlong long pub%d(long long x);\n" % j,
                encoding="utf-8")
            (d / ("m%d.%s" % (j, ext))).write_text(
                inc + '#include "m%d.h"\n' % j + filler +
                "long long pub%d(long long x) { return x%s; }\n" % (j, llam),
                encoding="utf-8")
            ficheros.append("m%d.%s" % (j, ext))
        inc = "".join('#include "m%d.h"\n' % o for o in raiz)
        llam = "".join("    s += pub%d(%d);\n" % (o, o) for o in raiz)
        (d / ("main.%s" % ext)).write_text(
            inc + "int main(void) {\n    long long s = 0;\n" + llam +
            "    return (int)(s % 251);\n}\n", encoding="utf-8")
        ficheros.append("main.%s" % ext)
        return ficheros

    if lang in ("vesta", "vesta_aot"):
        for j in range(k):
            imp = "".join('import "m%d" only pub%d;\n' % (o, o) for o in deps[j])
            llam = "".join(" + pub%d(x)" % o for o in deps[j])
            filler = "".join(
                "i64 r%d_%d(i64 x) { return x * 3 + %d; }\n" % (j, i, i)
                for i in trozos[j])
            (d / ("m%d.vx" % j)).write_text(
                imp + "namespace top.m%d;\n\n" % j + filler +
                "public i64 pub%d(i64 x) { return x%s; }\n" % (j, llam),
                encoding="utf-8")
            ficheros.append("m%d.vx" % j)
        imp = "".join('import "m%d" only pub%d;\n' % (o, o) for o in raiz)
        llam = "".join("    s = s + pub%d(%d);\n" % (o, o) for o in raiz)
        (d / "main.vx").write_text(
            imp + "\ni32 main() {\n    i64 s = 0;\n" + llam +
            "    return (i32) (s % 251);\n}\n", encoding="utf-8")
        ficheros.append("main.vx")
        return ficheros

    if lang == "rust":
        for j in range(k):
            usos = "".join("use crate::m%d::pub%d;\n" % (o, o) for o in deps[j])
            llam = "".join(".wrapping_add(pub%d(x))" % o for o in deps[j])
            filler = "".join(
                "fn r%d_%d(x: i64) -> i64 { x.wrapping_mul(3).wrapping_add(%d) }\n"
                % (j, i, i) for i in trozos[j])
            (d / ("m%d.rs" % j)).write_text(
                usos + filler +
                "pub fn pub%d(x: i64) -> i64 { x%s }\n" % (j, llam),
                encoding="utf-8")
            ficheros.append("m%d.rs" % j)
        mods = "".join("mod m%d;\n" % j for j in range(k))
        llam = "".join("    s = s.wrapping_add(m%d::pub%d(%d));\n" % (o, o, o)
                       for o in raiz)
        (d / "main.rs").write_text(
            mods + "fn main() {\n    let mut s: i64 = 0;\n" + llam +
            "    std::process::exit((s % 251) as i32);\n}\n", encoding="utf-8")
        ficheros.append("main.rs")
        return ficheros

    if lang == "go":
        # Go no deja importar dentro del mismo paquete: la topologia se expresa
        # con llamadas entre ficheros, que es como el lenguaje lo entiende.
        for j in range(k):
            llam = "".join(" + pub%d(x)" % o for o in deps[j])
            filler = "".join(
                "func r%d_%d(x int64) int64 { return x*3 + %d }\n" % (j, i, i)
                for i in trozos[j])
            (d / ("m%d.go" % j)).write_text(
                "package main\n\n" + filler +
                "func pub%d(x int64) int64 { return x%s }\n" % (j, llam),
                encoding="utf-8")
            ficheros.append("m%d.go" % j)
        llam = "".join("\ts += pub%d(%d)\n" % (o, o) for o in raiz)
        (d / "main.go").write_text(
            "package main\n\nimport \"os\"\n\nfunc main() {\n\tvar s int64 = 0\n"
            + llam + "\tos.Exit(int(s % 251))\n}\n", encoding="utf-8")
        ficheros.append("main.go")
        return ficheros

    if lang == "java":
        for j in range(k):
            llam = "".join(" + M%d.pub%d(x)" % (o, o) for o in deps[j])
            filler = "".join(
                "    static long r%d_%d(long x) { return x * 3 + %d; }\n"
                % (j, i, i) for i in trozos[j])
            (d / ("M%d.java" % j)).write_text(
                "public class M%d {\n" % j + filler +
                "    public static long pub%d(long x) { return x%s; }\n}\n"
                % (j, llam), encoding="utf-8")
            ficheros.append("M%d.java" % j)
        llam = "".join("        s += M%d.pub%d(%d);\n" % (o, o, o) for o in raiz)
        (d / "Main.java").write_text(
            "public class Main {\n    public static void main(String[] a) {\n"
            "        long s = 0;\n" + llam +
            "        System.exit((int)(s % 251));\n    }\n}\n", encoding="utf-8")
        ficheros.append("Main.java")
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


# Que artefactos intermedios deja cada herramienta, para poder CONTAR cuales
# rehace.  Los que no dejan ninguno observable (una sola invocacion de gcc, o
# una cache opaca como la de Go) se quedan fuera de esa cuenta en vez de
# rellenarse con un cero que se leeria como "no rehizo nada".
ARTEFACTOS = {
    "vesta": ("*.vxi", "*.vxir"),
    "vesta_aot": ("*.vxi", "*.vxir"),
    "java": ("clases/*.class",),
    "rust": ("*.rmeta", "*.rlib"),
}


def huella_artefactos(d: Path, lang: str) -> dict[str, str]:
    """Hash de cada artefacto intermedio que hay ahora mismo en @p d."""
    patrones = ARTEFACTOS.get(lang)
    if not patrones:
        return {}
    out: dict[str, str] = {}
    for pat in patrones:
        for p in d.glob(pat):
            try:
                out[p.name] = hashlib.sha256(p.read_bytes()).hexdigest()[:16]
            except OSError:
                pass
    return out


def contar_rehechos(antes: dict[str, str],
                    despues: dict[str, str]) -> tuple[int, int, int]:
    """Devuelve (rehechos, reutilizados, nuevos).

    Es la medida FUERTE del asunto: el tiempo lo ensucian la cache del sistema,
    otros procesos y la paginacion, pero que un artefacto cambie o no es un
    hecho.  Si cambiar el CUERPO de un modulo rehace solo el suyo y cambiar su
    INTERFAZ rehace ademas los de quien depende de el, la frontera esta
    haciendo su trabajo -- y eso ya no depende de cuanto tardara la maquina.
    """
    rehechos = sum(1 for k, v in despues.items()
                   if k in antes and antes[k] != v)
    reutilizados = sum(1 for k, v in despues.items()
                       if k in antes and antes[k] == v)
    nuevos = sum(1 for k in despues if k not in antes)
    return (rehechos, reutilizados, nuevos)


def orden_multi(lang: str, ficheros: list[str], salida: Path,
                vm: Path) -> list[str]:
    """Orden que compila el proyecto repartido.  Cada herramienta lo recibe
    como ella lo entiende: unos quieren todas las unidades, otros solo el
    fichero raiz y ya siguen las dependencias."""
    if lang == "c":
        return [ELEGIDO["c"], "-O2", "-std=c11"] + ficheros + ["-o", str(salida)]
    if lang == "cpp":
        return [ELEGIDO["cpp"], "-O2", "-std=c++17"] + ficheros + ["-o", str(salida)]
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
        return [ELEGIDO["c"], "-O2", "-std=c11", str(fuente), "-o", str(salida)]
    if lang == "cpp":
        return [ELEGIDO["cpp"], "-O2", "-std=c++17", str(fuente), "-o", str(salida)]
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
        return [ELEGIDO["c"], "-fsyntax-only", "-std=c11", str(fuente)]
    if lang == "cpp":
        return [ELEGIDO["cpp"], "-fsyntax-only", "-std=c++17", str(fuente)]
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
    p.add_argument("--tamanos", type=str, default="200,800",
                   help="numero de funciones generadas por fuente "
                        "(~5 lineas cada una).  Default: 200,800, que son "
                        "~1.4k y ~5.7k lineas: bastante para que haya algo que "
                        "compilar sin que la tanda dure una eternidad.")
    p.add_argument("--cc", type=str, default="",
                   help="compilador de C a usar.  Sin esto: si hay varios "
                        "instalados se pregunta.")
    p.add_argument("--cxx", type=str, default="",
                   help="compilador de C++ a usar.")
    p.add_argument("--escalado", action="store_true",
                   help="anade las dos curvas de escalado: por tamano de "
                        "programa y por numero de modulos.  Cuesta bastante "
                        "mas tiempo, por eso no va por defecto.")
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

    # Sin `--langs`, se usan todos los que ESTeN instalados.  Pedidos a mano,
    # se respetan aunque falten: si alguien nombra una herramienta que no
    # tiene, lo que quiere es enterarse, no que se le ignore en silencio.
    n_c, r_c = elegir_compilador(
        "C", buscar_compiladores(["gcc", "clang", "cc"]), args.cc)
    n_cpp, r_cpp = elegir_compilador(
        "C++", buscar_compiladores(["g++", "clang++", "c++"]), args.cxx)
    if r_c:
        ELEGIDO["c"] = r_c
    if r_cpp:
        ELEGIDO["cpp"] = r_cpp

    herramienta = {"c": r_c or "gcc", "cpp": r_cpp or "g++",
                   "rust": "rustc", "go": "go",
                   "java": "javac", "python": sys.executable}
    if args.langs:
        langs = [l.strip() for l in args.langs.split(",") if l.strip()]
    else:
        langs = []
        ausentes = []
        for ln in GENERADORES:
            h = herramienta.get(ln)
            if h is None or shutil.which(h) or Path(h).is_file():
                langs.append(ln)
            else:
                ausentes.append("%s (%s)" % (ln, h))
        if ausentes:
            print(f"{C.YELLOW}[aviso]{C.RESET} sin instalar, se omiten: "
                  + ", ".join(ausentes))
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

    # --- 2bis. ESCALADO.  Dos curvas, porque preguntan cosas distintas:
    #   por tamano  -- ¿el coste crece con el programa de forma lineal, o hay
    #                  algo superlineal escondido?  Con un solo punto esto es
    #                  invisible, y una superlinealidad se descubre tarde y cara.
    #   por modulos -- a tamano TOTAL constante, repartirlo en mas ficheros mide
    #                  el coste FIJO por modulo: lo que paga un proyecto muy
    #                  dividido solo por estarlo.
    if args.escalado:
        print()
        print(f"{C.BOLD}Escalado por tamano (un fichero){C.RESET}")
        print(f"{C.DIM}  Si el coste por linea sube con el tamano, hay algo "
              f"superlineal.{C.RESET}")
        cab = (f"{'lenguaje':<12}{'lineas':>9}{'ms':>10}{'ms/kloc':>10}"
               f"{'vs el anterior':>16}")
        print(f"{C.BOLD}{cab}{C.RESET}")
        print("-" * len(cab))
        escala = [200, 800, 3200]
        for ln in langs:
            nombre, gen = GENERADORES[ln]
            previo = None
            for nf in escala:
                d = base_tmp / ("esc_%s_%d" % (ln, nf))
                d.mkdir(parents=True, exist_ok=True)
                texto = gen(nf)
                (d / nombre).write_text(texto, encoding="utf-8")
                lineas = texto.count("\n")
                cmd = orden_compilar(ln, d / nombre, d / "out", vm)
                env = entorno_cache(ln, dir_cache, entorno_base)
                ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                               args.timeout)
                if not ok:
                    print(f"  {C.RED}[no compila]{C.RESET} {ln} {lineas}: {motivo}")
                    break
                s = medir_caliente(cmd, env, d, max(3, args.repes // 2),
                                   args.timeout)
                if not s:
                    break
                piso = (suelo.get(ln) or {}).get("p50") or 0.0
                neto = max(0.001, s["p50"] - piso)
                por_kloc = 1000.0 * neto / max(1, lineas)
                rel = ("%6.2fx" % (neto / previo)) if previo else "     -"
                print(f"  {ln:<10}{lineas:>9}{s['p50']:>10.0f}"
                      f"{por_kloc:>10.1f}{rel:>16}")
                resultados["casos"].append({
                    "lang": ln, "escalado": "tamano", "lineas": lineas,
                    "stats": s, "neto": neto})
                previo = neto
        print("-" * len(cab))

        print()
        print(f"{C.BOLD}Escalado por numero de modulos (mismo total){C.RESET}")
        print(f"{C.DIM}  Mismo codigo repartido en mas ficheros: lo que sube es "
              f"el coste FIJO por modulo.{C.RESET}")
        cab2 = f"{'lenguaje':<12}{'modulos':>9}{'ms':>10}{'ms/modulo':>12}"
        print(f"{C.BOLD}{cab2}{C.RESET}")
        print("-" * len(cab2))
        for ln in langs:
            for k in (1, 8, 32, 128):
                d = base_tmp / ("escm_%s_%d" % (ln, k))
                ficheros = escribir_multi(ln, 1024, k, d)
                if not ficheros:
                    continue
                cmd = orden_multi(ln, ficheros, d / "out", vm)
                env = entorno_cache(ln, dir_cache, entorno_base)
                ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                               args.timeout)
                if not ok:
                    print(f"  {C.RED}[no compila]{C.RESET} {ln} k={k}: {motivo}")
                    continue
                s = medir_frio(cmd, env, d, max(3, args.repes // 2),
                               args.timeout, ln, dir_cache)
                if not s:
                    continue
                print(f"  {ln:<10}{len(ficheros):>9}{s['p50']:>10.0f}"
                      f"{s['p50'] / len(ficheros):>12.1f}")
                resultados["casos"].append({
                    "lang": ln, "escalado": "modulos",
                    "ficheros": len(ficheros), "stats": s})
        print("-" * len(cab2))

    # --- 2c. TOPOLOGIA: la misma cantidad de codigo con otra forma de
    # dependencias.  El cambio se hace SIEMPRE en el modulo del que cuelgan los
    # demas (m0), que es el unico sitio desde donde se puede observar si la
    # invalidacion se propaga o se corta.
    filas_topo: list[tuple] = []
    filas_cuenta: list[tuple] = []
    n_topo = tamanos[-1]
    for forma in ("ancha", "cadena", "diamante"):
        for ln in langs:
            d = base_tmp / ("topo_%s_%s" % (ln, forma))
            ficheros = escribir_topologia(ln, n_topo, args.ficheros, forma, d)
            if not ficheros:
                continue
            cmd = orden_multi(ln, ficheros, d / "out", vm)
            if not cmd:
                continue
            env = entorno_cache(ln, dir_cache, entorno_base)
            ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                           args.timeout)
            if not ok:
                print(f"  {C.RED}[no compila]{C.RESET} topologia {forma}/{ln}: "
                      f"{motivo}")
                continue
            una_medida(cmd, env, args.timeout, d)   # dejarlo construido
            raiz_mod = [f for f in ficheros
                        if f.startswith("m0.") or f.startswith("M0.")]
            for clase, titulo in (("cuerpo", "cuerpo de m0"),
                                  ("interfaz", "interfaz de m0")):
                serie = []
                cuenta = None
                for v in range(args.repes):
                    for f in raiz_mod:
                        mutar(d / f, ln, clase, v)
                    # La primera vuelta se observa ademas por artefactos: que
                    # cambie o no un `.vxi` es un HECHO, mientras que el tiempo
                    # lo ensucian la cache del sistema y la maquina entera.
                    antes = huella_artefactos(d, ln) if v == 0 else {}
                    t = una_medida(cmd, env, args.timeout, d)
                    if v == 0 and antes:
                        cuenta = contar_rehechos(antes,
                                                 huella_artefactos(d, ln))
                    if t >= 0:
                        serie.append(t)
                s_t = _stats_summary(serie) if serie else {}
                filas_topo.append((ln, "%-9s %s  %s" % (forma, ln, titulo), s_t))
                if cuenta is not None:
                    filas_cuenta.append((forma, ln, titulo, cuenta))
                resultados["casos"].append({
                    "lang": ln, "topologia": forma, "cambio": clase,
                    "ficheros": len(ficheros), "stats": s_t,
                    "artefactos": ({"rehechos": cuenta[0],
                                    "reutilizados": cuenta[1],
                                    "nuevos": cuenta[2]} if cuenta else None),
                })
    if filas_topo:
        imprimir_tabla(
            "Topologia: donde cuelga cada modulo, y si el cambio se propaga (ms)",
            filas_topo, suelo,
            "El cambio va SIEMPRE en m0, del que cuelgan los demas.  Cambiar su "
            "CUERPO no cambia lo que ofrece, asi que sus dependientes no "
            "deberian rehacerse; cambiar su INTERFAZ obliga a revalidarlos.  La "
            "diferencia entre esas dos filas es lo que la interfaz esta "
            "cortando: si son iguales, no corta nada.")
    if filas_cuenta:
        # La medida FUERTE: no cuanto tardo, sino QUE rehizo.
        print()
        print(f"{C.BOLD}Que artefactos se rehacen, por tipo de cambio{C.RESET}")
        print(f"{C.DIM}  El tiempo lo ensucian la cache del sistema y la "
              f"maquina entera; que un artefacto cambie o no es un hecho.  "
              f"Solo salen las herramientas que dejan artefactos observables: "
              f"una sola invocacion de gcc no deja ninguno, y la cache de Go es "
              f"opaca.{C.RESET}")
        cab3 = (f"{'topologia / cambio':<40}{'rehechos':>10}"
                f"{'reutilizados':>14}{'nuevos':>9}")
        print(f"{C.BOLD}{cab3}{C.RESET}")
        print("-" * len(cab3))
        for forma, ln, titulo, (re_, reu, nue) in filas_cuenta:
            col = C.GREEN if reu > re_ else C.YELLOW
            print(f"  {forma + '  ' + ln + '  ' + titulo:<38}"
                  f"{col}{re_:>10}{C.RESET}{reu:>14}{nue:>9}")
        print("-" * len(cab3))

    # --- 2d. QUE codigo, no cuanto.  Cada familia pega en una parte distinta
    # del compilador, y son justo las que el generador anodino no toca.
    filas_fam: list[tuple] = []
    for familia, porlang in FAMILIAS.items():
        # Cuentas distintas por familia: mil niveles de anidamiento no es lo
        # mismo que mil instanciaciones de una plantilla, y forzar el mismo
        # numero solo conseguiria que unas tarden segundos y otras nada.
        cuenta = {"genericos": 150, "comptime": 60,
                  "anidamiento": 500, "tipos": 400}[familia]
        for ln in langs:
            par = porlang.get(ln)
            if par is None:
                continue
            nombre, gen = par
            d = base_tmp / ("fam_%s_%s" % (familia, ln))
            d.mkdir(parents=True, exist_ok=True)
            texto = gen(cuenta)
            (d / nombre).write_text(texto, encoding="utf-8")
            cmd = orden_compilar(ln, d / nombre, d / "out", vm)
            if not cmd:
                continue
            env = entorno_cache(ln, dir_cache, entorno_base)
            etiqueta = "%-12s %s" % (familia, ln)
            ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                           args.timeout)
            if not ok:
                print(f"  {C.RED}[no compila]{C.RESET} {etiqueta}: {motivo}")
                resultados["casos"].append({
                    "lang": ln, "familia": familia, "error": motivo})
                continue
            s = medir_caliente(cmd, env, d, args.repes, args.timeout)
            filas_fam.append((ln, etiqueta, s))
            resultados["casos"].append({
                "lang": ln, "familia": familia, "cuenta": cuenta,
                "lineas": texto.count("\n"), "stats": s})
    if filas_fam:
        imprimir_tabla(
            "Por FAMILIA de codigo (ms)", filas_fam, suelo,
            "No es lo mismo mucho codigo que codigo dificil.  Cada familia pega "
            "en una parte distinta: genericos multiplica lo que hay que "
            "generar, comptime EJECUTA al compilar, anidamiento pone a prueba "
            "la recursion del analizador y tipos estresa la tabla de simbolos.  "
            "Lo que un lenguaje no tiene no aparece, en vez de sustituirse por "
            "algo parecido.")

    # --- 2e. Familia x REGIMEN.  Un numero por familia dice cuanto cuesta;
    # el desglose dice DONDE se va y que parte se puede evitar tras un cambio.
    filas_fr: list[tuple] = []
    for familia in ("genericos", "comptime", "anidamiento", "tipos"):
        cuenta = {"genericos": 150, "comptime": 60,
                  "anidamiento": 500, "tipos": 400}[familia]
        for ln in ("vesta", "vesta_aot"):
            if ln not in langs:
                continue
            d = base_tmp / ("fr_%s_%s" % (familia, ln))
            ficheros = familia_modular_vx(familia, cuenta, d)
            if not ficheros:
                continue
            cmd = orden_multi(ln, ficheros, d / "out", vm)
            env = entorno_cache(ln, dir_cache, entorno_base)
            ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                           args.timeout)
            if not ok:
                print(f"  {C.RED}[no compila]{C.RESET} {familia}/{ln}: {motivo}")
                continue
            s_cero = medir_frio(cmd, env, d, args.repes, args.timeout, ln,
                                dir_cache)
            filas_fr.append((ln, "%-12s %-10s de cero" % (familia, ln), s_cero))
            una_medida(cmd, env, args.timeout, d)
            for clase, nombre_caso in ((None, "sin cambios"),
                                       ("cuerpo", "cambia el cuerpo"),
                                       ("interfaz", "cambia la interfaz")):
                serie = []
                fallo_mutacion = False
                for v in range(args.repes):
                    if clase is not None and not mutar(d / "m0.vx", ln, clase, v):
                        # Si el cambio no se puede aplicar, la fila saldria
                        # IDeNTICA a `sin cambios` y se leeria como que el
                        # compilador se lo salto.  Paso de verdad: la mutacion
                        # "cuerpo" buscaba un patron que no existia en estas
                        # familias y las tres filas eran la misma medida.
                        fallo_mutacion = True
                        break
                    t = una_medida(cmd, env, args.timeout, d)
                    if t >= 0:
                        serie.append(t)
                if fallo_mutacion:
                    print(f"  {C.RED}[sin medir]{C.RESET} {familia}/{ln} "
                          f"{nombre_caso}: no se pudo aplicar el cambio")
                    continue
                s_r = _stats_summary(serie) if serie else {}
                filas_fr.append((ln, "%-12s %-10s %s"
                                 % (familia, ln, nombre_caso), s_r))
                resultados["casos"].append({
                    "lang": ln, "familia": familia, "regimen": nombre_caso,
                    "cuenta": cuenta, "stats": s_r})
    if filas_fr:
        imprimir_tabla(
            "Familia x regimen: donde se va el tiempo y que se puede evitar (ms)",
            filas_fr, suelo,
            "Un numero por familia dice cuanto cuesta compilarla; el desglose "
            "dice donde se va.  `sin cambios` es coste de reutilizar, no "
            "velocidad.  La distancia entre cuerpo e interfaz es lo que la "
            "frontera esta cortando en ESA familia -- y no tiene por que ser "
            "igual en todas.")

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
