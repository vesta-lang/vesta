// Bench: aritmetica f64 intensiva. 5M iter x 6 ops FP.
package main

import (
	"math"
	"os"
)

func main() {
	acc := 0.0
	var bound int32 = 5000000
	for i := int32(0); i < bound; i++ {
		x := float64(i)
		s := math.Sqrt(x)
		a := math.Abs(s - 1000.0)
		m := math.Min(a, 999.0)
		M := math.Max(s, 1.0)
		f := math.Floor(M)
		c := math.Ceil(f)
		acc += m + c
	}
	os.Exit(int(int64(acc) & 0xFF))
}
