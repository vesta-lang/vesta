// Bench: copia secuencial byte-a-byte (1 MB x 100 iter = 100 MB).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o memcpy_loop_rust
//
// Bucle manual byte-a-byte (no `copy_from_slice`/memcpy intrinseco) para
// comparar con el codegen escalar/vectorizado del loop trivial, como en C.
use std::hint::black_box;
use std::process::exit;

fn main() {
    const N: usize = 1_048_576;
    let mut src: Vec<u8> = vec![0u8; N];
    let mut dst: Vec<u8> = vec![0u8; N];
    for i in 0..N {
        src[i] = (i & 0xFF) as u8;
    }
    let bound: i32 = black_box(100);
    for _it in 0..bound {
        for j in 0..N {
            dst[j] = src[j];
        }
    }
    let r = dst[1234] as i32;
    exit(r);
}
