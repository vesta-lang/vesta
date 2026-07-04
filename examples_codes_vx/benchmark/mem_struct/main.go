// mem_struct: 1M iter * 3 paths (stack, heap, malloc).  Los paths heap/malloc
// fuerzan el escape a heap (sink global); el path stack se mantiene en pila.
package main

import "os"

type Punto struct{ x, y int32 }

var sinkP *Punto

func stackStruct(base int32) int32 {
	var p Punto
	p.x = base
	p.y = base + 1
	return p.x + p.y
}

func heapStruct(base int32) int32 {
	p := new(Punto)
	p.x = base
	p.y = base + 1
	sinkP = p
	r := p.x + p.y
	return r
}

func mallocStruct(base int32) int32 {
	p := new(Punto)
	p.x = base
	p.y = base + 1
	sinkP = p
	r := p.x + p.y
	return r
}

func main() {
	var sum int64 = 0
	var i int64 = 0
	for i < 1000000 {
		sum += int64(stackStruct(1))
		sum += int64(heapStruct(1))
		sum += int64(mallocStruct(1))
		i++
	}
	os.Exit(int((sum & 0xFFFF) & 0xFF))
}
