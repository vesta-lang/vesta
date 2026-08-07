// callvirt: 30M llamadas indirectas a traves de un valor-funcion.
// Mismo algoritmo que main.c; ver alli donde esta la linea entre optimizar y
// fabricar el resultado sin ejecutar.
//
// Se usa un valor-funcion en el struct, que es el analogo exacto del
// `int (*inc)(Counter*)` de C (una interfaz anñadiria una vtable distinta).
package main

import "os"

// Counter modela el despacho indirecto: 'inc' es un valor-funcion.
type Counter struct {
	inc   func(*Counter) int32
	value int32
}

func incImpl(c *Counter) int32 { return c.value + 1 }

func main() {
	c := Counter{value: 0, inc: incImpl}
	var sum int64 = 0
	for i := int32(0); i < 30000000; i++ {
		t := uint32(c.inc(&c))*1664525 + 1013904223
		c.value = int32(t & 0xFF)
		sum += int64(c.value)
	}
	os.Exit(int(sum % 251))
}
