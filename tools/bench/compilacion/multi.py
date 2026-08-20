#!/usr/bin/env python3
"""El mismo programa repartido en VARIOS ficheros.

Un fichero suelto no tiene nada que reconstruir incrementalmente, asi que el
numero que mas se paga -- tocar una cosa y recompilar -- solo se puede medir
sobre un proyecto de verdad.  Aqui se parte el mismo total en k modulos.
"""
from __future__ import annotations

from pathlib import Path
from typing import Optional


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


