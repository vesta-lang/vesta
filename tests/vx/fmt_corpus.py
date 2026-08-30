"""Genera la lista de ficheros .vx sobre la que corre `fmt_test`.

El test necesita saber que ficheros mirar, y no hay glob portable en C++17 sin
`<filesystem>` -- que en este arbol ha dado guerra con mas de un toolchain --.
Se resuelve fuera: aqui se escribe la lista y el test la lee.

Uso:  python tests/vx/fmt_corpus.py
"""

import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORPUS_DIRS = ('examples_codes_vx', os.path.join('stdlib', 'vx'))
OUTPUT = os.path.join(ROOT, 'tests', 'vx', 'fmt_corpus.txt')


def collect():
    """Recorre los directorios del corpus y devuelve las rutas relativas."""
    paths = []
    for folder in CORPUS_DIRS:
        base = os.path.join(ROOT, folder)
        for current, _, files in os.walk(base):
            for name in files:
                if not name.endswith('.vx'):
                    continue
                full = os.path.join(current, name)
                paths.append(os.path.relpath(full, ROOT).replace('\\', '/'))
    paths.sort()
    return paths


def main():
    """Escribe la lista del corpus."""
    paths = collect()
    header = [
        '# Corpus de fmt_test: todos los .vx escritos a mano del arbol.',
        '# Se regenera con: python tests/vx/fmt_corpus.py',
        '',
    ]
    with io.open(OUTPUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(header + paths) + '\n')
    print('%d ficheros -> %s' % (len(paths), os.path.relpath(OUTPUT, ROOT)))


main()
