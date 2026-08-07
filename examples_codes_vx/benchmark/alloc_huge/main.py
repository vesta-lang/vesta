# alloc_huge: 100 bloques de 16 MB, con una ventana de 4 vivos.
# Mismo algoritmo que main.c; ver alli por que a este tamano lo que se mide es
# el fallo de pagina y no la llamada al asignador.
#
# Python NO tiene bloque crudo: `bytearray(n)` viene puesto a cero, asi que
# toca los 16 MB en cada iteracion quiera o no.
import sys

TAM = 16 * 1024 * 1024
ITERS = 100
VIVOS = 4  # potencia de 2
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
