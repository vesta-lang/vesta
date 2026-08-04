// callvirt: 30M virtual-call trivial via puntero a funcion.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o callvirt_rust
//
// El main.c usa un puntero a funcion en un struct (despacho indirecto).  Aqui
// se usa un fn-pointer en el struct para reproducir la MISMA llamada indirecta
// (no un trait object, que anñadiria una vtable distinta; el fn-ptr es el
// analogo exacto del `int (*inc)(Counter*)` de C).
use std::hint::black_box;
use std::process::exit;

struct Counter {
    inc: fn(&Counter) -> i32,
    value: i32,
}

fn inc_impl(_c: &Counter) -> i32 { 1 }

fn main() {
    let c = Counter { value: black_box(0), inc: inc_impl };
    let mut sum: i32 = 0;
    let mut i: i32 = 0;
    while i < 30_000_000 {
        sum = sum.wrapping_add((c.inc)(&c));
        i += 1;
    }
    exit(sum);
}
