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

RE_AGREGADO = re.compile(r"\[forma\] (momento=.*)")
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

    # (ejemplo, funcion, sitio de declaracion) -> {momento: forma}.  Esa clave
    # es la identidad del valor a traves del pipeline.
    visto = collections.defaultdict(dict)
    incoherentes = []
    formas_por_momento = collections.defaultdict(collections.Counter)
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
                formas_por_momento[c.get("momento", "?")][forma_actual] += 1
                clave = (os.path.basename(ruta), c.get("fn", "?"),
                         c.get("decl", "?"))
                anterior = visto[clave].get(c.get("momento", "?"))
                if anterior is not None and anterior != forma_actual:
                    # El mismo valor observado dos veces en el mismo momento
                    # con formas distintas: se marca en vez de elegir una.
                    visto[clave][c.get("momento", "?")] = "INCOHERENTE"
                    incoherentes.append((clave, c.get("momento", "?"),
                                         anterior, forma_actual))
                else:
                    visto[clave][c.get("momento", "?")] = forma_actual
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

    if incoherentes:
        print("\n-- el MISMO valor visto dos veces en el MISMO momento (%d) --"
              % len(incoherentes))
        for k, mom, a1, a2 in incoherentes[:8]:
            print("   %s %s decl=%s  %s: %s vs %s"
                  % (k[0], k[1], k[2], mom, a1, a2))

    print("\n-- forma (por valor, y en cuantos ejemplos aparece) --")
    for k, v in formas.most_common():
        print("  %-14s %5d  (%4.1f%%)   en %3d ejemplos"
              % (k, v, 100.0 * v / total, len(ejemplos_por_forma[k])))

    # LA TRANSICION: que le paso a cada valor entre los dos momentos.  Se
    # correlaciona por SITIO DE DECLARACION, que es lo unico que sobrevive al
    # pipeline (los value-id se renumeran).  Tres desenlaces y cada uno dice
    # algo distinto; el tercero es el que hay que mirar con lupa, porque una
    # transformacion no deberia cambiar la naturaleza semantica de un valor.
    transicion = collections.Counter()
    cambios = collections.Counter()
    # Una unidad que pasa a saco: o el optimizador la rompio, o uno de los dos
    # analisis miente.  Se listan con nombre y sitio para poder ir a mirarlas.
    sospechosos = []
    for clave, por_momento in visto.items():
        pre = por_momento.get("pre-opt")
        post = por_momento.get("post-opt")
        if pre is None:
            transicion["solo-despues (aparece al optimizar)"] += 1
        elif post is None:
            transicion["desaparece: %s" % pre] += 1
        elif pre == post:
            transicion["sobrevive igual: %s" % pre] += 1
        else:
            transicion["CAMBIA DE FORMA"] += 1
            cambios["%s -> %s" % (pre, post)] += 1
            if pre == "compuesto" and post == "agregado":
                sospechosos.append(clave)
    print("\n-- transicion pre-opt -> post-opt (%d valores distintos) --"
          % len(visto))
    for k, v in transicion.most_common():
        print("  %-34s %5d" % (k, v))
    if cambios:
        print("\n   cambios de forma (revisar: o el optimizador rompio algo,")
        print("   o uno de los dos analisis miente):")
        for k, v in cambios.most_common():
            print("     %-28s %5d" % (k, v))
    if sospechosos:
        print("\n   unidad -> saco (%d).  Los primeros:" % len(sospechosos))
        for k in sospechosos[:12]:
            print("     %s  %s  decl=%s" % k)

    # Dos poblaciones, no una: antes y despues de optimizar son dos verdades del
    # mismo programa, y mezclarlas hace creer que se ve "los agregados del
    # programa" cuando en realidad se ve "los que sobrevivieron".
    for mom in sorted(formas_por_momento):
        sub = formas_por_momento[mom]
        n = sum(sub.values())
        print("\n-- momento=%s (%d agregados) --" % (mom, n))
        for k, v in sub.most_common():
            print("  %-14s %5d  (%4.1f%%)" % (k, v, 100.0 * v / n))

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
