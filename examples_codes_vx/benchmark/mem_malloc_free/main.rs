// Bench: 5M alloc + free de bloques pequenos (96 bytes).  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o mem_malloc_free_rust
//
// Se usa el allocator global de Rust directamente (alloc/dealloc) para replicar
// el malloc(96)/free de C sin la sobrecarga de un Vec.  black_box impide que
// LLVM elimine el par alloc/dealloc.
use std::alloc::{alloc, dealloc, Layout};
use std::hint::black_box;
use std::process::exit;

fn do_iter(i: i32) {
    let layout = Layout::from_size_align(96, 1).unwrap();
    unsafe {
        let buf = alloc(layout);
        *buf.add(0) = i as u8;
        *buf.add(95) = (i + 95) as u8;
        black_box(buf);
        dealloc(buf, layout);
    }
}

fn main() {
    let bound: i32 = black_box(5_000_000);
    let mut i: i32 = 0;
    while i < bound {
        do_iter(i);
        i += 1;
    }
    exit(42);
}
