/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file builtin_docs.cpp
 * @brief La documentacion de los builtins, armada para el editor.
 *
 * Los DATOS no estan aqui: salen de `catalog/builtin_docs.toml` a traves de la
 * tabla generada (`gen/builtin_docs_gen.cpp`), que los trae en todos los
 * idiomas.  Estaban escritos a mano en este fichero, y entonces solo podian
 * estar en uno -- es texto que lee una persona, y una persona lee en el suyo
 * --.
 *
 * Lo que queda aqui son las dos DECISIONES que no son datos:
 *
 *   1. QUE IDIOMA se ensenya.  Lo dice @c vx::diag::current_language, el mismo
 *      criterio que usan los diagnosticos, y se traduce a la columna de este
 *      catalogo por su CoDIGO ISO: si los dos catalogos llegan a tener
 *      idiomas distintos, buscar por indice mostraria el texto equivocado sin
 *      que nada fallara.
 *   2. DE DONDE salen las RANURAS.  De `vx/builtin_params.h`, que es la misma
 *      tabla que lee el comprobador al registrar el builtin.  Desde que una
 *      llamada puede nombrarlas (`f(.a = 3)`) el nombre de una ranura es parte
 *      del CONTRATO, asi que tenerlo en dos sitios significa que un dia el
 *      editor sugiere algo que no compila.
 *
 * El mapa se arma una vez por proceso.  El hover y los hints no son camino
 * caliente, pero tampoco hay razon para rehacerlo en cada pulsacion.
 */

#include "lsp/builtin_docs.h"

#include "vx/builtin_params.h"    // las ranuras: un solo sitio, compartido
#include "vx/diag/diag_catalog.h" // y un solo criterio de idioma

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace lsp {

namespace {

/**
 * @brief La columna de este catalogo que toca al idioma activo.
 *
 * Se resuelve por CoDIGO, no por indice: los dos catalogos se ordenan por su
 * cuenta y nada garantiza que la columna 1 sea el mismo idioma en los dos.
 *
 * @return El indice de columna, o 0 (el de reserva) si no esta.
 */
int active_column() {
    const char *want = vx::diag::language_code(vx::diag::current_language());
    if (want == nullptr || *want == '\0') return 0;
    int n = 0;
    const char *const *langs = builtin_doc_languages(&n);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(langs[i], want) == 0) return i;
    return 0;
}

/// Construye el mapa nombre -> BuiltinDoc una sola vez (estable durante el
/// proceso, devuelto por referencia).
const std::unordered_map<std::string, BuiltinDoc> &table() {
    static const std::unordered_map<std::string, BuiltinDoc> T = [] {
        const int col = active_column();
        std::unordered_map<std::string, BuiltinDoc> m;
        const int n = builtin_doc_count();
        m.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            BuiltinDocView v;
            if (!builtin_doc_at(i, &v)) continue;
            BuiltinDoc d;
            d.name = v.name;
            d.signature = v.sig;
            /* Si a un idioma le falta la traduccion, se ensenya la del primero
             * en vez de un hueco: una descripcion en otro idioma sirve, y una
             * vacia parece que el builtin no esta documentado. */
            const char *doc = (col < v.doc_count) ? v.doc[col] : nullptr;
            if (doc == nullptr || *doc == '\0')
                doc = (v.doc_count > 0) ? v.doc[0] : "";
            d.doc = doc;
            /* Las ranuras, de su tabla. */
            const vx::BuiltinParams bp = vx::builtin_params_of(v.name);
            d.params.reserve(bp.count);
            for (uint8_t k = 0; k < bp.count; ++k)
                d.params.emplace_back(bp.names[k]);
            m[v.name] = std::move(d);
        }
        return m;
    }();
    return T;
}

} // namespace

const BuiltinDoc *lookup_builtin(const std::string &name) {
    const auto &T = table();
    auto it = T.find(name);
    return it == T.end() ? nullptr : &it->second;
}

const std::vector<std::string> &all_builtin_names() {
    // Se construye una sola vez a partir de las claves de la tabla, ordenadas
    // para que el completado del LSP tenga un orden estable.
    static const std::vector<std::string> names = [] {
        const auto &T = table();
        std::vector<std::string> v;
        v.reserve(T.size());
        for (const auto &kv : T)
            v.push_back(kv.first);
        std::sort(v.begin(), v.end());
        return v;
    }();
    return names;
}

} // namespace lsp
