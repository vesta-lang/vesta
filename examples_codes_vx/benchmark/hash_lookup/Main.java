// hash_lookup: 50M mezclas estilo FNV, con el resultado consumido.
// Mismo algoritmo que main.c; ver alli los DOS problemas que tenia.
//
// En Java no hay enteros sin signo, pero el desplazamiento a la derecha SIN
// signo (`>>>`) y la multiplicacion que envuelve dan exactamente la misma
// secuencia de bits que el `uint64_t` de C.
public class Main {
    static final int ITERS = 50000000;

    public static void main(String[] args) {
        long seed = 0xCAFEBABEDEADBEEFL;
        long acc = 0;
        for (int i = 0; i < ITERS; i++) {
            seed ^= (long) i;
            seed *= 1099511628211L;
            seed >>>= 7;              // sin signo, como en C
            seed |= 1L;
            acc += seed & 0xFFL;
        }
        System.exit((int)(acc % 251));
    }
}
