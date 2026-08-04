// Bench: aritmetica entera mixta intensiva (20M iter x 10 ops).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o int_mixed_rust
//
// `c *= 3` desborda i64 masivamente en 20M iteraciones; C lo trata como
// wrap de 2's-complement (UB tecnica, pero determinista con -O3).  Aqui se usa
// aritmetica `wrapping_*` para reproducir el MISMO resultado sin panic.
use std::hint::black_box;
use std::num::Wrapping;
use std::process::exit;

fn main() {
    let mut a = Wrapping(1i64);
    let mut b = Wrapping(2i64);
    let mut c = Wrapping(3i64);
    let mut d = Wrapping(5i64);
    let bound: i32 = black_box(20_000_000);
    let mut i: i32 = 0;
    while i < bound {
        a += Wrapping(i as i64);
        b -= Wrapping(1);
        c *= Wrapping(3);
        d += a ^ b;
        a &= Wrapping(0xFFFF_FFFF);
        b |= Wrapping(1);
        c = Wrapping(c.0 >> 1);
        d ^= a;
        a += b;
        c += d;
        i += 1;
    }
    let total = (a + b + c + d).0;
    exit((total & 0xFF) as i32);
}
