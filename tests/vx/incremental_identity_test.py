#!/usr/bin/env python3
"""Reconstruir a trozos tiene que dar lo MISMO que construir de cero.

    python3 tests/vx/incremental_identity_test.py <dir_build> [-k filtro] [--keep]

Esta es la propiedad que sostiene toda la compilacion incremental, y la unica
forma de romperla es en silencio: el compilador termina bien, el programa
arranca, los tests de comportamiento pasan, y el binario NO es el que
corresponde al codigo fuente.  Nadie lo nota hasta que alguien compara dos
artefactos que deberian ser iguales -- que es justo lo que hace este fichero.

Se comprueban tres cosas distintas, y son distintas a proposito:

  identidad     Tocar un fichero y reconstruir con las caches puestas produce
                el mismo binario, byte a byte, que borrar las caches y
                construir desde cero.  Es LA propiedad.  Si falla, alguna
                entrada del compilador no esta en la clave de la cache.

  ida y vuelta  Deshacer el cambio y reconstruir devuelve el binario original.
                Cubre el caso que la identidad no ve: una cache que guarda algo
                de mas y lo arrastra hacia adelante da el binario correcto la
                primera vez y el equivocado al volver.

  configuracion Compilar para otro objetivo (@Target / --aot-arch / nivel de
                optimizacion) y volver al primero devuelve el binario del
                primero.  El optimizador decide con la ISA de destino -- el
                if-conversion no vale lo mismo con `cmov` de x86 que con `csel`
                de ARM64 --, y esa ISA es estado GLOBAL que no viaja dentro de
                la funcion.  Si no esta en la clave, la segunda compilacion
                sirve el codigo pensado para la primera.

Las tres se comprueban sobre un proyecto de VARIOS modulos, porque el caso de
un solo fichero no distingue: ahi la unidad de cache y la unidad de cambio son
la misma cosa y cualquier implementacion parece correcta.

Salida 0 si todo pasa; != 0 con el detalle de que difiere y donde.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]

# El sello de tiempo de la cabecera cambia entre dos compilaciones y no dice
# nada del codigo.  Es el UNICO desvio que se perdona: cualquier otro byte
# distinto significa que el codigo emitido no es el mismo.
#
# El campo ENTERO, 32..39 (`build_timestamp` de HeaderVELB, 8 bytes justo
# detras de `flags`).  Taparlo a medias no da error: da un test que pasa casi
# siempre y falla cuando las dos compilaciones caen a caballo de un segundo, y
# entonces lo que hay que depurar es el test.  Ya paso.
SELLO_TIEMPO = range(32, 40)

# Cuantos modulos tiene el proyecto de prueba.  Suficientes para que "un
# modulo" sea una fraccion pequena del total (si fueran dos, reconstruir uno
# seria la mitad del trabajo y la diferencia entre incremental y de cero no se
# apreciaria), y pocos para que el test no tarde.
N_MODULOS = 6


def fuente_modulo(idx: int, funcs: int = 12) -> str:
    """Un modulo con `funcs` ayudantes internos y UNA funcion publica que los suma.

    Una sola exportada porque asi importa el lenguaje (`import "mN" only X`), y
    ademas conviene al test: los ayudantes son privados, de modo que un cambio
    en su cuerpo pone a prueba justo lo interesante -- que el compilador sepa
    que eso no altera lo que el modulo OFRECE.

    El `* 3 +` de cada ayudante es el ancla de la mutacion "cuerpo": cambiarlo
    por `* 4 +` altera la implementacion sin tocar ninguna firma.
    """
    out = ["namespace inc.m%d;" % idx, ""]
    for i in range(funcs):
        out.append("i64 aux%d_%d(i64 x) {" % (idx, i))
        out.append("    i64 s = x * 3 + %d;" % (i + 1))
        out.append("    return s + %d;" % idx)
        out.append("}")
        out.append("")
    out.append("public i64 entry%d(i64 x) {" % idx)
    out.append("    i64 t = 0;")
    for i in range(funcs):
        out.append("    t = t + aux%d_%d(x + %d);" % (idx, i, i))
    out.append("    return t;")
    out.append("}")
    return "\n".join(out)


def fuente_main(n: int, funcs: int = 12) -> str:
    out = []
    for i in range(n):
        out.append('import "m%d" only entry%d;' % (i, i))
    out.append("")
    out.append("i32 main() {")
    out.append("    i64 t = 0;")
    for i in range(n):
        out.append("    t = t + entry%d(%d);" % (i, i + 1))
    out.append("    return (i32)(t % 1000);")
    out.append("}")
    return "\n".join(out)


def crear_proyecto(dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for i in range(N_MODULOS):
        (dest / ("m%d.vx" % i)).write_text(fuente_modulo(i), encoding="utf-8")
    (dest / "main.vx").write_text(fuente_main(N_MODULOS), encoding="utf-8")


def mutar(ruta: Path, clase: str) -> bool:
    """Aplica un cambio de la PROFUNDIDAD pedida.  False si no aplica.

    Cada clase responde a una pregunta distinta sobre la granularidad:

      comentario  cambia el fichero sin cambiar nada de lo que declara.
      cuerpo      cambia una implementacion, no una firma.
      interfaz    aparece una funcion publica nueva: lo que el modulo OFRECE
                  cambia, asi que revalidar a los dependientes es obligado.
    """
    texto = ruta.read_text(encoding="utf-8")
    if clase == "comentario":
        ruta.write_text(texto + "\n// tocado\n", encoding="utf-8")
        return True
    if clase == "cuerpo":
        if "* 3 +" not in texto:
            return False
        ruta.write_text(texto.replace("* 3 +", "* 4 +", 1), encoding="utf-8")
        return True
    if clase == "interfaz":
        ruta.write_text(texto + "\npublic i64 nueva_func(i64 x) { return x + 7; }\n",
                        encoding="utf-8")
        return True
    return False


def orden(vm: str, proyecto: Path, salida: Path, modo: str,
          arch: str = "x86-64", opt: str | None = None) -> list[str]:
    if modo == "aot":
        fmt = "pe" if sys.platform == "win32" else "elf"
        cmd = [vm, "-m", "aot", "--vx", str(proyecto / "main.vx"),
               "-o", str(salida), "--emit", "exe", "--format", fmt,
               "--aot-arch", arch]
    else:
        cmd = [vm, "--vesta", str(proyecto / "main.vx"), "-o", str(salida)]
    if opt:
        cmd += ["-O", opt]
    return cmd


def artefacto(salida: Path) -> Path | None:
    for c in (salida, salida.with_suffix(".velb"), salida.with_suffix(".exe"),
              Path(str(salida) + ".velb"), Path(str(salida) + ".exe")):
        if c.is_file():
            return c
    return None


def compilar(vm, proyecto, salida, modo, cache_dir, arch="x86-64", opt=None):
    """Compila y devuelve la ruta del artefacto, o None si fallo."""
    entorno = dict(os.environ, VX_CACHE_DIR=str(cache_dir),
                   VX_CAS_DIR=str(Path(cache_dir) / "cas"))
    r = subprocess.run(orden(vm, proyecto, salida, modo, arch, opt),
                       capture_output=True, env=entorno, timeout=600,
                       cwd=str(RAIZ))
    if r.returncode != 0:
        return None, (r.stderr or b"").decode("utf-8", "replace")[-600:]
    a = artefacto(salida)
    return a, ""


def purgar(proyecto: Path, cache_dir: Path) -> None:
    """Deja el arbol como si nunca se hubiera compilado.

    Hay que quitar las DOS cosas: el directorio de cache y los artefactos por
    modulo que se escriben junto al fuente.  Olvidar los segundos hace que
    'de cero' no sea de cero y el test compare dos incrementales -- que es la
    forma de que pase siempre sin comprobar nada.
    """
    shutil.rmtree(cache_dir, ignore_errors=True)
    for patron in ("*.vxi", "*.vxir", "*.vel", "*.velb"):
        for f in proyecto.glob(patron):
            f.unlink(missing_ok=True)


def difieren(a: Path, b: Path):
    ba, bb = a.read_bytes(), b.read_bytes()
    if len(ba) != len(bb):
        return [("tamano", len(ba), len(bb))]
    return [(i, ba[i], bb[i]) for i in range(len(ba))
            if ba[i] != bb[i] and i not in SELLO_TIEMPO]


def informe(desvios) -> str:
    if desvios and desvios[0][0] == "tamano":
        _, x, y = desvios[0]
        return "tamanos distintos: %d vs %d bytes (%+d)" % (x, y, y - x)
    n = len(desvios)
    return "%d bytes distintos, el primero en el desplazamiento %d" % (
        n, desvios[0][0])


# ---------------------------------------------------------------------------
#  Las tres comprobaciones
# ---------------------------------------------------------------------------

def caso_identidad(vm, tmp, modo, clase, resultados):
    """Incremental tras un cambio == de cero con el codigo ya cambiado."""
    tag = "identidad/%s/%s" % (modo, clase)
    proy = tmp / ("id_%s_%s" % (modo, clase))
    cache = tmp / ("cache_id_%s_%s" % (modo, clase))
    crear_proyecto(proy)

    purgar(proy, cache)
    base, err = compilar(vm, proy, proy / "out", modo, cache)
    if base is None:
        resultados.append((tag, False, "no compila de partida: " + err))
        return
    # El estado de partida se guarda para la ida y vuelta.
    guardado = tmp / (tag.replace("/", "_") + ".base")
    shutil.copyfile(base, guardado)
    fuentes = {f.name: f.read_text(encoding="utf-8")
               for f in proy.glob("*.vx")}

    if not mutar(proy / "m0.vx", clase):
        resultados.append((tag, False, "la mutacion no aplica"))
        return

    # (a) reconstruir CON las caches puestas.
    inc, err = compilar(vm, proy, proy / "out", modo, cache)
    if inc is None:
        resultados.append((tag, False, "no compila incremental: " + err))
        return
    copia_inc = tmp / (tag.replace("/", "_") + ".inc")
    shutil.copyfile(inc, copia_inc)

    # (b) el MISMO codigo, desde cero.
    purgar(proy, cache)
    cero, err = compilar(vm, proy, proy / "out", modo, cache)
    if cero is None:
        resultados.append((tag, False, "no compila de cero: " + err))
        return

    d = difieren(copia_inc, cero)
    resultados.append((tag, not d,
                       "" if not d else "incremental != de cero: " + informe(d)))

    # ida y vuelta: deshacer el cambio y reconstruir con caches.
    tag2 = "ida-y-vuelta/%s/%s" % (modo, clase)
    for nombre, contenido in fuentes.items():
        (proy / nombre).write_text(contenido, encoding="utf-8")
    vuelta, err = compilar(vm, proy, proy / "out", modo, cache)
    if vuelta is None:
        resultados.append((tag2, False, "no compila al volver: " + err))
        return
    d2 = difieren(guardado, vuelta)
    resultados.append((tag2, not d2,
                       "" if not d2 else "volver != original: " + informe(d2)))


def caso_configuracion(vm, tmp, resultados):
    """Compilar para otra configuracion y volver devuelve el binario de la primera.

    El optimizador decide con la ISA de destino y con el nivel de optimizacion.
    Si alguno no esta en la clave de la cache, la vuelta sirve lo que se guardo
    para la ida.
    """
    proy = tmp / "cfg"
    cache = tmp / "cache_cfg"
    crear_proyecto(proy)

    # Eje 1: la arquitectura de destino del AOT.
    purgar(proy, cache)
    a1, err = compilar(vm, proy, proy / "o64", "aot", cache, arch="x86-64")
    if a1 is None:
        resultados.append(("configuracion/arch", False, "no compila x86-64: " + err))
    else:
        ref = tmp / "cfg_x86_64.ref"
        shutil.copyfile(a1, ref)
        # Compilar para otra arquitectura CON la misma cache.
        compilar(vm, proy, proy / "o32", "aot", cache, arch="x86-32")
        # Y volver.
        a3, err = compilar(vm, proy, proy / "o64b", "aot", cache, arch="x86-64")
        if a3 is None:
            resultados.append(("configuracion/arch", False,
                               "no compila al volver a x86-64: " + err))
        else:
            d = difieren(ref, a3)
            resultados.append(("configuracion/arch", not d,
                               "" if not d else
                               "volver a x86-64 != x86-64 original: " + informe(d)))

    # Eje 2: el nivel de optimizacion.
    purgar(proy, cache)
    b1, err = compilar(vm, proy, proy / "p2", "vm", cache, opt="2")
    if b1 is None:
        resultados.append(("configuracion/opt", False, "no compila -O2: " + err))
        return
    ref2 = tmp / "cfg_O2.ref"
    shutil.copyfile(b1, ref2)
    compilar(vm, proy, proy / "p0", "vm", cache, opt="0")
    b3, err = compilar(vm, proy, proy / "p2b", "vm", cache, opt="2")
    if b3 is None:
        resultados.append(("configuracion/opt", False, "no compila al volver: " + err))
        return
    d = difieren(ref2, b3)
    resultados.append(("configuracion/opt", not d,
                       "" if not d else "volver a -O2 != -O2 original: " + informe(d)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("build_dir")
    ap.add_argument("-k", dest="filtro", default="",
                    help="ejecutar solo los casos cuyo nombre contenga esto")
    ap.add_argument("--keep", action="store_true",
                    help="no borrar el directorio temporal")
    args = ap.parse_args()

    vm = None
    for cand in ("vm.exe", "vm"):
        p = Path(args.build_dir) / cand
        if p.is_file():
            vm = str(p)
            break
    if vm is None:
        print("no encuentro vm en %s" % args.build_dir)
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="vx_inc_ident_"))
    resultados: list[tuple[str, bool, str]] = []
    try:
        for modo in ("vm", "aot"):
            for clase in ("comentario", "cuerpo", "interfaz"):
                if args.filtro and args.filtro not in ("%s/%s" % (modo, clase)):
                    continue
                caso_identidad(vm, tmp, modo, clase, resultados)
        if not args.filtro or "config" in args.filtro:
            caso_configuracion(vm, tmp, resultados)
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("temporal conservado en %s" % tmp)

    fallos = [r for r in resultados if not r[1]]
    for tag, ok, detalle in resultados:
        print("  %-34s %s%s" % (tag, "ok" if ok else "FALLA",
                                "" if ok else "  -- " + detalle))
    print("\n=== identidad incremental: %d ok, %d fallidos ===" % (
        len(resultados) - len(fallos), len(fallos)))
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
