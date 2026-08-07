// mem_struct: 1M iteraciones x 3 caminos (pila, heap por sizeof, heap por
// tamano explicito), con ventana de 64 vivos en los dos de heap.
//
// La version anterior calculaba los tres y sumaba el resultado, pero ese
// resultado no dependia de nada: `gcc -O3` resolvia el bucle en forma cerrada y
// el `main` quedaba en `movl $21568, %eax; ret`.  Cero reservas, cero
// iteraciones -- el banco medida el arranque del proceso.
//
// Que compara: el mismo struct de dos campos puesto en tres sitios distintos.
// El de la pila deberia ser practicamente gratis en cualquier compilador que
// haga promocion a registros; los dos de heap pagan el asignador.  Lo
// interesante es la DIFERENCIA entre los tres, y para que exista los tres
// tienen que ejecutarse de verdad.
//
// Ver `alloc_small/main.c` para el porque de la ventana de vivos.  El valor
// base de cada vuelta sale del acumulador de la anterior, asi que la cadena no
// se puede resolver de antemano.
#include <stdlib.h>

#define ITERS 1000000
#define VIVOS 64          /* potencia de 2 */

typedef struct { int x; int y; } Punto;

int main(void) {
    Punto *anillo_a[VIVOS];   /* camino 2: malloc(sizeof(Punto)) */
    Punto *anillo_b[VIVOS];   /* camino 3: malloc(8) */
    for (int k = 0; k < VIVOS; k++) { anillo_a[k] = NULL; anillo_b[k] = NULL; }

    long long acc = 0;
    for (int i = 0; i < ITERS; i++) {
        int base = (int)(acc & 0xFF);   /* depende de la vuelta anterior */

        /* 1. en la pila */
        Punto p;
        p.x = base;
        p.y = base + 1;
        acc += p.x + p.y;

        int k = i & (VIVOS - 1);

        /* 2. en heap, tamano por sizeof */
        Punto *h = (Punto*)malloc(sizeof(Punto));
        h->x = base; h->y = base + 1;
        if (anillo_a[k] != NULL) {
            acc += anillo_a[k]->x + anillo_a[k]->y;
            free(anillo_a[k]);
        }
        anillo_a[k] = h;

        /* 3. en heap, tamano explicito */
        Punto *m = (Punto*)malloc(8);
        m->x = base; m->y = base + 1;
        if (anillo_b[k] != NULL) {
            acc += anillo_b[k]->x + anillo_b[k]->y;
            free(anillo_b[k]);
        }
        anillo_b[k] = m;
    }
    for (int k = 0; k < VIVOS; k++) {   /* vaciar las dos ventanas */
        if (anillo_a[k] != NULL) {
            acc += anillo_a[k]->x + anillo_a[k]->y; free(anillo_a[k]);
        }
        if (anillo_b[k] != NULL) {
            acc += anillo_b[k]->x + anillo_b[k]->y; free(anillo_b[k]);
        }
    }
    return (int)(acc % 251);
}
