// alloc_large: 500K bloques de 256 KB, con una ventana de 16 vivos.
//
// Tercero de los cuatro bancos de reserva.  A esta escala el asignador ya no
// atiende desde su cache por hilo, pero tampoco vuelve al sistema operativo en
// cada iteracion: RECICLA el bloque que se acaba de liberar, con sus paginas ya
// comprometidas.  Por eso el coste por byte se desploma respecto al pequeno, y
// por eso es un banco aparte: promediarlo con los otros esconderia el efecto.
//
// Ver `alloc_small/main.c` para el detalle de por que se toca cada pagina y por
// que la ventana de vivos.  Aqui el toque son 64 paginas por bloque, y es la
// parte que de verdad cuesta.
#include <stdlib.h>

#define TAM    (256 * 1024)
#define ITERS  500000
#define VIVOS  16         /* potencia de 2 */
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
