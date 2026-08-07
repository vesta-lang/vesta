/* hash_lookup: 50M mezclas estilo FNV, con el resultado consumido.
 *
 * Este banco tenia DOS problemas, y solo uno era del compilador.
 *
 * El de logica, que habria estado mal en cualquier lenguaje: el cuerpo hacia
 * `seed |= 1` -- dejando la semilla siempre IMPAR -- y justo despues preguntaba
 * `if ((seed & 7) == 0)`, que con una semilla impar no se cumple JAMaS.  El
 * acumulador era demostrablemente cero, asi que el banco no media busquedas:
 * media si tu compilador es lo bastante listo para darse cuenta.  Uno que lo
 * viera se saltaba todo el trabajo y otro que no lo hacia entero.
 *
 * El del compilador, consecuencia del anterior: `gcc -O3` probaba que `acc`
 * vale 0 y borraba la mezcla completa.  Quedaba un bucle VACIO contando hasta
 * 50M -- sobrevivia solo porque `bound` era `volatile` y habia que leerlo.
 *
 * Arreglo: el resultado del hash se USA en cada vuelta.  La cadena de
 * multiplicaciones no se puede resolver en forma cerrada y el acumulador acaba
 * en el valor de retorno, asi que el trabajo hace falta.  Ya no hace falta el
 * `volatile`, que era un parche para sostener un bucle que no hacia nada.
 */
#include <stdint.h>

#define ITERS 50000000

int main(void) {
    uint64_t seed = 0xCAFEBABEDEADBEEFULL;
    uint64_t acc = 0;
    for (int32_t i = 0; i < ITERS; ++i) {
        seed ^= (uint64_t) i;
        seed *= 1099511628211ULL;
        seed >>= 7;
        seed |= 1ULL;
        acc += seed & 0xFFULL;   /* el hash se consume: ya no es codigo muerto */
    }
    return (int32_t)(acc % 251);
}
