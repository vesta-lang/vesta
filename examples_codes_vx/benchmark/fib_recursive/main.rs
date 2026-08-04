// Bench: Fibonacci recursivo profundo.  fib(32) = 2178309.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o fib_recursive_rust
use std::hint::black_box;
use std::process::exit;

// Recursion binaria pura (sin memoizacion): reproduce el arbol de llamadas de C.
fn fib(n: i64) -> i64 {
    if n < 2 {
        return n;
    }
    fib(n - 1) + fib(n - 2)
}

fn main() {
    // `black_box` evita que LLVM const-folde fib(32) a la constante 2178309.
    let input: i64 = black_box(32);
    let r = fib(input);
    exit((r & 0xFFFF_FFFF) as i32);
}
