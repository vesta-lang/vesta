// Bench: string workout AGRESIVO (version Go).
//
// Diseno equivalente al .c: 1M iter, 6 ops string por iter.  Usa buffers
// manuales ([16]byte), copias, comparaciones y longitud por escaneo (como
// strlen/strcmp de C).  El fnv1a del .c es codigo muerto (no se invoca) y se
// omite aqui.
package main

import "os"

// strlenB escanea hasta el NUL (equivalente a strlen de C).
func strlenB(b []byte) int {
	i := 0
	for i < len(b) && b[i] != 0 {
		i++
	}
	return i
}

// eqStr compara el buffer (hasta NUL) con la cadena fija s.
func eqStr(b []byte, s string) bool {
	n := strlenB(b)
	if n != len(s) {
		return false
	}
	for i := 0; i < n; i++ {
		if b[i] != s[i] {
			return false
		}
	}
	return true
}

// eqBuf compara dos buffers (cada uno hasta su NUL).
func eqBuf(a, b []byte) bool {
	na := strlenB(a)
	nb := strlenB(b)
	if na != nb {
		return false
	}
	for i := 0; i < na; i++ {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func main() {
	var sum, hits1, hits2 int64
	var bound int32 = 1000000
	for i := int32(0); i < bound; i++ {
		var bufA [16]byte
		bufA[0] = byte(65 + (i & 7))
		bufA[1] = byte(65 + ((i >> 3) & 7))
		bufA[2] = byte(65 + ((i >> 6) & 7))
		bufA[3] = byte(65 + ((i >> 9) & 7))
		bufA[4] = 0

		var bufB [16]byte
		bufB[0] = byte(48 + (i % 10))
		bufB[1] = byte(48 + ((i / 10) % 10))
		bufB[2] = 0

		// OP1: concat A + B -> c (6 chars).
		var c [16]byte
		copy(c[0:4], bufA[0:4])
		copy(c[4:6], bufB[0:2])
		c[6] = 0

		// OP2: length.
		ln := strlenB(c[:])
		sum += int64(ln)

		// OP3: equals contra patron fijo.
		if eqStr(c[:], "AAAA00") {
			hits1++
		}

		// OP5: substring (a[0:3]).
		var sub [16]byte
		copy(sub[0:3], bufA[0:3])
		sub[3] = 0

		// OP6: equals contra patron variable.
		var bufQ [16]byte
		bufQ[0] = byte(65 + ((i / 100) & 7))
		bufQ[1] = byte(65 + ((i / 800) & 7))
		bufQ[2] = byte(65 + ((i / 6400) & 7))
		bufQ[3] = 0
		if eqBuf(sub[:], bufQ[:]) {
			hits2++
		}

		// Final: concat sub + B = 5 char string, suma length.
		var c2 [16]byte
		copy(c2[0:3], sub[0:3])
		copy(c2[3:5], bufB[0:2])
		c2[5] = 0
		sum += int64(strlenB(c2[:]))
	}

	r := sum + hits1*1000 + hits2*7
	os.Exit(int(r&0x7FFFFFFF) & 0xFF)
}
