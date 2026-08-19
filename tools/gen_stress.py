#!/usr/bin/env python3
"""Genera programas Vesta grandes para medir el compilador, no el arranque.

Por que hace falta.  El corpus de `examples_codes_vx/` esta hecho para probar
que cada caracteristica funciona, no para cargar el compilador: el ejemplo mas
grande son 456 lineas y compila en 3,2 s, de los cuales una parte importante es
arrancar el proceso y leer la biblioteca.  Perfilando eso se mide el arranque.

Este generador produce un fuente del tamano que se le pida, con una mezcla
deliberada -- aritmetica, estructuras, clases con metodos, bucles, cadenas con
interpolacion, llamadas entre funciones -- para que ninguna etapa quede fuera:

  aritmetica e indices  ->  analisis lexico y sintactico, emision de bytecode
  estructuras y campos  ->  comprobacion de tipos, calculo de posiciones
  clases y metodos      ->  registro de clases, despacho, `__module_init`
  cadenas               ->  bloques de datos del emisor
  bucles y condiciones  ->  bloques basicos, phi, saltos

El objetivo NO es que el programa haga algo util, sino que sea representativo
en forma.  El valor que devuelve se calcula de verdad para que nada se pliegue
a una constante y desaparezca lo que queriamos medir.

Uso:
    python tools/gen_stress.py --funciones 2000 -o F:/tmp/stress.vx
    python tools/gen_stress.py --escala 4 -o stress.vx
"""

import argparse
import sys

# Formas de cuerpo que se van alternando.  Cada una carga una parte distinta
# del compilador; mezclarlas evita un perfil sesgado hacia una sola etapa.
FORMAS = 6


def funcion(i: int) -> str:
    """Devuelve el texto de la funcion numero @p i.

    El cuerpo depende de `i % FORMAS`, de modo que un fichero grande contiene
    las seis formas en proporciones iguales.
    """
    f = i % FORMAS

    if f == 0:  # Aritmetica encadenada: muchos valores SSA vivos a la vez.
        return (f"i64 calc_{i}(i64 a, i64 b) {{\n"
                f"    i64 x = a * {i % 97 + 3} + b;\n"
                f"    i64 y = x - a * 2;\n"
                f"    i64 z = y + x * 3;\n"
                f"    return z - y + {i % 13};\n"
                f"}}\n")

    if f == 1:  # Bucle con acumulador: bloques basicos, phi y saltos.
        return (f"i64 calc_{i}(i64 a, i64 b) {{\n"
                f"    i64 acc = b;\n"
                f"    for (i64 k = 0; k < {i % 7 + 2}; k = k + 1) {{\n"
                f"        acc = acc + a * k;\n"
                f"    }}\n"
                f"    return acc;\n"
                f"}}\n")

    if f == 2:  # Condicionales anidados: mas aristas en el grafo de flujo.
        return (f"i64 calc_{i}(i64 a, i64 b) {{\n"
                f"    if (a > b) {{\n"
                f"        if (a > {i % 50}) {{ return a - b; }}\n"
                f"        return a + b;\n"
                f"    }} else if (b > {i % 30}) {{\n"
                f"        return b - a;\n"
                f"    }}\n"
                f"    return a ^ b;\n"
                f"}}\n")

    if f == 3:  # Estructura por valor: posiciones de campo y copia.
        return (f"struct Punto_{i} {{ i64 x; i64 y; }}\n"
                f"i64 calc_{i}(i64 a, i64 b) {{\n"
                f"    Punto_{i} p;\n"
                f"    p.x = a + {i % 11};\n"
                f"    p.y = b * 2;\n"
                f"    return p.x + p.y;\n"
                f"}}\n")

    if f == 4:  # Clase con metodo: registro de clases y despacho.
        return (f"class Caja_{i} {{\n"
                f"    public i64 v = 0;\n"
                f"    public Caja_{i}(i64 v) {{ this.v = v; }}\n"
                f"    public i64 get() {{ return this.v + {i % 17}; }}\n"
                f"}}\n"
                f"i64 calc_{i}(i64 a, i64 b) {{\n"
                f"    Caja_{i} c = new Caja_{i}(a + b);\n"
                f"    return c.get();\n"
                f"}}\n")

    # f == 5: cadena con interpolacion, que es lo que llena los bloques de datos.
    return (f"i64 calc_{i}(i64 a, i64 b) {{\n"
            f"    string s = \"caso {i}: ${{a}} y ${{b}}\";\n"
            f"    return a + b + s.length();\n"
            f"}}\n")


def genera(n: int) -> str:
    """Devuelve el fuente completo con @p n funciones mas su `main`."""
    partes = [
        "// Programa generado por tools/gen_stress.py -- no editar a mano.\n"
        "//\n"
        "// Existe para cargar el compilador lo suficiente como para que un\n"
        "// perfil diga algo.  No calcula nada con sentido; lo unico que\n"
        "// importa es que el resultado dependa de todas las funciones, para\n"
        "// que ninguna se pueda eliminar por inalcanzable.\n\n"
    ]
    partes.extend(funcion(i) for i in range(n))

    # `main` llama a todas.  Se acumula en una variable en vez de una unica
    # expresion gigante porque un arbol de n sumandos hace crecer la pila del
    # analizador de forma cuadratica y mediriamos eso.
    partes.append("\ni64 main() {\n    i64 t = 0;\n")
    partes.extend(f"    t = t + calc_{i}(t + {i % 23}, {i % 31});\n"
                  for i in range(n))
    partes.append("    return t & 0xff;\n}\n")
    return "".join(partes)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--funciones", type=int, default=1000,
                    help="numero de funciones a generar (por defecto 1000)")
    ap.add_argument("--escala", type=int,
                    help="atajo: multiplica el numero por defecto")
    ap.add_argument("-o", "--salida", required=True, help="fichero .vx de salida")
    args = ap.parse_args()

    n = args.funciones * args.escala if args.escala else args.funciones
    texto = genera(n)
    with open(args.salida, "w", encoding="ascii", newline="\n") as fh:
        fh.write(texto)

    print(f"{args.salida}: {n} funciones, {texto.count(chr(10))} lineas, "
          f"{len(texto) // 1024} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
