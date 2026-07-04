// Bench: branches no predecibles (xorshift) + 4 branches.
package main

import "os"

func main() {
	var rng uint64 = 1
	var a, b, c, d int64 = 0, 0, 0, 0
	var bound int32 = 10000000
	for i := int32(0); i < bound; i++ {
		rng ^= rng << 13
		rng ^= rng >> 7
		rng ^= rng << 17
		if rng&1 != 0 {
			a++
		} else {
			a--
		}
		if rng&2 != 0 {
			b++
		} else {
			b--
		}
		if rng&4 != 0 {
			c++
		} else {
			c--
		}
		if rng&8 != 0 {
			d++
		} else {
			d--
		}
	}
	os.Exit(int((a + b + c + d) & 0xFF))
}
