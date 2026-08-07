# hash_lookup: 50M mezclas estilo FNV, con el resultado consumido.
# Mismo algoritmo que main.c; ver alli los DOS problemas que tenia.
#
# En Python los enteros no tienen ancho, asi que hay que recortar a 64 bits a
# mano para que la secuencia sea la misma que en los otros seis lenguajes.
import sys

ITERS = 50000000
MASCARA = 0xFFFFFFFFFFFFFFFF


def main():
    seed = 0xCAFEBABEDEADBEEF
    acc = 0
    for i in range(ITERS):
        seed ^= i
        seed = (seed * 1099511628211) & MASCARA
        seed >>= 7
        seed |= 1
        acc += seed & 0xFF
    sys.exit(acc % 251)


main()
