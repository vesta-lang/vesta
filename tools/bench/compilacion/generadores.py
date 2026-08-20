#!/usr/bin/env python3
"""Fuentes generadas: el mismo programa en cada lenguaje.

Se GENERAN en vez de tomarse del corpus porque compilar cuarenta lineas cuesta
lo mismo que compilar un fichero vacio -- entre el 94% y el 103% del tiempo es
arrancar el compilador --, y entonces no se estaria midiendo compilar.  Aqui el
tamano se pide.

Todas las versiones hacen LO MISMO: N funciones con la misma aritmetica y un
`main` que llama a una de cada ocho.  Que sea el mismo trabajo es lo unico que
permite comparar los tiempos entre lenguajes.
"""
from __future__ import annotations

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


def gen_nim(n: int) -> str:
    """El mismo programa en Nim.

    Interesa aqui porque su modelo es el opuesto al de casi todos los demas:
    genera C y se lo pasa a un compilador de C, asi que su tiempo lleva dentro
    otro compilador entero.  Es justo el tipo de decision que un banco de
    tiempos de compilacion tiene que poder enseñar.

    `discard` en las llamadas porque Nim no deja tirar un valor en silencio, y
    los enteros van a `int64` explicito para que la aritmetica sea la misma que
    en el resto de lenguajes y no la de la palabra de la maquina.
    """
    partes = ["import std/os", ""]
    for i in range(n):
        partes.append(
            "proc calc%d(x: int64): int64 =\n  let a = x * 3 + %d\n"
            "  let b = a xor 5\n  let c = (a + b) * 7\n  return a + b + c\n"
            % (i, i))
    partes.append("proc main() =\n  var s: int64 = 0")
    for i in range(0, n, 8):
        partes.append("  s += calc%d(%d)" % (i, i))
    partes.append("  quit(int(s mod 251))\n\nmain()")
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
    "nim": ("gen.nim", gen_nim),
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
    "nim": ("vacio.nim", "discard\n"),
    "python": ("vacio.py", "pass\n"),
    "vesta": ("vacio.vx", "i32 main() { return 0; }\n"),
    "vesta_aot": ("vacio.vx", "i32 main() { return 0; }\n"),
}




def funciones_para_lineas(gen, objetivo: int) -> int:
    """Cuantas funciones generar para acercarse a @p objetivo LINEAS.

    Existe porque el tamano hay que pedirlo en lineas, no en funciones.  Cada
    lenguaje escribe un numero distinto de lineas por funcion -- Java abre una
    clase, Go declara su paquete, Nim usa dos lineas donde C usa una --, asi
    que generar "800 funciones" para todos daba programas de tamanos DISTINTOS
    y la tabla comparaba 6k lineas de unos contra 5k de otros.  Eso no es una
    comparacion.

    Se mide con una muestra pequena y se escala.  No sale exacto -- hay un
    preambulo fijo y el `main` crece a saltos --, pero deja a todos dentro de
    un margen estrecho, y cada fila publica las lineas que de verdad compilo.
    """
    muestra = 64
    por_funcion = max(1.0, gen(muestra).count("\n") / muestra)
    return max(1, int(round(objetivo / por_funcion)))
