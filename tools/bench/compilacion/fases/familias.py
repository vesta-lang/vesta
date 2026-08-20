#!/usr/bin/env python3
"""Que codigo, no cuanto: cada familia pega en una parte distinta del compilador."""
from __future__ import annotations

from ..comun import C, Spinner, una_medida
from ..contexto import Ctx
from ..familias import FAMILIAS
from ..informe import barra, cabecera_fase, imprimir_tabla
from ..medida import medir_caliente, repeticiones, verificar_en_paralelo
from ..ordenes import entorno_cache, orden_compilar


def fase(ctx: Ctx) -> None:
    """Que codigo, no cuanto: cada familia pega en una parte distinta del compilador."""
    args, vm = ctx.args, ctx.vm
    langs, tamanos, jobs = ctx.langs, ctx.tamanos, ctx.jobs
    base_tmp, dir_cache = ctx.base_tmp, ctx.dir_cache
    entorno_base = ctx.entorno_base
    suelo, resultados = ctx.suelo, ctx.resultados
    # --- 2d. QUE codigo, no cuanto.  Cada familia pega en una parte distinta
    # del compilador, y son justo las que el generador anodino no toca.
    cabecera_fase("2d", "Familias de codigo",
                  "QUE codigo, no cuanto: genericos, comptime, anidamiento y "
                  "tipos.  Cada una pega en una parte distinta del "
                  "compilador, y son justo las que el generador anodino de "
                  "las fases anteriores no toca.")
    filas_fam: list[tuple] = []
    prep_fam = []
    for familia, porlang in FAMILIAS.items():
        # Cuentas distintas por familia: mil niveles de anidamiento no es lo
        # mismo que mil instanciaciones de una plantilla, y forzar el mismo
        # numero solo conseguiria que unas tarden segundos y otras nada.
        cuenta = {"genericos": 150, "comptime": 60,
                  "anidamiento": 500, "tipos": 400}[familia]
        for ln in langs:
            par = porlang.get(ln)
            if par is None:
                continue
            nombre, gen = par
            d = base_tmp / ("fam_%s_%s" % (familia, ln))
            d.mkdir(parents=True, exist_ok=True)
            texto = gen(cuenta)
            (d / nombre).write_text(texto, encoding="utf-8")
            cmd = orden_compilar(ln, d / nombre, d / "out", vm)
            if not cmd:
                continue
            env = entorno_cache(ln, dir_cache, entorno_base)
            prep_fam.append(((familia, ln), ln, cmd, env, d, d / "out",
                             familia, cuenta, texto.count("\n")))
    with Spinner("verificando las familias de codigo", color=C.DIM):
        vered_fam = verificar_en_paralelo(
            [(c[0], c[1], c[2], c[3], c[4], c[5]) for c in prep_fam],
            jobs, args.timeout)

    with Spinner("", color=C.DIM) as spin:
      for hechos, (clave, ln, cmd, env, d, salida, familia, cuenta,
                   n_lineas) in enumerate(prep_fam):
            etiqueta = "%-12s %s" % (familia, ln)
            spin.etiqueta(f"familias  {barra(hechos, len(prep_fam))}  "
                          f"{C.BOLD}{familia} / {ln}{C.RESET}")
            ok, motivo = vered_fam.get(clave, (False, "no verificado"))
            if not ok:
                print(f"  {C.RED}[no compila]{C.RESET} {etiqueta}: {motivo}")
                resultados["casos"].append({
                    "lang": ln, "familia": familia, "error": motivo})
                continue
            tanteo = una_medida(cmd, env, args.timeout, d)
            s = medir_caliente(cmd, env, d,
                               repeticiones(args, tanteo if tanteo > 0 else 0.0),
                               args.timeout)
            filas_fam.append((ln, etiqueta, s))
            resultados["casos"].append({
                "lang": ln, "familia": familia, "cuenta": cuenta,
                "lineas": n_lineas, "stats": s})
    if filas_fam:
        imprimir_tabla(
            "Por FAMILIA de codigo (ms)", filas_fam, suelo,
            "No es lo mismo mucho codigo que codigo dificil.  Cada familia pega "
            "en una parte distinta: genericos multiplica lo que hay que "
            "generar, comptime EJECUTA al compilar, anidamiento pone a prueba "
            "la recursion del analizador y tipos estresa la tabla de simbolos.  "
            "Lo que un lenguaje no tiene no aparece, en vez de sustituirse por "
            "algo parecido.")

