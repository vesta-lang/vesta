/* GENERADO por tools/import/gen_diag_catalog.py -- NO EDITAR.
 * Fuente: catalog/diagnostics.toml.  Catalogo de diagnosticos
 * (codigo estable VXNNNN -> mensaje por idioma).  Ver vx/diag_catalog.h. */
#include "vx/diag_catalog.h"

#include <cstring>

namespace vx {
namespace diag {
namespace {

// Idiomas del catalogo (el orden fija el indice interno).
const char *const kLanguages[] = {"en", "es"};
const int kLanguageCount = 2;

struct CatEntry {
    const char *code;
    const char *tmpl[2];
};

// Ordenadas por codigo para busqueda binaria.
const CatEntry kEntries[] = {
    {"VXA001", {"asm: dead code: unreachable instruction in the asm block", "asm: codigo muerto: instruccion inalcanzable en el bloque asm"}},
    {"VXA002", {"asm: jump to label '{0}' not defined in the asm block", "asm: salto a etiqueta '{0}' no definida en el bloque asm"}},
    {"VXA003", {"asm: infinite loop: control cannot leave the asm block", "asm: bucle sin salida: el flujo no puede abandonar el bloque asm"}},
    {"VXA004", {"asm: register '{0}' read uninitialized in the asm block", "asm: registro '{0}' leido sin inicializar en el bloque asm"}},
    {"VXA005", {"asm: flags read without a prior comparison/operation in the asm block", "asm: flags leidas sin una comparacion/operacion previa en el bloque asm"}},
    {"VXA006", {"asm: register '{0}' modified but not declared in clobbers(...) with 'noinfer'", "asm: registro '{0}' modificado pero no declarado en clobbers(...) con 'noinfer'"}},
    {"VXA007", {"asm: flags are modified but clobbers(\"flags\") is not declared with 'noinfer'", "asm: las flags se modifican pero no se declara clobbers(\"flags\") con 'noinfer'"}},
};
const int kEntryCount = 7;

} // namespace

const char *const *catalog_languages(int *out_n) {
    if (out_n) *out_n = kLanguageCount;
    return kLanguages;
}

int catalog_entry_count() { return kEntryCount; }

const char *catalog_template(const char *code, int lang) {
    if (!code || lang < 0 || lang >= kLanguageCount) return nullptr;
    int lo = 0, hi = kEntryCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = std::strcmp(kEntries[mid].code, code);
        if (c == 0) return kEntries[mid].tmpl[lang];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

} // namespace diag
} // namespace vx
