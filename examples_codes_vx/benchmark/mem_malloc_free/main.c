/* mem_malloc_free: 5M bloques de 96 bytes, con una ventana de 64 vivos.
 *
 * La version anterior reservaba y liberaba en la misma iteracion escribiendo
 * dos bytes que nadie leia.  `gcc -O3` demostraba que el bloque no lo observa
 * nadie y borraba el par malloc/free entero: quedaba un bucle VACIO contando
 * hasta 5M -- sobrevivia solo porque `bound` era `volatile` y habia que leerlo
 * -- y un `return 42`.  Cero reservas.
 *
 * Ver `alloc_small/main.c` para el detalle de por que una ventana de vivos hace
 * el trabajo irreducible sin recurrir a barreras especificas de cada
 * compilador.  Este banco se distingue de `alloc_small` (16 B) y `alloc_medium`
 * (1 KB) por el tamano: 96 bytes cae en otra clase del asignador.
 */
#include <stdlib.h>
#include <stdint.h>

#define TAM   96
#define ITERS 5000000
#define VIVOS 64          /* potencia de 2 */

int main(void) {
    uint8_t *anillo[VIVOS];
    for (int k = 0; k < VIVOS; k++) anillo[k] = NULL;

    int acc = 0;
    for (int32_t i = 0; i < ITERS; ++i) {
        uint8_t *buf = (uint8_t *) malloc(TAM);
        buf[0] = (uint8_t) i;
        buf[TAM - 1] = (uint8_t) (i + TAM - 1);
        int k = i & (VIVOS - 1);
        if (anillo[k] != NULL) {       /* el mas viejo sale de la ventana */
            acc += anillo[k][0] + anillo[k][TAM - 1];
            free(anillo[k]);
        }
        anillo[k] = buf;
    }
    for (int k = 0; k < VIVOS; k++) {  /* vaciar la ventana */
        if (anillo[k] != NULL) {
            acc += anillo[k][0] + anillo[k][TAM - 1];
            free(anillo[k]);
        }
    }
    return acc % 251;
}
