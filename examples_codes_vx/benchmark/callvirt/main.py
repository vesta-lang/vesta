# callvirt: 30M llamadas a metodo.
# Mismo algoritmo que main.c; ver alli donde esta la linea entre optimizar y
# fabricar el resultado sin ejecutar.
#
# CPython no optimiza nada, asi que aqui la cadena congruencial no hace falta
# para evitar que se borre el trabajo: se mantiene para que los siete lenguajes
# ejecuten EXACTAMENTE el mismo algoritmo, que es lo unico que hace comparable
# el resultado.
import sys

MASCARA = 0xFFFFFFFF


class Counter:
    __slots__ = ("value",)

    def __init__(self):
        self.value = 0

    def inc(self):
        return self.value + 1


def main():
    c = Counter()
    sum_ = 0
    for i in range(30000000):
        t = (c.inc() * 1664525 + 1013904223) & MASCARA
        c.value = t & 0xFF
        sum_ += c.value
    sys.exit(sum_ % 251)


main()
