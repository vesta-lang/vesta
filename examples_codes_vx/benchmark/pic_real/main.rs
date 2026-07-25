// pic_real: 3M iter, array de 3 formas alternadas.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o pic_real_rust
//
// Se usa la MISMA union etiquetada (campo `kind` + campos crudos) que el main.c,
// no un enum/trait de Rust, para reproducir el mismo despacho por `kind`.
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
    let shapes = [
        Shape { kind: 0, r: 5, w: 0, h: 0, b: 0 },
        Shape { kind: 1, r: 0, w: 4, h: 6, b: 0 },
        Shape { kind: 2, r: 0, w: 0, h: 8, b: 3 },
    ];
    // black_box impide que LLVM pre-calcule el resultado del bucle periodico.
    let shapes = black_box(shapes);
    let mut sum: i32 = 0;
    let mut i: i32 = 0;
    while i < 3_000_000 {
        sum = sum.wrapping_add(area(&shapes[(i % 3) as usize]));
        i += 1;
    }
    exit(sum);
}
