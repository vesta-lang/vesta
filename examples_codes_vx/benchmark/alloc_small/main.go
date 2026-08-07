// alloc_small: 5M bloques de 16 bytes, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; ver alli por que hay cuatro bancos de reserva,
// por que se toca cada pagina y por que la ventana.
//
// Go NO tiene bloque crudo: `make([]byte, n)` pide al recolector y devuelve la
// memoria PUESTA A CERO, obligatoriamente.  O sea que ademas de reservar paga
// borrar, y no hay forma de pedirle que no lo haga.  Es una propiedad del
// lenguaje, no una eleccion del banco de pruebas, y por eso queda dicho aqui:
// en los tamanos grandes ese borrado es la mayor parte del coste.
package main

import "os"

const tam = 16
const iters = 5000000
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
