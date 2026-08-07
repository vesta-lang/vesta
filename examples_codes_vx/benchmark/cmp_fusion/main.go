// cmp_fusion: 50M comparaciones + salto, con el resultado consumido.
// Mismo algoritmo que main.c; ver alli por que la condicion tiene que ser
// impredecible en compilacion.
package main

import "os"

const iters = 50000000

func main() {
	var s uint32 = 12345
	var acc int32 = 0
	for i := int32(0); i < iters; i++ {
		s = s*1664525 + 1013904223
		if (s >> 31) == 0 {
			acc++
		}
	}
	os.Exit(int(acc & 0xFF))
}
