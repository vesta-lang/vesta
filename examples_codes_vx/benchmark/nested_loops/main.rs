// Bench: nested loops (matrix-like).  500 x 500 x 200 = 50M ops.
// Equivalente 1:1 al main.c.  Compilar: rustc -O -C target-cpu=native main.rs
//
// black_box SOLO sobre los limites UNA vez (opacos, para que LLVM no cierre el
// triple bucle a una constante) y sobre el resultado final.  DENTRO de los
// bucles no hay black_box -> LLVM optimiza libremente (lo que queremos observar).
use std::hint::black_box;
use std::process::exit;

fn main() {
    let bi: i32 = black_box(500);
    let bj: i32 = black_box(500);
    let bk: i32 = black_box(200);
    let mut sum: i64 = 0;
    let mut i: i32 = 0;
    while i < bi {
        let mut j: i32 = 0;
        while j < bj {
            let mut k: i32 = 0;
            while k < bk {
                sum += (i + j + k) as i64;
                k += 1;
            }
            j += 1;
        }
        i += 1;
    }
    exit((black_box(sum) & 0xFF) as i32);
}
