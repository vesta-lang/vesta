// alloc_large: 500K bloques de 256 KB, con una ventana de 16 vivos.
// Mismo algoritmo que main.c; el porque del diseno esta en
// `alloc_small/main.c`.
//
// Java NO tiene bloque crudo: `new byte[n]` lleva cabecera y la especificacion
// OBLIGA a entregarlo puesto a cero.  A esta escala eso son 256 KB borrados por
// iteracion, bastante mas que la reserva en si.  Es una propiedad del lenguaje,
// no una eleccion del banco, y es el efecto que este tamano deja ver.
public class Main {
    static final int TAM = 256 * 1024;
    static final int ITERS = 500000;
    static final int VIVOS = 16; // potencia de 2
    static final int PAGINA = 4096;

    public static void main(String[] args) {
        byte[][] anillo = new byte[VIVOS][];

        int acc = 0;
        for (int i = 0; i < ITERS; i++) {
            byte[] p = new byte[TAM];
            byte v = (byte)(i & 0xFF);
            for (int o = 0; o < TAM; o += PAGINA) p[o] = v;
            p[TAM - 1] = v;
            int k = i & (VIVOS - 1);
            if (anillo[k] != null) { // el mas viejo sale de la ventana
                acc += anillo[k][0] & 0xFF;
            }
            anillo[k] = p;
        }
        for (int k = 0; k < VIVOS; k++) { // vaciar la ventana
            if (anillo[k] != null) acc += anillo[k][0] & 0xFF;
        }
        System.exit(acc & 0xFF);
    }
}
