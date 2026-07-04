// Bench: aritmetica entera mixta intensiva (20M iter x 10 ops).
package main

import "os"

func main() {
	var a, b, c, d int64 = 1, 2, 3, 5
	var bound int32 = 20000000
	for i := int32(0); i < bound; i++ {
		a += int64(i)
		b -= 1
		c *= 3
		d += a ^ b
		a &= 0xFFFFFFFF
		b |= 1
		c >>= 1
		d ^= a
		a += b
		c += d
	}
	os.Exit(int((a + b + c + d) & 0xFF))
}
