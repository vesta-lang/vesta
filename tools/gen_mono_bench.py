#!/usr/bin/env python3
"""Banco de la MONOMORFIZACION: que cuesta escribir generico, en memoria.

Por que existe.  El coste de los genericos no se ve a un fichero -- ahi queda
enterrado bajo el arranque del proceso --, y solo aparece a escala de MODULOS,
porque cada uno construye su propia tabla de instancias y clona otra vez lo que
el vecino ya habia clonado.  Un banco de un solo fichero diria que no hay
problema.

Que mide.  Dos programas del MISMO tamano final y con el mismo trabajo:

  GEN      N funciones GENERICAS, instanciadas por cada modulo con su tipo.
  CONTROL  las mismas N escritas CONCRETAS, sin un solo generico.

La diferencia entre los dos es lo que cuesta la monomorfizacion, y nada mas:
el numero de funciones que acaban en el binario es el mismo, asi que no se esta
comparando un programa grande con uno pequenyo.

Se mide el PICO RESIDENTE del proceso compilador, no lo que reserva: lo que
duele es lo que el sistema tiene que tener a la vez en memoria, y eso son
paginas tocadas.  El tiempo se apunta al lado porque cuenta otra historia --
el paralelismo lo absorbe y a la memoria la amplifica --, y verlos juntos evita
la conclusion facil de mirar solo uno.

Uso:
    python tools/gen_mono_bench.py cmake-build-release
    python tools/gen_mono_bench.py cmake-build-release --modulos 1 4 16
    python tools/gen_mono_bench.py cmake-build-release --repeticiones 3
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

# Los de la medida de referencia, para que las corridas se puedan comparar.
N_FUNCIONES = 100
K_TIPOS = 40


def peak_bytes(proc):
    """Pico residente de un proceso ya terminado, en bytes (0 si no se sabe).

    Mismo mecanismo que usa la suite e2e, para que las dos hablen del mismo
    numero: en Windows @c PeakWorkingSetSize, en el resto @c ru_maxrss.
    """
    if os.name == "nt":
        try:
            import ctypes
            from ctypes import wintypes

            class _PMC(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD),
                            ("PageFaultCount", wintypes.DWORD),
                            ("PeakWorkingSetSize", ctypes.c_size_t),
                            ("WorkingSetSize", ctypes.c_size_t),
                            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                            ("PagefileUsage", ctypes.c_size_t),
                            ("PeakPagefileUsage", ctypes.c_size_t)]

            h = int(proc._handle)
            info = _PMC()
            info.cb = ctypes.sizeof(_PMC)
            if ctypes.windll.psapi.GetProcessMemoryInfo(
                    wintypes.HANDLE(h), ctypes.byref(info), info.cb):
                return int(info.PeakWorkingSetSize)
        except Exception:
            return 0
        return 0
    try:
        import resource
        ru = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        return int(ru) * (1 if sys.platform == "darwin" else 1024)
    except Exception:
        return 0


def escribe_comun(ruta, generico, n_modulos, compartido):
    """El modulo que declara los tipos y las funciones que los modulos usan.

    @param ruta       Fichero a escribir.
    @param generico   Si las N funciones se declaran genericas o concretas.
    @param n_modulos  Cuantos modulos van a usarlas, que es lo que decide
                      CUANTAS concretas hacen falta en el control.
    @param compartido Si todos los modulos piden el mismo tipo.
    """
    L = ["namespace bench.comun;", ""]
    # K structs distintos: son los type-args con los que cada modulo instancia.
    for k in range(K_TIPOS):
        L.append("public struct S%d {" % k)
        L.append("\ti64 a;")
        L.append("\ti64 b;")
        L.append("}")
        L.append("")
    if generico:
        # N plantillas.  El cuerpo toca los dos campos para que el clon tenga
        # algo dentro y no sea una firma vacia.
        for j in range(N_FUNCIONES):
            L.append("public i64 procesa%d<T>(T x) {" % j)
            L.append("\ti64 r = x.a + x.b;")
            L.append("\tfor (i64 i = 0; i < 3; i = i + 1) { r = r + i * %d; }"
                     % (j + 1))
            L.append("\treturn r;")
            L.append("}")
            L.append("")
    else:
        # Las mismas N, una por cada tipo QUE DE VERDAD SE USA: exactamente las
        # que la monomorfizacion habria generado, pero escritas a mano.
        #
        # Escribirlas para los K tipos seria comparar dos programas distintos
        # -- 4.000 funciones contra 100 --, y entonces el control sale peor sin
        # que eso diga nada de los genericos.
        usados = sorted({tipo_de(i, compartido) for i in range(n_modulos)})
        for j in range(N_FUNCIONES):
            for k in usados:
                L.append("public i64 procesa%d_S%d(S%d x) {" % (j, k, k))
                L.append("\ti64 r = x.a + x.b;")
                L.append("\tfor (i64 i = 0; i < 3; i = i + 1) { r = r + i * %d; }"
                         % (j + 1))
                L.append("\treturn r;")
                L.append("}")
                L.append("")
    with open(ruta, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))


def tipo_de(idx, compartido):
    """Con que tipo instancia el modulo @p idx.

    Es LA variable del banco, y decide que se esta midiendo:

      compartido  todos piden el MISMO, asi que la instancia de uno le sirve al
                  vecino y lo que se ve es cuanto cuesta que cada modulo la
                  clone por su cuenta.  Es el caso que el reparto deduplica.
      propio      cada uno el suyo: las instancias son genuinamente distintas y
                  no hay nada que compartir.  Sirve de suelo -- ahi el generico
                  no puede hacerlo mejor que escribirlas a mano --.

    Medir solo el segundo dice que los genericos no cuestan nada, y es verdad
    en ese caso y solo en ese.
    """
    return 0 if compartido else (idx % K_TIPOS)


def escribe_modulo(ruta, idx, generico, compartido):
    """Un modulo que usa las N funciones con su tipo."""
    k = tipo_de(idx, compartido)
    L = ["namespace bench.m%d;" % idx,
         "",
         "import bench.comun;",
         "",
         "public i64 usa%d() {" % idx,
         "\tbench.comun.S%d x;" % k,
         "\tx.a = %d;" % (idx + 1),
         "\tx.b = 2;",
         "\ti64 r = 0;"]
    for j in range(N_FUNCIONES):
        if generico:
            L.append("\tr = r + bench.comun.procesa%d(x);" % j)
        else:
            L.append("\tr = r + bench.comun.procesa%d_S%d(x);" % (j, k))
    L.append("\treturn r;")
    L.append("}")
    with open(ruta, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))


def escribe_main(ruta, n_modulos):
    """El `main` que llama a todos los modulos, para que ninguno se pode."""
    L = ["namespace bench.principal;", ""]
    for i in range(n_modulos):
        L.append("import bench.m%d;" % i)
    L.append("")
    L.append("i32 main() {")
    L.append("\ti64 r = 0;")
    for i in range(n_modulos):
        L.append("\tr = r + bench.m%d.usa%d();" % (i, i))
    L.append("\treturn (i32)(r & 0xFF);")
    L.append("}")
    with open(ruta, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))


def compila(vm, raiz, jobs):
    """Compila el programa y devuelve (segundos, pico en bytes, ok)."""
    main = os.path.join(raiz, "principal.vx")
    salida = os.path.join(raiz, "out")
    cmd = [vm, "--vesta", main, "-o", salida]
    if jobs:
        cmd += ["-j", str(jobs)]
    t0 = time.time()
    proc = subprocess.Popen(cmd, cwd=raiz, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    out, _ = proc.communicate()
    dt = time.time() - t0
    pico = peak_bytes(proc)
    if proc.returncode != 0:
        sys.stderr.write(out.decode("utf-8", "replace")[-2000:] + "\n")
    return dt, pico, proc.returncode == 0


def una_corrida(vm, n_modulos, generico, jobs, compartido):
    """Genera el programa entero en un directorio propio y lo compila."""
    raiz = tempfile.mkdtemp(prefix="monobench_")
    try:
        escribe_comun(os.path.join(raiz, "comun.vx"), generico, n_modulos,
                      compartido)
        for i in range(n_modulos):
            escribe_modulo(os.path.join(raiz, "m%d.vx" % i), i, generico,
                           compartido)
        escribe_main(os.path.join(raiz, "principal.vx"), n_modulos)
        return compila(vm, raiz, jobs)
    finally:
        shutil.rmtree(raiz, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", help="directorio de build con el binario `vm`")
    ap.add_argument("--modulos", type=int, nargs="+", default=[1, 4, 16],
                    help="escalas a medir (por defecto 1 4 16)")
    ap.add_argument("--repeticiones", type=int, default=2,
                    help="corridas por punto; se queda el MEJOR pico")
    ap.add_argument("-j", "--jobs", type=int, default=0,
                    help="modulos en vuelo a la vez (0 = el defecto del vm)")
    ap.add_argument("--reparto", choices=["compartido", "propio"],
                    default="compartido",
                    help="si todos los modulos piden el MISMO tipo (por "
                         "defecto) o cada uno el suyo")
    a = ap.parse_args()

    vm = os.path.join(a.build, "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        vm = os.path.join(a.build, "vm")
    if not os.path.exists(vm):
        sys.exit("no encuentro el binario `vm` en " + a.build)

    print("N=%d funciones, K=%d tipos, reparto %s, pico residente del "
          "compilador" % (N_FUNCIONES, K_TIPOS, a.reparto))
    print()
    print("| escala | GEN pico | CONTROL pico | GEN ms | CONTROL ms | x memoria |")
    print("| :-- | --: | --: | --: | --: | --: |")
    for n in a.modulos:
        res = {}
        for generico in (True, False):
            mejor_pico, mejor_ms = None, None
            for _ in range(a.repeticiones):
                # INTERCALADO y quedandose con el mejor: dos corridas seguidas
                # no distinguen el cambio del ruido del sistema.
                dt, pico, ok = una_corrida(vm, n, generico, a.jobs,
                                           a.reparto == "compartido")
                if not ok:
                    sys.exit("la compilacion fallo (%s, %d modulos)"
                             % ("GEN" if generico else "CONTROL", n))
                if mejor_pico is None or pico < mejor_pico:
                    mejor_pico = pico
                if mejor_ms is None or dt < mejor_ms:
                    mejor_ms = dt
            res[generico] = (mejor_pico, mejor_ms)
        g_pico, g_ms = res[True]
        c_pico, c_ms = res[False]
        mib = 1024.0 * 1024.0
        veces = (g_pico / c_pico) if c_pico else 0.0
        etiqueta = "1 fichero" if n == 1 else ("%d modulos" % n)
        print("| %s | %.1f MiB | %.1f | %.0f | %.0f | %.2fx |"
              % (etiqueta, g_pico / mib, c_pico / mib,
                 g_ms * 1000, c_ms * 1000, veces))


if __name__ == "__main__":
    main()
