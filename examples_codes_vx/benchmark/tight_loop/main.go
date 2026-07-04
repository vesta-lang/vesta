// Bench: tight loop aritmetico simple.  50M iteraciones de suma acumulada.
package main

import "os"

func main() {
	var acc int64 = 0
	var bound int32 = 50000000
	for i := int32(0); i < bound; i++ {
		acc += int64(i)
	}
	os.Exit(int((acc & 0xFFFFFFFF) & 0xFF))
}
