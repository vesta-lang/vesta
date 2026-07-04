// callvirt_hot: 10M virtual-call hot con estado.
package main

import "os"

// C modela el despacho indirecto con estado mutable en el receptor.
type C struct {
	inc func(*C, int32) int32
	n   int32
}

func incImpl(c *C, d int32) int32 {
	c.n += d
	return c.n
}

func main() {
	c := C{n: 0, inc: incImpl}
	var sum int32 = 0
	var i int32 = 0
	for i < 10000000 {
		sum = c.inc(&c, 1)
		i++
	}
	os.Exit(int(sum & 0xFF))
}
