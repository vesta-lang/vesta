// alloc_small: 5M bloques de 16 bytes, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; ver alli por que hay cuatro bancos de reserva,
// por que se toca cada pagina y por que la ventana.
//
// Java NO tiene bloque crudo: `new byte[n]` es un objeto con cabecera (12-16
// bytes) y la especificacion OBLIGA a entregarlo puesto a cero.  Asi que ademas
// de reservar paga borrar, y no hay forma de pedirle que no lo haga.  Es una
// propiedad del lenguaje, no una eleccion del banco de pruebas.
public class Main {
    static final int TAM = 16;
    static final int ITERS = 5000000;
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
