#!/usr/bin/env python3
"""Familias de codigo: QUE se compila, no cuanto.

Mil lineas de genericos y mil de aritmetica plana no cuestan lo mismo, y no por
el tamano: pegan en partes distintas del compilador.  Estas familias existen
para que el banco pueda decir DoNDE se va el tiempo, no solo cuanto.
"""
from __future__ import annotations

from pathlib import Path
from typing import Optional

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


