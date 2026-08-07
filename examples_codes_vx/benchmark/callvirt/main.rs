// callvirt: 30M llamadas indirectas a traves de un puntero a funcion.
// Mismo algoritmo que main.c; ver alli donde esta la linea entre optimizar y
// fabricar el resultado sin ejecutar.
//
// Se usa un fn-pointer en el struct, que es el analogo exacto del
// `int (*inc)(Counter*)` de C (un trait object anñadiria una vtable distinta).
// Ya no hace falta `black_box`: la cadena congruencial impide la forma cerrada.
// Compilar: rustc -O -C target-cpu=native main.rs -o callvirt_rust
use std::process::exit;

struct Counter {
    inc: fn(&Counter) -> i32,
    value: i32,
}

fn inc_impl(c: &Counter) -> i32 { c.value + 1 }

fn main() {
    let mut c = Counter { value: 0, inc: inc_impl };
    let mut sum: i64 = 0;
    for _ in 0..30_000_000i32 {
        let t = (c.inc)(&c) as u32;
        let t = t.wrapping_mul(1664525).wrapping_add(1013904223);
        c.value = (t & 0xFF) as i32;
        sum += c.value as i64;
    }
    exit((sum % 251) as i32);
}
