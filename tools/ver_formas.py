#!/usr/bin/env python3
"""Vista legible del conocimiento de ASA sobre la forma de los valores.

Es un CONSUMIDOR, no parte del analisis: ASA emite datos -- codigos, sitios,
identificadores -- y aqui se convierten en algo que una persona puede leer.  Esa
separacion es la que permite que manana un IDE, un JSON o un diagnostico
presenten lo mismo sin que el analisis sepa quien pregunta.

Presenta la HISTORIA de cada valor, no un veredicto:

  - agrupado por MODULO, porque dos estados de modulos distintos no estan en la
    misma cadena de transformacion y comparar entre ellos no compara nada;
  - dentro de cada modulo, los estados en orden, que es lo que convierte una
    lista de observaciones en una historia;
  - y por cada estado, que se demostro, en que ambito, y que lo limitaba.

Uso:  python tools/ver_formas.py fichero.vx [directorio-build]
"""
import collections
import os
import re
import subprocess
import sys

RE_ESTADO = re.compile(r"\[forma\] estado=(\d+) etapa=(\S+) modulo=(\S+) funciones=(\d+)")
RE_VALOR = re.compile(r"\[forma\] momento=(\S+?)#(\d+) (.*)")
RE_DETALLE = re.compile(r"\[forma\]\s{2,}(\w+)=(.*)")


def campos(texto):
    d = {}
    for trozo in texto.split():
        if "=" in trozo:
            k, v = trozo.split("=", 1)
            d[k] = v
    return d


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    fuente = sys.argv[1]
    build = sys.argv[2] if len(sys.argv) > 2 else "cmake-build-release"
    vm = os.path.join(build, "vm.exe")
    if not os.path.exists(vm):
        vm = os.path.join(build, "vm")
    tmp = os.environ.get("TEMP", "/tmp")
    entorno = dict(os.environ, VESTA_ASA_FORMAS="1")
    p = subprocess.run([vm, "--vesta", fuente, "-o", os.path.join(tmp, "ver")],
                       capture_output=True, text=True, env=entorno, timeout=300)

    estados = {}                                  # id -> (etapa, modulo, nfn)
    # (modulo, funcion, decl) -> [(estado, campos, detalles)]
    historia = collections.defaultdict(list)
    actual = None
    for linea in (p.stderr or "").splitlines():
        m = RE_ESTADO.search(linea)
        if m:
            estados[int(m.group(1))] = (m.group(2), m.group(3), int(m.group(4)))
            continue
        m = RE_VALOR.search(linea)
        if m:
            c = campos(m.group(3))
            est = int(m.group(2))
            mod = estados.get(est, ("?", "?", 0))[1]
            actual = (mod, c.get("fn", "?"), c.get("decl", "?"))
            historia[actual].append((est, c, []))
            continue
        m = RE_DETALLE.search(linea)
        if m and actual is not None and historia[actual]:
            historia[actual][-1][2].append((m.group(1), m.group(2).strip()))

    if not historia:
        print("(sin observaciones: ¿esta el volcado activo?)")
        return 1

    # Agrupado por modulo: cada uno es una cadena de transformacion aparte.
    por_modulo = collections.defaultdict(list)
    for clave in historia:
        por_modulo[clave[0]].append(clave)

    for modulo in sorted(por_modulo):
        cadena = sorted({e for k in por_modulo[modulo]
                         for e, _, _ in historia[k]})
        print("=" * 72)
        print("MODULO %s   estados: %s" % (
            modulo, " -> ".join("S%d(%s)" % (e, estados[e][0]) for e in cadena)))
        print("=" * 72)

        for clave in sorted(por_modulo[modulo], key=lambda k: (k[1], k[2])):
            obs = sorted(historia[clave], key=lambda o: o[0])
            print("\n%s   declarado en %s" % (clave[1], clave[2]))
            print("-" * 72)
            vistos = {e for e, _, _ in obs}
            for e in cadena:
                if e not in vistos:
                    # Que un valor NO aparezca en un estado es informacion: o no
                    # existia aun, o dejo de existir.  Callarlo lo esconderia.
                    print("  S%-3d %-9s  (no observado)" % (e, estados[e][0]))
                    continue
                est, c, det = next(o for o in obs if o[0] == e)
                print("  S%-3d %-9s  %s / %s   %s bytes, %s desplazamientos"
                      % (e, estados[e][0], c.get("forma", "?"),
                         c.get("certeza", "?"), c.get("bytes", "?"),
                         c.get("offsets", "?")))
                perfil = [k for k in ("unidad", "accprop", "accop", "indep",
                                      "dinamico", "escapa", "abi", "bloque")
                          if c.get(k) == "1"]
                if perfil:
                    print("         uso: %s" % ", ".join(perfil))
                for clase, resto in det:
                    if clase == "motivo":
                        print("         porque: %s" % resto)
                    elif clase == "frontera":
                        d = campos(resto)
                        print("         frontera %s: U%s -> U%s  (%s:%s)"
                              % (resto.split()[0], d.get("desde", "?"),
                                 d.get("hacia", "?"), d.get("fn", "?"),
                                 d.get("linea", "?")))
                    elif clase == "limite":
                        print("         no pude: %s" % resto)
                    elif clase == "efecto":
                        d = campos(resto)
                        print("         alcance: lo de U%s no sube a U%s (%s)"
                              % (d.get("universo", "?"),
                                 d.get("bloqueado_en", "?"),
                                 d.get("causa", "?")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
