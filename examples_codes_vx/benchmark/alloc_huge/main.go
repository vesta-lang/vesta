// alloc_huge: 100 bloques de 16 MB, con una ventana de 4 vivos.
// Mismo algoritmo que main.c; ver alli por que a este tamano lo que se mide es
// el fallo de pagina y no la llamada al asignador.
//
// Go NO tiene bloque crudo: `make([]byte, n)` devuelve la memoria puesta a
// cero, obligatoriamente.  A este tamano eso son 16 MB borrados por iteracion,
// o sea que toca las 4096 paginas quiera o no.  Es una propiedad del lenguaje
// -- el precio de su garantia de seguridad -- y este banco existe para que se
// vea.
package main

import "os"

const tam = 16 * 1024 * 1024
const iters = 100
const vivos = 4 // potencia de 2
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
