// callvirt: 30M llamadas indirectas a traves de un puntero a funcion.
//
// La version anterior llamaba y tiraba el resultado (`sum += c.inc()` con un
// `inc` que devolvia siempre 1).  `gcc -O3` desvirtualizaba, inlinaba el
// `return 1` y resolvia el bucle en forma cerrada: el `main` quedaba en
// `movl $30000000, %eax; ret`.  Cero llamadas -- el banco medida el arranque
// del proceso, no el despacho.
//
// Donde esta la linea.  Que el compilador DESVIRTUALICE e inline cuando puede
// probar el destino es exactamente lo que este banco quiere ver: es calidad de
// codigo, y no todos llegan igual de lejos.  Lo que no puede pasar es que el
// resultado se fabrique sin ejecutar nada.
//
// Por eso NO se le esconde el destino al compilador.  Lo que se hace es que
// cada vuelta dependa de la anterior a traves del estado del objeto, con una
// recurrencia que ningun analisis de induccion resuelve en forma cerrada.  El
// destino sigue siendo evidente y quien sepa aprovecharlo se lleva el merito.
typedef struct Counter Counter;
struct Counter { int (*inc)(Counter*); int value; };

static int inc_impl(Counter *c) { return c->value + 1; }

int main(void) {
    Counter c;
    c.value = 0;
    c.inc = inc_impl;

    long long sum = 0;
    for (int i = 0; i < 30000000; i++) {
        /* El resultado de la llamada alimenta el estado del que depende la
         * llamada siguiente.  La mezcla congruencial impide la forma cerrada. */
        c.value = (int)(((unsigned)c.inc(&c) * 1664525u + 1013904223u) & 0xFFu);
        sum += c.value;
    }
    /* Modulo un PRIMO, no una mascara de 8 bits: la secuencia recorre ciclos
     * completos de 0..255 y su suma resultaba ser multiplo de 256, o sea que el
     * banco "verificaba" devolviendo 0 -- que es justo lo que devuelve un
     * programa que no hace nada.  Con 251 el valor esperado distingue. */
    return (int)(sum % 251);
}
