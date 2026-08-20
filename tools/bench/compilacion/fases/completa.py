#!/usr/bin/env python3
"""El mismo programa entero, en frio y en caliente, por tamano.  La medida base."""
from __future__ import annotations

from ..comun import C, Spinner, una_medida
from ..contexto import Ctx
from ..generadores import GENERADORES, funciones_para_lineas
from ..informe import barra, cabecera_fase, imprimir_ganancia, imprimir_tabla
from ..medida import (medir_caliente, medir_frio, repeticiones,
                     verificar_en_paralelo)
from ..ordenes import entorno_cache, orden_compilar


def fase(ctx: Ctx) -> None:
    """El mismo programa entero, en frio y en caliente, por tamano.  La medida base."""
    args, vm = ctx.args, ctx.vm
    langs, tamanos, jobs = ctx.langs, ctx.tamanos, ctx.jobs
    base_tmp, dir_cache = ctx.base_tmp, ctx.dir_cache
    entorno_base = ctx.entorno_base
    suelo, resultados = ctx.suelo, ctx.resultados
    # --- 2. Compilacion completa, caliente y en frio, por tamano.
    cabecera_fase("2", "Compilacion completa, por tamano",
                  "El mismo programa en un solo fichero, en varios tamanos, "
                  "con las caches calientes y en frio.  Es la medida base: "
                  "cuanto cuesta compilar N lineas.")
    filas_cal: list[tuple] = []
    filas_frio: list[tuple] = []
    frio_por_lang: dict[str, dict] = {}
    cal_por_lang: dict[str, dict] = {}

    # Se preparan TODOS los casos y se verifican en paralelo; medir viene
    # despues y en fila de uno.  La generacion de fuentes tambien entra aqui
    # porque escribir cuarenta ficheros de cinco mil lineas no es gratis.
    preparados = []
    for n in tamanos:
        for ln in langs:
            nombre, gen = GENERADORES[ln]
            d = base_tmp / ("gen_%s_%d" % (ln, n))
            d.mkdir(parents=True, exist_ok=True)
            fuente = d / nombre
            # El tamano se pide en LINEAS: generar el mismo numero de
            # funciones para todos daba programas distintos y comparaba 6k
            # lineas de unos contra 5k de otros.
            texto = gen(funciones_para_lineas(gen, n))
            fuente.write_text(texto, encoding="utf-8")
            cmd = orden_compilar(ln, fuente, d / "out", vm)
            if not cmd:
                continue
            env = entorno_cache(ln, dir_cache, entorno_base)
            etiqueta = "%s  %dk lineas" % (ln, round(texto.count("\n") / 1000))
            preparados.append(((ln, n), ln, cmd, env, d, d / "out",
                               etiqueta, texto.count("\n")))
    with Spinner("verificando que todo compila (en paralelo)", color=C.DIM):
        veredicto = verificar_en_paralelo(
            [(c[0], c[1], c[2], c[3], c[4], c[5]) for c in preparados],
            jobs, args.timeout)

    with Spinner("", color=C.DIM) as spin:
        for hechos, (clave, ln, cmd, env, d, salida, etiqueta,
                     lineas) in enumerate(preparados):
            n = clave[1]
            spin.etiqueta(f"midiendo  {barra(hechos, len(preparados))}  "
                          f"{C.BOLD}{etiqueta}{C.RESET}")
            ok, motivo = veredicto.get(clave, (False, "no verificado"))
            if not ok:
                print(f"  {C.RED}[no compila]{C.RESET} {etiqueta}: {motivo}")
                resultados["casos"].append({
                    "lang": ln, "funciones": n, "lineas": lineas,
                    "error": motivo,
                })
                continue
            # Una sola medida de tanteo fija cuantas repeticiones merece la
            # pena: repetir cinco veces algo que tarda cinco segundos son
            # veinticinco segundos para afinar un numero que ya se conoce.
            tanteo = una_medida(cmd, env, args.timeout, d)
            reps = repeticiones(args, tanteo if tanteo > 0 else 0.0)

            spin.etiqueta(f"midiendo  {barra(hechos, len(preparados))}  "
                          f"{C.BOLD}{etiqueta}{C.RESET} {C.DIM}caliente"
                          f"{C.RESET}")
            s_cal = medir_caliente(cmd, env, d, reps, args.timeout)
            filas_cal.append((ln, etiqueta, s_cal))

            spin.etiqueta(f"midiendo  {barra(hechos, len(preparados))}  "
                          f"{C.BOLD}{etiqueta}{C.RESET} {C.DIM}en frio"
                          f"{C.RESET}")
            s_frio = medir_frio(cmd, env, d, reps, args.timeout, ln,
                                dir_cache)
            filas_frio.append((ln, etiqueta, s_frio))

            if n == tamanos[-1]:
                cal_por_lang[ln] = s_cal
                frio_por_lang[ln] = s_frio
            resultados["casos"].append({
                "lang": ln, "fase": "2", "funciones": n, "lineas": lineas,
                "caliente": s_cal, "frio": s_frio,
            })

    imprimir_tabla("Compilacion completa, con las caches CALIENTES (ms)",
                   filas_cal, suelo,
                   "`sin arranque` descuenta el suelo: es el tiempo que se va en "
                   "compilar de verdad.")
    imprimir_tabla("Compilacion completa EN FRIO (ms)", filas_frio, suelo,
                   "Sin ninguna cache.  Cada medida vacia la cache antes, asi "
                   "que no se calienta y son las mas ruidosas del modulo.")
    imprimir_ganancia(frio_por_lang, cal_por_lang, langs)

