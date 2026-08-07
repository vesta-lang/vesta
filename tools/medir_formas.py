#!/usr/bin/env python3
"""Mide el dominio de FORMA de ASA sobre el corpus de ejemplos.

Existe para responder a "este analisis, ¿distingue algo?" ANTES de que ningun
consumidor dependa de el.  Un clasificador que mete todo en la misma casilla
pasa desapercibido si primero se engancha y luego se mira; medir antes es lo
unico que lo destapa, y ya lo destapo dos veces.

Tabula tres cosas, y la tercera es la que dice que hay que arreglar:

  1. la distribucion de formas, por VALOR y por EJEMPLO (no dicen lo mismo:
     muchos valores en pocos programas es concentracion, no generalidad);
  2. el perfil de uso, que es lo que sostiene cada forma;
  3. el desglose de las formas sin conclusion POR MOTIVO -- separar "el programa
     no me dio senal" de "mi frontera no llego" cambia por completo que hay que
     tocar: lo primero es del programa, lo segundo del analisis.

No modifica nada: compila cada ejemplo con VESTA_ASA_FORMAS activo y lee lo que
el analisis vuelca por stderr.
"""
import collections
import os
import re
import subprocess
import sys

RE_AGREGADO = re.compile(r"\[forma\] (fn=.*)")
RE_DETALLE = re.compile(r"\[forma\]\s+(motivo|escape|limite)=(\S+)")


def campos(linea):
    d = {}
    for trozo in linea.split():
        if "=" in trozo:
            k, v = trozo.split("=", 1)
            d[k] = v
    return d


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-release"
    vm = os.path.join(build, "vm.exe")
    if not os.path.exists(vm):
        vm = os.path.join(build, "vm")
    ejemplos = sorted(
        os.path.join("examples_codes_vx", f)
        for f in os.listdir("examples_codes_vx")
        if f.endswith(".vx")
    )
    entorno = dict(os.environ, VESTA_ASA_FORMAS="1", TEMP="F:/vxtmp", TMP="F:/vxtmp")

    formas = collections.Counter()
    certezas = collections.Counter()
    perfil = collections.Counter()
    ejemplos_por_forma = collections.defaultdict(set)
    # Desglose de lo que NO concluye: por que no.
    motivos_por_forma = collections.defaultdict(collections.Counter)
    limites_por_forma = collections.defaultdict(collections.Counter)
    escapes_por_forma = collections.defaultdict(collections.Counter)
    total = 0
    fallos = 0

    for ruta in ejemplos:
        try:
            p = subprocess.run(
                [vm, "--vesta", ruta, "-o", os.path.join(entorno["TEMP"], "medida")],
                capture_output=True,
                text=True,
                timeout=180,
                env=entorno,
            )
        except Exception:
            fallos += 1
            continue
        forma_actual = None
        for linea in (p.stderr or "").splitlines():
            m = RE_AGREGADO.search(linea)
            if m:
                c = campos(m.group(1))
                forma_actual = c.get("forma", "?")
                total += 1
                formas[forma_actual] += 1
                certezas[c.get("certeza", "?")] += 1
                ejemplos_por_forma[forma_actual].add(os.path.basename(ruta))
                for s in ("completo", "unidad", "accprop", "accop", "dinamico",
                          "escapa", "abi", "dev", "bloque"):
                    if c.get(s) == "1":
                        perfil[s] += 1
                continue
            d = RE_DETALLE.search(linea)
            if d and forma_actual is not None:
                clase, valor = d.group(1), d.group(2)
                if clase == "motivo":
                    motivos_por_forma[forma_actual][valor] += 1
                elif clase == "limite":
                    limites_por_forma[forma_actual][valor] += 1
                else:
                    escapes_por_forma[forma_actual][valor] += 1

    print("=== agregados observados: %d (en %d ejemplos, %d fallos) ==="
          % (total, len(ejemplos), fallos))
    if total == 0:
        return 1

    print("\n-- forma (por valor, y en cuantos ejemplos aparece) --")
    for k, v in formas.most_common():
        print("  %-14s %5d  (%4.1f%%)   en %3d ejemplos"
              % (k, v, 100.0 * v / total, len(ejemplos_por_forma[k])))

    print("\n-- certeza --")
    for k, v in certezas.most_common():
        print("  %-14s %5d  (%4.1f%%)" % (k, v, 100.0 * v / total))

    print("\n-- perfil de uso (cuantos agregados lo presentan) --")
    for k, v in perfil.most_common():
        print("  %-14s %5d  (%4.1f%%)" % (k, v, 100.0 * v / total))

    # Lo importante: por que las que no concluyen no concluyen.
    for f in ("sin-evidencia", "desconocida"):
        if formas.get(f, 0) == 0:
            continue
        print("\n-- '%s' (%d): por que --" % (f, formas[f]))
        for k, v in motivos_por_forma[f].most_common():
            print("   motivo  %-30s %5d" % (k, v))
        for k, v in limites_por_forma[f].most_common():
            print("   limite  %-30s %5d   <- del ANALISIS" % (k, v))
        for k, v in escapes_por_forma[f].most_common():
            print("   escape  %-30s %5d   <- del PROGRAMA" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
