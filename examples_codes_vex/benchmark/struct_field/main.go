// struct_field: 30M field read/write.  Se usa int32 para replicar el
// wraparound de 'int' de C en la acumulacion.
package main

import "os"

type Vec3 struct {
	x, y, z int32
}

func main() {
	v := Vec3{x: 1, y: 2, z: 3}
	var sum int32 = 0
	var i int32 = 0
	for i < 30000000 {
		v.x += 1
		v.y += 2
		v.z += 3
		sum += v.x + v.y + v.z
		i++
	}
	os.Exit(int(sum) & 0xFF)
}
