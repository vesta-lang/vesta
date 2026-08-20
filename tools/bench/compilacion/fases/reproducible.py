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


def _compilar_en(ctx, ln, nombre, texto, d, guardar_en):
    """Compila @p texto en @p d y copia el artefacto a @p guardar_en.

    Se copia porque despues hay que comparar artefactos de compilaciones que
    han usado el MISMO directorio: si se dejaran donde salen, la segunda
    pisaria a la primera y se compararia un fichero consigo mismo.

    @return "" si fue bien, o el motivo del fallo.
    """
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True, exist_ok=True)
    (d / nombre).write_text(texto, encoding="utf-8")
    cmd = orden_compilar(ln, d / nombre, d / "out", ctx.vm)
    if not cmd:
        return "sin orden de compilacion"
    env = entorno_cache(ln, ctx.dir_cache, ctx.entorno_base)
    ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                   ctx.args.timeout)
    if not ok:
        return motivo
    a = _artefacto(d / "out")
    if a is None:
        return "no encuentro el artefacto"
    shutil.copyfile(a, guardar_en)
    return ""


def _veredicto(a, b) -> tuple:
    """(texto SIN color, color, iguales, bytes_distintos).

    El texto va sin color y el color aparte porque las columnas se alinean por
    el ancho de la CADENA, y los codigos de escape ocupan caracteres que no se
    ven: colorear antes de rellenar descuadra la tabla tanto como caracteres
    invisibles tenga cada celda.
    """
    iguales, distintos, primero = _comparar(a, b)
    if iguales:
        return ("identico", C.GREEN, True, 0)
    pista = ("sello o ruta" if distintos <= 64 else "el codigo cambia")
    return ("difiere en %d B (desde %d): %s" % (distintos, primero, pista),
            C.RED, False, distintos)


def fase(ctx: Ctx) -> None:
    """Comprueba las DOS propiedades por separado."""
    args = ctx.args
    # Un solo tamano basta: esto no es una curva, es un si o un no.  Se coge el
    # mayor de los pedidos, que es donde mas oportunidades hay de que algo se
    # cuele.
    objetivo = max([int(x) for x in args.tamanos.split(",") if x.strip()]
                   or [1500])
    cabecera_fase(
        "reproducible", "Compilar dos veces da lo mismo?",
        "Dos propiedades DISTINTAS, y por eso se comprueban aparte: si dos "
        "compilaciones desde el mismo sitio dan lo mismo (determinismo), y si "
        "dan lo mismo desde sitios distintos (independencia de la ruta).  La "
        "primera sostiene el cacheado por contenido; la segunda, que dos "
        "maquinas compartan artefactos y que un binario publicado se pueda "
        "verificar.")

    cab = (f"{'lenguaje':<12}{'artefacto':>12}   {'misma ruta':<40}"
           f"{'otra ruta':<40}")
    print(f"{C.BOLD}{cab}{C.RESET}")
    print(f"{C.DIM}  {'':<10}{'':>12}   {'(determinismo)':<40}"
          f"{'(independencia de la ruta)':<40}{C.RESET}")
    print("-" * 100)
    filas: list = []
    for i, ln in enumerate(ctx.langs):
        entrada = GENERADORES.get(ln)
        if entrada is None:
            continue
        nombre, gen = entrada
        texto = gen(funciones_para_lineas(gen, objetivo))
        guardados = ctx.base_tmp / "repro_art"
        guardados.mkdir(parents=True, exist_ok=True)
        fallo = ""
        with Spinner("", color=C.DIM) as spin:
            spin.etiqueta(f"reproducible  {barra(i, len(ctx.langs))}  "
                          f"{C.BOLD}{ln}{C.RESET}")
            # Dos veces desde la MISMA ruta, y una tercera desde otra.  La
            # tercera es la unica que cambia de sitio, asi que lo que aparezca
            # solo en ella es dependencia de la ruta y nada mas.
            a1 = guardados / (ln + ".1")
            a2 = guardados / (ln + ".2")
            a3 = guardados / (ln + ".3")
            misma = ctx.base_tmp / ("repro_%s_A" % ln)
            otra = ctx.base_tmp / ("repro_otra_%s_B" % ln)
            for d, dest in ((misma, a1), (misma, a2), (otra, a3)):
                fallo = _compilar_en(ctx, ln, nombre, texto, d, dest)
                if fallo:
                    break
        if fallo:
            filas.append((ln, "-", ("no se pudo comprobar: " + fallo,
                                    C.YELLOW), ("", "")))
            continue
        t_det, c_det, det_ok, det_n = _veredicto(a1, a2)
        t_ruta, c_ruta, ruta_ok, ruta_n = _veredicto(a1, a3)
        filas.append((ln, formatear_bytes(a1.stat().st_size),
                      (t_det, c_det), (t_ruta, c_ruta)))
        ctx.resultados["casos"].append({
            "lang": ln, "fase": "reproducible", "lineas": texto.count("\n"),
            "determinista": det_ok, "bytes_no_deterministas": det_n,
            "independiente_de_ruta": ruta_ok, "bytes_por_la_ruta": ruta_n})
    for ln, tam, (t1, c1), (t2, c2) in filas:
        print(f"  {color_de(ln)}{ln:<10}{C.RESET}{tam:>12}   "
              f"{c1}{t1:<40}{C.RESET}{c2}{t2:<40}{C.RESET}")
    print("-" * 100)
    print(f"{C.DIM}  Las dos comprobaciones NO son la misma, y mezclarlas "
          f"acusa de un defecto que no existe: un artefacto que embebe la ruta "
          f"del fuente -- para el depurador -- difiere al compilar desde otro "
          f"sitio siendo perfectamente determinista.\n"
          f"  Quien falle solo en la segunda columna lo arregla mapeando "
          f"prefijos de ruta, como `--remap-path-prefix` de rustc o "
          f"`-ffile-prefix-map` de gcc.{C.RESET}")
