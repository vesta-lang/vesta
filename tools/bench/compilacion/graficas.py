#!/usr/bin/env python3
"""Graficas del banco de compilacion, UNA POR FASE.

Antes salia un puñado de figuras que mezclaban fases distintas en los mismos
ejes.  El resultado eran barras de colores sin eje comun ni pregunta detras: si
en una figura conviven el suelo del compilador, un proyecto de veinte modulos y
cuatro familias de codigo, no hay escala que sirva para las tres y no se puede
leer ninguna.

Aqui cada fase produce SUS figuras, con la misma regla que el banco de
ejecucion: **una grafica, una pregunta**.  Mejor ocho imagenes claras que una
sobrecargada.

Anadir graficas a una fase es escribir su funcion y ponerla en `POR_FASE`; las
demas no se enteran.  Si falta matplotlib, todo esto se salta y el banco sigue
publicando sus tablas.
"""
from __future__ import annotations

from pathlib import Path
from typing import Optional


def _mpl():
    """matplotlib, o None si no esta.  Las graficas son un extra, no un requisito."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        return plt
    except ImportError:
        return None


# Los mismos colores y etiquetas que el banco de ejecucion: las dos tandas se
# leen a menudo juntas, y que C sea azul en una y verde en otra obliga a releer
# la leyenda en cada figura.
try:  # pragma: no cover - solo para reutilizar las tablas si estan
    import sys as _sys
    _sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from plots import LANG_COLORS, LANG_LABELS  # type: ignore # noqa: E402
except Exception:  # noqa: BLE001
    LANG_COLORS, LANG_LABELS = {}, {}


def _col(ln: str) -> str:
    return LANG_COLORS.get(ln, "#888888")


def _lab(ln: str) -> str:
    return LANG_LABELS.get(ln, ln)


def _guardar(plt, fig, destino: Path, nombre: str, hechas: dict) -> None:
    """Cierra y escribe una figura, y la apunta para el resumen final."""
    plt.tight_layout()
    plt.savefig(destino / (nombre + ".png"), dpi=110, bbox_inches="tight")
    plt.close(fig)
    hechas[nombre] = True


def _casos_de(datos: dict, fase: str) -> list:
    return [c for c in datos.get("casos", []) if c.get("fase") == fase]


# ---------------------------------------------------------------------------
#  Fase 1 -- el suelo: lo que cuesta arrancar cada compilador.
# ---------------------------------------------------------------------------

def fase_suelo(datos: dict, destino: Path, hechas: dict) -> None:
    plt = _mpl()
    suelo = datos.get("suelo") or {}
    filas = [(ln, s.get("p50"), s.get("mem_kib")) for ln, s in suelo.items()
             if s.get("p50")]
    if not plt or not filas:
        return
    filas.sort(key=lambda t: t[1])
    # Tiempo y memoria en la misma figura pero en ejes SEPARADOS: son unidades
    # distintas y ponerlas en el mismo eje aplasta una de las dos.
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(13, 5))
    nombres = [_lab(f[0]) for f in filas]
    a1.barh(nombres, [f[1] for f in filas],
            color=[_col(f[0]) for f in filas])
    a1.set_xlabel("ms")
    a1.set_title("Arrancar y no hacer nada (tiempo)")
    a1.grid(axis="x", alpha=0.3)
    mem = [(f[2] or 0) / 1024.0 for f in filas]
    a2.barh(nombres, mem, color=[_col(f[0]) for f in filas])
    a2.set_xlabel("MiB (pico del arbol de procesos)")
    a2.set_title("Arrancar y no hacer nada (memoria)")
    a2.grid(axis="x", alpha=0.3)
    fig.suptitle("Fase 1 -- suelo del compilador\n"
                 "Esto esta DENTRO de cualquier otra medida y por eso se resta",
                 fontsize=12, fontweight="bold")
    _guardar(plt, fig, destino, "f1_suelo", hechas)


# ---------------------------------------------------------------------------
#  Fase 2 -- compilar el programa entero, por tamano.
# ---------------------------------------------------------------------------

def fase_completa(datos: dict, destino: Path, hechas: dict) -> None:
    plt = _mpl()
    casos = _casos_de(datos, "2")
    if not plt or not casos:
        return
    tamanos = sorted({c["lineas"] for c in casos})
    langs = sorted({c["lang"] for c in casos})

    # (a) frio contra caliente, agrupado por tamano.  La pregunta es "cuanto
    # cuesta", y la respuesta son dos numeros por lenguaje, no uno.
    fig, axes = plt.subplots(1, len(tamanos), figsize=(7 * len(tamanos), 5),
                             squeeze=False)
    for i, n in enumerate(tamanos):
        ax = axes[0][i]
        pres = [ln for ln in langs
                if any(c["lang"] == ln and c["lineas"] == n for c in casos)]
        xs = list(range(len(pres)))
        f = [next((c["frio"].get("p50") for c in casos
                   if c["lang"] == ln and c["lineas"] == n and c.get("frio")),
                  0) for ln in pres]
        cal = [next((c["caliente"].get("p50") for c in casos
                     if c["lang"] == ln and c["lineas"] == n
                     and c.get("caliente")), 0) for ln in pres]
        ax.bar([x - 0.2 for x in xs], f, 0.38, label="en frio",
               color=[_col(l) for l in pres])
        ax.bar([x + 0.2 for x in xs], cal, 0.38, label="en caliente",
               color=[_col(l) for l in pres], alpha=0.5)
        ax.set_xticks(xs)
        ax.set_xticklabels([_lab(l) for l in pres], rotation=20, ha="right")
        ax.set_ylabel("ms")
        ax.set_title("%d lineas" % n)
        ax.grid(axis="y", alpha=0.3)
        ax.legend(fontsize=8)
    fig.suptitle("Fase 2 -- compilar el programa entero",
                 fontsize=12, fontweight="bold")
    _guardar(plt, fig, destino, "f2_completa", hechas)

    # (b) contra la base, que es la lectura que se quiere de un vistazo.
    base = "vesta"
    n = tamanos[-1]
    pares = []
    b = next((c["frio"].get("p50") for c in casos
              if c["lang"] == base and c["lineas"] == n and c.get("frio")), 0)
    if b:
        for ln in langs:
            v = next((c["frio"].get("p50") for c in casos
                      if c["lang"] == ln and c["lineas"] == n and c.get("frio")),
                     0)
            if v:
                pares.append((v / b, ln))
        pares.sort()
        fig, ax = plt.subplots(figsize=(9, max(3, 0.5 * len(pares) + 2)))
        ax.barh([_lab(p[1]) for p in pares], [p[0] for p in pares],
                color=[_col(p[1]) for p in pares])
        ax.axvline(1.0, color="#444", linestyle=":", linewidth=1.5)
        ax.set_xscale("log")
        ax.set_xlabel("veces respecto a %s (log).  <1 = tarda menos" % base)
        ax.set_title("Fase 2 -- cuanto tarda cada uno comparado con %s\n"
                     "(%d lineas, en frio)" % (base, n),
                     fontsize=11, fontweight="bold")
        ax.grid(axis="x", which="both", alpha=0.3)
        _guardar(plt, fig, destino, "f2_vs_base", hechas)

    # (c) memoria, que es el otro limite y no se deduce del tiempo.
    fig, ax = plt.subplots(figsize=(10, 5))
    hay = False
    for ln in langs:
        pts = sorted((c["lineas"], (c.get("frio") or {}).get("mem_kib"))
                     for c in casos if c["lang"] == ln)
        pts = [(x, y / 1024.0) for x, y in pts if y]
        if not pts:
            continue
        hay = True
        ax.plot([p[0] for p in pts], [p[1] for p in pts], "o-", linewidth=2,
                color=_col(ln), label=_lab(ln))
    if hay:
        ax.set_xlabel("lineas del programa")
        ax.set_ylabel("MiB (pico)")
        ax.set_title("Fase 2 -- memoria maxima al compilar\n"
                     "(es un limite duro: pasarse no hace la compilacion "
                     "lenta, la hace imposible)", fontsize=11,
                     fontweight="bold")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=9)
        _guardar(plt, fig, destino, "f2_memoria", hechas)
    else:
        plt.close(fig)


# ---------------------------------------------------------------------------
#  Fase crecimiento -- como escala, que es lo que un solo tamano no dice.
# ---------------------------------------------------------------------------

def fase_crecimiento(datos: dict, destino: Path, hechas: dict) -> None:
    plt = _mpl()
    casos = _casos_de(datos, "crecimiento")
    if not plt or not casos:
        return
    langs = sorted({c["lang"] for c in casos})

    def _serie(ln: str, clave: str, div: float = 1.0):
        pts = sorted((c["lineas"], c.get(clave)) for c in casos
                     if c["lang"] == ln)
        return [(x, y / div) for x, y in pts if y]

    # (a) tiempo contra lineas, en log-log.  En esos ejes la pendiente ES el
    # exponente, asi que la forma se lee directamente; en escala normal todas
    # las curvas suben y no se distingue una de otra.
    for clave, etiq, nombre in (("neto_frio", "en frio", "fc_tiempo_frio"),
                                ("neto_caliente", "en caliente",
                                 "fc_tiempo_caliente")):
        fig, ax = plt.subplots(figsize=(9, 6))
        hay = False
        for ln in langs:
            pts = _serie(ln, clave)
            if len(pts) < 2:
                continue
            hay = True
            ax.plot([p[0] for p in pts], [p[1] for p in pts], "o-",
                    linewidth=2, color=_col(ln), label=_lab(ln))
        if not hay:
            plt.close(fig)
            continue
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("lineas del programa (log)")
        ax.set_ylabel("ms sin el arranque (log)")
        ax.set_title("Crecimiento del TIEMPO %s\n"
                     "(en log-log la pendiente es el exponente: recta a 45 "
                     "grados = lineal)" % etiq, fontsize=11, fontweight="bold")
        ax.grid(alpha=0.3, which="both")
        ax.legend(fontsize=9)
        _guardar(plt, fig, destino, nombre, hechas)

    # (b) lo mismo para la memoria.  Va aparte y no como segunda serie de la
    # figura anterior: son unidades distintas, y superponerlas obligaria a un
    # eje doble que se lee peor que dos imagenes.
    fig, ax = plt.subplots(figsize=(9, 6))
    hay = False
    for ln in langs:
        pts = _serie(ln, "mem_frio_kib", 1024.0)
        if len(pts) < 2:
            continue
        hay = True
        ax.plot([p[0] for p in pts], [p[1] for p in pts], "o-", linewidth=2,
                color=_col(ln), label=_lab(ln))
    if hay:
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("lineas del programa (log)")
        ax.set_ylabel("MiB de pico (log)")
        ax.set_title("Crecimiento de la MEMORIA (en frio)\n"
                     "(no tiene por que crecer como el tiempo: se puede tardar "
                     "el doble y pedir cuatro veces mas)",
                     fontsize=11, fontweight="bold")
        ax.grid(alpha=0.3, which="both")
        ax.legend(fontsize=9)
        _guardar(plt, fig, destino, "fc_memoria", hechas)
    else:
        plt.close(fig)

    # (c) los exponentes, que es el resumen de todo lo anterior en una cifra.
    resumen = datos.get("crecimiento") or []
    if resumen:
        fig, ax = plt.subplots(figsize=(max(8, 1.6 * len(resumen)), 5))
        nombres = [r["lang"] for r in resumen]
        xs = list(range(len(nombres)))
        for desp, clave, etiq, alfa in ((-0.26, "k_frio", "tiempo en frio", 1.0),
                                        (0.0, "k_caliente", "tiempo caliente",
                                         0.65),
                                        (0.26, "k_memoria", "memoria", 0.35)):
            ax.bar([x + desp for x in xs],
                   [(r.get(clave) or 0) for r in resumen], 0.25, label=etiq,
                   alpha=alfa, color=[_col(n) for n in nombres])
        ax.axhline(1.0, color="#444", linestyle=":", linewidth=1.5)
        ax.text(len(nombres) - 0.5, 1.03, "lineal", fontsize=8, color="#444",
                ha="right")
        ax.set_xticks(xs)
        ax.set_xticklabels([_lab(n) for n in nombres], rotation=20, ha="right")
        ax.set_ylabel("exponente k de  coste = a * lineas^k")
        ax.set_title("Como escala cada compilador\n"
                     "(1.0 = el doble de codigo cuesta el doble; 2.0 = cuatro "
                     "veces)", fontsize=11, fontweight="bold")
        ax.grid(axis="y", alpha=0.3)
        ax.legend(fontsize=9)
        _guardar(plt, fig, destino, "fc_exponentes", hechas)

    # (d) lo que aporta la cache, y si aguanta al crecer el programa.  No se
    # deduce de las dos curvas por separado sin ir restando a ojo.
    fig, ax = plt.subplots(figsize=(9, 5))
    hay = False
    for ln in langs:
        frio = dict(_serie(ln, "neto_frio"))
        cal = dict(_serie(ln, "neto_caliente"))
        comunes = sorted(set(frio) & set(cal))
        if len(comunes) < 2:
            continue
        hay = True
        ax.plot(comunes, [frio[n] / cal[n] for n in comunes], "o-",
                linewidth=2, color=_col(ln), label=_lab(ln))
    if hay:
        ax.axhline(1.0, color="#444", linestyle=":", linewidth=1.5)
        ax.set_xscale("log")
        ax.set_xlabel("lineas del programa (log)")
        ax.set_ylabel("veces mas rapido con la cache caliente")
        ax.set_title("Cuanto aporta la cache, y si aguanta al crecer\n"
                     "(un fichero suelto no tiene nada que reutilizar: ahi "
                     "1.0 es lo esperado)", fontsize=11, fontweight="bold")
        ax.grid(alpha=0.3, which="both")
        ax.legend(fontsize=9)
        _guardar(plt, fig, destino, "fc_ahorro_cache", hechas)
    else:
        plt.close(fig)


# ---------------------------------------------------------------------------
#  Registro: que dibuja cada fase.  Anadir una es poner su funcion aqui.
# ---------------------------------------------------------------------------

POR_FASE = [
    ("1", fase_suelo),
    ("2", fase_completa),
    ("crecimiento", fase_crecimiento),
]


def dibujar(datos: dict, destino: Path) -> dict:
    """Todas las graficas que los datos permitan, en @p destino.

    Cada fase escribe las suyas y ninguna depende de las demas: una tanda de
    una sola fase produce sus figuras y ya, sin huecos ni ejes vacios.
    """
    if _mpl() is None:
        return {}
    destino.mkdir(parents=True, exist_ok=True)
    hechas: dict = {}
    for _id, fn in POR_FASE:
        try:
            fn(datos, destino, hechas)
        except Exception as e:  # noqa: BLE001
            # Una figura que falla no puede llevarse por delante las demas ni
            # la tanda: el banco ya midio, y eso es lo que no se puede perder.
            print("[aviso] grafica de la fase %s: %s" % (_id, e))
    return hechas
