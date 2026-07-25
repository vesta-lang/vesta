// Bench: state machine 8 estados (lexer-like).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o state_machine_rust
use std::hint::black_box;
use std::process::exit;

fn main() {
    let mut state: i32 = 0;
    let mut counts: i64 = 0;
    let mut rng: u64 = 7;
    let bound: i32 = black_box(10_000_000);
    let mut i: i32 = 0;
    while i < bound {
        // LCG de 64 bits (constantes de PCG); multiplicacion con wrap.
        rng = rng.wrapping_mul(6364136223846793005);
        rng = rng.wrapping_add(1442695040888963407);
        let b: u8 = ((rng >> 33) & 0xFF) as u8;
        if state == 0 { state = if b < 32 { 1 } else { 2 }; }
        else if state == 1 { state = if b < 64 { 3 } else { 4 }; }
        else if state == 2 { state = if b < 96 { 5 } else { 6 }; }
        else if state == 3 { state = 7; }
        else if state == 4 { state = 7; }
        else if state == 5 { state = 0; counts += 1; }
        else if state == 6 { state = 0; counts += 1; }
        else { state = 0; }
        i += 1;
    }
    exit((counts & 0xFFFF) as i32);
}
