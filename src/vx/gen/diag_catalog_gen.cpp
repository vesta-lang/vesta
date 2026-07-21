/* GENERADO por tools/import/gen_diag_catalog.py -- NO EDITAR.
 * Fuente: catalog/diagnostics.toml.  Catalogo de diagnosticos
 * (codigo estable VXNNNN -> mensaje por idioma).  Ver vx/diag/diag_catalog.h. */
#include "vx/diag/diag_catalog.h"

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
    {"VXA008", {"asm: cannot pin operand '{0}' to '{1}' (the stack/frame pointer): a Vesta value cannot live there without breaking the stack. Manipulate the stack directly in the asm body instead (push/pop, sub rsp, mov rax rsp), or use @Naked for a stack switch", "asm: no se puede pinear el operando '{0}' a '{1}' (el puntero de pila/marco): un valor Vesta no puede vivir ahi sin romper la pila. Manipula la pila directamente en el cuerpo del asm (push/pop, sub rsp, mov rax rsp), o usa @Naked para un cambio de pila"}},
    {"VXA009", {"asm: operand '{0}' pinned to '{1}' -- that register is reserved by the runtime in VM mode; this asm block cannot be JIT-compiled (runs in the interpreter)", "asm: operando '{0}' pineado a '{1}' -- ese registro esta reservado por el runtime en modo VM; este bloque asm no se puede compilar en JIT (corre en el interprete)"}},
    {"VXA010", {"asm: reassigns the stack pointer ('{0}') in a normal function; its epilogue manages the stack, so a persistent stack switch (coroutines/fibers) will not survive the return -- mark the function @Naked to own the stack, or balance the change (restore '{0}') before the block ends", "asm: reasigna el puntero de pila ('{0}') en una funcion normal; su epilogue gestiona la pila, asi que un cambio de pila persistente (corrutinas/fibras) no sobrevivira al retorno -- marca la funcion @Naked para ser dueno de la pila, o equilibra el cambio (restaura '{0}') antes de cerrar el bloque"}},
};
const int kEntryCount = 10;

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
