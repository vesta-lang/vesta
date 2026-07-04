// Bench: Fibonacci recursivo profundo. fib(32) = 2178309.
package main

import "os"

func fib(n int64) int64 {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	var in int64 = 32
	r := fib(in)
	os.Exit(int(r & 0xFF))
}
