// alloc_huge: 100 bloques de 16 MB, con una ventana de 4 vivos.
//
// Cuarto de los cuatro bancos de reserva, y el que mas se aparta de los otros:
// aqui la llamada al asignador no se nota, lo que cuesta es el FALLO DE PAGINA.
// Cien bloques de 16 MB "costaban" medio milisegundo cuando no se tocaban,
// porque `malloc` solo RESERVA y los 1600 MB no llegaban a existir.  Al tocar
// una vez cada pagina -- 4096 por bloque -- aparece el coste real de conseguir
// memoria usable, que es lo que paga el programa.
//
// Es tambien donde mas se separan los lenguajes: Go, Java y Python estan
// OBLIGADOS a entregar la memoria puesta a cero, o sea que tocan las 4096
// paginas quieran o no; C, C++ y Rust-crudo solo tocan lo que el programa
// escribe.  Esa diferencia no es un sesgo del banco: es lo que cuesta la
// garantia de seguridad de esos lenguajes, y a este tamano se ve.
//
// Ver `alloc_small/main.c` para el porque de la ventana de vivos.
#include <stdlib.h>

#define TAM    (16 * 1024 * 1024)
#define ITERS  100
#define VIVOS  4          /* potencia de 2; 4 x 16 MB = 64 MB vivos */
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
