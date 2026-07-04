/*
 * Consumidor C del modulo Vex 205_c_interop.vx (Fase 4 interop C).
 *
 * Demuestra que codigo C llama a funciones Vex sin friccion: incluye el
 * header generado por `vx --emit-header` y enlaza el .c del port-C.
 *
 * Build (lo hace el e2e automaticamente):
 *   vx --vx 205_c_interop.vx --port c --emit-header -o iface
 *   gcc -std=c11 consumer.c iface.c -o consumer && ./consumer
 *
 * El nombre del header lo inyecta el e2e via -DVEX_IFACE_HEADER="..." para no
 * acoplar este fichero a una ruta concreta.
 */

#include <stdio.h>
#include VX_IFACE_HEADER /* el .h generado por vx --emit-header */

/* Una funcion C cualquiera, pasada como callback cfn a Vex. */
static int64_t doble(int64_t x) { return x * 2; }

int main(void) {
    /* Struct Vec2 construido en C, pasado a Vex por puntero (ABI agregados). */
    Vec2 a = {1.0, 2.0};
    Vec2 b = {3.0, 4.0};
    double dot = vec2_dot(&a, &b); /* 1*3 + 2*4 = 11 */

    /* Callback: Vex invoca nuestra funcion C `doble`. */
    int64_t r = apply(doble, 21); /* doble(21) = 42 */

    /* Funcion escalar. */
    int64_t s = add3(10, 20, 12); /* 42 */

    printf("dot=%.1f apply=%lld add3=%lld\n", dot, (long long)r,
           (long long)s);

    /* Codigo de salida = 42 si todo cuadra (lo verifica el e2e). */
    return (dot == 11.0 && r == 42 && s == 42) ? 42 : 1;
}
