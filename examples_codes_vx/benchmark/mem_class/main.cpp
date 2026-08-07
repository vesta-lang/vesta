// mem_class: 1M objetos en heap, con una ventana de 64 vivos.
// Mismo algoritmo que main.c; ver alli el porque de la ventana y en que se
// diferencia este banco del de bloques crudos (`alloc`).
static const int ITERS = 1000000;
static const int VIVOS = 64;   // potencia de 2

class Foo { public: int x; Foo(int v) : x(v) {} };

int main() {
    Foo *anillo[VIVOS];
    for (int k = 0; k < VIVOS; k++) anillo[k] = nullptr;

    int acc = 0;
    for (int i = 0; i < ITERS; i++) {
        Foo *f = new Foo(i & 0xFF);
        int k = i & (VIVOS - 1);
        if (anillo[k] != nullptr) {   // el mas viejo sale de la ventana
            acc += anillo[k]->x;
            delete anillo[k];
        }
        anillo[k] = f;
    }
    for (int k = 0; k < VIVOS; k++) { // vaciar la ventana
        if (anillo[k] != nullptr) { acc += anillo[k]->x; delete anillo[k]; }
    }
    return acc & 0xFF;
}
