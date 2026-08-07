// alloc_huge: 100 bloques de 16 MB, con una ventana de 4 vivos.
// Mismo algoritmo que main.c; ver alli por que a este tamano lo que se mide es
// el fallo de pagina y no la llamada al asignador.
//
// Java NO tiene bloque crudo: `new byte[n]` lleva cabecera y la especificacion
// OBLIGA a entregarlo puesto a cero.  A este tamano eso son 16 MB borrados por
// iteracion, o sea que toca las 4096 paginas quiera o no.  Es el precio de su
// garantia de seguridad, y este banco existe para que se vea.
public class Main {
    static final int TAM = 16 * 1024 * 1024;
    static final int ITERS = 100;
    static final int VIVOS = 4; // potencia de 2
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
