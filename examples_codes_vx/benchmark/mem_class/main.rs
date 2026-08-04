// mem_class: 1M heap-alloc class equiv (via Box).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o mem_class_rust
use std::hint::black_box;
use std::process::exit;

struct Foo { x: i32 }

fn helper(i: i32) -> i32 {
    let f = Box::new(Foo { x: i });
    let r = f.x;
    black_box(&f); // fuerza asignacion + liberacion real (equiv. malloc/free)
    r
}

fn main() {
    let mut sum: i64 = 0;
    let mut i: i64 = 0;
    while i < 1_000_000 {
        sum += helper(black_box(1)) as i64;
        i += 1;
    }
    exit((sum & 0xFFFF) as i32);
}
