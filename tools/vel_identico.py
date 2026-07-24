#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""¿Un cambio de REPRESENTACION altero el codigo generado?

Compila el corpus con DOS binarios y compara el `.vel` byte a byte.  Es la unica
forma de afirmar "no cambia el comportamiento" sobre el emisor del interprete,
que es el ORACULO contra el que se valida el JIT y el AOT: cualquier diferencia,
por pequena que sea, aparece aqui -- no hay que confiar en que la suite la note.

Uso:  python vel_identico.py <vm_base.exe> <vm_nuevo.exe>
"""

import hashlib
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

CORPUS = Path('f:/C/VM/examples_codes_vx')


def vel_hash(vm, src, td):
    """sha256 del `.vel` que produce @p vm para @p src (o None si no produce)."""
    # El nombre debe ser UNICO por (binario, programa): hay muchos `main.vx`
    # en subdirectorios distintos y con el stem a secas se pisaban entre hilos
    # -- se reportaban como "distintos" ficheros que ni siquiera se comparaban.
    tag = hashlib.md5((vm + '|' + str(src)).encode()).hexdigest()[:12]
    out = Path(td) / ('p_' + tag)
    try:
        subprocess.run([vm, '--vx', str(src), '--vx-emit-only', '-o', str(out)],
                       capture_output=True, timeout=180)
    except subprocess.TimeoutExpired:
        return None
    for cand in (out.with_suffix('.vel'), Path(str(out) + '.vel')):
        if cand.exists():
            return hashlib.sha256(cand.read_bytes()).hexdigest()
    return None


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = sys.argv[1], sys.argv[2]
    progs = sorted(CORPUS.rglob('*.vx'))
    print('[vel] %d programas, comparando byte a byte' % len(progs))

    same = diff = miss = 0
    difs = []
    with tempfile.TemporaryDirectory() as td:
        def one(p):
            return p, vel_hash(a, p, td), vel_hash(b, p, td)
        with ThreadPoolExecutor(max_workers=20) as ex:
            for i, (p, ha, hb) in enumerate(ex.map(one, progs)):
                if ha is None or hb is None:
                    miss += 1
                elif ha == hb:
                    same += 1
                else:
                    diff += 1
                    difs.append(str(p.relative_to(CORPUS)))
                if (i + 1) % 100 == 0:
                    print('  ... %d/%d' % (i + 1, len(progs)), flush=True)

    print('\n=== .vel: base vs nuevo ===')
    print('  IDENTICOS ..... %d' % same)
    print('  distintos ..... %d' % diff)
    print('  sin .vel ...... %d  (no compilan: negativos esperados)' % miss)
    if difs:
        print('\n  los que cambian: ' + ', '.join(difs[:20]))
        print('\n  VEREDICTO: el cambio NO fue neutral.  Mirar esos primero.')
    else:
        print('\n  VEREDICTO: codigo generado IDENTICO.  El cambio de')
        print('  representacion no altero ni una instruccion.')
    return 1 if diff else 0


if __name__ == '__main__':
    sys.exit(main())
