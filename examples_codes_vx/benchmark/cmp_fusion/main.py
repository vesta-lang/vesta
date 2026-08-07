# cmp_fusion: 50M comparaciones + salto, con el resultado consumido.
# Mismo algoritmo que main.c; ver alli por que la condicion tiene que ser
# impredecible en compilacion.
#
# En Python los enteros no tienen ancho, asi que hay que recortar a 32 bits a
# mano para que el generador de siempre la misma secuencia que en los otros
# seis lenguajes.
import sys

ITERS = 50000000
MASCARA = 0xFFFFFFFF


def main():
    s = 12345
    acc = 0
    for i in range(ITERS):
        s = (s * 1664525 + 1013904223) & MASCARA
        if (s >> 31) == 0:
            acc += 1
    sys.exit(acc & 0xFF)


main()
