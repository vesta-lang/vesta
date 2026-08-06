#!/usr/bin/env python3
"""Compara dos ficheros de resultados de `run_all_benches.py`.

Uso:  python tools/bench/compare_json.py referencia.json actual.json [modos...]

Imprime, por modo, la media geometrica de actual/referencia (menor es mejor) y
los benches que se salen del ruido.  Se mide asi -- y no por la media
aritmetica -- porque los tiempos van de milisegundos a decenas de segundos y la
aritmetica la dominarian los lentos.
"""

import json
import sys

RUIDO = 0.05  # +-5%: por debajo de eso no se distingue del ruido de la maquina.


def cargar(ruta):
    """Devuelve {(bench, modo): milisegundos} a partir de un fichero."""
    filas = json.load(open(ruta, encoding="utf-8"))["results"]
    tabla = {}
    for f in filas:
        nombre = f.get("bench")
        for clave, valor in f.items():
            if clave.startswith("vx_") and isinstance(valor, (int, float)):
                tabla[(nombre, clave)] = valor
    return tabla


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    ref, act = cargar(sys.argv[1]), cargar(sys.argv[2])
    modos = sys.argv[3:] or sorted({m for _, m in act})
    for modo in modos:
        razones, fuera, sin_dato = [], [], []
        for (nombre, m), valor in act.items():
            if m != modo:
                continue
            previo = ref.get((nombre, m))
            # El arnes marca con un valor NO POSITIVO el bench que no llego a
            # medirse (planton, fallo al compilar).  Es un centinela, no un
            # tiempo: meterlo en la media la vuelve un sinsentido.
            if previo is None or previo <= 0 or valor <= 0:
                sin_dato.append(nombre)
                continue
            razon = valor / previo
            razones.append(razon)
            if abs(razon - 1.0) > RUIDO:
                fuera.append((razon, nombre))
        if not razones:
            print(f"{modo:14s} sin datos comparables")
            continue
        geo = 1.0
        for r in razones:
            geo *= r
        geo **= 1.0 / len(razones)
        print(f"{modo:14s} geomean {geo:.4f}   ({len(razones)} benches)")
        for razon, nombre in sorted(fuera, reverse=True):
            marca = "peor" if razon > 1.0 else "mejor"
            print(f"                {nombre:20s} {razon:6.3f}x  {marca}")
        if sin_dato:
            print(f"                sin medir: {', '.join(sorted(sin_dato))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
