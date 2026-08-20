#!/usr/bin/env python3
"""Compilar dos veces el mismo fuente, .da lo mismo?

No es una medida de tiempo, y por eso vive aparte: es una propiedad del
compilador que se comprueba, no una cifra que se compara.  Pero pertenece a
este banco porque se responde con lo mismo que ya monta -- fuentes generadas y
una orden de compilacion por lenguaje -- y porque lo que descubre cuesta caro
descubrirlo de otra forma.

Un compilador reproducible permite cachear por contenido, distribuir binarios
que cualquiera puede verificar, y comparar dos construcciones para saber si un
cambio hizo algo.  Uno que no lo es rompe las tres cosas EN SILENCIO: el
programa funciona, los tests pasan, y solo se nota cuando alguien compara dos
artefactos que deberian ser iguales y no lo son.

Que se hace: compilar el mismo fuente dos veces en directorios distintos y
comparar los bytes.  Distintos a proposito, porque compilar encima puede
reutilizar lo de la vez anterior y entonces se estaria comprobando que un
fichero es igual a si mismo.

Si difieren, se dice CUANTO -- unos pocos bytes suele ser un sello de fecha o
una ruta embebida; muchos, que el codigo generado depende de algo que cambia --
y DoNDE empieza la primera diferencia, que es por donde se tira del hilo.
"""
from __future__ import annotations

import shutil

from ..comun import C, Spinner, color_de
from ..contexto import Ctx
from ..generadores import GENERADORES, funciones_para_lineas
from ..informe import barra, cabecera_fase
from ..medida import compila_de_verdad
from ..memoria import formatear_bytes
from ..ordenes import entorno_cache, orden_compilar


def _artefacto(salida):
    """El fichero que produjo la compilacion, se llame como se llame."""
    for c in (salida, salida.with_suffix(".exe"), salida.with_suffix(".velb")):
        if c.is_file():
            return c
    return None


def _comparar(a, b):
    """(iguales, bytes_distintos, primer_desvio) entre dos artefactos."""
    da, db = a.read_bytes(), b.read_bytes()
    if len(da) != len(db):
        # Tamanos distintos: no es un sello, es otro contenido.
        return (False, abs(len(da) - len(db)), min(len(da), len(db)))
    distintos = 0
    primero = -1
    for i, (x, y) in enumerate(zip(da, db)):
        if x != y:
            distintos += 1
            if primero < 0:
                primero = i
    return (distintos == 0, distintos, primero)


def fase(ctx: Ctx) -> None:
    """Compila dos veces cada fuente y compara los bytes del resultado."""
    args = ctx.args
    # Un solo tamano basta: esto no es una curva, es un si o un no.  Se coge el
    # mayor de los pedidos, que es donde mas oportunidades hay de que algo no
    # determinista se cuele.
    objetivo = max([int(x) for x in args.tamanos.split(",") if x.strip()]
                   or [1500])
    cabecera_fase(
        "reproducible", "Compilar dos veces da lo mismo?",
        "Se compila el MISMO fuente dos veces, en directorios distintos, y se "
        "comparan los bytes.  Un compilador que no es reproducible rompe el "
        "cacheado por contenido y la verificacion de binarios, y lo hace en "
        "silencio: el programa funciona igual.")

    cab = f"{'lenguaje':<14}{'artefacto':>14}{'resultado':>34}"
    print(f"{C.BOLD}{cab}{C.RESET}")
    print("-" * len(cab))
    filas: list = []
    for i, ln in enumerate(ctx.langs):
        entrada = GENERADORES.get(ln)
        if entrada is None:
            continue
        nombre, gen = entrada
        texto = gen(funciones_para_lineas(gen, objetivo))
        with Spinner("", color=C.DIM) as spin:
            spin.etiqueta(f"reproducible  {barra(i, len(ctx.langs))}  "
                          f"{C.BOLD}{ln}{C.RESET}")
            arts = []
            fallo = None
            for vuelta in (1, 2):
                d = ctx.base_tmp / ("repro_%s_%d" % (ln, vuelta))
                shutil.rmtree(d, ignore_errors=True)
                d.mkdir(parents=True, exist_ok=True)
                (d / nombre).write_text(texto, encoding="utf-8")
                cmd = orden_compilar(ln, d / nombre, d / "out", ctx.vm)
                if not cmd:
                    fallo = "sin orden de compilacion"
                    break
                env = entorno_cache(ln, ctx.dir_cache, ctx.entorno_base)
                ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                               args.timeout)
                if not ok:
                    fallo = motivo
                    break
                a = _artefacto(d / "out")
                if a is None:
                    fallo = "no encuentro el artefacto"
                    break
                arts.append(a)
        if fallo or len(arts) != 2:
            filas.append((ln, "-", f"{C.YELLOW}no se pudo comprobar{C.RESET}: "
                                   f"{fallo or 'faltan artefactos'}"))
            continue
        iguales, distintos, primero = _comparar(arts[0], arts[1])
        tam = formatear_bytes(arts[0].stat().st_size)
        if iguales:
            veredicto = f"{C.GREEN}identico{C.RESET}"
        else:
            # Pocos bytes suele ser un sello o una ruta; muchos, que el codigo
            # generado depende de algo que cambia entre corridas.
            pista = ("sello o ruta embebida" if distintos <= 64
                     else "el codigo generado cambia")
            veredicto = (f"{C.RED}difiere{C.RESET} en {distintos} bytes "
                         f"(desde {primero}): {pista}")
        filas.append((ln, tam, veredicto))
        ctx.resultados["casos"].append({
            "lang": ln, "fase": "reproducible", "lineas": texto.count("\n"),
            "reproducible": iguales, "bytes_distintos": distintos,
            "primer_desvio": primero})
    for ln, tam, v in filas:
        print(f"  {color_de(ln)}{ln:<12}{C.RESET}{tam:>14}   {v}")
    print("-" * len(cab))
    print(f"{C.DIM}  Se compila en directorios DISTINTOS a proposito: encima "
          f"del anterior, el compilador puede reutilizar lo que ya hizo y "
          f"entonces se estaria comprobando que un fichero es igual a si "
          f"mismo.{C.RESET}")
