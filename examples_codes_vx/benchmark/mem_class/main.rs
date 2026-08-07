// mem_class: 1M objetos en heap, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; ver alli el porque de la ventana.
//
// Ya no hace falta `black_box`: el trabajo no se elimina porque hace falta de
// verdad -- los objetos viven 64 iteraciones y su contenido acaba en el codigo
// de salida.  Preferible a una barrera, que es un truco distinto en cada
// lenguaje y acababa midiendo cosas distintas en cada uno.
// Compilar: rustc -O -C target-cpu=native main.rs -o mem_class_rust
use std::process::exit;

const ITERS: i32 = 1_000_000;
const VIVOS: usize = 64; // potencia de 2

struct Foo { x: i32 }

fn main() {
    let mut anillo: Vec<Option<Box<Foo>>> = Vec::with_capacity(VIVOS);
    for _ in 0..VIVOS { anillo.push(None); }

    let mut acc: i32 = 0;
    for i in 0..ITERS {
        let f = Box::new(Foo { x: i & 0xFF });
        let k = (i as usize) & (VIVOS - 1);
        if let Some(viejo) = anillo[k].take() {  // sale de la ventana
            acc += viejo.x;                       // y se libera al salir
        }
        anillo[k] = Some(f);
    }
    for k in 0..VIVOS {                           // vaciar la ventana
        if let Some(viejo) = anillo[k].take() { acc += viejo.x; }
    }
    exit(acc & 0xFF);
}
