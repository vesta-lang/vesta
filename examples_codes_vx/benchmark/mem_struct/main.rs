// mem_struct: 1M iteraciones x 3 caminos (pila, heap por sizeof, heap por
// tamano explicito), con ventana de 64 vivos en los dos de heap.
// Mismo algoritmo que main.c; ver alli que compara y por que la ventana.
//
// Los dos caminos de heap usan `Box`, que es el equivalente directo del
// malloc/free de C (se libera al soltarlo).  Ya no hace falta `black_box`: la
// cadena de dependencias impide la forma cerrada.
// Compilar: rustc -O -C target-cpu=native main.rs -o mem_struct_rust
use std::process::exit;

const ITERS: i32 = 1_000_000;
const VIVOS: usize = 64; // potencia de 2

struct Punto { x: i32, y: i32 }

fn main() {
    let mut anillo_a: Vec<Option<Box<Punto>>> = Vec::with_capacity(VIVOS);
    let mut anillo_b: Vec<Option<Box<Punto>>> = Vec::with_capacity(VIVOS);
    for _ in 0..VIVOS { anillo_a.push(None); anillo_b.push(None); }

    let mut acc: i64 = 0;
    for i in 0..ITERS {
        let base = (acc & 0xFF) as i32;

        let p = Punto { x: base, y: base + 1 };   // 1. en la pila
        acc += (p.x + p.y) as i64;

        let k = (i as usize) & (VIVOS - 1);

        let h = Box::new(Punto { x: base, y: base + 1 });   // 2. heap
        if let Some(viejo) = anillo_a[k].take() {
            acc += (viejo.x + viejo.y) as i64;
        }
        anillo_a[k] = Some(h);

        let m = Box::new(Punto { x: base, y: base + 1 });   // 3. heap
        if let Some(viejo) = anillo_b[k].take() {
            acc += (viejo.x + viejo.y) as i64;
        }
        anillo_b[k] = Some(m);
    }
    for k in 0..VIVOS {
        if let Some(v) = anillo_a[k].take() { acc += (v.x + v.y) as i64; }
        if let Some(v) = anillo_b[k].take() { acc += (v.x + v.y) as i64; }
    }
    exit((acc % 251) as i32);
}
