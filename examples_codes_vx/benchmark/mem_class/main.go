// mem_class: 1M heap-alloc class equiv.  Se fuerza el escape del objeto a
// heap (sink global) para reproducir el malloc/free de C.
package main

import "os"

type Foo struct{ x int32 }

var sink *Foo

func helper(i int32) int32 {
	f := new(Foo)
	f.x = i
	sink = f
	r := f.x
	return r
}

func main() {
	var sum int64 = 0
	var i int64 = 0
	for i < 1000000 {
		sum += int64(helper(1))
		i++
	}
	os.Exit(int((sum & 0xFFFF) & 0xFF))
}
