// Bench: axpy compound element-wise sobre arrays f64 (VECTORIZABLE).
// Hot loop interno `a[i] = a[i]*0.5 + b[i]` (mul + add) sobre arrays f64,
// M pasadas (compute-bound, cache-resident).  g++ -O3 lo auto-vectoriza.
// Resultado determinista: a[N/2] converge a 2*b[N/2] = 16.
#include <cstdint>
#include <cstdlib>

int main() {
    int N = 4096;
    int M = 50000;
    double *a = static_cast<double *>(std::malloc(static_cast<size_t>(N) * 8));
    double *b = static_cast<double *>(std::malloc(static_cast<size_t>(N) * 8));

    for (int i = 0; i < N; i++) {
        a[i] = static_cast<double>(i % 7) + 1.0;
        b[i] = static_cast<double>(i % 13) + 1.0;
    }
    for (int p = 0; p < M; p++) {
        for (int i = 0; i < N; i++) {
            a[i] = a[i] * 0.5 + b[i];
        }
    }
    double r = a[N / 2];
    std::free(a);
    std::free(b);
    return static_cast<int>(static_cast<int64_t>(r) & 0xFF);
}
