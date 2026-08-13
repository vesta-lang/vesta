#!/usr/bin/env python3
"""Comprueba que el almacen de nodos es un CACHE PURO.

Un cache puro es aquel cuyo contenido se puede volver a derivar del fuente:
borrarlo entero y recompilar tiene que dar el MISMO binario.  No es un detalle
de eficiencia -- es la propiedad de la que depende que reclamar espacio sea
seguro.

  Si se cumple, borrar de mas es una recompilacion mas lenta, y la reclamacion
  puede usar cualquier politica simple (por antiguedad, por tamano, por epocas)
  sin recorrer el grafo de dependencias.

  Si deja de cumplirse -- porque alguien guarde ahi algo que no se pueda
  regenerar -- entonces borrar pasa a PERDER datos, y el fallo no da error: da
  ausencia, y aparece mucho despues como un artefacto que falta.

Por eso este test existe: no para comprobar que hoy funciona, sino para que el
dia que alguien rompa la propiedad se entere EN ESE COMMIT y no meses despues.

Uso:  python tests/vx/test_cache_puro.py <dir_build>
"""
import os
import shutil
import subprocess
import sys
import tempfile

# El `.velb` lleva un sello de tiempo en segundos: cambia entre dos
# compilaciones aunque todo lo demas sea identico, asi que se excluye de la
# comparacion.  Se excluye ESTE rango y nada mas: cualquier otra diferencia es
# justo lo que el test busca.
SELLO_TIEMPO = range(32, 36)

# Casos variados a proposito: cada familia pega en una parte distinta del
# compilador, y la propiedad tiene que cumplirse en todas.
CASOS = [
    "examples_codes_vx/02_hola_mundo.vx",        # el minimo
    "examples_codes_vx/18_reflexion_basica.vx",  # clases y reflexion
    "examples_codes_vx/103_async_args_typed.vx", # concurrencia
    "examples_codes_vx/132_comptime_introspect.vx",  # EJECUTA al compilar
]


def compila(vm, fuente, salida, cache_dir):
    entorno = dict(os.environ, VX_CACHE_DIR=cache_dir)
    r = subprocess.run([vm, "--vesta", fuente, "-o", salida],
                       capture_output=True, env=entorno, timeout=300)
    return r.returncode == 0


def difieren(a, b):
    """Bytes distintos, ignorando el sello de tiempo.

    Si los tamanos no coinciden devuelve UNA entrada y no sigue: ya se sabe que
    son distintos y enumerar el resto no anade nada.  Con tamanos iguales -- que
    es el caso de este test, dos compilaciones del mismo fuente -- se comparan
    todos los bytes.
    """
    ba, bb = open(a, "rb").read(), open(b, "rb").read()
    if len(ba) != len(bb):
        return [("tamano", len(ba), len(bb))]
    return [(i, ba[i], bb[i]) for i in range(len(ba))
            if ba[i] != bb[i] and i not in SELLO_TIEMPO]


def main():
    if len(sys.argv) < 2:
        print("uso: test_cache_puro.py <dir_build>")
        return 2
    vm = os.path.join(sys.argv[1], "vm.exe")
    if not os.path.exists(vm):
        vm = os.path.join(sys.argv[1], "vm")
    if not os.path.exists(vm):
        print(f"[cache puro] no encuentro el binario en {sys.argv[1]}")
        return 2

    fallos = 0
    tmp = tempfile.mkdtemp(prefix="vesta_cache_puro_")
    print("[cache puro] borrar el almacen no puede cambiar el binario")
    try:
        for caso in CASOS:
            if not os.path.exists(caso):
                print(f"  saltado  {caso} (no esta)")
                continue
            nombre = os.path.basename(caso)[:-3]
            cache = os.path.join(tmp, "cache")
            a = os.path.join(tmp, nombre + "_a")
            b = os.path.join(tmp, nombre + "_b")

            # 1) con el almacen construyendose de cero
            shutil.rmtree(cache, ignore_errors=True)
            if not compila(vm, caso, a, cache):
                print(f"  FALLA    {nombre}: no compila")
                fallos += 1
                continue
            nodos = sum(len(f) for _, _, f in os.walk(cache))

            # 2) con el almacen BORRADO entero: todo se regenera
            shutil.rmtree(cache, ignore_errors=True)
            if not compila(vm, caso, b, cache):
                print(f"  FALLA    {nombre}: no compila sin cache")
                fallos += 1
                continue

            d = difieren(a + ".velb", b + ".velb")
            if d:
                print(f"  FALLA    {nombre}: {len(d)} bytes distintos {d[:4]}")
                print("           el almacen guarda algo que NO se regenera:")
                print("           reclamar espacio dejaria de ser seguro.")
                fallos += 1
            else:
                print(f"  ok       {nombre}  ({nodos} ficheros en el almacen)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("[cache puro] " + ("TODO OK" if fallos == 0 else f"{fallos} FALLOS"))
    return 0 if fallos == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
