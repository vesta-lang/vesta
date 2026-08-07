// mem_struct: 1M iteraciones x 3 caminos (pila, heap por sizeof, heap por
// tamano explicito), con ventana de 64 vivos en los dos de heap.
// Mismo algoritmo que main.c; ver alli que compara y por que la ventana.
//
// En Go los dos caminos de heap son `new(Punto)`: el lenguaje no distingue
// entre pedir por tamano de tipo o por bytes.  El del "tamano explicito" se
// conserva para que la carga sea la misma que en los otros seis.
package main

import "os"

const iters = 1000000
const vivos = 64 // potencia de 2

// Punto es el struct de dos campos que se pone en los tres sitios.
type Punto struct {
	x int32
	y int32
}

func main() {
	var anilloA [vivos]*Punto
	var anilloB [vivos]*Punto

	var acc int64 = 0
	for i := int32(0); i < iters; i++ {
		base := int32(acc & 0xFF)

		p := Punto{x: base, y: base + 1} // 1. en la pila
		acc += int64(p.x + p.y)

		k := int(i) & (vivos - 1)

		h := new(Punto) // 2. en heap
		h.x = base
		h.y = base + 1
		if anilloA[k] != nil {
			acc += int64(anilloA[k].x + anilloA[k].y)
		}
		anilloA[k] = h

		m := new(Punto) // 3. en heap
		m.x = base
		m.y = base + 1
		if anilloB[k] != nil {
			acc += int64(anilloB[k].x + anilloB[k].y)
		}
		anilloB[k] = m
	}
	for k := 0; k < vivos; k++ {
		if anilloA[k] != nil {
			acc += int64(anilloA[k].x + anilloA[k].y)
		}
		if anilloB[k] != nil {
			acc += int64(anilloB[k].x + anilloB[k].y)
		}
	}
	os.Exit(int(acc % 251))
}
