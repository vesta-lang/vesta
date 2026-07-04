// Bench: hash lookup simulado (FNV-style int ops).
package main

import "os"

func main() {
	var seed uint64 = 0xCAFEBABEDEADBEEF
	var acc uint64 = 0
	var bound int32 = 50000000
	for i := int32(0); i < bound; i++ {
		seed ^= uint64(i)
		seed *= 1099511628211
		seed >>= 7
		seed |= 1
		if seed&7 == 0 {
			acc++
		}
	}
	os.Exit(int(acc & 0xFF))
}
