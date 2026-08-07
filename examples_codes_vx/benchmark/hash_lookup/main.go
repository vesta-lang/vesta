// hash_lookup: 50M mezclas estilo FNV, con el resultado consumido.
// Mismo algoritmo que main.c; ver alli los DOS problemas que tenia.
package main

import "os"

const iters = 50000000

func main() {
	var seed uint64 = 0xCAFEBABEDEADBEEF
	var acc uint64 = 0
	for i := int32(0); i < iters; i++ {
		seed ^= uint64(i)
		seed *= 1099511628211
		seed >>= 7
		seed |= 1
		acc += seed & 0xFF
	}
	os.Exit(int(acc % 251))
}
