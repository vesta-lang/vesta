// Bench: axpy compound element-wise sobre arrays f64.
// Hot loop interno a[i] = a[i]*0.5 + b[i] (mul + add), M pasadas.
// Go (gc) NO auto-vectoriza este bucle, a diferencia de gcc -O3.
package main

import "os"

func main() {
	N := 4096
	M := 50000
	a := make([]float64, N)
	b := make([]float64, N)
	for i := 0; i < N; i++ {
		a[i] = float64(i%7) + 1.0
		b[i] = float64(i%13) + 1.0
	}
	for p := 0; p < M; p++ {
		for i := 0; i < N; i++ {
			a[i] = a[i]*0.5 + b[i]
		}
	}
	r := a[N/2]
	os.Exit(int(int64(r) & 0xFF))
}
