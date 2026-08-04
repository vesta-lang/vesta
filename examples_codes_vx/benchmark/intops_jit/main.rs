// intops_jit: 5M iter, imin/imax/abs.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o intops_jit_rust
use std::hint::black_box;
use std::process::exit;

fn abs_l(x: i64) -> i64 { if x < 0 { -x } else { x } }
fn min_l(a: i64, b: i64) -> i64 { if a < b { a } else { b } }
fn max_l(a: i64, b: i64) -> i64 { if a > b { a } else { b } }

fn main() {
    let mut acc: i64 = 0;
    let bound: i32 = black_box(5_000_000);
    let mut i: i32 = 1;
    while i < bound {
        let a = i as i64;
        let b = (i + 7) as i64;
        acc += min_l(a, b);
        acc += max_l(a, b);
        acc += abs_l(a - 5000);
        i += 1;
    }
    exit((acc & 0xFFFF) as i32);
}
