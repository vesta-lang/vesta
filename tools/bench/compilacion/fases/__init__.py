#!/usr/bin/env python3
"""El registro de fases: cuales hay, en que orden y cuales van por defecto.

Anadir una fase es escribir su modulo con una funcion `fase(ctx)` y ponerla en
esta lista.  No hay que tocar nada mas: `--fase` la reconoce sola y
`--listar-fases` la enseña.

TODAS corren por defecto.  El campo `por_defecto` se queda por si alguna vez
hace falta dejar una fuera, pero mientras no lo sea, una tanda normal mide todo
lo que el banco sabe medir: una fase que no corre es una fase que se pudre.
"""
from __future__ import annotations

from ..contexto import Fase
from . import (completa, crecimiento, escalado, familias, proyecto,
               realimentacion, regimen, suelo, topologia)

FASES = [
    Fase("1", "Suelo del compilador", suelo.fase),
    Fase("2", "Compilacion completa, por tamano", completa.fase),
    Fase("2b", "Un proyecto, no un fichero", proyecto.fase),
    # Cuestan de largo lo que mas: varios tamanos, cada uno medido dos veces.
    # Va POR DEFECTO pese a costar: es la unica que responde como escala, y
    # esa es la pregunta que no se puede contestar con un solo tamano.  Dejarla
    # fuera hacia que una tanda normal no la enseñara y pareciera que no existe.
    Fase("crecimiento", "Crecimiento contra el tamano del codigo",
         crecimiento.fase),
    Fase("2bis", "Escalado (tamano y numero de modulos)", escalado.fase),
    Fase("2c", "Topologia de dependencias", topologia.fase),
    Fase("2d", "Familias de codigo", familias.fase),
    Fase("2e", "Familia por regimen", regimen.fase),
    Fase("3", "Realimentacion", realimentacion.fase),
]

# Por nombre, para `--fase`.
POR_ID = {f.id: f for f in FASES}


def seleccionar(pedidas: str) -> list:
    """Las fases a correr.

    Sin argumento, las de por defecto.  Con `todas`, todas.  Si se nombra una
    que no existe se avisa y se sigue con las demas -- pero devolviendo la
    lista vacia si NINGUNA existia, para que el banco no arranque a medias
    creyendo que hace lo que le pidieron.
    """
    if not pedidas:
        return [f for f in FASES if f.por_defecto]
    if pedidas.strip().lower() in ("todas", "all", "*"):
        return list(FASES)
    fuera = []
    elegidas = []
    for nombre in [p.strip() for p in pedidas.split(",") if p.strip()]:
        f = POR_ID.get(nombre)
        if f is None:
            fuera.append(nombre)
        else:
            elegidas.append(f)
    if fuera:
        print("[aviso] no existe esa fase: " + ", ".join(fuera))
        print("        hay: " + ", ".join(f.id for f in FASES))
    # En el orden del registro, no en el que se escribieron: la de suelo mide
    # lo que las demas descuentan, asi que correrla despues no serviria.
    return [f for f in FASES if f in elegidas]
