// Bench: tight loop aritmetico simple (equivalente 1:1 al main.c).
// Workload: 50M iteraciones de suma acumulada.
//   rustc -O -C target-cpu=native main.rs -o tight_loop_rust
//
// black_box SOLO sobre el limite UNA vez (opaco) y el resultado final.  Dentro
// del bucle no hay black_box -> LLVM optimiza libre (puede reconocer el idioma
// de reduccion; eso es una observacion valida del codegen, no un truco a evitar).
use std::hint::black_box;
use std::process::exit;

fn main() {
    let bound: i32 = black_box(50_000_000);
    let mut acc: i64 = 0;
    let mut i: i32 = 0;
    while i < bound {
        acc += i as i64;
        i += 1;
    }
    exit((black_box(acc) & 0xFFFF_FFFF) as i32);
}
