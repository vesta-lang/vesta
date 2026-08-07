// alloc_small: 5M bloques de 16 bytes, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; ver alli por que hay cuatro bancos de reserva,
// por que se toca cada pagina y por que la ventana.
//
// Se usa el asignador CRUDO (`std::alloc`) y no `vec![0; n]`: este banco pide
// un bloque sin inicializar, igual que el `malloc` de C.  Un `Vec` puesto a
// cero mediria ademas el borrado, que es lo que pagan Go, Java y Python porque
// no tienen otra opcion -- pero Rust si la tiene, y usarla es lo que lo deja en
// el mismo terreno que C.
// Compilar: rustc -O -C target-cpu=native main.rs -o alloc_small_rust
use std::alloc::{alloc, dealloc, Layout};
use std::process::exit;

const TAM: usize = 16;
const ITERS: i32 = 5_000_000;
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
            if !anillo[k].is_null() {          // el mas viejo sale de la ventana
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
