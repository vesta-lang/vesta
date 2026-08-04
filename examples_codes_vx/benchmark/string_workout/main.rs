// Bench: string workout AGRESIVO (version Rust).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o string_workout_rust
//
// Diseno equivalente al main.c: 1M iter, 6 ops string por iter, con buffers de
// bytes manuales + FNV-1a (aunque el hash no entra en el resultado final, se
// mantiene la estructura del bench de C).  black_box previene el desenrollado
// / la eliminacion de codigo muerto.
use std::hint::black_box;
use std::process::exit;

// FNV-1a 64-bit (con wrap), equivalente al del runtime.  No usado en el
// resultado (igual que en el main.c), pero se conserva por fidelidad.
#[allow(dead_code)]
fn fnv1a(s: &[u8]) -> u64 {
    let mut h: u64 = 14695981039346656037;
    for &c in s {
        h ^= c as u64;
        h = h.wrapping_mul(1099511628211);
    }
    h
}

fn main() {
    let mut sum: i64 = 0;
    let mut hits1: i64 = 0;
    let mut hits2: i64 = 0;
    let bound: i32 = black_box(1_000_000);

    let mut i: i32 = 0;
    while i < bound {
        // bufA: 4 letras derivadas de i (mismos desplazamientos que C).
        let mut buf_a = [0u8; 16];
        buf_a[0] = (65 + (i & 7)) as u8;
        buf_a[1] = (65 + ((i >> 3) & 7)) as u8;
        buf_a[2] = (65 + ((i >> 6) & 7)) as u8;
        buf_a[3] = (65 + ((i >> 9) & 7)) as u8;
        buf_a[4] = 0;

        // bufB: 2 digitos.
        let mut buf_b = [0u8; 16];
        buf_b[0] = (48 + (i % 10)) as u8;
        buf_b[1] = (48 + ((i / 10) % 10)) as u8;
        buf_b[2] = 0;

        // OP1: concat A(4) + B(2) -> c (6 chars).
        let mut c = [0u8; 16];
        c[..4].copy_from_slice(&buf_a[..4]);
        c[4..6].copy_from_slice(&buf_b[..2]);
        c[6] = 0;

        // OP2: length.
        let mut ln = 0usize;
        while c[ln] != 0 { ln += 1; }
        sum += ln as i64;

        // OP3: equals contra patron fijo "AAAA00".
        if &c[..6] == b"AAAA00" { hits1 += 1; }

        // OP5: substring a[0:3].
        let mut sub = [0u8; 16];
        sub[..3].copy_from_slice(&buf_a[..3]);
        sub[3] = 0;

        // OP6: equals contra patron variable bufQ.
        let mut buf_q = [0u8; 16];
        buf_q[0] = (65 + ((i / 100) & 7)) as u8;
        buf_q[1] = (65 + ((i / 800) & 7)) as u8;
        buf_q[2] = (65 + ((i / 6400) & 7)) as u8;
        buf_q[3] = 0;
        if &sub[..3] == &buf_q[..3] { hits2 += 1; }

        // Final: concat sub(3) + B(2) = 5 chars, suma length.
        let mut c2 = [0u8; 16];
        c2[..3].copy_from_slice(&sub[..3]);
        c2[3..5].copy_from_slice(&buf_b[..2]);
        c2[5] = 0;
        let mut ln2 = 0usize;
        while c2[ln2] != 0 { ln2 += 1; }
        sum += ln2 as i64;

        black_box(&c);
        i += 1;
    }

    let r = sum + hits1 * 1000 + hits2 * 7;
    exit((r & 0x7FFF_FFFF) as i32);
}
