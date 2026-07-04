// Bench: copia secuencial byte-a-byte (1 MB x 100 iter = 100 MB).
package main

import "os"

func main() {
	const N = 1048576
	src := make([]byte, N)
	dst := make([]byte, N)
	for i := 0; i < N; i++ {
		src[i] = byte(i & 0xFF)
	}
	var bound int32 = 100
	for it := int32(0); it < bound; it++ {
		// Loop manual byte-a-byte (sin copy() vectorizado) para comparar
		// con el mismo bucle trivial de C.
		for j := 0; j < N; j++ {
			dst[j] = src[j]
		}
	}
	os.Exit(int(dst[1234]))
}
