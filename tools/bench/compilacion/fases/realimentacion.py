#!/usr/bin/env python3
"""Solo analizar y diagnosticar: lo que tarda en decirte si el codigo esta bien."""
from __future__ import annotations

from ..comun import C, Spinner
from ..contexto import Ctx
from ..generadores import GENERADORES, funciones_para_lineas
from ..informe import barra, cabecera_fase, imprimir_tabla
from ..medida import medir_caliente
from ..ordenes import entorno_cache, orden_comprobar


def fase(ctx: Ctx) -> None:
    """Solo analizar y diagnosticar: lo que tarda en decirte si el codigo esta bien."""
    args, vm = ctx.args, ctx.vm
    langs, tamanos, jobs = ctx.langs, ctx.tamanos, ctx.jobs
    base_tmp, dir_cache = ctx.base_tmp, ctx.dir_cache
    entorno_base = ctx.entorno_base
    suelo, resultados = ctx.suelo, ctx.resultados
    # --- 3. Realimentacion: cuanto tarda en salir el diagnostico.
    cabecera_fase("diagnostico", "Cuanto tarda en decirte si el codigo esta bien",
                  "Solo analizar y diagnosticar, sin generar codigo ni "
                  "enlazar: lo que tarda en decirte si tu codigo esta bien, "
                  "que es lo que se espera mientras se edita.")
    filas_chk: list[tuple] = []
    with Spinner("", color=C.DIM) as spin:
        for i_chk, (n, ln) in enumerate(
                [(n, ln) for n in tamanos for ln in langs]):
            spin.etiqueta(
                f"realimentacion  "
                f"{barra(i_chk, len(tamanos) * len(langs))}  "
                f"{C.BOLD}{ln}{C.RESET}")
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
            {"lang": ln, "fase": "3", "etiqueta": et, "stats": s} for ln, et, s in filas_chk]

