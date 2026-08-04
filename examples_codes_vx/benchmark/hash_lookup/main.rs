// Bench: hash lookup simulado (FNV-style int ops).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o hash_lookup_rust
use std::hint::black_box;
use std::process::exit;

fn main() {
    let mut seed: u64 = 0xCAFE_BABE_DEAD_BEEF;
    let mut acc: u64 = 0;
    let bound: i32 = black_box(50_000_000);
    let mut i: i32 = 0;
    while i < bound {
        seed ^= i as u64;
        seed = seed.wrapping_mul(1099511628211); // primo FNV; desborda -> wrap
        seed >>= 7;
        seed |= 1;
        // `seed |= 1` deja seed impar, asi que seed&7 nunca es 0: LLVM lo prueba
        // y ELIMINA el bucle entero (gcc no lo hace).  Para medir el trabajo real
        // del hash (como C), la comprobacion se hace opaca via black_box (identidad;
        // mismo resultado: acc queda en 0).
        if black_box(seed) & 7 == 0 {
            acc += 1;
        }
        i += 1;
    }
    exit(acc as i32);
}
