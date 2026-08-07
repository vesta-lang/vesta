# alloc_small: 5M bloques de 16 bytes, con una ventana de 64 vivos.
# Mismo algoritmo que main.c; ver alli por que hay cuatro bancos de reserva,
# por que se toca cada pagina y por que la ventana.
#
# Python NO tiene bloque crudo: `bytearray(n)` es un objeto con cabecera y
# contador de referencias, y viene puesto a cero.  Es lo mas cercano a "n bytes
# en el heap" que el lenguaje permite.
#
# CPython no optimiza nada, asi que aqui la ventana no hace falta para evitar
# que se borre el trabajo: se mantiene para que los siete lenguajes ejecuten
# EXACTAMENTE el mismo algoritmo, que es lo unico que hace comparable el
# resultado.
import sys

TAM = 16
ITERS = 5000000
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
