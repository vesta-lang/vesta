/**
 * @file comptime_values.cpp
 * @brief Los valores comptime que se ensenan en el editor.
 * @see vx/comptime/comptime_values.h
 */
#include "vx/comptime/comptime_values.h"

#include "vx/type_checker.h"

#include <string>

namespace vx {

namespace {

/**
 * @brief Escribe una constante comptime como (tipo, valor) legible.
 *
 * Conservador: un entero en decimal, una cadena entre comillas, un array y un
 * struct por su tamano, un tipo por su nombre.  El texto lo lee una persona en
 * el editor, asi que va en ingles.
 *
 * @param name El nombre de la constante.
 * @param c    Su valor.
 * @return La instantanea.
 */
CompileResult::ComptimeValueSnapshot
snapshot_of(const std::string &name, const TypeChecker::ComptimeConst &c) {
    CompileResult::ComptimeValueSnapshot snap;
    snap.name = name;
    snap.scope = ""; // nivel superior
    if (c.is_type) {
        snap.type_kind = "type";
        snap.value_str = type_to_string(c.type_val);
    } else if (c.is_str) {
        snap.type_kind = "string";
        snap.value_str = "\"" + c.str_value + "\"";
    } else if (c.is_array) {
        snap.type_kind = "array";
        snap.value_str =
            "[" + std::to_string(c.array_vals.size()) + " elements]";
    } else if (c.is_struct) {
        snap.type_kind = "struct";
        snap.value_str =
            "{" + std::to_string(c.struct_fields.size()) + " fields}";
    } else {
        snap.type_kind = "int";
        snap.value_str = std::to_string(c.value);
    }
    return snap;
}

} // namespace

void collect_comptime_values(
    const TypeChecker &tc,
    std::vector<CompileResult::ComptimeValueSnapshot> &out) {
    for (const auto &kv : tc.comptime_const_values())
        out.push_back(snapshot_of(kv.first, kv.second));
    /* Las locales que calcularon los bloques `comptime { ... }` (arrays y
     * structs poblados por bucles, etc.): el comprobador las captura justo
     * antes de salir de cada bloque. */
    for (const auto &b : tc.comptime_block_snapshots()) {
        CompileResult::ComptimeValueSnapshot snap;
        snap.name = b.name;
        snap.scope = b.scope;
        snap.type_kind = b.type_kind;
        snap.value_str = b.value_str;
        out.push_back(std::move(snap));
    }
    /* Lo que contestaron los builtins de introspeccion (`sizeof<T>`,
     * `alignof<T>`, `kind<T>`...), con su ubicacion para que el editor lo
     * ensene al pasar por encima de la expresion. */
    for (const auto &h : tc.comptime_builtin_hits()) {
        CompileResult::ComptimeValueSnapshot snap;
        snap.name = h.name;
        snap.scope = "";
        snap.type_kind = h.type_kind;
        snap.value_str = h.value_str;
        snap.loc = h.loc;
        // El builtin es el nombre antes del '<' (p.ej. "type.size").
        const size_t lt = h.name.find('<');
        snap.builtin_kind =
            (lt != std::string::npos) ? h.name.substr(0, lt) : h.name;
        out.push_back(std::move(snap));
    }
}

} // namespace vx
