// mem_class: 1M objetos en heap, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; ver alli en que se diferencia este banco de los
// de bloque crudo (`alloc_small` y companeros) y por que la ventana.
//
// La ventana hace falta igual que en C: sin ella el analisis de escape de
// HotSpot descompone el objeto en registros y no reserva nada.
public class Main {
    static final int ITERS = 1000000;
    static final int VIVOS = 64; // potencia de 2

    static class Foo { int x; Foo(int v) { x = v; } }

    public static void main(String[] args) {
        Foo[] anillo = new Foo[VIVOS];

        int acc = 0;
        for (int i = 0; i < ITERS; i++) {
            Foo f = new Foo(i & 0xFF);
            int k = i & (VIVOS - 1);
            if (anillo[k] != null) { // el mas viejo sale de la ventana
                acc += anillo[k].x;
            }
            anillo[k] = f;
        }
        for (int k = 0; k < VIVOS; k++) { // vaciar la ventana
            if (anillo[k] != null) acc += anillo[k].x;
        }
        System.exit(acc & 0xFF);
    }
}
