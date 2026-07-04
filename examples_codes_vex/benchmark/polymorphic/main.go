// polymorphic: 10M, 3 subtipos via union etiquetada.
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
	c := Shape{kind: 0, r: 5, w: 0, h: 0, b: 0}
	r := Shape{kind: 1, r: 0, w: 4, h: 6, b: 0}
	t := Shape{kind: 2, r: 0, w: 0, h: 8, b: 3}
	var sum int32 = 0
	var i int32 = 0
	for i < 10000000 {
		mod := i % 3
		if mod == 0 {
			sum += area(&c)
		} else if mod == 1 {
			sum += area(&r)
		} else {
			sum += area(&t)
		}
		i++
	}
	os.Exit(int(sum) & 0xFF)
}
