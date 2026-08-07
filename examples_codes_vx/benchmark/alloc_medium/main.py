# alloc_medium: 4M bloques de 1 KB, con una ventana de 64 vivos.
# Mismo algoritmo que main.c; el porque del diseno esta en
# `alloc_small/main.c`.
#
# Python NO tiene bloque crudo: `bytearray(n)` es un objeto con cabecera y
# contador de referencias, y viene puesto a cero.
import sys

TAM = 1024
ITERS = 4000000
VIVOS = 64  # potencia de 2
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
