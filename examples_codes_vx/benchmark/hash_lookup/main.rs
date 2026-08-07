// hash_lookup: 50M mezclas estilo FNV, con el resultado consumido.
// Mismo algoritmo que main.c; ver alli los DOS problemas que tenia.
// Compilar: rustc -O -C target-cpu=native main.rs -o hash_lookup_rust
use std::process::exit;

const ITERS: i32 = 50_000_000;

fn main() {
    let mut seed: u64 = 0xCAFE_BABE_DEAD_BEEF;
    let mut acc: u64 = 0;
    for i in 0..ITERS {
        seed ^= i as u64;
        // `wrapping_mul` porque en Rust el desbordamiento es un panico en
        // debug; el resto de lenguajes envuelven y aqui hay que igualarlo.
        seed = seed.wrapping_mul(1099511628211);
        seed >>= 7;
        seed |= 1;
        acc = acc.wrapping_add(seed & 0xFF);
    }
    exit((acc % 251) as i32);
}
