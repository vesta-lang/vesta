// mem_struct: 1M iter * 3 paths (stack, heap, malloc).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o mem_struct_rust
use std::hint::black_box;
use std::process::exit;

struct Punto { x: i32, y: i32 }

// Path 1: struct en pila.
fn stack_struct(base: i32) -> i32 {
    let p = Punto { x: base, y: base + 1 };
    p.x + p.y
}

// Path 2: struct en heap (Box == malloc + free).
fn heap_struct(base: i32) -> i32 {
    let p = Box::new(Punto { x: base, y: base + 1 });
    let r = p.x + p.y;
    black_box(&p);
    r
}

// Path 3: identico al 2 (el main.c usa malloc(8) crudo; Box<Punto> es 8 bytes).
fn malloc_struct(base: i32) -> i32 {
    let p = Box::new(Punto { x: base, y: base + 1 });
    let r = p.x + p.y;
    black_box(&p);
    r
}

fn main() {
    let mut sum: i64 = 0;
    let mut i: i64 = 0;
    while i < 1_000_000 {
        sum += stack_struct(black_box(1)) as i64;
        sum += heap_struct(black_box(1)) as i64;
        sum += malloc_struct(black_box(1)) as i64;
        i += 1;
    }
    exit((sum & 0xFFFF) as i32);
}
