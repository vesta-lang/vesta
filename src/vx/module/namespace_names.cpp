/**
 * @file namespace_names.cpp
 * @brief Implementacion de los nombres fisicos de namespace.
 */
#include "vx/module/namespace_names.h"

namespace vx {

namespace {

/// Longitud del separador, sacada de la constante y no escrita a mano.
constexpr size_t kSeparatorLen = sizeof(kSymbolPathSeparator) - 1;

} // namespace

std::string namespace_symbol_path(const std::string &dotted) {
    if (dotted.find('.') == std::string::npos) return dotted;
    std::string out;
    out.reserve(dotted.size() + 8);
    for (const char c : dotted) {
        if (c == '.')
            out.append(kSymbolPathSeparator, kSeparatorLen);
        else
            out.push_back(c);
    }
    return out;
}

std::string namespace_symbol_prefix(const std::string &dotted) {
    if (dotted.empty()) return std::string();
    return namespace_symbol_path(dotted) + kSymbolPathSeparator;
}

std::string qualified_symbol(const std::string &path,
                             const std::string &member) {
    std::string out;
    out.reserve(path.size() + kSeparatorLen + member.size());
    out += path;
    out.append(kSymbolPathSeparator, kSeparatorLen);
    out += member;
    return out;
}

std::string namespace_member_symbol(const std::string &dotted,
                                    const std::string &member) {
    return qualified_symbol(namespace_symbol_path(dotted), member);
}

std::string strip_symbol_path(const std::string &name,
                              const std::string &path) {
    if (path.empty()) return name;
    const size_t prefix = path.size() + kSeparatorLen;
    // Tiene que quedar algo detras: `path__` a secas no es un miembro.
    if (name.size() > prefix && name.compare(0, path.size(), path) == 0 &&
        name.compare(path.size(), kSeparatorLen, kSymbolPathSeparator) == 0)
        return name.substr(prefix);
    return name;
}

} // namespace vx
