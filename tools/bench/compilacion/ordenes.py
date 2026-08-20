#!/usr/bin/env python3
"""Ordenes de compilacion, y donde guarda su cache cada herramienta.

Lo segundo es tan importante como lo primero: si "en frio" se mide sin vaciar
la cache de quien la tenga, se compara el primer arranque de unos con el
enesimo de otros.  Cada herramienta la guarda en un sitio distinto, asi que
"frio" hay que definirlo una vez por herramienta o no significa nada.
"""
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path
from typing import Optional

from .comun import ELEGIDO

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
    if lang == "nim":
        # `-d:release` para que sea comparable con el `-O2` de los demas.
        # `--nimcache` dentro del directorio del caso: Nim guarda el C que
        # genera, y si esa cache viviera en el sitio de siempre, "en frio" no
        # seria frio -- que es exactamente el error que este modulo evita.
        # `--hints:off` porque su salida normal es muy verbosa y aqui no se
        # lee.
        return ["nim", "c", "-d:release", "--hints:off",
                "--nimcache:" + str(salida.parent / "nimcache"),
                "-o:" + str(salida), str(fuente)]
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
    if lang == "nim":
        # Nim guarda el C que genera, y de ahi para adelante no vuelve a
        # generarlo ni a compilarlo.  Sin borrarlo, "en frio" era caliente
        # desde la segunda medida: se vio en el banco -- 1484 lineas y 5900
        # daban el MISMO tiempo, que es imposible si de verdad estuviera
        # compilando las dos.
        shutil.rmtree(dir_trabajo / "nimcache", ignore_errors=True)
    if lang in ("vesta", "vesta_aot"):
        shutil.rmtree(dir_trabajo / ".cache", ignore_errors=True)
        shutil.rmtree(dir_trabajo / ".vx_cache", ignore_errors=True)
        for p in list(dir_trabajo.glob("*.vxi")) + list(dir_trabajo.glob("*.vxir")):
            try:
                p.unlink()
            except OSError:
                pass


