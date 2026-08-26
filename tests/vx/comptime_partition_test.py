#!/usr/bin/env python3
"""Que el conjunto comptime de un modulo sea EXACTAMENTE el que decimos.

    python3 tests/vx/comptime_partition_test.py <dir_build>

El conjunto comptime es lo que se compilara APARTE para alimentar a la maquina
de compilacion, en vez de compilar el proyecto entero (hoy: un artefacto
monolitico de cientos de KB para lo que son unas pocas funciones).  De el
depende que el artefacto lleve lo que hace falta.

Las dos formas de equivocarse son opuestas y las dos son caras:

  de menos  una declaracion comptime que no entra -> el artefacto se queda sin
            ella.  NO da error: da un artefacto incompleto, y el fallo aparece
            mucho despues y lejos de aqui.
  de mas    codigo normal que entra -> se compila de mas y la clave de cache se
            mueve cuando no debia, asi que el artefacto deja de reusarse.

Por eso se afirma el contenido EXACTO, no "contiene al menos".  Y hay un caso
`@Macro` obligatorio: si el recolector solo mirara la palabra `comptime`, los
macros se quedarian fuera y se romperia una feature entera sin que ningun test
de `inject` lo notara.

Ademas se comprueba lo que el recolector VE Y NO SE LLEVA: un conjunto vacio no
puede significar a la vez "aqui no hay comptime" y "aqui hay comptime que no
recojo".

Salida 0 si todo cuadra; != 0 con el detalle.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]

# El volcado imprime una linea por lista: "  <etiqueta>   (N): a b c".
LINEA = re.compile(r"^\s{2}([a-zA-Z@ ]+?)\s*\((\d+)[^)]*\):(.*)$")

# Como se llama cada lista en el volcado -> clave interna.
ETIQUETAS = {
    "comptime fns": "fns",
    "@Macro": "macros",
    "comptime const": "consts",
    "helper deps": "helpers",
    "visto y NO recogido": "no_recogido",
}


def recolectar(vm: str, fuente: Path, tmp: Path) -> dict[str, list[str]]:
    """El conjunto del modulo RAIZ, tal como lo cuenta el compilador."""
    entorno = dict(os.environ, VESTA_DUMP_COMPTIME_UNIT="1",
                   VX_CACHE_DIR=str(tmp / "cache"))
    r = subprocess.run(
        [vm, "--vx-emit-only", "--vesta", str(fuente), "-o", str(tmp / "out")],
        capture_output=True, env=entorno, timeout=600, cwd=str(RAIZ))
    texto = (r.stderr or b"").decode("utf-8", "replace")

    # El volcado sale por MODULO, y ademas se repite: mientras exista la doble
    # pasada, el mismo modulo se vuelca una vez por pasada.  Nos quedamos con
    # la ULTIMA aparicion del modulo que nos interesa -- la de la pasada que
    # produce el artefacto final.
    #
    # Dos formas segun el camino: el de PROYECTO antepone "modulo <nombre>"; el
    # de fichero suelto solo dice "resumen del conjunto" (no hay ambiguedad
    # posible, es el unico modulo).
    quiero = fuente.stem
    mio = None
    for b in texto.split("[comptime-unit]"):
        cuerpo = b.lstrip()
        if cuerpo.startswith("modulo " + quiero) or \
                cuerpo.startswith("resumen del conjunto"):
            mio = b
    out: dict[str, list[str]] = {k: [] for k in ETIQUETAS.values()}
    if mio is None:
        return out
    for linea in mio.splitlines():
        m = LINEA.match(linea)
        if not m:
            continue
        clave = ETIQUETAS.get(m.group(1).strip())
        if clave is None:
            continue
        out[clave] = m.group(3).split()
    return out


def sin_prefijo(nombres: list[str]) -> set[str]:
    """Los nombres viajan mangled por namespace (`t__p__f`); comparar el ultimo
    segmento hace el test legible sin atarlo al esquema de mangling."""
    return {n.rsplit("__", 1)[-1].rsplit(".", 1)[-1] for n in nombres}


CASOS = [
    (
        "funcion comptime libre",
        """namespace t.p;
public comptime string gen() { return "x"; }
public i64 normal(i64 x) { return x + 1; }
""",
        # Lo que TIENE que salir, exacto.
        {"fns": {"gen"}, "macros": set(), "no_recogido": set()},
    ),
    (
        "@Macro (si solo se mirara `comptime`, este caso se pierde)",
        """namespace t.q;
@Macro
comptime string saluda() { return "hola"; }
public i64 normal(i64 x) { return x + 1; }
""",
        {"fns": set(), "macros": {"saluda"}, "no_recogido": set()},
    ),
    (
        "negativo: un modulo sin nada comptime no aporta conjunto",
        """namespace t.r;
public i64 uno(i64 x) { return x + 1; }
public i64 dos(i64 x) { return uno(x) * 2; }
""",
        {"fns": set(), "macros": set(), "no_recogido": set()},
    ),
    (
        "constructor comptime de struct: se VE y NO se recoge",
        """namespace t.s;
public struct U {
    u64 lo;
    public comptime U(i64 n) { this.lo = 1; }
}
""",
        # No entra en el conjunto -- es un metodo, no una decl de nivel
        # superior -- pero se CUENTA.  Hoy funciona porque el artefacto es el
        # programa entero; con el artefacto separado se quedaria sin bytecode.
        {"fns": set(), "macros": set(), "no_recogido": {"U"}},
    ),
]


def main() -> int:
    if len(sys.argv) < 2:
        print("uso: comptime_partition_test.py <dir_build>")
        return 2
    vm = None
    for cand in ("vm.exe", "vm"):
        p = Path(sys.argv[1]) / cand
        if p.is_file():
            vm = str(p)
            break
    if vm is None:
        print("no encuentro vm en %s" % sys.argv[1])
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="vx_particion_"))
    fallos = 0
    hechas = 0
    try:
        for i, (nombre, fuente, esperado) in enumerate(CASOS):
            f = tmp / ("caso%d.vx" % i)
            f.write_text(fuente, encoding="utf-8")
            got = recolectar(vm, f, tmp)
            for clave, quiero in esperado.items():
                hechas += 1
                tengo = sin_prefijo(got.get(clave, []))
                if tengo != quiero:
                    fallos += 1
                    print("  FALLA  %s" % nombre)
                    print("         %-12s esperado=%s  obtenido=%s" % (
                        clave, sorted(quiero) or "{}", sorted(tengo) or "{}"))
            if all(sin_prefijo(got.get(k, [])) == v
                   for k, v in esperado.items()):
                print("  ok     %s" % nombre)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n=== particion comptime: %d comprobaciones, %d fallidas ===" % (
        hechas, fallos))
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
