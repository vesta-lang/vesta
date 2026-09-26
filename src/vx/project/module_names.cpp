/**
 * @file module_names.cpp
 * @brief Los nombres de simbolo que se derivan del nombre de un modulo.
 * @see vx/project/module_names.h
 */
#include "vx/project/module_names.h"

#include "ir/synthetic_symbols.h" // el nombre base de `__module_init`
#include "vx/module/namespace_names.h" // el separador de un nombre con ruta

namespace vx {

namespace {

/// Separa el `__module_init` (o una tanda suya) del nombre del modulo al que
/// se le renombra.
constexpr const char *kModuleSuffixSeparator = "_";

} // namespace

std::string module_symbol_prefix(const std::string &module) {
    /* El modulo hace de ruta de un solo segmento: el mismo separador que un
     * namespace, que es lo que permite que `module_member_symbol` y
     * `qualified_symbol` den el mismo nombre. */
    return module + kSymbolPathSeparator;
}

std::string module_member_symbol(const std::string &module,
                                 const std::string &member) {
    return qualified_symbol(module, member);
}

std::string module_init_symbol(const std::string &module) {
    return std::string(ir::kModuleInit) + kModuleSuffixSeparator + module;
}

std::string module_init_part_symbol(const std::string &part,
                                    const std::string &module) {
    return part + kModuleSuffixSeparator + module;
}

} // namespace vx
