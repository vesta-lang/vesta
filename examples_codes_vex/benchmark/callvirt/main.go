// callvirt: 30M virtual-call trivial via puntero a funcion (equivalente al
// puntero a funcion en el struct de C).
package main

import "os"

// Counter modela el despacho indirecto: 'inc' es un valor-funcion.
type Counter struct {
	inc   func(*Counter) int32
	value int32
}

func incImpl(c *Counter) int32 { return 1 }

func main() {
	c := Counter{value: 0, inc: incImpl}
	var sum int32 = 0
	var i int32 = 0
	for i < 30000000 {
		sum += c.inc(&c)
		i++
	}
	os.Exit(int(sum & 0xFF))
}
