// Bench: quicksort Lomuto sobre array i32 de 100K.  Equivalente 1:1 al main.c.
// Compilar: rustc -O -C target-cpu=native main.rs -o quicksort_rust
//
// Genera el MISMO array pseudoaleatorio que el main.c (LCG de 64 bits con las
// constantes de PCG) y ordena in-place con particion de Lomuto recursiva.
use std::process::exit;

fn partition_(arr: &mut [i32], lo: i32, hi: i32) -> i32 {
    let pivot = arr[hi as usize];
    let mut i = lo - 1;
    let mut j = lo;
    while j < hi {
        if arr[j as usize] <= pivot {
            i += 1;
            arr.swap(i as usize, j as usize);
        }
        j += 1;
    }
    arr.swap((i + 1) as usize, hi as usize);
    i + 1
}

fn qsort_rec(arr: &mut [i32], lo: i32, hi: i32) {
    if lo < hi {
        let p = partition_(arr, lo, hi);
        qsort_rec(arr, lo, p - 1);
        qsort_rec(arr, p + 1, hi);
    }
}

fn main() {
    const N: i32 = 100_000;
    let mut arr: Vec<i32> = vec![0; N as usize];
    let mut seed: u64 = 12345;
    for i in 0..N as usize {
        seed = seed.wrapping_mul(6364136223846793005);
        seed = seed.wrapping_add(1442695040888963407);
        arr[i] = ((seed >> 33) & 0xFFFF_FFFF) as i32;
    }
    qsort_rec(&mut arr, 0, N - 1);
    let r = arr[(N / 2) as usize] & 0xFF;
    exit(r);
}
