#!/usr/bin/env python3
"""Comprueba que un fallo de ejecucion se cuenta con la MISMA calidad en Linux
que en Windows.

La suite e2e no corre entera en el sistema donde no esta el compilador de
referencia, y eso hacia que Linux se quedara atras sin que nadie lo notara: el
manejador de senales TIRABA el estado de la maquina, asi que alli un fallo se
contaba sin la instruccion culpable, sin el rasgo que exige y, en codigo
compilado, sin saber siquiera en que funcion.  Esto es el equivalente minimo
para verlo de un vistazo.

Comprueba dos cosas:
  1. Que los ejemplos que ejercitan un fallo capturado devuelven lo mismo en
     interprete y en JIT.
  2. Que un fallo SIN capturar se cuenta entero en los dos modos: codigo del
     catalogo, instruccion, rasgo exigido, lo que la maquina declara y la
     funcion donde ocurrio.

Uso:
    python3 tools/check_fallos_linux.py <dir_del_build> [raiz_del_repo]
"""

import os
import re
import subprocess
import sys
import tempfile

# Ejemplo -> valor que tiene que devolver en los dos modos.
CASOS = {
    "369_fallo_del_sistema_capturado": 48,
    "370_instruccion_no_soportada": 46,
}

# Un fallo sin capturar: una variante AVX-512 en una maquina que no lo tiene.
FUENTE_FALLO = """\
import std.memory.x86_64;
import std.types only usize;
u8[512] g_o;
u8[512] g_d;
i32 main() {
    std.memory.x86_64.memcpy_small_avx512(&g_d[0], &g_o[0], (usize) 96);
    return 7;
}
"""

# Lo que el informe TIENE que decir.  Si falta una, Linux esta contando menos.
EXIGIDO = ("fatal error:", "instruction:", "requires:", "this processor has:",
           "  at ")


def corre(args):
    """Ejecuta y devuelve (codigo, salida combinada)."""
    p = subprocess.run(args, capture_output=True, text=True, errors="replace",
                       timeout=300)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    vm = os.path.join(sys.argv[1], "vm")
    raiz = sys.argv[2] if len(sys.argv) > 2 else "."
    tmp = tempfile.mkdtemp(prefix="vxfallos_")
    fallos = 0

    print("== valores esperados en los dos modos ==")
    for caso, quiere in CASOS.items():
        fuente = os.path.join(raiz, "examples_codes_vx", caso + ".vx")
        corre([vm, "--vesta", fuente, "-o", os.path.join(tmp, caso)])
        for modo in ("vm", "jit"):
            _, log = corre([vm, "--run", os.path.join(tmp, caso + ".velb"),
                            "-m", modo, "--stats"])
            m = re.search(r"R00=0x([0-9a-f]+)", log)
            got = int(m.group(1), 16) if m else None
            ok = (got == quiere)
            fallos += 0 if ok else 1
            print("  %-36s -m %-3s -> %s  %s"
                  % (caso, modo, got if got is not None else "MUERE",
                     "ok" if ok else "MAL, se esperaba %d" % quiere))

    print()
    print("== un fallo SIN capturar se cuenta entero ==")
    fuente = os.path.join(tmp, "ud.vx")
    with open(fuente, "w", encoding="ascii") as f:
        f.write(FUENTE_FALLO)
    corre([vm, "--vesta", fuente, "-o", os.path.join(tmp, "ud"), "--vx-debug"])
    for modo in ("vm", "jit"):
        _, log = corre([vm, "--run", os.path.join(tmp, "ud.velb"), "-m", modo])
        faltan = [t for t in EXIGIDO if t not in log]
        fallos += len(faltan)
        print("  --- modo %s ---" % modo)
        if faltan:
            print("    FALTA en el informe: %s" % ", ".join(faltan))
        for linea in log.splitlines():
            if any(linea.lstrip().startswith(t.strip()) for t in EXIGIDO):
                print("    " + linea.strip())

    print()
    print("=== %d comprobaciones mal ===" % fallos if fallos
          else "=== todo bien: Linux cuenta igual que Windows ===")
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
