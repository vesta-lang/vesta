// pic_real: 3M iter, array de 3 shapes alternados.
package main

import "os"

type Shape struct {
	kind, r, w, h, b int32
}

func area(s *Shape) int32 {
	if s.kind == 0 {
		return s.r * s.r * 3
	} else if s.kind == 1 {
		return s.w * s.h
	}
	return (s.b * s.h) / 2
}

func main() {
	var shapes [3]Shape
	shapes[0] = Shape{kind: 0, r: 5, w: 0, h: 0, b: 0}
	shapes[1] = Shape{kind: 1, r: 0, w: 4, h: 6, b: 0}
	shapes[2] = Shape{kind: 2, r: 0, w: 0, h: 8, b: 3}
	var sum int32 = 0
	var i int32 = 0
	for i < 3000000 {
		sum += area(&shapes[i%3])
		i++
	}
	os.Exit(int(sum) & 0xFF)
}
