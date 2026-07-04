// Bench: 5M alloc + free de bloques pequenos (96 bytes).  Se fuerza el
// escape del buffer a heap (sink global) para reproducir el malloc/free.
package main

import "os"

var sinkB []byte

func doIter(i int32) {
	buf := make([]byte, 96)
	buf[0] = byte(i)
	buf[95] = byte(i + 95)
	sinkB = buf
}

func main() {
	var bound int32 = 5000000
	for i := int32(0); i < bound; i++ {
		doIter(i)
	}
	os.Exit(42)
}
