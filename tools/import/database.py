#!/usr/bin/env python3
"""Lectura de la base de datos generada (.vxisa / .vxarch).

Parser UNICO y compartido -- dump_db, tests y futuros verificadores reusan esto
en vez de duplicar el `split("|")`.  Devuelve diccionarios con claves nombradas
(no tuplas posicionales), para que anadir una columna no rompa a los lectores.

Nota: hoy el formato es TEXTO.  Cuando llegue el binario, solo cambia este
modulo; los consumidores no se enteran."""


def load_vxisa(path):
    """@return { form_id: {checksum,uid,iclass,ext,opcode,enc,rmask,wmask,mem,
    imm,wflags,rflags,operands,overlay} }."""
    cols = ("checksum", "uid", "iclass", "ext", "opcode", "enc", "rmask",
            "wmask", "mem", "imm", "wflags", "rflags", "wflagset", "rflagset",
            "operands", "overlay", "category", "summary", "string", "url")
    # Las mismas columnas SIN los conjuntos de banderas, para leer ficheros
    # escritos antes de que existieran.  Se distinguen por el numero de campos, no
    # por la version de la cabecera: un fichero es lo que tiene, y contar es mas
    # fiable que fiarse de una etiqueta.
    cols_sin_flagsets = tuple(c for c in cols
                              if c not in ("wflagset", "rflagset"))
    forms = {}
    with open(path, "r", encoding="ascii") as f:
        for line in f:
            if line.startswith(("vxisa", "#")) or not line.strip():
                continue
            c = line.rstrip("\n").split("|")
            campos = c[1:]
            usar = cols if len(campos) >= len(cols) else cols_sin_flagsets
            d = dict(zip(usar, campos))
            d.setdefault("wflagset", "-")
            d.setdefault("rflagset", "-")
            forms[int(c[0])] = d
    return forms


def load_vxarch(path):
    """@return (name, port_names, classes, form_class) con
    classes[cid]={recip_tp,uops,microcoded,macro_fusible,div_cycles,latencies,
    ports} y form_class[form_id]=class_id."""
    ccols = ("recip_tp", "uops", "microcoded", "macro_fusible", "div_cycles",
             "latencies", "ports")
    name, ports, classes, form_class = "", [], {}, {}
    with open(path, "r", encoding="ascii") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("vxarch"):
                for tok in line.split():
                    if tok.startswith("name="):
                        name = tok[5:]
            elif line.startswith("ports:"):
                for tok in line[6:].split():
                    if "=" in tok:
                        ports.append(tok.split("=", 1)[1])
            elif line.startswith("class "):
                c = line[6:].split("|")
                classes[int(c[0])] = dict(zip(ccols, c[1:]))
            elif line and line[0].isdigit():
                fid, cid = line.split("|")
                form_class[int(fid)] = int(cid)
    return name, ports, classes, form_class


def load_vxfeat(path):
    """@return (meta, table:list[str], cpus:dict[cpu]->{sched,features:set[str]}).

    La DB usa tabla+IDs (como las ISAs con FormID): `table[id]` da el nombre y
    cada CPU referencia esos IDs, sin repetir strings."""
    meta, table, cpus = {}, [], {}
    with open(path, "r", encoding="ascii") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("vxfeat"):
                for tok in line.split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        meta[k] = v
            elif line.startswith("feats:"):
                for tok in line[6:].split():
                    if "=" in tok:
                        i, nm = tok.split("=", 1)
                        idx = int(i)
                        while len(table) <= idx:
                            table.append("")
                        table[idx] = nm
            elif line and not line.startswith("#") and "|" in line:
                cpu, sched, ids = line.split("|", 2)
                fset = {table[int(x)] for x in ids.split(",") if x != ""}
                cpus[cpu] = {"sched": sched, "features": fset}
    return meta, table, cpus
