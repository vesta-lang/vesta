// rotops_jit: 5M iter, rotl/rotr.
package main

import "os"

func rotlU64(v uint64, n uint) uint64 { return (v << n) | (v >> (64 - n)) }
func rotrU64(v uint64, n uint) uint64 { return (v >> n) | (v << (64 - n)) }

func main() {
	var acc uint64 = 0
	i := 1
	for i < 5000000 {
		v := uint64(i)
		acc += rotlU64(v, 7)
		acc += rotrU64(v, 3)
		i++
	}
	os.Exit(int(acc&0xFFFF) & 0xFF)
}
