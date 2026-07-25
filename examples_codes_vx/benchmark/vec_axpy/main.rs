// Bench: axpy compound element-wise sobre arrays f64 (VECTORIZABLE).
// Hot loop interno a[i] = a[i]*0.5 + b[i] (mul + add), M pasadas.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o vec_axpy_rust
//
// Con target-cpu=native, LLVM auto-vectoriza el bucle interno a SIMD packed
// (SSE2/AVX segun el host), igual que gcc -O3.  La recurrencia x = 0.5x + b
// converge a 2b (acotada) y depende de la pasada previa: nada se hoistea.
// Resultado determinista: a[N/2] -> 2*b[N/2] = 16.
use std::process::exit;

fn main() {
    let n: usize = 4096;
    let m: usize = 50_000;
    let mut a: Vec<f64> = vec![0.0; n];
    let mut b: Vec<f64> = vec![0.0; n];

    // Init (no vectorizable: depende de i).
    for i in 0..n {
        a[i] = (i % 7) as f64 + 1.0;
        b[i] = (i % 13) as f64 + 1.0;
    }

    // Hot loop: M pasadas del axpy compound (inner VECTORIZABLE).
    for _p in 0..m {
        for i in 0..n {
            a[i] = a[i] * 0.5 + b[i];
        }
    }

    let r = a[n / 2]; // converge a 2*b[N/2] = 16
    exit(((r as i64) & 0xFF) as i32);
}
