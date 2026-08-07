// alloc_medium: 4M bloques de 1 KB, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; el porque del diseno esta en
// `alloc_small/main.c`.
//
// Go NO tiene bloque crudo: `make([]byte, n)` devuelve la memoria puesta a
// cero, obligatoriamente, asi que ademas de reservar paga borrar 1 KB por
// iteracion.  Es una propiedad del lenguaje, no una eleccion del banco.
package main

import "os"

const tam = 1024
const iters = 4000000
const vivos = 64 // potencia de 2
const pagina = 4096

func main() {
	anillo := make([][]byte, vivos)

	var acc int32 = 0
	for i := int32(0); i < iters; i++ {
		p := make([]byte, tam)
		v := byte(i & 0xFF)
		for o := 0; o < tam; o += pagina {
			p[o] = v
		}
		p[tam-1] = v
		k := int(i) & (vivos - 1)
		if anillo[k] != nil { // el mas viejo sale de la ventana
			acc += int32(anillo[k][0])
		}
		anillo[k] = p
	}
	for k := 0; k < vivos; k++ { // vaciar la ventana
		if anillo[k] != nil {
			acc += int32(anillo[k][0])
		}
	}
	os.Exit(int(acc & 0xFF))
}
