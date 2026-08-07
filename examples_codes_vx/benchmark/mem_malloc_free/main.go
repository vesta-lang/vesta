// mem_malloc_free: 5M bloques de 96 bytes, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; el porque de la ventana esta en
// `alloc_small/main.c`.
//
// Go NO tiene bloque crudo: `make([]byte, n)` pide al recolector y devuelve la
// memoria puesta a cero, obligatoriamente.  Es una propiedad del lenguaje, no
// una eleccion del banco.
package main

import "os"

const tam = 96
const iters = 5000000
const vivos = 64 // potencia de 2

func main() {
	anillo := make([][]byte, vivos)

	var acc int32 = 0
	for i := int32(0); i < iters; i++ {
		buf := make([]byte, tam)
		buf[0] = byte(i)
		buf[tam-1] = byte(int(i) + tam - 1)
		k := int(i) & (vivos - 1)
		if anillo[k] != nil { // el mas viejo sale de la ventana
			acc += int32(anillo[k][0]) + int32(anillo[k][tam-1])
		}
		anillo[k] = buf
	}
	for k := 0; k < vivos; k++ { // vaciar la ventana
		if anillo[k] != nil {
			acc += int32(anillo[k][0]) + int32(anillo[k][tam-1])
		}
	}
	os.Exit(int(acc % 251))
}
