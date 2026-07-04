// Bench: suma de array i32 (100K x 200 pasadas).
package main

import "os"

func main() {
	arr := make([]int32, 100000)
	for i := int32(0); i < 100000; i++ {
		arr[i] = i
	}
	var sum int64 = 0
	var passes int32 = 200
	for p := int32(0); p < passes; p++ {
		for i := int32(0); i < 100000; i++ {
			sum += int64(arr[i])
		}
	}
	os.Exit(int(sum & 0xFF))
}
