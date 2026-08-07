// alloc_medium: 4M bloques de 1 KB, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; el porque del diseno esta en
// `alloc_small/main.c`.  Se usa el asignador CRUDO (`std::alloc`) para pedir el
// bloque sin inicializar, igual que el `malloc` de C.
// Compilar: rustc -O -C target-cpu=native main.rs -o alloc_medium_rust
use std::alloc::{alloc, dealloc, Layout};
use std::process::exit;

const TAM: usize = 1024;
const ITERS: i32 = 4_000_000;
const VIVOS: usize = 64; // potencia de 2
const PAGINA: usize = 4096;

fn main() {
    let disposicion = Layout::from_size_align(TAM, 1).unwrap();
    let mut anillo: Vec<*mut u8> = vec![std::ptr::null_mut(); VIVOS];

    let mut acc: i32 = 0;
    for i in 0..ITERS {
        let v = (i & 0xFF) as u8;
        unsafe {
            let p = alloc(disposicion);
            let mut o = 0usize;
            while o < TAM { *p.add(o) = v; o += PAGINA; }
            *p.add(TAM - 1) = v;
            let k = (i as usize) & (VIVOS - 1);
            if !anillo[k].is_null() {          // sale de la ventana
                acc += *anillo[k] as i32;
                dealloc(anillo[k], disposicion);
            }
            anillo[k] = p;
        }
    }
    for k in 0..VIVOS {                        // vaciar la ventana
        unsafe {
            if !anillo[k].is_null() {
                acc += *anillo[k] as i32;
                dealloc(anillo[k], disposicion);
            }
        }
    }
    exit(acc & 0xFF);
}
