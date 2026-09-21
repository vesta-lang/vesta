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
        /* Por su nombre, tal cual.  Que una INSTANCIACION entre por el de su
         * plantilla -- para que `Caja<i64>` y `Caja<f64>` caigan donde vive lo
         * declarado contra `Caja<T>` -- se resuelve ANTES de llegar aqui, con
         * la ficha que dejo la instanciacion.
         *
         * Y se resuelve ahi y no aqui porque sacarlo del nombre seria adivinar:
         * el aplanado pone los argumentos detras de un `_`, y un tipo del
         * usuario puede llamarse exactamente igual. */
        const std::string &n = t.struct_name.str();
        return util::intern_name(n);
    }
    default: break;
    }
    /* Los primitivos por su nombre de tipo, que es lo que el usuario escribe:
     * `u64`, `i32`, `bool`. */
    return util::intern_name(primitive_name(t.kind));
}

const std::string *head_of_decl(const ast::TypeNode *t,
                                const std::vector<std::string> &vars) {
    if (t == nullptr || vars.empty()) return nullptr;
    switch (t->kind) {
    case ast::NodeKind::NamedTypeNode: {
        const auto *n = static_cast<const ast::NamedTypeNode *>(t);
        /* Con argumentos es una instanciacion, y su cubo es el del TEMPLATE:
         * `Caja<T>` cae donde cae un receptor `Caja<i64>`, que es lo que hace
         * que lo declarado contra la plantilla lo encuentre la instancia. */
        if (!n->type_args.empty()) return util::intern_name(n->name);
        /* Sin argumentos, o es una variable -- y entonces vale para cualquier
         * receptor -- o es un tipo con nombre, y ahi no hay variable ninguna:
         * que lo indexe la via normal, que resuelve el tipo de verdad. */
        for (const std::string &v : vars)
            if (v == n->name) return util::intern_name("any");
        return nullptr;
    }
    case ast::NodeKind::PointerTypeNode: return util::intern_name("ptr");
    case ast::NodeKind::ArrayTypeNode: return util::intern_name("array");
    case ast::NodeKind::FunctionTypeNode: return util::intern_name("fn");
    /* Un primitivo CON argumentos (`unique<T>`) tiene cubo propio por su clase,
     * y saber cual exige resolverlo.  Se deja fuera antes que meterlo en uno
     * equivocado: no estar es que el punto no lo alcanza, y estar mal es que
     * alcanza lo que no debe. */
    default: return nullptr;
    }
}

namespace {

/**
 * @brief La cabeza bajo la que se indexa un tipo sabiendo que es un PARaMETRO.
 *
 * Igual que @ref head_of salvo en un caso, y el caso importa: un parametro que
 * resolvio a `void` no es un parametro de tipo `void` -- eso no existe, no hay
 * nada que pasarle --, es un parametro que NO SE PUDO RESOLVER, y lo unico que
 * no resuelve en una firma es una variable de tipo.  O sea una plantilla, y su
 * cubo es `any`: vale para cualquier receptor.
 *
 * Donde se nota es al cruzar el modulo.  Una plantilla declarada AQUI se indexa
 * por lo ESCRITO (@ref head_of_decl, que ve el `T`), pero una que llega por un
 * `import` llega ya resuelta -- de su interfaz binaria sale una firma, no un
 * arbol --, y ahi `T` se habia vuelto `void`: todas las plantillas importadas
 * del programa acababan en un cubo llamado `void`, que ningun receptor
 * pregunta.  El efecto era que el punto alcanzaba o no la MISMA funcion segun
 * si estaba escrita en este fichero o en otro, que es justo lo que un import
 * viene a que no pase.
 *
 * @param t El tipo del parametro.
 * @return Su cabeza, internada.
 */
const std::string *head_of_param(const Type &t) {
    if (t.kind == PrimitiveKind::VOID) return util::intern_name("any");
    return head_of(t);
}

} // namespace

void Index::declare_head(const std::string *head, const std::string &name,
                         uint32_t slot) {
    if (head == nullptr) return;
    const std::string *n = util::intern_name(name);
    Candidates &c = by_head_[Key{head, n}];
    /* Si no habia ninguna, este nombre es NUEVO en el cubo.  Preguntarselo a
     * la tabla que ya se consulto sale gratis; buscarlo en la lista del cubo
     * seria recorrerla entera por declaracion, y los cubos `any` y `num` son
     * uno solo para todo el programa -- o sea, cuadratico en el numero de
     * funciones que compilas. */
    const bool first_here = c.empty();
    for (uint32_t s : c)
        if (s == slot) return;
    c.push_back(slot);
    by_name_[n].push_back(slot);
    if (first_here) note_name_in_head(head, n);
}

void Index::declare(const Type &first_param, const std::string &name,
                    uint32_t slot) {
    const std::string *n = util::intern_name(name);
    const std::string *head = head_of_param(first_param);
    Candidates &c = by_head_[Key{head, n}];
    const bool first_here = c.empty(); // ver declare_head
    for (uint32_t s : c)
        if (s == slot) return; // ya estaba: declarar dos veces no duplica
    c.push_back(slot);
    by_name_[n].push_back(slot);
    if (first_here) note_name_in_head(head, n);
}

void Index::note_name_in_head(const std::string *head, const std::string *n) {
    /* Sin buscar si ya estaba: quien llama solo lo hace la PRIMERA vez que ese
     * nombre cae en ese cubo, y eso lo sabe en O(1) (ver declare_head).  Con
     * la busqueda aqui, declarar era cuadratico en las funciones del cubo. */
    HeadEntry e;
    e.declared = n;
    /* Partirlo AQUI y no al enumerar: el nombre no cambia nunca, asi que el
     * corte se paga una vez por declaracion en vez de una vez por pregunta. */
    const size_t sep = n->rfind("__");
    if (sep == std::string::npos || sep == 0) {
        e.public_name = n;
        e.ns_prefix = nullptr;
        e.origin = nullptr;
    } else {
        e.public_name = util::intern_name(n->substr(sep + 2));
        e.ns_prefix = util::intern_name(n->substr(0, sep + 2));
        /* El origen se guarda como el usuario lo ESCRIBE -- con puntos --,
         * porque es lo que sale por `scoped_method_origin` y por el editor: un
         * namespace anidado devuelto como `geo__sub` no se podria ni teclear.
         */
        std::string dotted = n->substr(0, sep);
        size_t at = 0;
        while ((at = dotted.find("__", at)) != std::string::npos) {
            dotted.replace(at, 2, ".");
            at += 1;
        }
        e.origin = util::intern_name(dotted);
    }
    names_by_head_[head].push_back(e);
}

void Index::reachable_for(const Type &recv, const std::string &site_prefix,
                          std::vector<Reachable> &out) const {
    /* Los mismos dos cubos que mira `find`, y en el mismo orden: el del tipo y
     * el de las que valen para cualquier receptor. */
    const std::string *heads[2] = {head_of(recv), util::intern_name("any")};
    /* El prefijo del sitio se interna UNA vez, no por entrada: a partir de
     * aqui el alcance es comparar dos punteros. */
    const std::string *site =
        site_prefix.empty() ? nullptr : util::intern_name(site_prefix);

    for (const std::string *head : heads) {
        if (head == nullptr) continue;
        auto it = names_by_head_.find(head);
        if (it == names_by_head_.end()) continue;
        for (const HeadEntry &e : it->second) {
            /* El MISMO criterio que `find`, visto del otro lado: alli se
             * prueba el nombre tal cual y luego con el prefijo del sitio, asi
             * que alcanza lo que no lleva prefijo y lo que lleva el de aqui.
             * Lo de otro namespace no se alcanza escribiendo el nombre corto,
             * y por eso tampoco se enumera. */
            if (e.ns_prefix != nullptr && e.ns_prefix != site) continue;
            auto cands = by_head_.find(Key{head, e.declared});
            if (cands == by_head_.end()) continue;
            for (uint32_t slot : cands->second)
                out.push_back(Reachable{e.public_name, e.origin, slot});
        }
    }
}

const Candidates *Index::find(const Type &recv, const std::string &written,
                              const std::string &site_prefix,
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
    // Y si no, con el prefijo del namespace DESDE EL QUE SE LLAMA.
    if (!site_prefix.empty()) {
        const std::string *pref = util::intern_name(site_prefix + written);
        it = by_head_.find(Key{head, pref});
        if (it != by_head_.end()) {
            if (matched != nullptr) *matched = pref;
            return &it->second;
        }
    }
    /* Y por ultimo el cubo de las que valen para CUALQUIER receptor: una
     * generica cuyo primer parametro es la variable a secas (`T id<T>(T x)`)
     * no puede estar bajo ninguna cabeza concreta -- no la tiene hasta que se
     * instancia --, asi que vive aparte.
     *
     * Se mira DESPUES de la cabeza exacta, y ese orden es la regla: lo
     * declarado para ESTE tipo gana a lo declarado para todos.  Al reves, una
     * generica cualquiera taparia la funcion escrita a proposito para el
     * receptor que se tiene delante. */
    const std::string *any = util::intern_name("any");
    it = by_head_.find(Key{any, name});
    if (it != by_head_.end()) {
        if (matched != nullptr) *matched = name;
        return &it->second;
    }
    if (site_prefix.empty()) return nullptr;
    name = util::intern_name(site_prefix + written);
    it = by_head_.find(Key{any, name});
    if (it == by_head_.end()) return nullptr;
    if (matched != nullptr) *matched = name;
    return &it->second;
}

void Index::declare_any(const Type &param, const std::string &name,
                        uint32_t slot) {
    Candidates &c = by_any_[Key{head_of_param(param), util::intern_name(name)}];
    for (uint32_t s : c)
        if (s == slot) return; // dos parametros de la misma cabeza no duplican
    c.push_back(slot);
}

const Candidates *Index::find_any(const Type &recv, const std::string &written,
                                  const std::string &site_prefix,
                                  const std::string **matched) const {
    const std::string *head = head_of(recv);
    const std::string *name = util::intern_name(written);
    auto it = by_any_.find(Key{head, name});
    if (it != by_any_.end()) {
        if (matched != nullptr) *matched = name;
        return &it->second;
    }
    if (site_prefix.empty()) return nullptr;
    name = util::intern_name(site_prefix + written);
    it = by_any_.find(Key{head, name});
    if (it == by_any_.end()) return nullptr;
    if (matched != nullptr) *matched = name;
    return &it->second;
}

const Candidates *Index::all_named(const std::string &written,
                                   const std::string **matched) const {
    /* Aqui SI se prueban todos los prefijos del fichero, al reves que en
     * `find`: esto no resuelve una llamada, busca que decir cuando no hay
     * ninguna.  Justo lo que `find` no debe alcanzar -- una declarada en otro
     * namespace del mismo fichero -- es lo que aqui hay que encontrar para
     * poder senyalarla. */
    const std::string *name = util::intern_name(written);
    auto it = by_name_.find(name);
    if (it != by_name_.end()) {
        if (matched != nullptr) *matched = name;
        return &it->second;
    }
    for (const std::string &p : prefixes_) {
        name = util::intern_name(p + written);
        it = by_name_.find(name);
        if (it == by_name_.end()) continue;
        if (matched != nullptr) *matched = name;
        return &it->second;
    }
    return nullptr;
}

void Index::note_flattened(const std::string &mangled,
                           const std::string &public_name) {
    if (mangled.size() <= public_name.size()) return;
    if (mangled.compare(mangled.size() - public_name.size(), public_name.size(),
                        public_name) != 0)
        return;
    std::string prefix = mangled.substr(0, mangled.size() - public_name.size());
    for (const std::string &p : prefixes_)
        if (p == prefix) return;
    prefixes_.push_back(std::move(prefix));
}

} // namespace ufcs
} // namespace vx
