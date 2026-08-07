# mem_class: 1M objetos en heap, con una ventana de 64 vivos.
# Mismo algoritmo que main.c; ver alli en que se diferencia este banco de los
# de bloque crudo (`alloc_small` y companeros) y por que la ventana.
#
# CPython no optimiza nada, asi que aqui la ventana no hace falta para evitar
# que se borre el trabajo: se mantiene para que los siete lenguajes ejecuten
# EXACTAMENTE el mismo algoritmo, que es lo unico que hace comparable el
# resultado.
import sys

ITERS = 1000000
VIVOS = 64  # potencia de 2


class Foo:
    __slots__ = ("x",)

    def __init__(self, v):
        self.x = v


def main():
    anillo = [None] * VIVOS
    acc = 0
    for i in range(ITERS):
        f = Foo(i & 0xFF)
        k = i & (VIVOS - 1)
        if anillo[k] is not None:  # el mas viejo sale de la ventana
            acc += anillo[k].x
        anillo[k] = f
    for k in range(VIVOS):  # vaciar la ventana
        if anillo[k] is not None:
            acc += anillo[k].x
    sys.exit(acc & 0xFF)


main()
