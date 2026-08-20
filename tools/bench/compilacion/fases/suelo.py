#!/usr/bin/env python3
"""Lo que tarda cada compilador en no hacer nada.  Se descuenta de todo lo demas."""
from __future__ import annotations

from ..comun import C, Spinner
from ..contexto import Ctx
from ..generadores import VACIAS
from ..informe import barra, cabecera_fase, imprimir_tabla
from ..medida import medir_caliente, medir_frio
from ..ordenes import entorno_cache, orden_compilar


def fase(ctx: Ctx) -> None:
    """Lo que tarda cada compilador en no hacer nada.  Se descuenta de todo lo demas."""
    args, vm = ctx.args, ctx.vm
    langs, tamanos, jobs = ctx.langs, ctx.tamanos, ctx.jobs
    base_tmp, dir_cache = ctx.base_tmp, ctx.dir_cache
    entorno_base = ctx.entorno_base
    suelo, resultados = ctx.suelo, ctx.resultados
    # --- 1. Suelo de cada herramienta: compilar un fichero que no declara nada.
    cabecera_fase("arranque", "Arrancar el compilador y no hacer nada",
                  "Compila un fichero vacio en cada lenguaje: arrancar el "
                  "proceso y no hacer nada.  Se descuenta despues de todo lo "
                  "demas.")
    with Spinner("", color=C.DIM) as spin:
        for i, ln in enumerate(langs):
            spin.etiqueta(f"suelo  {barra(i, len(langs))}  {C.BOLD}{ln}{C.RESET}")
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
            # El suelo EN FRIO es otro numero, y hace falta.  Restar el
            # caliente a una medida en frio deja dentro la parte del arranque
            # que solo se paga sin cache; como ese resto es fijo, pesa mucho en
            # los programas pequenos y poco en los grandes, y eso APLANA la
            # curva de crecimiento -- llegaba a dar exponentes por debajo de
            # 1, que se leerian como "compilar el doble de codigo cuesta menos
            # del doble", que no puede ser.
            sf = medir_frio(cmd, env, d, max(3, args.repes // 2), args.timeout,
                            ln, dir_cache)
            if sf:
                ctx.suelo_frio[ln] = sf
    imprimir_tabla(
        "Suelo del compilador (ms): lo que tarda en compilar un fichero vacio",
        [(ln, ln, suelo.get(ln, {})) for ln in langs], {},
        "Esta DENTRO de cada medida de abajo.  Compilar uno de los benchmarks "
        "del corpus (40 lineas) cuesta lo mismo que esto: por eso aqui las "
        "fuentes se generan con tamano.")

