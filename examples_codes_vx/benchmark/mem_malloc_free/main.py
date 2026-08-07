# mem_malloc_free: 5M bloques de 96 bytes, con una ventana de 64 vivos.
# Mismo algoritmo que main.c; el porque de la ventana esta en
# `alloc_small/main.c`.
#
# Python NO tiene bloque crudo: `bytearray(n)` es un objeto con cabecera y
# contador de referencias, y viene puesto a cero.
import sys

TAM = 96
ITERS = 5000000
VIVOS = 64  # potencia de 2


def main():
    anillo = [None] * VIVOS
    acc = 0
    for i in range(ITERS):
        buf = bytearray(TAM)
        buf[0] = i & 0xFF
        buf[TAM - 1] = (i + TAM - 1) & 0xFF
        k = i & (VIVOS - 1)
        if anillo[k] is not None:  # el mas viejo sale de la ventana
            acc += anillo[k][0] + anillo[k][TAM - 1]
        anillo[k] = buf
    for k in range(VIVOS):  # vaciar la ventana
        if anillo[k] is not None:
            acc += anillo[k][0] + anillo[k][TAM - 1]
    sys.exit(acc % 251)


main()
