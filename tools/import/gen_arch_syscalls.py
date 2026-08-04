#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Genera linux/<arch>.vx (invoke + constantes _NR_*) desde el JSON de la API.

Solo para arm64/arm (x86_64/x86_32 ya estan escritos a mano en el repo).  Cada
fichero define:
  - el `invoke` de bajo nivel (register() por arg segun la ABI de syscall del
    arch + el asm de la instruccion de trap), y
  - una constante `public const syscall_id _NR_<name> = <nr>;` por syscall.

ABI de syscall por arch (numero de servicio + args + retorno + trap):
  arm64: nr=X8, args=X0..X5 (6), ret=X0, `svc #0`.
  arm  : nr=R7, args=R0..R6 (7), ret=R0, `svc #0`.

ASCII (excepto la enye) + comentarios en espanol.
"""
import json
import os
import re

# Identificador C valido (para filtrar placeholders de la API: "not implemented",
# "?", etc.).
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SYS = os.path.join(ROOT, "stdlib", "vx", "std", "syscall")
DATA = os.path.join(HERE, "syscall_data")

# (ns, id_reg, [arg_regs], trap_asm, ret_reg_es_primer_arg)
ARCHS = {
    "arm64": {
        "ns": "std.syscall.linux.arm64",
        "id_reg": "x8",
        "arg_regs": ["x0", "x1", "x2", "x3", "x4", "x5"],
        "trap": "svc 0",
        "desc": "ARM64 (AArch64): el numero de servicio va en X8 y los argumentos\n"
                " * en X0..X5; `svc #0` hace la llamada; el resultado queda en X0.",
    },
    "arm": {
        "ns": "std.syscall.linux.arm",
        "id_reg": "r7",
        "arg_regs": ["r0", "r1", "r2", "r3", "r4", "r5"],
        "trap": "svc 0",
        "desc": "ARM (32-bit EABI): el numero de servicio va en R7 y los argumentos\n"
                " * en R0..R5; `svc #0` hace la llamada; el resultado queda en R0.",
    },
}


def build(arch):
    cfg = ARCHS[arch]
    rows = json.load(open(os.path.join(DATA, "%s.json" % arch),
                          encoding="utf-8", errors="replace"))
    # nr -> name (unico; nos quedamos con el primero por nombre).
    seen = {}
    for r in rows:
        nm = r["name"]
        if nm and IDENT.match(nm) and nm not in seen:
            seen[nm] = int(r["nr"])

    out = []
    out.append("namespace %s;" % cfg["ns"])
    out.append("")
    out.append("import std.syscall.abi only syscall_id;")
    out.append("import std.types only size_t;")
    out.append("")
    # invoke: register() por arg + asm del trap + return del reg de resultado.
    # El resultado del syscall queda en el 1er arg-reg (X0/R0); ligamos `a1` a el
    # y lo devolvemos (read-back tras el trap).
    ids = cfg["id_reg"]
    ar = cfg["arg_regs"]
    out.append("/**")
    out.append(" * Ejecuta una syscall Linux %s." % cfg["desc"])
    out.append(" * Un envoltorio castea `invoke` a un `cfn` con SOLO los argumentos")
    out.append(" * que esa syscall usa (aridad/tipos/orden); el compilador toma los")
    out.append(" * primeros N registros de esta firma e INLINA la llamada.")
    out.append(" */")
    params = ["register(\"%s\") syscall_id id" % ids]
    for i, reg in enumerate(ar):
        params.append("register(\"%s\") size_t a%d" % (reg, i + 1))
    # Formato multi-linea de los params.
    out.append("public size_t invoke(%s," % params[0])
    for p in params[1:-1]:
        out.append("                     %s," % p)
    out.append("                     %s) {" % params[-1])
    out.append("    asm { %s }" % cfg["trap"])
    out.append("    return (size_t) a1;   // %s tras el trap = resultado"
               % ar[0].upper())
    out.append("}")
    out.append("")
    # Constantes _NR_*.
    width = max((len(nm) for nm in seen), default=1)
    for nm in sorted(seen, key=lambda n: seen[n]):
        out.append("public const syscall_id _NR_%-*s = %d;"
                   % (width, nm, seen[nm]))
    out.append("")

    dst = os.path.join(SYS, "linux", "%s.vx" % arch)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("generado %s (%d syscalls)" % (dst, len(seen)))


if __name__ == "__main__":
    for a in ("arm64", "arm"):
        build(a)
