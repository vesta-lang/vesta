// callvirt: 30M llamadas a metodo VIRTUAL.
// Mismo algoritmo que main.c; ver alli donde esta la linea entre optimizar
// (bien: desvirtualizar e inlinar es calidad) y fabricar el resultado sin
// ejecutar (mal: era lo que pasaba antes).
//
// C no tiene metodos virtuales y usa un puntero a funcion, que es su
// equivalente mas cercano; aqui se usa el mecanismo idiomatico del lenguaje.
class Counter {
public:
    int value = 0;
    virtual int inc() { return value + 1; }
    virtual ~Counter() {}
};

int main() {
    Counter *c = new Counter();
    long long sum = 0;
    for (int i = 0; i < 30000000; i++) {
        c->value = (int)(((unsigned)c->inc() * 1664525u + 1013904223u) & 0xFFu);
        sum += c->value;
    }
    delete c;
    return (int)(sum % 251);
}
