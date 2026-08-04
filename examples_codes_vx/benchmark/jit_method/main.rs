// jit_method: 30M (3 * 10M) sum-of-i en metodo.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o jit_method_rust
use std::hint::black_box;
use std::process::exit;

// Suma 0..n en un metodo separado (llamado 3 veces).
fn run_impl(n: i32) -> i32 {
    let mut sum: i32 = 0;
    let mut i: i32 = 0;
    while i < n {
        sum = sum.wrapping_add(i); // i32 puede desbordar; wrap como en C -O3
        i += 1;
    }
    sum
}

fn main() {
    let a = run_impl(black_box(10_000_000));
    let b = run_impl(black_box(10_000_000));
    let c = run_impl(black_box(10_000_000));
    exit(a.wrapping_add(b).wrapping_add(c));
}
