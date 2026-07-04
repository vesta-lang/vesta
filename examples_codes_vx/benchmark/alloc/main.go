// alloc: 5M heap-alloc trivial.  Equivalente al malloc/free de C via el
// recolector de basura de Go.  Se fuerza el escape del objeto a heap (sink
// global) para que Go NO lo asigne en pila y realice el mismo trabajo de
// asignacion que C.
package main

import "os"

// Foo es un objeto trivial de un solo campo (equivalente al struct de C).
type Foo struct{ x int32 }

// sink obliga a que 'f' escape al heap (asignacion real por iteracion).
var sink *Foo

// helper asigna un Foo, lo inicializa y devuelve su campo (siempre 0).
func helper() int32 {
	f := new(Foo)
	f.x = 0
	sink = f
	r := f.x
	return r
}

func main() {
	var i int32 = 0
	var acc int32 = 0
	for i < 5000000 {
		acc += helper()
		i++
	}
	// C devuelve 'i' (5000000); acc es 0 pero mantiene vivo el bucle.
	os.Exit(int((i + acc) & 0xFF))
}
