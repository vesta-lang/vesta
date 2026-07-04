// cmp_fusion: 50M loop con cmp trivial.
package main

import "os"

func runImpl(n int32) int64 {
	var acc int64 = 0
	var i int32 = 0
	for i < n {
		acc++
		i++
	}
	return acc
}

func main() {
	r := runImpl(50000000)
	os.Exit(int((r & 0xFFFFFFFF) & 0xFF))
}
