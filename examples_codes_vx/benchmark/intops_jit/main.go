// intops_jit: 5M iter, imin/imax/abs.
package main

import "os"

func absL(x int64) int64 {
	if x < 0 {
		return -x
	}
	return x
}

func minL(a, b int64) int64 {
	if a < b {
		return a
	}
	return b
}

func maxL(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

func main() {
	var acc int64 = 0
	i := 1
	for i < 5000000 {
		a := int64(i)
		b := int64(i + 7)
		acc += minL(a, b)
		acc += maxL(a, b)
		acc += absL(a - 5000)
		i++
	}
	os.Exit(int((acc & 0xFFFF) & 0xFF))
}
