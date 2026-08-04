// rotops_jit: 5M iter, rotl/rotr.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o rotops_jit_rust
use std::hint::black_box;
use std::process::exit;

// rotate_left/right de Rust equivalen EXACTAMENTE al idioma (v<<n)|(v>>(64-n)).
fn rotl_u64(v: u64, n: u32) -> u64 { v.rotate_left(n) }
fn rotr_u64(v: u64, n: u32) -> u64 { v.rotate_right(n) }

fn main() {
    let mut acc: u64 = 0;
    let bound: i32 = black_box(5_000_000);
    let mut i: i32 = 1;
    while i < bound {
        let v = i as u64;
        acc = acc.wrapping_add(rotl_u64(v, 7));
        acc = acc.wrapping_add(rotr_u64(v, 3));
        i += 1;
    }
    exit((acc & 0xFFFF) as i32);
}
