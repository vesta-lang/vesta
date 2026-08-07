// mem_struct: 1M iteraciones x 3 caminos (pila, heap por sizeof, heap por
// tamano explicito), con ventana de 64 vivos en los dos de heap.
// Mismo algoritmo que main.c; ver alli que compara y por que la ventana.
//
// Java no tiene structs de valor: el camino "en la pila" son dos variables
// locales, que es lo mas cercano y lo que HotSpot obtendria de todas formas al
// descomponer un objeto que no escapa.  Los dos de heap son objetos.
public class Main {
    static final int ITERS = 1000000;
    static final int VIVOS = 64; // potencia de 2

    static class Punto { int x; int y; }

    public static void main(String[] args) {
        Punto[] anilloA = new Punto[VIVOS];
        Punto[] anilloB = new Punto[VIVOS];

        long acc = 0;
        for (int i = 0; i < ITERS; i++) {
            int base = (int)(acc & 0xFF);

            int px = base, py = base + 1;   // 1. en la pila
            acc += px + py;

            int k = i & (VIVOS - 1);

            Punto h = new Punto();          // 2. en heap
            h.x = base; h.y = base + 1;
            if (anilloA[k] != null) acc += anilloA[k].x + anilloA[k].y;
            anilloA[k] = h;

            Punto m = new Punto();          // 3. en heap
            m.x = base; m.y = base + 1;
            if (anilloB[k] != null) acc += anilloB[k].x + anilloB[k].y;
            anilloB[k] = m;
        }
        for (int k = 0; k < VIVOS; k++) {
            if (anilloA[k] != null) acc += anilloA[k].x + anilloA[k].y;
            if (anilloB[k] != null) acc += anilloB[k].x + anilloB[k].y;
        }
        System.exit((int)(acc % 251));
    }
}
