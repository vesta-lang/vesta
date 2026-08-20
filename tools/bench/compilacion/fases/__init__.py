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
               realimentacion, regimen, rendimiento, suelo, topologia)

FASES = [
    # El orden NO es alfabetico ni historico: va de lo mas simple a lo mas
    # compuesto, porque cada fase se apoya en entender la anterior.  Los
    # nombres dicen QUE mide cada una; antes eran `2`, `2b`, `2bis`, `2c`...,
    # que solo tenian sentido para quien hubiera leido el codigo en el orden en
    # que se escribio.
    Fase("arranque", "Arrancar el compilador y no hacer nada", suelo.fase),
    Fase("fichero", "Un fichero, por tamano", completa.fase),
    Fase("caudal", "Lineas por segundo", rendimiento.fase),
    Fase("crecimiento", "Como crece con el tamano del codigo",
         crecimiento.fase),
    Fase("escalado", "Escalado por tamano y por numero de modulos",
         escalado.fase),
    Fase("proyecto", "Un proyecto de varios modulos, y que cuesta rehacerlo",
         proyecto.fase),
    Fase("dependencias", "La forma de las dependencias", topologia.fase),
    Fase("familias", "Que codigo se compila, no cuanto", familias.fase),
    Fase("familias-regimen", "Cada familia, por regimen", regimen.fase),
    Fase("diagnostico", "Cuanto tarda en decirte si el codigo esta bien",
         realimentacion.fase),
]

# Los nombres viejos siguen valiendo.  Renombrar no puede romper un guion que
# ya existe ni una nota con el comando apuntado, y el coste de mantenerlo es
# esta tabla.
ALIAS = {
    "1": "arranque", "2": "fichero", "2b": "proyecto", "2bis": "escalado",
    "2c": "dependencias", "2d": "familias", "2e": "familias-regimen",
    "3": "diagnostico",
}

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
        f = POR_ID.get(ALIAS.get(nombre, nombre))
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
