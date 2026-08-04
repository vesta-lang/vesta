// polymorphic: 10M, 3 subtipos via union etiquetada.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o polymorphic_rust
use std::hint::black_box;
use std::process::exit;

#[derive(Clone, Copy)]
struct Shape { kind: i32, r: i32, w: i32, h: i32, b: i32 }

fn area(s: &Shape) -> i32 {
    if s.kind == 0 { s.r * s.r * 3 }
    else if s.kind == 1 { s.w * s.h }
    else { (s.b * s.h) / 2 }
}

fn main() {
    let c = black_box(Shape { kind: 0, r: 5, w: 0, h: 0, b: 0 });
    let r = black_box(Shape { kind: 1, r: 0, w: 4, h: 6, b: 0 });
    let t = black_box(Shape { kind: 2, r: 0, w: 0, h: 8, b: 3 });
    let mut sum: i32 = 0;
    let mut i: i32 = 0;
    while i < 10_000_000 {
        let m = i % 3;
        if m == 0 { sum = sum.wrapping_add(area(&c)); }
        else if m == 1 { sum = sum.wrapping_add(area(&r)); }
        else { sum = sum.wrapping_add(area(&t)); }
        i += 1;
    }
    exit(sum);
}
