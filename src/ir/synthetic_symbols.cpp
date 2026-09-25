/**
 * @file synthetic_symbols.cpp
 * @brief Los nombres de simbolo que genera el propio compilador.
 * @see ir/synthetic_symbols.h
 */
#include "ir/synthetic_symbols.h"

#include <cstring>

namespace ir {

bool is_compiler_generated(const std::string &name) noexcept {
    if (is_module_init_family(name)) return true;
    static const char *const kPrefixes[] = {
        "__new_",    // constructor sintetico de una clase
        "__async_",  // cuerpo de una funcion asincrona
        "__lambda_", // cuerpo de una lambda
        "__spawn_",  // cuerpo de un spawn
        "__rspawn_", // cuerpo de un spawn remoto
        "__ovl_",    // resolutores de una vista sobre bytes
        "__macro_",  // cuerpo de una macro bajado a funcion
        "__clone_",  // copia profunda generada para un tipo
        "__vx_",     // todo lo que aporta el runtime
    };
    for (const char *p : kPrefixes)
        if (name.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

} // namespace ir
