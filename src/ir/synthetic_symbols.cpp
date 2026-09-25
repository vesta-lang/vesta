/**
 * @file synthetic_symbols.cpp
 * @brief Los nombres de simbolo que genera el propio compilador.
 * @see ir/synthetic_symbols.h
 */
#include "ir/synthetic_symbols.h"
#include "ir/runtime_symbols.h" // el prefijo de lo que aporta el runtime

namespace ir {

namespace {

/// Si @p name termina en @p suffix.
bool ends_with(const std::string &name, const char *suffix) noexcept {
    const size_t n = std::char_traits<char>::length(suffix);
    return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
}

} // namespace

bool is_shared_new_helper(const std::string &name) noexcept {
    return is_any_new_helper(name) && ends_with(name, kNewHelperSharedSuffix);
}

bool is_new_helper_name(const std::string &name, std::string *out_class) {
    const size_t prefix_len = sizeof(kNewHelperPrefix) - 1;
    if (name.size() <= prefix_len) return false;
    if (!is_any_new_helper(name)) return false;
    /* Excluir la variante compartida: registra el objeto en la tabla de
     * compartidos -- eliminar esa reserva cambia `shared_heap_live_count`, un
     * efecto observable. */
    if (ends_with(name, kNewHelperSharedSuffix)) return false;
    if (out_class) *out_class = name.substr(prefix_len);
    return true;
}

bool is_compiler_generated(const std::string &name) noexcept {
    if (is_module_init_family(name)) return true;
    static const char *const kPrefixes[] = {
        kNewHelperPrefix,   // constructor sintetico de una clase
        kAsyncPrefix,       // cuerpo de una funcion asincrona
        kLambdaPrefix,      // cuerpo de una lambda
        kSpawnPrefix,       // cuerpo de un spawn
        kRemoteSpawnPrefix, // cuerpo de un spawn remoto
        kOverlayPrefix,     // resolutores de una vista sobre bytes
        kMacroPrefix,       // cuerpo de una macro bajado a funcion
        rt::kPrefix,        // todo lo que aporta el runtime
    };
    for (const char *p : kPrefixes)
        if (has_synthetic_prefix(name, p)) return true;
    return false;
}

} // namespace ir
