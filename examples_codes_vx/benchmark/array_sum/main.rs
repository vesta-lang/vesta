// Bench: suma de array i32 (100K x 200 pasadas).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o array_sum_rust
use std::hint::black_box;
use std::process::exit;

fn main() {
    // Vec en heap, analogo al malloc de C (no vive en pila).
    let mut arr: Vec<i32> = vec![0; 100_000];
    for i in 0..100_000i32 {
        arr[i as usize] = i;
    }
    let mut sum: i64 = 0;
    // `black_box` en el numero de pasadas replica el `volatile` de C.
    let passes: i32 = black_box(200);
    for _p in 0..passes {
        for i in 0..100_000usize {
            sum += arr[i] as i64;
        }
    }
    exit((sum & 0xFF) as i32);
}
