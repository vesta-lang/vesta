# mem_struct: 1M iteraciones x 3 caminos (pila, heap por sizeof, heap por
# tamano explicito), con ventana de 64 vivos en los dos de heap.
# Mismo algoritmo que main.c; ver alli que compara y por que la ventana.
#
# Python no tiene structs de valor ni pila: el camino "en la pila" son dos
# variables locales, y los dos de heap son objetos.
import sys

ITERS = 1000000
VIVOS = 64  # potencia de 2


class Punto:
    __slots__ = ("x", "y")

    def __init__(self, x, y):
        self.x = x
        self.y = y


def main():
    anillo_a = [None] * VIVOS
    anillo_b = [None] * VIVOS
    acc = 0
    for i in range(ITERS):
        base = acc & 0xFF

        px = base            # 1. en la pila
        py = base + 1
        acc += px + py

        k = i & (VIVOS - 1)

        h = Punto(base, base + 1)     # 2. en heap
        if anillo_a[k] is not None:
            acc += anillo_a[k].x + anillo_a[k].y
        anillo_a[k] = h

        m = Punto(base, base + 1)     # 3. en heap
        if anillo_b[k] is not None:
            acc += anillo_b[k].x + anillo_b[k].y
        anillo_b[k] = m
    for k in range(VIVOS):
        if anillo_a[k] is not None:
            acc += anillo_a[k].x + anillo_a[k].y
        if anillo_b[k] is not None:
            acc += anillo_b[k].x + anillo_b[k].y
    sys.exit(acc % 251)


main()
