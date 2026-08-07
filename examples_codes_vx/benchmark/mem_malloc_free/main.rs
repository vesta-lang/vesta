// mem_malloc_free: 5M bloques de 96 bytes, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; el porque de la ventana esta en
// `alloc_small/main.c`.  Se usa el asignador CRUDO (`std::alloc`) para pedir el
// bloque sin inicializar, igual que el `malloc` de C.
// Compilar: rustc -O -C target-cpu=native main.rs -o mem_malloc_free_rust
use std::alloc::{alloc, dealloc, Layout};
use std::process::exit;

const TAM: usize = 96;
const ITERS: i32 = 5_000_000;
const VIVOS: usize = 64; // potencia de 2

fn main() {
    let disposicion = Layout::from_size_align(TAM, 1).unwrap();
    let mut anillo: Vec<*mut u8> = vec![std::ptr::null_mut(); VIVOS];

    let mut acc: i32 = 0;
    for i in 0..ITERS {
        unsafe {
            let buf = alloc(disposicion);
            *buf = i as u8;
            *buf.add(TAM - 1) = (i as usize + TAM - 1) as u8;
            let k = (i as usize) & (VIVOS - 1);
            if !anillo[k].is_null() {          // sale de la ventana
                acc += *anillo[k] as i32 + *anillo[k].add(TAM - 1) as i32;
                dealloc(anillo[k], disposicion);
            }
            anillo[k] = buf;
        }
    }
    for k in 0..VIVOS {                        // vaciar la ventana
        unsafe {
            if !anillo[k].is_null() {
                acc += *anillo[k] as i32 + *anillo[k].add(TAM - 1) as i32;
                dealloc(anillo[k], disposicion);
            }
        }
    }
    exit(acc % 251);
}
