// callvirt: 30M llamadas a metodo virtual.
// Mismo algoritmo que main.c; ver alli donde esta la linea entre optimizar
// (bien: HotSpot desvirtualiza cuando solo ve un tipo, y eso es calidad) y
// fabricar el resultado sin ejecutar (mal).
public class Main {
    static class Counter {
        int value = 0;
        int inc() { return value + 1; }
    }

    public static void main(String[] args) {
        Counter c = new Counter();
        long sum = 0;
        for (int i = 0; i < 30000000; i++) {
            c.value = (c.inc() * 1664525 + 1013904223) & 0xFF;
            sum += c.value;
        }
        System.exit((int)(sum % 251));
    }
}
