// Bench: bitops (and/or/xor/shl/shr).  30M iter x 8 ops.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o bitops_rust
//
// Se usa aritmetica `wrapping_*` en las sumas u64 para reproducir el desbordamiento
// de 2's-complement bien definido de C (evita panics con overflow-checks activo).
use std::hint::black_box;
use std::process::exit;

fn main() {
    let mut a: u64 = 0xDEAD_BEEF_CAFE_BABE;
    let mut b: u64 = 0x1234_5678_90AB_CDEF;
    let bound: i32 = black_box(30_000_000);
    let mut i: i32 = 0;
    while i < bound {
        a ^= i as u64;
        a &= 0xFFFF_FFFF_FFFF;
        a |= 0x1_0101_0101_0101;
        a <<= 1;
        a >>= 1;
        b = b.wrapping_add(a & 0xFFFF);
        b ^= a >> 16;
        b <<= 1;
        i += 1;
    }
    exit((a.wrapping_add(b) & 0xFF) as i32);
}
