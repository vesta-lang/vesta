// jit_method: 30M (3 * 10M) sum-of-i en metodo.  Se usa int32 para replicar
// el wraparound de 'int' de C en la acumulacion.
package main

import "os"

func runImpl(n int32) int32 {
	var sum int32 = 0
	var i int32 = 0
	for i < n {
		sum += i
		i++
	}
	return sum
}

func main() {
	a := runImpl(10000000)
	b := runImpl(10000000)
	c := runImpl(10000000)
	os.Exit(int(a+b+c) & 0xFF)
}
