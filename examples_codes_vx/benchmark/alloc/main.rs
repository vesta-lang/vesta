// alloc: 5M heap-alloc trivial.  Equivalente 1:1 al main.c (malloc/free) y al
// main.go (new + escape).  En Rust se usa Box para forzar una asignacion real
// en heap por iteracion; black_box impide que LLVM elimine el par alloc/free.
// Compilar: rustc -O -C target-cpu=native main.rs -o alloc_rust
use std::hint::black_box;
use std::process::exit;

struct Foo { x: i32 }

fn helper() -> i32 {
    let mut f = Box::new(Foo { x: 0 });
    f.x = black_box(0); // impide que se descarte la asignacion
    let r = f.x;
    black_box(&mut *f); // el Box se libera al salir (equiv. a free)
    r
}

fn main() {
    let mut i: i32 = 0;
    while i < 5_000_000 {
        helper();
        i += 1;
    }
    // C devuelve 'i' (5000000).
    exit(i);
}
