#!/usr/bin/env python3
# VestaVM -- Maquina Virtual Distribuida
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
"""Foto del IR de todos los ejemplos, para VER que cambio en el bajado.

Existe porque la suite e2e comprueba que los programas dan el mismo RESULTADO,
no que se genere el mismo CODIGO.  Un cambio en el bajado puede alterar lo
emitido y seguir dando el mismo numero -- ya paso: sacar una familia de
builtins cambio el ORDEN de dos bloques, el plegado al compilar dejo de
ocurrir, y la suite paso igual porque el resultado era el mismo.

LO QUE ESTO NO ES: un veredicto.  Que dos fotos difieran no significa que algo
se rompiera, y que coincidan no significa que este bien.  Si un cambio ARREGLA
un fallo del bajado, la foto marca diferencia -- exactamente igual que si lo
hubiera roto --, y tratar esa diferencia como una regresion seria deshacer el
arreglo.  Al reves tambien: dos fotos identicas solo dicen que se emite lo
mismo, no que lo que se emite sea correcto.

Lo que SI hace es decir DONDE mirar: en que ejemplos cambio lo emitido, para
leer el cambio y decidir si era el que se buscaba.  La respuesta la da leer el
diff, no la herramienta.

    python tools/ir_snapshot.py cmake-build-release /ruta/antes
    ... cambiar el codigo y reconstruir ...
    python tools/ir_snapshot.py cmake-build-release /ruta/despues
    python tools/ir_snapshot.py --diff /ruta/antes /ruta/despues
    python tools/ir_snapshot.py --diff /ruta/antes /ruta/despues --show 42_caso

Los ejemplos que no compilan (los negativos, que EXIGEN un error) se apuntan
como tales y se comparan igual: que uno empiece o deje de compilar tambien es
un cambio que hay que mirar.
"""
import argparse
import os
import subprocess
import sys
import tempfile

EXAMPLES = "examples_codes_vx"


MARCA_INESTABLE = "NO REPRODUCIBLE: difiere consigo mismo entre dos corridas\n"


def snapshot(build_dir: str, out_dir: str, check_stable: bool = False) -> int:
    """Vuelca el IR de cada ejemplo a @p out_dir.  Devuelve cuantos salieron.

    Con @p check_stable cada ejemplo se compila DOS veces y, si las dos
    salidas no coinciden, se apunta como no reproducible en lugar de guardar
    una de ellas.  Los hay: un ejemplo que ejecuta cosas al compilar puede
    hornear el identificador del proceso del compilador, y entonces difiere de
    si mismo.  Sin esto aparece en cada comparacion como si el cambio lo
    hubiera tocado, y se pierde el tiempo mirandolo.
    """
    vm = os.path.join(build_dir, "vm.exe")
    if not os.path.exists(vm):
        vm = os.path.join(build_dir, "vm")
    if not os.path.exists(vm):
        print(f"no encuentro el binario en {build_dir}", file=sys.stderr)
        return -1
    os.makedirs(out_dir, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="irsnap_")
    n_ok = n_fail = n_inest = 0

    def volcar(src: str, base: str):
        """Compila un ejemplo y devuelve su IR ya normalizado, o None."""
        r = subprocess.run(
            [vm, "--vx-emit-ir", "--vesta", src, "-o", base],
            capture_output=True, text=True, timeout=300)
        ir = base + ".ir"
        if r.returncode != 0 or not os.path.exists(ir):
            return None, r.returncode
        # El .ir lleva rutas absolutas del temporal; se normalizan para que dos
        # fotos hechas en directorios distintos sigan comparandose.
        with open(ir, encoding="utf-8", errors="replace") as fh:
            return fh.read().replace(tmp.replace("\\", "/"), "<tmp>"), 0

    for name in sorted(os.listdir(EXAMPLES)):
        if not name.endswith(".vx"):
            continue
        stem = name[:-3]
        src = os.path.join(EXAMPLES, name)
        dst = os.path.join(out_dir, stem + ".ir")
        text, rc = volcar(src, os.path.join(tmp, stem))
        if text is None:
            # No compila: se guarda ESE hecho, no el volcado.  Que un ejemplo
            # empiece o deje de compilar es tan cambio como el IR.
            with open(dst, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(f"NO COMPILA (rc={rc})\n")
            n_fail += 1
            continue
        if check_stable:
            otra, _ = volcar(src, os.path.join(tmp, stem + "__2"))
            if otra != text:
                with open(dst, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(MARCA_INESTABLE)
                n_inest += 1
                continue
        with open(dst, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        n_ok += 1
    extra = f", {n_inest} no reproducibles" if check_stable else ""
    print(f"{n_ok} con IR, {n_fail} sin compilar{extra} -> {out_dir}")
    return n_ok


def diff(a_dir: str, b_dir: str, show: str = "") -> int:
    """Compara dos fotos y dice DONDE cambio lo emitido.

    No dictamina: una diferencia es una pregunta -- que cambio y por que --,
    y la respuesta sale de leerla con @p show, no de este recuento.

    @param a_dir Foto anterior.
    @param b_dir Foto posterior.
    @param show  Nombre de un ejemplo del que imprimir el diff entero.
    @return Cuantos ejemplos difieren.
    """
    names = sorted(set(os.listdir(a_dir)) | set(os.listdir(b_dir)))
    distintos = []
    for n in names:
        pa, pb = os.path.join(a_dir, n), os.path.join(b_dir, n)
        if not os.path.exists(pa) or not os.path.exists(pb):
            distintos.append((n, "solo en una de las dos fotos"))
            continue
        with open(pa, encoding="utf-8", errors="replace") as fh:
            ta = fh.read().splitlines(keepends=True)
        with open(pb, encoding="utf-8", errors="replace") as fh:
            tb = fh.read().splitlines(keepends=True)
        if ta == tb:
            continue
        if ta == [MARCA_INESTABLE] or tb == [MARCA_INESTABLE]:
            # No se puede comparar: la foto ya dice que difiere de si mismo.
            continue
        distintos.append((n, f"{len(ta)} -> {len(tb)} lineas"))
        if show and show in n:
            import difflib
            sys.stdout.writelines(
                difflib.unified_diff(ta, tb, f"antes/{n}", f"despues/{n}"))
    if not distintos:
        print(f"IDENTICOS: {len(names)} ejemplos, ni un byte de diferencia.")
        print("Eso dice que se emite LO MISMO, no que lo emitido sea correcto.")
        return 0
    print(f"CAMBIO lo emitido en {len(distintos)} de {len(names)}:")
    for n, why in distintos[:40]:
        print(f"  {n}: {why}")
    if len(distintos) > 40:
        print(f"  ... y {len(distintos) - 40} mas")
    print("Cambiar no es romper: mira cada uno con --show <nombre> y decide si")
    print("es el cambio que buscabas.  Un arreglo tambien sale aqui.")
    return len(distintos)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("args", nargs="+")
    ap.add_argument("--diff", action="store_true",
                    help="comparar dos fotos en lugar de hacer una")
    ap.add_argument("--show", default="",
                    help="imprimir el diff entero de ese ejemplo")
    ap.add_argument("--check-stable", action="store_true",
                    help="compilar cada ejemplo dos veces y apuntar los que "
                         "difieren de si mismos (tarda el doble)")
    ns = ap.parse_args()
    if ns.diff:
        if len(ns.args) != 2:
            print("--diff necesita dos directorios", file=sys.stderr)
            return 2
        # Difieren -> 1, para que un guion sepa que hay algo que MIRAR.  No es
        # un fallo: es que hay una pregunta abierta.
        return 1 if diff(ns.args[0], ns.args[1], ns.show) else 0
    if len(ns.args) != 2:
        print("uso: ir_snapshot.py <build_dir> <out_dir>", file=sys.stderr)
        return 2
    return 0 if snapshot(ns.args[0], ns.args[1], ns.check_stable) > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
