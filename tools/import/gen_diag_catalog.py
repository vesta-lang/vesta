#!/usr/bin/env python3
"""Genera el catalogo de diagnosticos del compilador (C++ autocontenido) a partir
del fichero de datos catalog/diagnostics.toml.

    python tools/import/gen_diag_catalog.py [catalog/diagnostics.toml] [salida.cpp]

Salida por defecto: src/vx/gen/diag_catalog_gen.cpp.  El C++ generado es una tabla
plana ordenada por codigo (busqueda binaria O(log N)) con una columna por idioma.
Anadir un idioma = anadir su codigo ISO a `languages` en el TOML y su clave a cada
entrada, y volver a ejecutar este script.  Sin ficheros externos en runtime: la
tabla se compila DENTRO del compilador.
"""
import os
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_IN = os.path.join(ROOT, "catalog", "diagnostics.toml")
DEFAULT_OUT = os.path.join(ROOT, "src", "vx", "gen", "diag_catalog_gen.cpp")


def cesc(s):
    """Escapa una cadena para un literal C++ entre comillas."""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return "".join(out)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IN
    out = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT

    with open(src, "rb") as fh:
        data = tomllib.load(fh)

    langs = data.get("meta", {}).get("languages")
    if not langs:
        sys.exit("error: falta [meta].languages en " + src)

    # Recolectar entradas (todas menos [meta]), ordenadas por codigo.
    entries = []
    for code, val in data.items():
        if code == "meta":
            continue
        if not isinstance(val, dict):
            sys.exit("error: la entrada '%s' no es una tabla" % code)
        row = []
        for lang in langs:
            # Si falta la traduccion, se deja vacia -> el runtime cae al idioma 0.
            row.append(val.get(lang, ""))
        entries.append((code, row))
    entries.sort(key=lambda e: e[0])

    lines = []
    lines.append("/* GENERADO por tools/import/gen_diag_catalog.py -- NO EDITAR.")
    lines.append(" * Fuente: catalog/diagnostics.toml.  Catalogo de diagnosticos")
    lines.append(" * (codigo estable VXNNNN -> mensaje por idioma).  Ver vx/diag/diag_catalog.h. */")
    lines.append('#include "vx/diag/diag_catalog.h"')
    lines.append("")
    lines.append("#include <cstring>")
    lines.append("")
    lines.append("namespace vx {")
    lines.append("namespace diag {")
    lines.append("namespace {")
    lines.append("")
    n = len(langs)
    lines.append("// Idiomas del catalogo (el orden fija el indice interno).")
    lines.append("const char *const kLanguages[] = {%s};" %
                 ", ".join('"%s"' % cesc(l) for l in langs))
    lines.append("const int kLanguageCount = %d;" % n)
    lines.append("")
    lines.append("struct CatEntry {")
    lines.append("    const char *code;")
    lines.append("    const char *tmpl[%d];" % n)
    lines.append("};")
    lines.append("")
    lines.append("// Ordenadas por codigo para busqueda binaria.")
    lines.append("const CatEntry kEntries[] = {")
    for code, row in entries:
        cols = ", ".join('"%s"' % cesc(t) for t in row)
        lines.append('    {"%s", {%s}},' % (cesc(code), cols))
    lines.append("};")
    lines.append("const int kEntryCount = %d;" % len(entries))
    lines.append("")
    lines.append("} // namespace")
    lines.append("")
    lines.append("const char *const *catalog_languages(int *out_n) {")
    lines.append("    if (out_n) *out_n = kLanguageCount;")
    lines.append("    return kLanguages;")
    lines.append("}")
    lines.append("")
    lines.append("int catalog_entry_count() { return kEntryCount; }")
    lines.append("")
    lines.append("const char *catalog_template(const char *code, int lang) {")
    lines.append("    if (!code || lang < 0 || lang >= kLanguageCount) return nullptr;")
    lines.append("    int lo = 0, hi = kEntryCount - 1;")
    lines.append("    while (lo <= hi) {")
    lines.append("        int mid = (lo + hi) / 2;")
    lines.append("        int c = std::strcmp(kEntries[mid].code, code);")
    lines.append("        if (c == 0) return kEntries[mid].tmpl[lang];")
    lines.append("        if (c < 0) lo = mid + 1; else hi = mid - 1;")
    lines.append("    }")
    lines.append("    return nullptr;")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace diag")
    lines.append("} // namespace vx")
    lines.append("")

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(lines))
    print("generado %s (%d entradas, %d idiomas: %s)" %
          (out, len(entries), n, ", ".join(langs)))


if __name__ == "__main__":
    main()
