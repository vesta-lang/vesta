// Bench: aritmetica f64 intensiva.  5M iter x 6 ops FP.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o fp_jit_rust
//
// sqrt/abs/min/max/floor/ceil de f64 mapean 1:1 a las intrinsecas de C
// (sqrt/fabs/fmin/fmax/floor/ceil).  fmin/fmax de C: si un operando es NaN
// devuelve el otro; aqui no hay NaN (x>=0), asi que f64::min/max coinciden.
use std::hint::black_box;
use std::process::exit;

fn main() {
    let mut acc: f64 = 0.0;
    let bound: i32 = black_box(5_000_000);
    let mut i: i32 = 0;
    while i < bound {
        let x = i as f64;
        let s = x.sqrt();
        let a = (s - 1000.0).abs();
        let m = a.min(999.0);
        let big = s.max(1.0);
        let f = big.floor();
        let c = f.ceil();
        acc += m + c;
        i += 1;
    }
    exit(((acc as i64) & 0xFF) as i32);
}
