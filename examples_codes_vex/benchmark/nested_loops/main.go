// Bench: nested loops (matrix-like).  500 x 500 x 200 = 50M ops.
package main

import "os"

func main() {
	var sum int64 = 0
	var bi, bj, bk int32 = 500, 500, 200
	for i := int32(0); i < bi; i++ {
		for j := int32(0); j < bj; j++ {
			for k := int32(0); k < bk; k++ {
				sum += int64(i + j + k)
			}
		}
	}
	os.Exit(int(sum & 0xFF))
}
