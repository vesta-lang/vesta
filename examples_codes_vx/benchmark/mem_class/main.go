// mem_class: 1M objetos en heap, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; ver alli en que se diferencia este banco de los
// de bloque crudo (`alloc_small` y companeros) y por que la ventana.
//
// Ya no hace falta la global de sumidero: los objetos escapan porque viven en
// el anillo 64 iteraciones y su contenido acaba en el codigo de salida.  El
// trabajo no se elimina porque hace falta, no porque se le haya puesto una
// barrera delante.
package main

import "os"

const iters = 1000000
const vivos = 64 // potencia de 2

// Foo es el objeto de un solo campo (equivalente al struct de C).
type Foo struct{ x int32 }

func main() {
	var anillo [vivos]*Foo

	var acc int32 = 0
	for i := int32(0); i < iters; i++ {
		f := new(Foo)
		f.x = i & 0xFF
		k := int(i) & (vivos - 1)
		if anillo[k] != nil { // el mas viejo sale de la ventana
			acc += anillo[k].x
		}
		anillo[k] = f
	}
	for k := 0; k < vivos; k++ { // vaciar la ventana
		if anillo[k] != nil {
			acc += anillo[k].x
		}
	}
	os.Exit(int(acc & 0xFF))
}
