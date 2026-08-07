// mem_malloc_free: 5M bloques de 96 bytes, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; el porque de la ventana esta en
// `alloc_small/main.c`.
//
// Java NO tiene bloque crudo: `new byte[n]` lleva cabecera y la especificacion
// OBLIGA a entregarlo puesto a cero.  Es una propiedad del lenguaje, no una
// eleccion del banco.
public class Main {
    static final int TAM = 96;
    static final int ITERS = 5000000;
    static final int VIVOS = 64; // potencia de 2

    public static void main(String[] args) {
        byte[][] anillo = new byte[VIVOS][];

        int acc = 0;
        for (int i = 0; i < ITERS; i++) {
            byte[] buf = new byte[TAM];
            buf[0] = (byte) i;
            buf[TAM - 1] = (byte) (i + TAM - 1);
            int k = i & (VIVOS - 1);
            if (anillo[k] != null) { // el mas viejo sale de la ventana
                acc += (anillo[k][0] & 0xFF) + (anillo[k][TAM - 1] & 0xFF);
            }
            anillo[k] = buf;
        }
        for (int k = 0; k < VIVOS; k++) { // vaciar la ventana
            if (anillo[k] != null) {
                acc += (anillo[k][0] & 0xFF) + (anillo[k][TAM - 1] & 0xFF);
            }
        }
        System.exit(acc % 251);
    }
}
