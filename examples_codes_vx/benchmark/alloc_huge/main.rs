// alloc_huge: 100 bloques de 16 MB, con una ventana de 4 vivos.
// Mismo algoritmo que main.c; ver alli por que a este tamano lo que se mide es
// el fallo de pagina y no la llamada al asignador.
//
// Se usa el asignador CRUDO (`std::alloc`) para pedir el bloque sin
// inicializar, igual que el `malloc` de C.  Un `vec![0u8; n]` borraria 16 MB
// por iteracion -- que es exactamente lo que Go, Java y Python no pueden
// evitar, y de lo que Rust si puede librarse.
// Compilar: rustc -O -C target-cpu=native main.rs -o alloc_huge_rust
use std::alloc::{alloc, dealloc, Layout};
use std::process::exit;

const TAM: usize = 16 * 1024 * 1024;
const ITERS: i32 = 100;
const VIVOS: usize = 4; // potencia de 2
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
