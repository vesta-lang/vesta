// cmp_fusion: 50M comparaciones + salto, con el resultado consumido.
//
// Que mide: el patron comparar-y-saltar, que es lo que la maquina virtual
// fusiona en una sola instruccion (`cmpjmp`).  Por eso el cuerpo tiene que
// SER una comparacion cuyo resultado dirija un salto de verdad.
//
// La version anterior era `while (i < n) { acc++; i++; }` y devolvia `acc`.
// Eso es una recurrencia afin: `gcc -O3` la resuelve en forma cerrada y el
// `main` quedaba en `movl $50000000, %eax; ret` -- cero iteraciones.  El bench
// no medida la comparacion: medida el arranque del proceso.
//
// El arreglo no es ponerle una barrera al compilador -- eso seria un truco
// distinto en cada lenguaje, y era la razon de que unos ejecutaran el bucle y
// otros no -- sino que la condicion NO SEA PREDECIBLE en compilacion.  Un
// generador congruencial da un valor nuevo por vuelta que ningun compilador
// puede resolver de antemano, y sobre el se hace la comparacion.  El contador
// sigue siendo el mismo bucle de 50M; lo unico que cambia es que ahora se
// ejecuta.
#include <stdint.h>

#define ITERS 50000000

int main(void) {
    uint32_t s = 12345u;
    int32_t acc = 0;
    for (int32_t i = 0; i < ITERS; i++) {
        s = s * 1664525u + 1013904223u;   /* siguiente valor, no plegable */
        /* `s >> 31 == 0` es "s < 2^31" sin depender de como cada lenguaje
         * trate el signo de un entero de 32 bits: en Java no hay `unsigned`, y
         * escrito con una comparacion directa cada uno daria un resultado
         * distinto.  Asi los siete calculan lo mismo. */
        if ((s >> 31) == 0u) acc++;        /* comparar + saltar */
    }
    return acc & 0xFF;
}
