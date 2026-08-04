// cmp_fusion: 50M loop con cmp trivial.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o cmp_fusion_rust
use std::hint::black_box;
use std::process::exit;

// El limite se recibe via black_box para que el bucle no colapse a `n`.
fn run_impl(n: i32) -> i64 {
    let mut acc: i64 = 0;
    let mut i: i32 = 0;
    while i < n {
        acc += 1;
        i += 1;
    }
    acc
}

fn main() {
    let r = run_impl(black_box(50_000_000));
    exit((r & 0xFFFF_FFFF) as i32);
}
