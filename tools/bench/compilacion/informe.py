#!/usr/bin/env python3
"""Como se ensena el resultado.

Una tabla que hay que ir a buscar a la documentacion se lee mal, asi que las
columnas llevan su unidad y debajo una linea que dice que significa cada una.
Y la anchura de la primera columna sale de los DATOS: con un ancho fijo, una
etiqueta larga no se recorta -- se sale y empuja los numeros de esa fila --, y
la tabla queda escalonada justo cuando mas filas tiene.
"""
from __future__ import annotations

import re

from .comun import C, color_de
from .memoria import formatear as formatear_mem

# Contra quien se compara.  Igual que en el banco de ejecucion: un numero suelto
# no dice si 400 ms es mucho, y una columna de razones responde de un vistazo.
BASE = "vesta"


def _clave_de_caso(lang: str, etiqueta: str) -> str:
    """El caso que describe @p etiqueta, sin el nombre del lenguaje.

    Sirve para emparejar la fila de cada lenguaje con la de la base.  Se quita
    el nombre como PALABRA y no como texto: con `replace` a secas, el lenguaje
    `c` se comia la `c` de `cadena` y dos casos distintos acababan con la misma
    clave.  El nombre no siempre va delante -- hay tablas donde la etiqueta es
    `cadena  vesta  cambia la interfaz` --, por eso se busca donde este.
    """
    sin = re.sub(r"\b%s\b" % re.escape(lang), "", etiqueta, count=1)
    return " ".join(sin.split())


def _razon(s: dict, base: dict) -> str:
    """La fila comparada con la de la base, en veces."""
    if not s or not base:
        return f"{'-':>10}"
    a = s.get("p50") or 0.0
    b = base.get("p50") or 0.0
    if a <= 0 or b <= 0:
        return f"{'-':>10}"
    r = a / b
    return f"{_color_razon(r)}{_texto_razon(r):>10}{C.RESET}"


def _color_razon(r: float) -> str:
    """Escala de cuatro colores para la razon contra la base.

    Con solo dos -- verde o rojo -- todo lo que pasaba de 2x se veia igual, y
    `x2.3` acababa pintado como `x28`.  Justo donde mas interesa distinguir:
    tardar el doble es discutible, tardar veinte veces es otra categoria.

        verde     tarda MENOS que la base
        amarillo  hasta el doble
        naranja   hasta cinco veces
        rojo      mas de cinco veces
    """
    if r < 0.995:
        return C.GREEN
    if r <= 2.0:
        return C.YELLOW
    if r <= 5.0:
        return C.ORANGE
    return C.RED


def _texto_razon(r: float) -> str:
    """La razon con los decimales que aporten algo y no mas.

    Tres decimales sirven para leer `x0.240`, pero en `x28.955` sobran: el
    tercero es ruido de medida y solo alarga la columna.
    """
    if r < 10.0:
        return "x%.3f" % r
    if r < 100.0:
        return "x%.1f" % r
    return "x%.0f" % r

def _en_bloques(filas: list) -> list:
    """Parte las filas en los GRUPOS que de verdad son.

    Varias fases recorren dos bucles -- por tamano y por lenguaje -- y vuelcan
    todo a una sola tabla.  Lo que sale son dos tablas pegadas: `c` aparece dos
    veces con la misma etiqueta, una por tamano.  Verlo ya es confuso; lo grave
    es que la comparacion contra la base empareja por etiqueta, asi que las
    filas de un grupo se dividian entre la referencia del OTRO y las razones
    publicadas no significaban nada.

    Aqui no se pide a cada fase que ponga etiquetas distintas -- eso hay que
    acordarse de hacerlo cada vez que se anade una, y no se hizo en varias --,
    sino que se DETECTA: los grupos salen en orden, asi que en cuanto un caso
    se repite es que empezo el siguiente.  Cada bloque calcula despues su
    propia base.
    """
    bloques: list = []
    actual: list = []
    vistos: set = set()
    for lang, etiqueta, s in filas:
        clave = (lang, _clave_de_caso(lang, etiqueta))
        if clave in vistos:
            bloques.append(actual)
            actual, vistos = [], set()
        vistos.add(clave)
        actual.append((lang, etiqueta, s))
    if actual:
        bloques.append(actual)
    return bloques


def _color_ruido(mad_pct: float) -> str:
    if mad_pct < 2.0:
        return C.GREEN
    if mad_pct < 5.0:
        return C.YELLOW
    return C.RED


def cabecera_fase(num: str, titulo: str, explicacion: str) -> None:
    """Anuncia una fase antes de empezarla, diciendo QUE mide y por que.

    La tabla de resultados ya lo explica, pero llega al final: mientras la
    fase corre -- y algunas tardan minutos -- lo unico visible era un cursor
    parado.  Saber que se esta midiendo en ese momento tambien permite parar
    a tiempo cuando lo que corre no es lo que se queria medir.
    """
    print()
    print(f"{C.BOLD}{C.CYAN}== {num} {titulo}{C.RESET}")
    print(f"{C.DIM}   {explicacion}{C.RESET}")


def barra(hecho: int, total: int, ancho: int = 16) -> str:
    """Barra de progreso en texto: `[####------] 4/10`.

    Va dentro del rotulo del spinner, que ya se reescribe solo cada 80 ms.
    Asi la ventana deja de estar quieta y ademas se sabe cuanto queda, que en
    una tanda de varios minutos es la diferencia entre esperar y no saber si
    se ha colgado.
    """
    if total <= 0:
        return ""
    hecho = max(0, min(hecho, total))
    llenos = int(ancho * hecho / total)
    return (f"{C.GREEN}[{'#' * llenos}{C.DIM}{'-' * (ancho - llenos)}"
            f"{C.RESET}{C.GREEN}]{C.RESET} {hecho}/{total}")


def imprimir_tabla(titulo: str, filas: list[tuple], suelo: dict,
                   nota: str = "") -> None:
    """Una fila por (lenguaje, tamano) con estimacion, dispersion y neto.

    Las columnas van con su UNIDAD en la cabecera y con nombres que se
    entienden sin saber estadistica: `p50` no le dice nada a nadie, y un
    numero suelto no deja claro si son lineas o milisegundos.  Debajo se
    recuerda en una linea que significa cada una, porque una tabla que hay
    que ir a buscar a la documentacion se lee mal.
    """
    print()
    print(f"{C.BOLD}{titulo}{C.RESET}")
    if nota:
        print(f"{C.DIM}  {nota}{C.RESET}")
    # La anchura de la primera columna sale de los DATOS, no de un numero
    # fijo.  Con un ancho fijo, una etiqueta mas larga no se recorta: se sale,
    # y empuja los numeros de ESA fila a la derecha.  Como cada etiqueta se
    # pasa por una cantidad distinta, la tabla queda escalonada y no se puede
    # comparar una columna de un vistazo, que es para lo unico que sirve una
    # tabla.  Las fases con etiquetas compuestas (familia + variante + caso)
    # la rompian entera.
    ancho_et = max(24, max((len(e) for _, e, _ in filas), default=24))
    bloques = _en_bloques(filas)
    cab = (f"{'lenguaje / caso':<{ancho_et + 2}}{'tiempo':>11}{'+-':>9}"
           f"{'ruido':>8}{'mas rapido':>12}{'mas lento':>12}"
           f"{'sin arranque':>14}{'memoria':>11}{'vs ' + BASE:>10}")
    print(f"{C.BOLD}{cab}{C.RESET}")
    print(f"{C.DIM}  {'':<{ancho_et}}{'(ms)':>11}{'(ms)':>9}{'':>8}{'(ms)':>12}"
          f"{'(ms)':>12}{'(ms)':>14}{'(pico)':>11}{'(veces)':>10}"
          f"{C.RESET}")
    print("-" * len(cab))
    for i, bloque in enumerate(bloques):
        if i:
            # Se ve que empieza otro grupo.  Sin la raya parecia una tabla
            # sola, y ahi es donde se colaba el error: quien la leia comparaba
            # entre si filas que no eran comparables.
            print(f"{C.DIM}{'- ' * (len(cab) // 2)}{C.RESET}")
        base_de = {_clave_de_caso(l, e): s
                   for l, e, s in bloque if l == BASE and s}
        for lang, etiqueta, s in bloque:
            col = color_de(lang)
            if not s:
                print(f"  {col}{etiqueta:<{ancho_et}}{C.RESET}"
                      f"{C.GREY}{'sin dato':>11}{C.RESET}")
                continue
            piso = (suelo.get(lang) or {}).get("p50")
            if piso is None:
                neto = f"{'-':>14}"
            elif s["p50"] - piso <= 0:
                neto = f"{C.DIM}{'~0':>14}{C.RESET}"
            else:
                neto = f"{s['p50'] - piso:>14.0f}"
            razon = _razon(s, base_de.get(_clave_de_caso(lang, etiqueta)))
            print(f"  {col}{etiqueta:<{ancho_et}}{C.RESET}{s['p50']:>11.0f}"
                  f"{s['mad']:>9.1f}"
                  f"{_color_ruido(s['mad_pct'])}{s['mad_pct']:>7.1f}%{C.RESET}"
                  f"{s['min']:>12.0f}{s['max']:>12.0f}{neto}"
                  f"{formatear_mem(s.get('mem_kib')):>11}{razon}")
    print("-" * len(cab))
    print(f"{C.DIM}  tiempo = valor tipico (mediana de las medidas).  "
          f"+- = cuanto se desvia una medida corriente.  "
          f"ruido = ese desvio en porcentaje.\n"
          f"  sin arranque = el tiempo descontando lo que cuesta arrancar el "
          f"compilador (la fila 'Suelo' de arriba).\n"
          f"  memoria = pico del ARBOL de procesos (el compilador y los que "
          f"lance).  Un guion = no se pudo medir, que no es cero.\n"
          f"  vs {BASE} = cuantas veces tarda comparado con {BASE} EN EL MISMO "
          f"CASO.  x2.000 = tarda el doble; x0.240 = tarda menos de la cuarta "
          f"parte.{C.RESET}")


def imprimir_ganancia(frio: dict, caliente: dict, langs: list[str]) -> None:
    """Lo que aporta la cache de cada uno: frio dividido por caliente.

    Es la pregunta que motiva el modulo.  Un `1.0x` no significa que la cache
    sea mala: significa que ese lenguaje no tiene nada que cachear en este
    escenario -- compilar un fichero suelto -- y el numero solo empieza a
    decir algo con un proyecto de varios modulos.
    """
    print()
    print(f"{C.BOLD}Lo que aporta la cache (frio / caliente){C.RESET}")
    print(f"{C.DIM}  Mas alto = la cache ahorra mas.  1.0x = no hay nada que "
          f"cachear en este escenario.{C.RESET}")
    pares = []
    for ln in langs:
        f = frio.get(ln)
        c = caliente.get(ln)
        if not f or not c or c.get("p50", 0) <= 0:
            continue
        pares.append((f["p50"] / c["p50"], ln, f["p50"], c["p50"]))
    for g, ln, f, c in sorted(pares, reverse=True):
        col = C.GREEN if g >= 2.0 else (C.YELLOW if g >= 1.2 else C.DIM)
        print(f"  {color_de(ln)}{ln:<12}{C.RESET}{col}{g:>7.2f}x{C.RESET}"
              f"   frio {f:>8.0f} ms  ->  caliente {c:>8.0f} ms")


# ===========================================================================
# Programa
# ===========================================================================
