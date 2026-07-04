// Bench: bitops (and/or/xor/shl/shr).  30M iter x 8 ops.
package main

import "os"

func main() {
	var a uint64 = 0xDEADBEEFCAFEBABE
	var b uint64 = 0x1234567890ABCDEF
	var bound int32 = 30000000
	for i := int32(0); i < bound; i++ {
		a ^= uint64(i)
		a &= 0xFFFFFFFFFFFF
		a |= 0x1010101010101
		a <<= 1
		a >>= 1
		b += a & 0xFFFF
		b ^= a >> 16
		b <<= 1
	}
	os.Exit(int((a + b) & 0xFF))
}
