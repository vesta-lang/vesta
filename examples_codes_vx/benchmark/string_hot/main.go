// Bench: strings en hot loop (concat + length + compare).
// El C usa snprintf; se replica con fmt.Sprintf (construccion formateada por
// iteracion, sin plegado a constante) para un trabajo equivalente.
package main

import (
	"fmt"
	"os"
)

func main() {
	base := "abc"
	suffix := "xyz"
	var hits int64 = 0
	var bound int32 = 200000
	for i := int32(0); i < bound; i++ {
		buf := fmt.Sprintf("%s%s", base, suffix)
		ln := len(buf)
		if ln == 6 {
			hits++
		}
		if buf == "abcxyz" {
			hits++
		}
	}
	os.Exit(int(hits & 0xFF))
}
