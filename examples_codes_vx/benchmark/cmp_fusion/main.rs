// cmp_fusion: 50M comparaciones + salto, con el resultado consumido.
// Mismo algoritmo que main.c; ver alli por que la condicion tiene que ser
// impredecible en compilacion.
// Compilar: rustc -O -C target-cpu=native main.rs -o cmp_fusion_rust
use std::process::exit;

const ITERS: i32 = 50_000_000;

fn main() {
    let mut s: u32 = 12345;
    let mut acc: i32 = 0;
    for _ in 0..ITERS {
        // `wrapping_*` porque en Rust el desbordamiento es un panico en debug;
        // el resto de lenguajes envuelven, y aqui hay que hacer lo mismo.
        s = s.wrapping_mul(1664525).wrapping_add(1013904223);
        if (s >> 31) == 0 { acc += 1; }
    }
    exit(acc & 0xFF);
}
