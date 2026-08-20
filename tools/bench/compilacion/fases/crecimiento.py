#!/usr/bin/env python3
"""Como crece el tiempo de compilar con el numero de lineas, con y sin cache.

Es la pregunta que un solo tamano no puede responder.  Un lenguaje que tarda el
doble que otro en mil lineas puede tardar la mitad en cien mil si crece mejor,
y eso no se ve mirando un numero suelto: hay que medir varios tamanos y mirar
la FORMA de la curva.

Se mide en los dos regimenes a proposito, porque no tienen por que crecer
igual.  En frio se paga todo; en caliente se paga lo que la cache no pudo
reutilizar, y hay herramientas cuya cache aguanta bien al crecer el programa y
otras cuyo ahorro se diluye.  Publicar solo uno de los dos deja fuera la mitad
del comportamiento.

El exponente se ajusta por minimos cuadrados sobre los logaritmos.  Si el
tiempo sigue t = a * n^k, entonces log t = log a + k * log n es una recta y k
es su pendiente.  Se lee asi:

    k ~ 1.0   lineal: el doble de codigo, el doble de tiempo.
    k ~ 1.5   superlineal: el doble de codigo, casi el triple.
    k ~ 2.0   cuadratico: el doble de codigo, cuatro veces el tiempo.

Un k por encima de 1 no es necesariamente un fallo -- hay analisis que son
superlineales por definicion --, pero SI es lo que decide si un proyecto grande
va a ser usable, y es justo lo que se descubre tarde y caro cuando nadie lo
mide.
"""
from __future__ import annotations

import math

from ..comun import C, Spinner, color_de
from ..contexto import Ctx
from ..generadores import GENERADORES, funciones_para_lineas
from ..informe import BASE, barra, cabecera_fase
from ..medida import compila_de_verdad, medir_caliente, medir_frio
from ..memoria import formatear as formatear_mem
from ..ordenes import entorno_cache, orden_compilar


def exponente(puntos: list):
    """Pendiente de log(tiempo) contra log(lineas): el k de t = a * n^k.

    Devuelve None cuando NO se puede ajustar -- menos de dos puntos utiles --,
    que no es lo mismo que ajustar y que salga cero.  Confundir las dos cosas
    ya enseño un `sin datos` sobre una curva que si se habia medido y lo que
    pasaba era que salia plana, que es justo el sintoma de que la cache no se
    estaba vaciando.
    """
    utiles = [(x, y) for x, y in puntos if x > 0 and y > 0]
    if len(utiles) < 2:
        return None
    xs = [math.log(x) for x, _ in utiles]
    ys = [math.log(y) for _, y in utiles]
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    return num / den if den > 0 else None


def lectura(k) -> str:
    """Que significa ese exponente, en una palabra."""
    if k is None:
        return "sin datos"
    if k < 0.5:
        # Cuatro veces mas codigo en el mismo tiempo no existe.  Lo que
        # existe es una cache que no se vacio, o un compilador que se salto
        # el trabajo: en los dos casos el numero no vale y hay que decirlo.
        return "plano (sospechoso)"
    if k < 1.15:
        return "lineal"
    if k < 1.6:
        return "algo superlineal"
    if k < 2.2:
        return "muy superlineal"
    return "cuadratico o peor"


def fase(ctx: Ctx) -> None:
    """Curva de tiempo contra lineas, en frio y en caliente, con su exponente."""
    args = ctx.args
    objetivos = [int(x) for x in args.lineas.split(",") if x.strip()]
    cabecera_fase(
        "crecimiento",
        "Crecimiento contra el tamano del codigo",
        "El mismo programa en varios tamanos, medido en frio y en caliente. "
        "Al final, el exponente de cada curva: cuanto se multiplica el tiempo "
        "al multiplicarse las lineas.  Un solo tamano no puede decir esto.")

    cab = (f"{'lenguaje':<12}{'lineas':>9}{'frio':>10}{'caliente':>10}"
           f"{'ahorro':>9}{'frio/kloc':>12}{'cal/kloc':>11}"
           f"{'mem frio':>11}{'mem cal':>11}{'vs ' + BASE:>11}")
    print(f"{C.BOLD}{cab}{C.RESET}")
    print(f"{C.DIM}  {'':<10}{'(codigo)':>9}{'(ms)':>10}{'(ms)':>10}"
          f"{'(veces)':>9}{'(ms)':>12}{'(ms)':>11}{'(pico)':>11}"
          f"{'(pico)':>11}{'(en frio)':>11}{C.RESET}")
    print("-" * len(cab))
    # La base se mide primero para que el resto pueda compararse con ella en la
    # misma pasada: publicar la razon al final, en otra tabla, obliga a buscar
    # la fila correspondiente a mano.
    orden = ([BASE] + [l for l in ctx.langs if l != BASE]
             if BASE in ctx.langs else list(ctx.langs))
    base_frio: dict = {}

    curvas: dict = {}
    total = max(1, len(ctx.langs) * len(objetivos))
    hechos = 0
    with Spinner("", color=C.DIM) as spin:
        for ln in orden:
            entrada = GENERADORES.get(ln)
            if entrada is None:
                continue
            nombre, gen = entrada
            curvas[ln] = {"frio": [], "caliente": [],
                          "mem_frio": [], "mem_cal": []}
            for objetivo in objetivos:
                spin.etiqueta(f"crecimiento  {barra(hechos, total)}  "
                              f"{C.BOLD}{ln}{C.RESET} {objetivo} lineas")
                hechos += 1
                nf = funciones_para_lineas(gen, objetivo)
                d = ctx.base_tmp / ("crec_%s_%d" % (ln, objetivo))
                d.mkdir(parents=True, exist_ok=True)
                texto = gen(nf)
                (d / nombre).write_text(texto, encoding="utf-8")
                lineas = texto.count("\n")
                cmd = orden_compilar(ln, d / nombre, d / "out", ctx.vm)
                if not cmd:
                    continue
                env = entorno_cache(ln, ctx.dir_cache, ctx.entorno_base)
                ok, motivo = compila_de_verdad(ln, cmd, env, d, d / "out",
                                               args.timeout)
                if not ok:
                    print(f"  {C.RED}[no compila]{C.RESET} {ln} "
                          f"{lineas} lineas: {motivo}")
                    break
                repes = max(3, args.repes // 2)
                s_cal = medir_caliente(cmd, env, d, repes, args.timeout)
                s_frio = medir_frio(cmd, env, d, repes, args.timeout, ln,
                                    ctx.dir_cache)
                if not s_cal or not s_frio:
                    continue
                # El suelo se descuenta porque arrancar el compilador no crece
                # con el programa: dejarlo dentro aplana la curva y hace pasar
                # por lineal algo que no lo es.  Y CADA regimen descuenta el
                # suyo: arrancar en frio no cuesta lo mismo que en caliente, y
                # restar el que no toca dejaba dentro un resto fijo que hundia
                # el exponente por debajo de 1 -- o sea, "el doble de codigo
                # cuesta menos del doble", que no puede ser.
                piso_cal = (ctx.suelo.get(ln) or {}).get("p50") or 0.0
                piso_frio = ((ctx.suelo_frio.get(ln) or {}).get("p50")
                             or piso_cal)
                n_frio = max(0.001, s_frio["p50"] - piso_frio)
                n_cal = max(0.001, s_cal["p50"] - piso_cal)
                curvas[ln]["frio"].append((lineas, n_frio))
                curvas[ln]["caliente"].append((lineas, n_cal))
                # La memoria NO se le resta el suelo: no es un coste de
                # arranque que se acumule, es un pico -- lo que el proceso
                # llego a tener a la vez --, y restarle el del fichero vacio
                # daria negativos sin significado.
                if s_frio.get("mem_kib"):
                    curvas[ln]["mem_frio"].append((lineas, s_frio["mem_kib"]))
                if s_cal.get("mem_kib"):
                    curvas[ln]["mem_cal"].append((lineas, s_cal["mem_kib"]))
                ahorro = n_frio / n_cal if n_cal > 0 else 0.0
                if ln == BASE:
                    base_frio[objetivo] = n_frio
                b = base_frio.get(objetivo)
                vs = (("x%.3f" % (n_frio / b)) if b else "-")
                print(f"  {color_de(ln)}{ln:<10}{C.RESET}{lineas:>9}"
                      f"{s_frio['p50']:>10.0f}{s_cal['p50']:>10.0f}"
                      f"{ahorro:>8.2f}x"
                      f"{1000.0 * n_frio / max(1, lineas):>12.1f}"
                      f"{1000.0 * n_cal / max(1, lineas):>11.1f}"
                      f"{formatear_mem(s_frio.get('mem_kib')):>11}"
                      f"{formatear_mem(s_cal.get('mem_kib')):>11}"
                      f"{vs:>11}")
                ctx.resultados["casos"].append({
                    "lang": ln, "fase": "crecimiento", "lineas": lineas,
                    "frio": s_frio, "caliente": s_cal,
                    "neto_frio": n_frio, "neto_caliente": n_cal,
                    "mem_frio_kib": s_frio.get("mem_kib"),
                    "mem_cal_kib": s_cal.get("mem_kib")})
    print("-" * len(cab))
    print(f"{C.DIM}  ahorro = cuantas veces mas rapido va con la cache "
          f"caliente.  /kloc = tiempo por cada mil lineas, ya sin el arranque: "
          f"si sube con el tamano, la curva no es lineal.{C.RESET}")

    # --- El exponente, que es lo que la tabla de arriba no deja ver de un
    # vistazo: dos lenguajes pueden tener el mismo tiempo en el tamano medido y
    # separarse del todo en el siguiente.
    print()
    print(f"{C.BOLD}Como crece cada uno (exponente de t = a * n^k){C.RESET}")
    cab2 = (f"{'lenguaje':<12}{'k en frio':>12}{'k caliente':>12}"
            f"{'k memoria':>12}{'lectura (en frio)':>22}")
    print(f"{C.BOLD}{cab2}{C.RESET}")
    print("-" * len(cab2))
    resumen = []
    for ln, c in curvas.items():
        k_frio = exponente(c["frio"])
        k_cal = exponente(c["caliente"])
        # La memoria tambien crece, y NO tiene por que hacerlo como el tiempo:
        # un compilador puede tardar el doble y pedir cuatro veces mas, que es
        # justo el caso que impide compilar un proyecto grande aunque haya
        # paciencia de sobra.
        k_mem = exponente(c["mem_frio"])
        if k_frio is None and k_cal is None:
            continue
        f_txt = f"{k_frio:>12.2f}" if k_frio is not None else f"{'-':>12}"
        c_txt = f"{k_cal:>12.2f}" if k_cal is not None else f"{'-':>12}"
        m_txt = f"{k_mem:>12.2f}" if k_mem is not None else f"{'-':>12}"
        print(f"  {color_de(ln)}{ln:<10}{C.RESET}{f_txt}{c_txt}{m_txt}"
              f"{lectura(k_frio):>22}")
        resumen.append({"lang": ln, "k_frio": k_frio, "k_caliente": k_cal,
                        "k_memoria": k_mem})
    print("-" * len(cab2))
    print(f"{C.DIM}  k ~ 1 lineal (el doble de codigo, el doble de tiempo); "
          f"k ~ 2 cuadratico (el doble de codigo, cuatro veces el tiempo).\n"
          f"  k memoria = lo mismo para el PICO de memoria en frio: un k de "
          f"1 quiere decir que el doble de codigo pide el doble de memoria.\n"
          f"  Hacen falta al menos dos tamanos para ajustar una recta; con uno "
          f"solo sale `-`.{C.RESET}")
    ctx.resultados["crecimiento"] = resumen
