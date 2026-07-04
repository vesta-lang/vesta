// Bench: quicksort Lomuto sobre array i32 de 100K.
package main

import "os"

func partition(arr []int32, lo, hi int32) int32 {
	pivot := arr[hi]
	i := lo - 1
	for j := lo; j < hi; j++ {
		if arr[j] <= pivot {
			i++
			arr[i], arr[j] = arr[j], arr[i]
		}
	}
	arr[i+1], arr[hi] = arr[hi], arr[i+1]
	return i + 1
}

func qsortRec(arr []int32, lo, hi int32) {
	if lo < hi {
		p := partition(arr, lo, hi)
		qsortRec(arr, lo, p-1)
		qsortRec(arr, p+1, hi)
	}
}

func main() {
	const N = 100000
	arr := make([]int32, N)
	var seed uint64 = 12345
	for i := int32(0); i < N; i++ {
		seed = seed * 6364136223846793005
		seed = seed + 1442695040888963407
		arr[i] = int32((seed >> 33) & 0xFFFFFFFF)
	}
	qsortRec(arr, 0, N-1)
	r := arr[N/2] & 0xFF
	os.Exit(int(r))
}
