// Bench: state machine 8 estados (lexer-like).
package main

import "os"

func main() {
	var state int32 = 0
	var counts int64 = 0
	var rng uint64 = 7
	var bound int32 = 10000000
	for i := int32(0); i < bound; i++ {
		rng = rng * 6364136223846793005
		rng = rng + 1442695040888963407
		b := uint8((rng >> 33) & 0xFF)
		if state == 0 {
			if b < 32 {
				state = 1
			} else {
				state = 2
			}
		} else if state == 1 {
			if b < 64 {
				state = 3
			} else {
				state = 4
			}
		} else if state == 2 {
			if b < 96 {
				state = 5
			} else {
				state = 6
			}
		} else if state == 3 {
			state = 7
		} else if state == 4 {
			state = 7
		} else if state == 5 {
			state = 0
			counts++
		} else if state == 6 {
			state = 0
			counts++
		} else {
			state = 0
		}
	}
	os.Exit(int(counts&0xFFFF) & 0xFF)
}
