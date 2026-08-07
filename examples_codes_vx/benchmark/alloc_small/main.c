// alloc_small: 5M bloques de 16 bytes, con una ventana de 64 vivos.
//
// Uno de los cuatro bancos de reserva (small / medium / large / huge).  Son
// bancos SEPARADOS a proposito: el asignador se comporta de forma distinta en
// cada escala -- el pequeno vive de la cache por hilo, el grande recicla
// bloques ya comprometidos y el enorme lo domina el fallo de pagina -- y
// meterlos en un solo numero borraria justo eso.  Este mide el caso en el que
// manda el COSTE POR LLAMADA: cinco millones de ellas.
//
// Que se pide: el bloque de heap mas directo que tiene cada lenguaje.  Los
// objetos con cabecera tienen su propio banco en `mem_class`.  Go, Java y
// Python NO tienen bloque crudo -- lo mas cercano que saben pedir ya lleva
// cabecera y ademas viene puesto a cero --, y eso es una propiedad suya, no una
// eleccion del banco de pruebas.
//
// Por que se TOCA una vez cada pagina: `malloc` solo RESERVA.  Sin tocar, cien
// bloques de 16 MB "costaban" medio milisegundo porque los 1600 MB no llegaban
// a existir nunca.  Tocando, se mide conseguir memoria USABLE, que es lo que
// paga el programa de verdad; y de paso iguala el terreno con los lenguajes que
// estan obligados a poner a cero lo que reservan, que ya tocan cada pagina.
//
// Por que una ventana de 64 vivos y no reservar-y-liberar en la misma
// iteracion: con el bloque muriendo donde nace, `gcc -O3` demuestra que nadie
// lo observa y borra el par malloc/free ENTERO -- el `main` quedaba en un
// `return` de una constante y lo que se media era el arranque del proceso.  Con
// las vidas cruzadas no hay par que emparejar, y con el resultado en el valor
// de retorno no hay bucle que cerrar en formula.  El trabajo no se elimina
// porque hace falta, no porque se le haya puesto delante una barrera -- que
// ademas seria un truco distinto en cada compilador, y era la razon de que
// hasta ahora unos lenguajes ejecutaran el bucle y otros no.
#include <stdlib.h>

#define TAM    16
#define ITERS  5000000
#define VIVOS  64         /* potencia de 2: el indice sale con una mascara */
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
