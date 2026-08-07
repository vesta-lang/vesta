// mem_class: 1M objetos en heap, con una ventana de 64 vivos.
//
// Que mide, y en que se diferencia de `alloc`.  Aqui se pide el OBJETO
// idiomatico de cada lenguaje -- con su cabecera, su constructor y su registro
// en el recolector si lo hay --, no un bloque crudo.  El bloque crudo tiene su
// propio banco en `alloc`, y mezclarlos no compara asignadores sino modelos de
// objeto.  C no tiene objetos: lo mas cercano es un struct con `malloc`, que es
// justo la referencia contra la que se quiere comparar el resto.
//
// La ventana de 64 vivos hace irreducible el trabajo.  Con el objeto naciendo y
// muriendo en la misma iteracion, `gcc -O3` demostraba que nadie lo observa y
// borraba el par malloc/free ENTERO: el `main` quedaba en
// `movl $16960, %eax; ret`.  Con las vidas cruzadas no hay par que emparejar, y
// con el resultado en el valor de retorno no hay bucle que cerrar en formula.
// El trabajo no se elimina porque hace falta, no porque se le haya puesto una
// barrera delante -- que ademas seria un truco distinto en cada compilador.
#include <stdlib.h>

#define ITERS 1000000
#define VIVOS 64          /* potencia de 2: el indice sale con una mascara */

typedef struct { int x; } Foo;

int main(void) {
    Foo *anillo[VIVOS];
    for (int k = 0; k < VIVOS; k++) anillo[k] = NULL;

    int acc = 0;
    for (int i = 0; i < ITERS; i++) {
        Foo *f = (Foo*)malloc(sizeof(Foo));
        f->x = i & 0xFF;
        int k = i & (VIVOS - 1);
        if (anillo[k] != NULL) {      /* el mas viejo sale de la ventana */
            acc += anillo[k]->x;
            free(anillo[k]);
        }
        anillo[k] = f;
    }
    for (int k = 0; k < VIVOS; k++) { /* vaciar la ventana */
        if (anillo[k] != NULL) { acc += anillo[k]->x; free(anillo[k]); }
    }
    return acc & 0xFF;
}
