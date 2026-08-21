#!/usr/bin/env python3
"""Ninguna variable de entorno se lee sin estar declarada en la tabla.

    python3 tests/util/env_flags_registry_test.py

La tabla (`include/util/env_flags_table.h`) dice de cada mando QUE cambia y a
QUE parte afecta, y de ahi salen las huellas que entran en las claves de cache.
Un mando que se lee sin estar declarado no aparece en ninguna huella: la cache
sirve entonces un artefacto compilado con OTRA configuracion.  Eso no da error
ni falla ningun test de comportamiento -- da un binario que no corresponde al
fuente.

Por eso la comprobacion es esta y no otra: no basta con que la tabla este bien,
hace falta que sea COMPLETA.  Mientras este test pase, anadir un mando y
olvidarse de declararlo es imposible.

Tambien avisa de lo contrario -- declarado y no leido por nadie -- que no rompe
nada pero es tabla muerta que confunde al siguiente que la lea.

Salida 0 si todo esta declarado; != 0 con la lista de lo que falta.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
TABLA = RAIZ / "include" / "util" / "env_flags_table.h"

# Donde se lee el entorno de verdad.  Los tests quedan fuera a proposito: un
# test PUEDE inventarse una variable para comprobar un caso.
ARBOLES = ["src", "include", "main.cpp"]

# Sin excepciones por fichero, y a proposito.
#
# La consola y el interprete de guiones exponen `getenv(nombre)` al USUARIO: ese
# nombre lo pone quien escribe el guion y ninguna tabla puede declararlo.  Pero
# no hace falta apartar esos ficheros, porque lo de abajo solo reconoce nombres
# escritos como LITERAL: un `getenv(nombre.c_str())` no casa.  Apartar el
# fichero entero, ademas de sobrar, tapaba los literales que esos mismos
# ficheros SI leen -- y entonces salian como "declarados y no los lee nadie",
# que es justo la senal contraria a la verdadera.

# Como se lee un mando.  Ademas de `getenv` directo hay ayudantes locales que
# reciben el nombre; sin mirarlos, nueve mandos del proyecto no aparecian en
# ningun barrido.
LECTURAS = re.compile(
    r'(?:std::)?getenv\s*\(\s*"([A-Za-z_][A-Za-z_0-9]*)"'
    r'|(?:env_flag_on|env_disables|env_on|flag_activo)\s*\(\s*"([A-Za-z_][A-Za-z_0-9]*)"'
)

DECLARACION = re.compile(r'VESTA_ENV_FLAG\s*\(\s*\w+\s*,\s*"([^"]+)"')


def declarados() -> set[str]:
    if not TABLA.is_file():
        print("no encuentro la tabla: %s" % TABLA)
        sys.exit(2)
    texto = TABLA.read_text(encoding="utf-8", errors="replace")
    return set(DECLARACION.findall(texto))


def ficheros():
    for a in ARBOLES:
        p = RAIZ / a
        if p.is_file():
            yield p
        elif p.is_dir():
            for f in p.rglob("*"):
                if f.suffix in (".cpp", ".h", ".hpp", ".c"):
                    yield f


def leidos() -> dict[str, list[str]]:
    """nombre del mando -> ficheros donde se lee (relativos a la raiz)."""
    fuera: dict[str, list[str]] = {}
    for f in ficheros():
        rel = f.relative_to(RAIZ).as_posix()
        try:
            texto = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "getenv" not in texto and "env_flag_on" not in texto \
                and "env_disables" not in texto:
            continue
        for m in LECTURAS.finditer(texto):
            nombre = m.group(1) or m.group(2)
            fuera.setdefault(nombre, [])
            if rel not in fuera[nombre]:
                fuera[nombre].append(rel)
    return fuera


def main() -> int:
    tabla = declarados()
    usados = leidos()

    sin_declarar = {k: v for k, v in usados.items() if k not in tabla}
    sin_usar = sorted(tabla - set(usados))

    if sin_declarar:
        print("MANDOS QUE SE LEEN Y NO ESTAN EN LA TABLA (%d):" % len(sin_declarar))
        print("  Cada uno de estos queda FUERA de las huellas, asi que la cache")
        print("  puede servir un artefacto compilado con otro valor.  Declararlos")
        print("  en include/util/env_flags_table.h diciendo que cambian.")
        for nombre in sorted(sin_declarar):
            print("    %-34s %s" % (nombre, ", ".join(sin_declarar[nombre][:3])))
        print()

    if sin_usar:
        # No rompe nada: solo es tabla que ya no describe al codigo.
        print("declarados y no leidos por nadie (%d): %s" % (
            len(sin_usar), " ".join(sin_usar)))
        print()

    print("=== mandos: %d declarados, %d leidos, %d sin declarar ===" % (
        len(tabla), len(usados), len(sin_declarar)))
    return 1 if sin_declarar else 0


if __name__ == "__main__":
    sys.exit(main())
