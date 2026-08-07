// cmp_fusion: 50M comparaciones + salto, con el resultado consumido.
// Mismo algoritmo que main.c; ver alli por que la condicion tiene que ser
// impredecible en compilacion.
//
// Que el compilador acabe resolviendo la comparacion sin saltar (movimiento
// condicional) es legitimo y forma parte de lo que este banco mide: hasta
// donde optimiza cada uno.  Lo que no puede pasar es que el trabajo
// DESAPAREZCA, que es lo que ocurria antes.
public class Main {
    static final int ITERS = 50000000;

    public static void main(String[] args) {
        int s = 12345;              // int de Java = 32 bits que envuelven
        int acc = 0;
        for (int i = 0; i < ITERS; i++) {
            s = s * 1664525 + 1013904223;
            if ((s >>> 31) == 0) acc++;   // desplazamiento SIN signo
        }
        System.exit(acc & 0xFF);
    }
}
