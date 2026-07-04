// obj_accum: objeto mutable de 4 campos, RMW + 2 escrituras condicionales en
// un loop de 20M.
package main

import "os"

type Stats struct {
	sum, cnt, mn, mx int64
}

func main() {
	var s Stats
	s.sum = 0
	s.cnt = 0
	s.mn = 2000000000
	s.mx = -2000000000
	var seed int64 = 12345
	var i int64 = 0
	for i < 20000000 {
		seed = (seed*1103515245 + 12345) & 2147483647
		v := seed % 1000
		s.sum += v
		s.cnt += 1
		if v < s.mn {
			s.mn = v
		}
		if v > s.mx {
			s.mx = v
		}
		i++
	}
	os.Exit(int((s.sum+s.cnt+s.mn+s.mx)%1000000) & 0xFF)
}
