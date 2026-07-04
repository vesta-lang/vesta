/* Bench: axpy compound element-wise sobre arrays f64 (VECTORIZABLE).
 * Hot loop interno `a[i] = a[i]*0.5 + b[i]` (mul + add) sobre malloc f64,
 * M pasadas (compute-bound, cache-resident).  gcc -O3 lo auto-vectoriza.
 * Resultado determinista: a[N/2] converge a 2*b[N/2] = 16. */
#include <stdlib.h>
#include <stdint.h>

int main(void) {
    int N = 4096;
    int M = 50000;
    double *a = (double *) malloc((size_t) N * 8);
    double *b = (double *) malloc((size_t) N * 8);

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
    free(a);
    free(b);
    return (int)((int64_t) r & 0xFF);
}
