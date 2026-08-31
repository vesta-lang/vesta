#!/usr/bin/env python3
"""corpus_memoria.py -- ejecuta el corpus VIGILANDO la memoria, con tope.

Para que existe: un ejemplo que se desboca reservando memoria tumba la maquina
entera, y cuando eso pasa no queda ni rastro de CUAL fue.  Esta herramienta los
corre de uno en uno con un tope; el que lo pase se MATA y se anota, y la
maquina sigue en pie.

    python tools/corpus_memoria.py cmake-build-release/vm.exe
    python tools/corpus_memoria.py cmake-build-release/vm.exe --tope 1024
    python tools/corpus_memoria.py cmake-build-release/vm.exe -k gc --modo aot

Lo que mide es el pico de memoria RESIDENTE del proceso (y de sus hijos, que el
modo AOT lanza un ejecutable aparte), muestreado mientras corre.

Complementa a las que ya hay: `corpus_compila.py` dice si algo se CAE al
compilar, la suite `e2e_test.py` si da el resultado correcto, y esta si algo se
come la maquina al EJECUTAR.
"""

import argparse
import os
import subprocess
import sys
import threading
import time

import psutil

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(RAIZ, "examples_codes_vx")


def pico_de(proc, tope_bytes, parar):
    """Muestrea la memoria del proceso y sus hijos; mata si pasa del tope.

    Devuelve el pico visto en bytes.  Sale en cuanto el proceso termina o
    alguien pide parar.
    """
    pico = 0
    try:
        p = psutil.Process(proc.pid)
    except psutil.Error:
        return 0
    while not parar.is_set() and proc.poll() is None:
        total = 0
        try:
            total = p.memory_info().rss
            for h in p.children(recursive=True):
                try:
                    total += h.memory_info().rss
                except psutil.Error:
                    pass
        except psutil.Error:
            break
        if total > pico:
            pico = total
        if tope_bytes and total > tope_bytes:
            # Se pasa del tope: se mata el arbol entero.  Matar solo al padre
            # dejaria vivo al ejecutable nativo que lanzo el modo AOT, que es
            # justo el que puede estar comiendose la maquina.
            try:
                for h in p.children(recursive=True):
                    h.kill()
                p.kill()
            except psutil.Error:
                pass
            return pico
        time.sleep(0.02)
    return pico


def corre(cmd, cwd, tope_bytes, segundos):
    """Lanza @p cmd vigilado.  Devuelve (rc, pico_bytes, motivo)."""
    try:
        proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
    except OSError as e:
        return (None, 0, "no arranca: %s" % e)
    parar = threading.Event()
    resultado = {}

    def vigila():
        resultado["pico"] = pico_de(proc, tope_bytes, parar)

    hilo = threading.Thread(target=vigila, daemon=True)
    hilo.start()
    try:
        rc = proc.wait(timeout=segundos)
        motivo = ""
    except subprocess.TimeoutExpired:
        try:
            psutil.Process(proc.pid).kill()
        except psutil.Error:
            pass
        rc, motivo = None, "tiempo"
    parar.set()
    hilo.join(timeout=2)
    pico = resultado.get("pico", 0)
    if tope_bytes and pico > tope_bytes:
        motivo = "PASA DEL TOPE"
    return (rc, pico, motivo)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vm", help="ruta al binario vm")
    ap.add_argument("--corpus", default=CORPUS, help="directorio de ejemplos")
    ap.add_argument("--tope", type=int, default=2048,
                    help="tope en MB por ejemplo (0 = sin tope)")
    ap.add_argument("--segundos", type=int, default=60, help="tope de tiempo")
    ap.add_argument("--modo", default="jit", choices=["vm", "jit", "aot"],
                    help="con que se ejecuta")
    ap.add_argument("-k", "--filtro", default="",
                    help="solo los ejemplos cuyo nombre contenga esto")
    args = ap.parse_args()

    vm = os.path.abspath(args.vm)
    tope = args.tope * 1024 * 1024
    tmp = os.environ.get("TEMP", os.path.join(RAIZ, ".tmp_mem"))
    os.makedirs(tmp, exist_ok=True)

    fuentes = []
    for dp, _, fns in os.walk(args.corpus):
        for fn in sorted(fns):
            if fn.endswith(".vx") and args.filtro in fn:
                fuentes.append(os.path.join(dp, fn))
    fuentes.sort()
    print("%d ejemplos, modo %s, tope %d MB" % (len(fuentes), args.modo,
                                                args.tope))

    picos, malos = [], []
    for i, src in enumerate(fuentes, 1):
        nom = os.path.splitext(os.path.basename(src))[0]
        out = os.path.join(tmp, "mem_" + nom)
        sys.stdout.write("\r  %d/%d %-40s" % (i, len(fuentes), nom[:40]))
        sys.stdout.flush()
        if args.modo == "aot":
            cmd = [vm, "-m", "aot", "--vesta", src, "--emit", "exe", "-o", out]
            rc, pico, motivo = corre(cmd, RAIZ, tope, args.segundos)
            if rc != 0 or not os.path.exists(out):
                continue  # no compila en este nivel: no es lo que se busca
            rc, pico, motivo = corre([out], RAIZ, tope, args.segundos)
        else:
            rc, _, _ = corre([vm, "--vesta", src, "-o", out], RAIZ, tope,
                             max(args.segundos, 300))
            velb = out + ".velb"
            if rc != 0 or not os.path.exists(velb):
                continue
            cmd = [vm, "--run", velb]
            if args.modo == "vm":
                cmd += ["-m", "vm"]
            rc, pico, motivo = corre(cmd, RAIZ, tope, args.segundos)
        picos.append((pico, nom))
        if motivo:
            malos.append((nom, pico, motivo))
    print()

    picos.sort(reverse=True)
    print("\nlos que mas memoria piden al ejecutar:")
    for pico, nom in picos[:15]:
        print("   %8.1f MB  %s" % (pico / 1048576.0, nom))
    if malos:
        print("\n%d PROBLEMA(S):" % len(malos))
        for nom, pico, motivo in malos:
            print("   %-44s %8.1f MB  %s" % (nom, pico / 1048576.0, motivo))
        return 1
    print("\nninguno pasa del tope.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
