# Bench: axpy compound element-wise sobre arrays f64 (VECTORIZABLE).
# Hot loop interno a[i] = a[i]*0.5 + b[i] (mul + add), M pasadas.
# Python puro (sin numpy) -> lento por diseno (sirve de techo del rango).
# Resultado determinista: a[N/2] converge a 2*b[N/2] = 16.
import sys


def main() -> int:
    N = 4096
    M = 50000
    a = [float(i % 7) + 1.0 for i in range(N)]
    b = [float(i % 13) + 1.0 for i in range(N)]
    for _ in range(M):
        for i in range(N):
            a[i] = a[i] * 0.5 + b[i]
    r = a[N // 2]
    return int(r) & 0xFF


if __name__ == "__main__":
    sys.exit(main())
