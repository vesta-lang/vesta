// obj_accum: objeto mutable de 4 campos, RMW + 2 escrituras condicionales en
// un loop de 20M.  Equivalente 1:1 al main.c.  gcc -O3 hace SROA del struct
// local -> campos en registros; LLVM (rustc) hace lo mismo.
// Compilar: rustc -O -C target-cpu=native main.rs -o obj_accum_rust
use std::hint::black_box;
use std::process::exit;

struct Stats { sum: i64, cnt: i64, mn: i64, mx: i64 }

fn main() {
    let mut s = Stats { sum: 0, cnt: 0, mn: 2_000_000_000, mx: -2_000_000_000 };
    let mut seed: i64 = 12345;
    let bound: i64 = black_box(20_000_000);
    let mut i: i64 = 0;
    while i < bound {
        // LCG y mascara a 31 bits (identico al main.c).
        seed = seed.wrapping_mul(1103515245).wrapping_add(12345) & 2147483647;
        let v: i64 = seed % 1000;
        s.sum += v;
        s.cnt += 1;
        if v < s.mn { s.mn = v; }
        if v > s.mx { s.mx = v; }
        i += 1;
    }
    exit(((s.sum + s.cnt + s.mn + s.mx) % 1_000_000) as i32);
}
