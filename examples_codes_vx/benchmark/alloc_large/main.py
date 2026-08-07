# alloc_large: 500K bloques de 256 KB, con una ventana de 16 vivos.
# Mismo algoritmo que main.c; el porque del diseno esta en
# `alloc_small/main.c`.
#
# Python NO tiene bloque crudo: `bytearray(n)` viene puesto a cero, asi que a
# esta escala paga 256 KB de borrado por iteracion ademas de la reserva.
import sys

TAM = 256 * 1024
ITERS = 500000
VIVOS = 16  # potencia de 2
PAGINA = 4096


def main():
    anillo = [None] * VIVOS
    acc = 0
    for i in range(ITERS):
        p = bytearray(TAM)
        v = i & 0xFF
        for o in range(0, TAM, PAGINA):
            p[o] = v
        p[TAM - 1] = v
        k = i & (VIVOS - 1)
        if anillo[k] is not None:  # el mas viejo sale de la ventana
            acc += anillo[k][0]
        anillo[k] = p
    for k in range(VIVOS):  # vaciar la ventana
        if anillo[k] is not None:
            acc += anillo[k][0]
    sys.exit(acc & 0xFF)


main()
