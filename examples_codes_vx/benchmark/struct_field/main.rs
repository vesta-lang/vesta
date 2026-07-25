// struct_field: 30M field read/write.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o struct_field_rust
use std::hint::black_box;
use std::process::exit;

struct Vec3 { x: i32, y: i32, z: i32 }

fn main() {
    let mut v = Vec3 { x: 1, y: 2, z: 3 };
    let mut sum: i32 = 0;
    let bound: i32 = black_box(30_000_000);
    let mut i: i32 = 0;
    while i < bound {
        v.x = v.x.wrapping_add(1);
        v.y = v.y.wrapping_add(2);
        v.z = v.z.wrapping_add(3);
        sum = sum.wrapping_add(v.x).wrapping_add(v.y).wrapping_add(v.z);
        i += 1;
    }
    exit(sum);
}
