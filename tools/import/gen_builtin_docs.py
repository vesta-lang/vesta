#!/usr/bin/env python3
"""Genera la tabla de DOCUMENTACION de builtins del servidor de lenguaje a
partir de catalog/builtin_docs.toml.

    python tools/import/gen_builtin_docs.py [catalog/builtin_docs.toml] [salida.cpp]

Salida por defecto: src/lsp/gen/builtin_docs_gen.cpp.  Hermano de
gen_diag_catalog.py y con la misma forma: una tabla plana ordenada por nombre
(busqueda binaria) con una columna por idioma.  Nada de ficheros externos en
ejecucion -- la tabla se compila DENTRO del servidor --.

La FIRMA y los PARAMETROS no se traducen: son codigo, iguales en todos los
idiomas.  Lo unico por idioma es la explicacion.

Anadir un idioma = anadir su codigo ISO a `languages` en el TOML y su clave a
cada entrada, y volver a ejecutar esto.
"""
import os
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_IN = os.path.join(ROOT, "catalog", "builtin_docs.toml")
DEFAULT_OUT = os.path.join(ROOT, "src", "lsp", "gen", "builtin_docs_gen.cpp")


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

    entries = []
    for name, val in data.items():
        if name == "meta":
            continue
        if not isinstance(val, dict):
            sys.exit("error: la entrada '%s' no es una tabla" % name)
        sig = val.get("sig", "")
        if not sig:
            sys.exit("error: la entrada '%s' no tiene firma" % name)
        # Si falta la traduccion se deja vacia -> el runtime cae al idioma 0.
        docs = [val.get(lang, "") for lang in langs]
        entries.append((name, sig, docs))
    entries.sort(key=lambda e: e[0])

    n = len(langs)
    L = []
    L.append("/* GENERADO por tools/import/gen_builtin_docs.py -- NO EDITAR.")
    L.append(" * Fuente: catalog/builtin_docs.toml.  La documentacion de los")
    L.append(" * builtins que el editor ensenya, con una columna por idioma. */")
    L.append('#include "lsp/builtin_docs.h"')
    L.append("")
    L.append("#include <cstring>")
    L.append("")
    L.append("namespace lsp {")
    L.append("namespace {")
    L.append("")
    L.append("// Idiomas (el orden fija el indice interno; 0 = fallback).")
    L.append("const char *const kLanguages[] = {%s};" %
             ", ".join('"%s"' % cesc(x) for x in langs))
    L.append("const int kLanguageCount = %d;" % n)
    L.append("")
    L.append("struct DocEntry {")
    L.append("    const char *name;")
    L.append("    const char *sig;")
    L.append("    const char *doc[%d];" % n)
    L.append("};")
    L.append("")
    L.append("// Ordenadas por nombre para busqueda binaria.")
    L.append("const DocEntry kEntries[] = {")
    for name, sig, docs in entries:
        cols = ", ".join('"%s"' % cesc(d) for d in docs)
        L.append('    {"%s", "%s", {%s}},' % (cesc(name), cesc(sig), cols))
    L.append("};")
    L.append("const int kEntryCount = %d;" % len(entries))
    L.append("")
    L.append("} // namespace")
    L.append("")
    L.append("const char *const *builtin_doc_languages(int *out_n) {")
    L.append("    if (out_n) *out_n = kLanguageCount;")
    L.append("    return kLanguages;")
    L.append("}")
    L.append("")
    L.append("int builtin_doc_count() { return kEntryCount; }")
    L.append("")
    L.append("bool builtin_doc_at(int idx, BuiltinDocView *out) {")
    L.append("    if (idx < 0 || idx >= kEntryCount || out == nullptr)")
    L.append("        return false;")
    L.append("    const DocEntry &e = kEntries[idx];")
    L.append("    out->name = e.name;")
    L.append("    out->sig = e.sig;")
    L.append("    out->doc = e.doc;")
    L.append("    out->doc_count = kLanguageCount;")
    L.append("    return true;")
    L.append("}")
    L.append("")
    L.append("bool builtin_doc_find(const char *name, BuiltinDocView *out) {")
    L.append("    if (name == nullptr || out == nullptr) return false;")
    L.append("    int lo = 0, hi = kEntryCount - 1;")
    L.append("    while (lo <= hi) {")
    L.append("        const int mid = (lo + hi) / 2;")
    L.append("        const int c = std::strcmp(kEntries[mid].name, name);")
    L.append("        if (c == 0) return builtin_doc_at(mid, out);")
    L.append("        if (c < 0)")
    L.append("            lo = mid + 1;")
    L.append("        else")
    L.append("            hi = mid - 1;")
    L.append("    }")
    L.append("    return false;")
    L.append("}")
    L.append("")
    L.append("} // namespace lsp")
    L.append("")

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L))
    print("generado %s (%d entradas, %d idiomas: %s)" %
          (out, len(entries), n, ", ".join(langs)))


main()
