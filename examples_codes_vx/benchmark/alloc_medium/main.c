// alloc_medium: 4M bloques de 1 KB, con una ventana de 64 vivos.
//
// Segundo de los cuatro bancos de reserva.  A esta escala el coste por llamada
// del asignador empieza a repartirse entre mas bytes y aparece la primera pagina
// tocada por bloque; el pequeno (16 B) esta dominado por la llamada y el grande
// por el reciclado de bloques ya comprometidos.  Por eso son bancos separados:
// un solo numero de "reserva" mezclaria tres regimenes distintos.
//
// Ver `alloc_small/main.c` para el detalle de por que se toca cada pagina y por
// que la ventana de vivos en vez de reservar y liberar en la misma iteracion.
#include <stdlib.h>

#define TAM    1024
#define ITERS  4000000
#define VIVOS  64         /* potencia de 2 */
#define PAGINA 4096

int main(void) {
    unsigned char *anillo[VIVOS];
    for (int k = 0; k < VIVOS; k++) anillo[k] = NULL;

    int acc = 0;
    for (int i = 0; i < ITERS; i++) {
        unsigned char *p = (unsigned char*)malloc(TAM);
        unsigned char v = (unsigned char)(i & 0xFF);
        for (size_t o = 0; o < TAM; o += PAGINA) p[o] = v;
        p[TAM - 1] = v;
        int k = i & (VIVOS - 1);
        if (anillo[k] != NULL) {       /* el mas viejo sale de la ventana */
            acc += anillo[k][0];
            free(anillo[k]);
        }
        anillo[k] = p;
    }
    for (int k = 0; k < VIVOS; k++) {  /* vaciar la ventana */
        if (anillo[k] != NULL) { acc += anillo[k][0]; free(anillo[k]); }
    }
    return acc & 0xFF;
}
