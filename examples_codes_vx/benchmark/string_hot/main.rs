// Bench: strings en hot loop (concat + length + compare).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o string_hot_rust
//
// El main.c usa char[] con snprintf/strlen/strcmp.  Aqui se replica con un
// buffer de bytes fijo (sin asignaciones ni String managed): concat manual,
// longitud por conteo hasta el NUL y comparacion byte a byte.
use std::hint::black_box;
use std::process::exit;

fn main() {
    let base: &[u8] = b"abc";
    let suffix: &[u8] = b"xyz";
    let target: &[u8] = b"abcxyz";
    let mut hits: i64 = 0;
    let bound: i32 = black_box(200_000);
    let mut i: i32 = 0;
    while i < bound {
        // Concat manual base+suffix en buffer local con NUL final.
        let mut buf = [0u8; 16];
        let mut p = 0usize;
        for &ch in base { buf[p] = ch; p += 1; }
        for &ch in suffix { buf[p] = ch; p += 1; }
        buf[p] = 0;
        // strlen: conteo hasta el NUL.
        let mut ln = 0usize;
        while buf[ln] != 0 { ln += 1; }
        if ln == 6 { hits += 1; }
        // strcmp contra "abcxyz".
        if &buf[..6] == target { hits += 1; }
        black_box(&buf);
        i += 1;
    }
    exit(hits as i32);
}
