#!/usr/bin/env python3
"""Compila todo el corpus y avisa de lo que NO termina bien.

Es la primera puerta al tocar el compilador, y va antes que comparar
diagnosticos o bytes.  Motivo, aprendido caro: un cambio en el analisis de
rangos hizo que `255_mutex.vx` cascara con violacion de segmento, y la
comparacion de diagnosticos lo enseño como "difieren dos ejemplos" -- un
sintoma confuso, al final de una vuelta de varios minutos.  Mirar solo el
CoDIGO DE SALIDA lo dice en el primer minuto y sin ambiguedad: `rc=139` no se
confunde con nada.

Lo que comprueba es deliberadamente poco: que el proceso termine con cero y
deje artefacto.  No mira que dice ni que emite -- para eso estan la suite e2e y
la comparacion de `.velb` --, porque una puerta que tarda es una puerta que se
salta.

Los ejemplos que deben fallar A PROPOSITO (los negativos del comprobador de
limites y del borrow checker) salen aparte y no cuentan como fallo: lo que
importa de ellos es que fallen COMPILANDO, no que se lleven el compilador por
delante.

Uso:
    python tools/corpus_compila.py cmake-build-release/vm.exe
    python tools/corpus_compila.py vm.exe --antes otra/vm.exe   # comparar dos
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def compilar(vm: Path, fuente: Path, salida: Path, timeout: float) -> int:
    """Codigo de salida de compilar @p fuente.  -1 si se paso del tiempo."""
    try:
        r = subprocess.run([str(vm), "--vesta", str(fuente), "-o", str(salida)],
                           capture_output=True, timeout=timeout)
        return r.returncode
    except subprocess.TimeoutExpired:
        return -1


def barrer(vm: Path, fuentes: list, timeout: float) -> dict:
    """{nombre: codigo} de compilar cada fuente."""
    out = {}
    with tempfile.TemporaryDirectory() as tmp:
        for i, f in enumerate(fuentes):
            print("\r  %d/%d  %-40.40s" % (i + 1, len(fuentes), f.name),
                  end="", file=sys.stderr, flush=True)
            out[f.name] = compilar(vm, f, Path(tmp) / "out", timeout)
    print("\r" + " " * 60 + "\r", end="", file=sys.stderr)
    return out


def clasificar(rc: int) -> str:
    """Que significa ese codigo, en palabras.

    Se distingue CAER de fallar: un compilador que rechaza un programa hace su
    trabajo; uno que se cae con una senal tiene un fallo dentro.
    """
    if rc == 0:
        return "ok"
    if rc == 1:
        return "rechaza el programa"
    if rc == -1:
        return "se paso del tiempo"
    if rc == 139 or rc == -11:
        return "VIOLACION DE SEGMENTO"
    if rc in (134, -6, 0xC0000409):
        # 0xC0000409 es el fallo-rapido de Windows, que es como sale un
        # `abort()`.  Un negativo que rechaza el programa abortando no es una
        # caida del compilador, asi que se nombra por lo que es.
        return "abortado (rechazo o panico)"
    if rc == 136 or rc == -8:
        return "excepcion aritmetica"
    return "codigo %d" % rc


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("vm", help="binario a comprobar")
    p.add_argument("--antes", default="",
                   help="otro binario con el que comparar: solo se listan los "
                        "que CAMBIAN de veredicto, que es lo que interesa al "
                        "tocar el compilador.")
    p.add_argument("--corpus", default="examples_codes_vx")
    p.add_argument("--timeout", type=float, default=120.0)
    args = p.parse_args()

    vm = Path(args.vm)
    if not vm.is_file():
        print("[error] no encuentro %s" % vm)
        return 2
    fuentes = sorted(Path(args.corpus).glob("*.vx"))
    if not fuentes:
        print("[error] no hay fuentes en %s" % args.corpus)
        return 2

    print("compilando %d ejemplos con %s" % (len(fuentes), vm))
    ahora = barrer(vm, fuentes, args.timeout)

    if args.antes:
        antes = barrer(Path(args.antes), fuentes, args.timeout)
        cambios = [(n, antes.get(n), ahora[n]) for n in ahora
                   if antes.get(n) != ahora[n]]
        if not cambios:
            print("[ok] ningun ejemplo cambia de veredicto (%d comprobados)"
                  % len(fuentes))
            return 0
        print("[CAMBIAN] %d ejemplos:" % len(cambios))
        for n, a, b in sorted(cambios):
            print("  %-42s %s -> %s" % (n, clasificar(a), clasificar(b)))
        return 1

    # Sin comparacion: lo que importa es que NADIE se caiga.  Rechazar un
    # programa es trabajo hecho; caerse es un fallo del compilador.
    caidas = {n: rc for n, rc in ahora.items()
              if rc not in (0, 1) or rc == -1}
    rechazos = sum(1 for rc in ahora.values() if rc == 1)
    print("  %d compilan, %d rechazados (esperable en los negativos), "
          "%d se CAEN" % (sum(1 for r in ahora.values() if r == 0),
                          rechazos, len(caidas)))
    for n, rc in sorted(caidas.items()):
        print("  %-42s %s" % (n, clasificar(rc)))
    return 1 if caidas else 0


if __name__ == "__main__":
    sys.exit(main())
