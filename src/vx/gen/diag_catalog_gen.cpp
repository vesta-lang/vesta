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
    {"VX4001", {"'{0}' is declared with @Target(\"{1}\"), which does not hold for this target -- that declaration is not available here", "'{0}' esta declarado con @Target(\"{1}\"), que no se cumple en este objetivo -- esa declaracion no esta disponible aqui"}},
    {"VX4002", {"'{0}' is only declared for other targets ({1}) -- no variant matches the target being compiled", "'{0}' solo esta declarado para otros objetivos ({1}) -- ninguna variante encaja con el objetivo que se esta compilando"}},
    {"VX7001", {"fatal error: null pointer", "error fatal: puntero nulo"}},
    {"VX7002", {"fatal error: division by zero", "error fatal: division entre cero"}},
    {"VX7003", {"fatal error: stack overflow", "error fatal: desbordamiento de pila"}},
    {"VX7004", {"fatal error: stack underflow", "error fatal: pila vacia"}},
    {"VX7005", {"fatal error: invalid instruction", "error fatal: instruccion invalida"}},
    {"VX7006", {"fatal error: unresolved symbol", "error fatal: simbolo sin resolver"}},
    {"VX7007", {"fatal error: invalid memory access", "error fatal: acceso a memoria invalido"}},
    {"VX7008", {"fatal error: crash in native code", "error fatal: fallo en codigo nativo"}},
    {"VX7009", {"fatal error: exception from native code", "error fatal: excepcion de codigo nativo"}},
    {"VX7010", {"fatal error: out of memory", "error fatal: memoria agotada"}},
    {"VX7011", {"fatal error: aborted by the program", "error fatal: abortado por el programa"}},
    {"VX7012", {"fatal error", "error fatal"}},
    {"VX7013", {"at address {0}", "en la direccion {0}"}},
    {"VX7014", {"source unavailable (file modified since build)", "fuente no disponible (el fichero cambio desde que se compilo)"}},
    {"VX7015", {"expanded by the macro {0}, invoked at {1}", "expandido por la macro {0}, invocada en {1}"}},
    {"VXA001", {"asm: dead code: unreachable instruction in the asm block", "asm: codigo muerto: instruccion inalcanzable en el bloque asm"}},
    {"VXA002", {"asm: jump to label '{0}' not defined in the asm block", "asm: salto a etiqueta '{0}' no definida en el bloque asm"}},
    {"VXA003", {"asm: infinite loop: control cannot leave the asm block", "asm: bucle sin salida: el flujo no puede abandonar el bloque asm"}},
    {"VXA004", {"asm: register '{0}' read uninitialized in the asm block", "asm: registro '{0}' leido sin inicializar en el bloque asm"}},
    {"VXA005", {"asm: flags read without a prior comparison/operation in the asm block", "asm: flags leidas sin una comparacion/operacion previa en el bloque asm"}},
    {"VXA006", {"asm: register '{0}' modified but not declared in clobbers(...) with 'noinfer'", "asm: registro '{0}' modificado pero no declarado en clobbers(...) con 'noinfer'"}},
    {"VXA007", {"asm: flags are modified but clobbers(\"flags\") is not declared with 'noinfer'", "asm: las flags se modifican pero no se declara clobbers(\"flags\") con 'noinfer'"}},
    {"VXA008", {"asm: pinning operand '{0}' to '{1}' (the stack/frame pointer) may break the stack. Allowed under your responsibility (some ABIs require it, e.g. Linux x86-32 passes the 6th syscall arg in ebp): save/restore the register yourself or use @Naked to own the stack", "asm: pinear el operando '{0}' a '{1}' (el puntero de pila/marco) puede romper la pila. Permitido bajo tu responsabilidad (algunos ABIs lo exigen, p.ej. Linux x86-32 pasa el 6o arg de syscall en ebp): salva/restaura el registro tu mismo o usa @Naked para ser dueno de la pila"}},
    {"VXA009", {"asm: operand '{0}' pinned to '{1}' -- that register is reserved by the runtime in VM mode; this asm block cannot be JIT-compiled (runs in the interpreter)", "asm: operando '{0}' pineado a '{1}' -- ese registro esta reservado por el runtime en modo VM; este bloque asm no se puede compilar en JIT (corre en el interprete)"}},
    {"VXA010", {"asm: reassigns the stack pointer ('{0}') in a normal function; its epilogue manages the stack, so a persistent stack switch (coroutines/fibers) will not survive the return -- mark the function @Naked to own the stack, or balance the change (restore '{0}') before the block ends", "asm: reasigna el puntero de pila ('{0}') en una funcion normal; su epilogue gestiona la pila, asi que un cambio de pila persistente (corrutinas/fibras) no sobrevivira al retorno -- marca la funcion @Naked para ser dueno de la pila, o equilibra el cambio (restaura '{0}') antes de cerrar el bloque"}},
};
const int kEntryCount = 27;

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
