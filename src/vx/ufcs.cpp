/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file vx/ufcs.cpp
 * @brief El indice de la llamada uniforme.  Ver @c vx/ufcs.h.
 */

#include "vx/ufcs.h"

#include "util/name_pool.h"

namespace vx {
namespace ufcs {

const std::string *head_of(const Type &t) {
    /* Un tipo FUERTE tiene su propio cubo, aunque por dentro sea un entero: esa
     * es toda su razon de ser.  Va ANTES del switch a proposito -- un newtype
     * es un primitivo con nombre, asi que el cubo comun de los escalares se lo
     * tragaria y `Edad` compartiria candidatas con `u32`, que es justo lo que
     * declararlo fuerte prohibe. */
    if (t.nominal_id != 0) return util::intern_name(t.nominal_name.str());
    /* Las familias que no llevan nombre propio caen en un cubo por FAMILIA: lo
     * declarado contra `T*` vale para cualquier puntero, y de eso hay uno solo
     * por programa en vez de uno por tipo apuntado. */
    switch (t.kind) {
    /* Los ESCALARES caen todos en el mismo cubo, y no cada uno en el suyo.  No
     * es una aproximacion: una llamada libre acepta `add(y, 3)` con `y : i32`
     * para `add(u64, u64)` por conversion implicita, y un literal sin sufijo se
     * re-tipa si cabe -- `doble(6)` va a `doble(u64)` --, asi que con la clave
     * exacta `y.add(3)` y `6.doble()` no encontrarian la candidata y las dos
     * grafias dejarian de ser la misma llamada, que es toda la propuesta.
     *
     * El cubo solo tiene que TRAERLAS todas; quien elige sigue siendo
     * `overload::select`, que pone la exacta por delante de la compatible
     * igual que en una llamada libre.  Y no engorda nada: lo que hay dentro
     * son las sobrecargas de ESE nombre, que son las mismas que la llamada
     * libre ya considera. */
    case PrimitiveKind::BOOL:
    case PrimitiveKind::CHAR:
    case PrimitiveKind::I8:
    case PrimitiveKind::I16:
    case PrimitiveKind::I32:
    case PrimitiveKind::I64:
    case PrimitiveKind::U8:
    case PrimitiveKind::U16:
    case PrimitiveKind::U32:
    case PrimitiveKind::U64:
    case PrimitiveKind::F32:
    case PrimitiveKind::F64: return util::intern_name("num");
    case PrimitiveKind::PTR: return util::intern_name("ptr");
    case PrimitiveKind::ARRAY: return util::intern_name("array");
    case PrimitiveKind::FUNCTION: return util::intern_name("fn");
    case PrimitiveKind::STRUCT:
    case PrimitiveKind::CLASS: {
        /* Un tipo con nombre entra por el suyo, y uno generico por el del
         * TEMPLATE: `Caja<i64>` y `Caja<f64>` caen en el mismo cubo, que es
         * donde vive lo declarado contra `Caja<T>`.  El nombre mangleado lleva
         * los argumentos detras de un `_`, y cortar por ahi seria adivinar:
         * mientras la instanciacion no diga de que plantilla sale, un generico
         * se indexa por su nombre completo -- correcto, aunque mas estrecho de
         * lo que la fase 4 necesitara. */
        const std::string &n = t.struct_name.str();
        return util::intern_name(n);
    }
    default: break;
    }
    /* Los primitivos por su nombre de tipo, que es lo que el usuario escribe:
     * `u64`, `i32`, `bool`. */
    return util::intern_name(primitive_name(t.kind));
}

void Index::declare(const Type &first_param, const std::string &name,
                    uint32_t slot) {
    const std::string *n = util::intern_name(name);
    Candidates &c = by_head_[Key{head_of(first_param), n}];
    for (uint32_t s : c)
        if (s == slot) return; // ya estaba: declarar dos veces no duplica
    c.push_back(slot);
    by_name_[n].push_back(slot);
}

const Candidates *Index::find(const Type &recv, const std::string &written,
                              const std::string **matched) const {
    const std::string *head = head_of(recv);
    /* Tal y como se escribio: lo normal es que ahi acabe.  La cabeza se calcula
     * UNA vez para todas las grafias. */
    const std::string *name = util::intern_name(written);
    auto it = by_head_.find(Key{head, name});
    if (it != by_head_.end()) {
        if (matched != nullptr) *matched = name;
        return &it->second;
    }
    // Y si no, con el prefijo que el aplanado le puso a lo de este modulo.
    for (const std::string &p : prefixes_) {
        name = util::intern_name(p + written);
        it = by_head_.find(Key{head, name});
        if (it == by_head_.end()) continue;
        if (matched != nullptr) *matched = name;
        return &it->second;
    }
    return nullptr;
}

const Candidates *Index::all_named(const std::string &written) const {
    // Las mismas grafias que prueba `find`, contra la tabla por NOMBRE.
    auto it = by_name_.find(util::intern_name(written));
    if (it != by_name_.end()) return &it->second;
    for (const std::string &p : prefixes_) {
        it = by_name_.find(util::intern_name(p + written));
        if (it != by_name_.end()) return &it->second;
    }
    return nullptr;
}

void Index::note_flattened(const std::string &mangled,
                           const std::string &public_name) {
    if (mangled.size() <= public_name.size()) return;
    if (mangled.compare(mangled.size() - public_name.size(),
                        public_name.size(), public_name) != 0)
        return;
    std::string prefix =
        mangled.substr(0, mangled.size() - public_name.size());
    for (const std::string &p : prefixes_)
        if (p == prefix) return;
    prefixes_.push_back(std::move(prefix));
}

} // namespace ufcs
} // namespace vx
