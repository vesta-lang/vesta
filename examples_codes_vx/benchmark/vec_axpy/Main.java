// Bench: axpy compound element-wise sobre arrays f64 (VECTORIZABLE).
// Hot loop interno a[i] = a[i]*0.5 + b[i] (mul + add), M pasadas.
// HotSpot C2 lo auto-vectoriza tras el warmup.
// Resultado determinista: a[N/2] converge a 2*b[N/2] = 16.
public class Main {
    public static void main(String[] args) {
        int N = 4096;
        int M = 50000;
        double[] a = new double[N];
        double[] b = new double[N];
        for (int i = 0; i < N; i++) {
            a[i] = (double)(i % 7) + 1.0;
            b[i] = (double)(i % 13) + 1.0;
        }
        for (int p = 0; p < M; p++) {
            for (int i = 0; i < N; i++) {
                a[i] = a[i] * 0.5 + b[i];
            }
        }
        double r = a[N / 2];
        System.exit((int)((long) r & 0xFF));
    }
}
