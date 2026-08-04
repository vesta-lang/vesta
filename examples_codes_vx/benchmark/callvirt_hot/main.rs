// callvirt_hot: 10M virtual-call hot con estado.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o callvirt_hot_rust
use std::hint::black_box;
use std::process::exit;

struct C {
    inc: fn(&mut C, i32) -> i32,
    n: i32,
}

fn inc_impl(c: &mut C, d: i32) -> i32 {
    c.n = c.n.wrapping_add(d);
    c.n
}

fn main() {
    let mut c = C { n: black_box(0), inc: inc_impl };
    let mut sum: i32 = 0;
    let mut i: i32 = 0;
    while i < 10_000_000 {
        let f = c.inc;
        sum = f(&mut c, 1);
        i += 1;
    }
    exit(sum);
}
