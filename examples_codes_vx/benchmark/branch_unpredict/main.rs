// Bench: branches no predecibles (xorshift) + 4 branches.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o branch_unpredict_rust
use std::hint::black_box;
use std::process::exit;

fn main() {
    let mut rng: u64 = 1;
    let mut a: i64 = 0;
    let mut b: i64 = 0;
    let mut c: i64 = 0;
    let mut d: i64 = 0;
    let bound: i32 = black_box(10_000_000);
    let mut i: i32 = 0;
    while i < bound {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        if rng & 1 != 0 { a += 1; } else { a -= 1; }
        if rng & 2 != 0 { b += 1; } else { b -= 1; }
        if rng & 4 != 0 { c += 1; } else { c -= 1; }
        if rng & 8 != 0 { d += 1; } else { d -= 1; }
        i += 1;
    }
    exit(((a + b + c + d) & 0xFF) as i32);
}
