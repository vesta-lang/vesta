// alloc_medium: 4M bloques de 1 KB, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; el porque del diseno esta en
// `alloc_small/main.c`.
//
// Java NO tiene bloque crudo: `new byte[n]` lleva cabecera y la especificacion
// OBLIGA a entregarlo puesto a cero, asi que ademas de reservar paga borrar
// 1 KB por iteracion.  Es una propiedad del lenguaje, no una eleccion del banco.
public class Main {
    static final int TAM = 1024;
    static final int ITERS = 4000000;
    static final int VIVOS = 64; // potencia de 2
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
