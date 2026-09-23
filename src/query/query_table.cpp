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
 * @file query/query_table.cpp
 * @brief El registro de consultas y la lectura de argumentos contra su fila.
 */

#include "query/query_table.h"

#include <cstring>

namespace query {

// ---------------------------------------------------------------------------
// Nombres estables.  Van en ingles como el resto del vocabulario: viajan al
// protocolo y al volcado, donde los leen herramientas y gente que no tiene por
// que saber espanol.  Lo que ve el usuario sale del catalogo multi-idioma.
// ---------------------------------------------------------------------------

const char *param_type_name(ParamType t) {
    switch (t) {
    case ParamType::String: return "string";
    case ParamType::UInt: return "uint";
    case ParamType::Int: return "int";
    case ParamType::Bool: return "bool";
    }
    return "?";
}

const char *presence_name(Presence p) {
    switch (p) {
    case Presence::Required: return "required";
    case Presence::Optional: return "optional";
    }
    return "?";
}

const char *effect_name(Effect e) {
    switch (e) {
    case Effect::ReadOnly: return "read-only";
    case Effect::Writes: return "writes";
    case Effect::Executes: return "executes";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Los PAQUETES de parametros que aporta cada @c Needs.
//
// Escritos UNA vez.  La alternativa -- repetirlos en cada fila que los use --
// es la misma forma de fallo que esta capa existe para cerrar: quince copias
// del mismo vocabulario, y basta con que una se quede sin `cpu` para que su
// esquema mienta sin que nadie lo note.
// ---------------------------------------------------------------------------

namespace {

/// Lo que pide una consulta que habla de UN documento.
constexpr QueryParam kDocumentParams[] = {required(p::uri)};

/// Con que se compila y para que maquina.  No son adornos: el nivel de
/// optimizacion, el juego de instrucciones de coma flotante y la
/// microarquitectura CAMBIAN la respuesta, asi que forman parte de la pregunta.
/// Sin ellos se contesta siempre por el mismo binario.
constexpr QueryParam kTargetParams[] = {
    optional(p::os, DefaultValue::str("")),
    optional(p::arch, DefaultValue::str("")),
    optional(p::opt, DefaultValue::integer(-1)),
    optional(p::float_isa, DefaultValue::str("")),
    optional(p::cpu, DefaultValue::str("")),
};

} // namespace

ParamList bundle_params(Needs one) {
    switch (one) {
    case Needs::Document: return ParamList(kDocumentParams);
    case Needs::Target: return ParamList(kTargetParams);
    default: break;
    }
    return ParamList();
}

/**
 * @brief El siguiente valor puesto en @p set a partir del bit @p desde, o
 *        @c Needs::None si no queda ninguno.
 *
 * Se recorre por BITS y no por una lista escrita al lado, que es lo que habia:
 * esa lista repetia lo que ya dice el `switch` de @ref bundle_params, y anadir
 * un valor con parametros obligaba a acordarse de los dos sitios -- olvidarse
 * no da un error, da un parametro que se acepta y no se publica.  Un valor sin
 * paquete devuelve una lista vacia, asi que recorrerlos todos sale gratis.
 *
 * @param set  Conjunto de la fila.
 * @param from Bit por el que seguir (1 para empezar).
 * @param[out] bit El bit encontrado, para continuar desde el siguiente.
 */
Needs next_need(Needs set, uint32_t from, uint32_t &bit) {
    const uint32_t bits = static_cast<uint32_t>(set);
    for (uint32_t b = from; b != 0; b <<= 1)
        if ((bits & b) != 0u) {
            bit = b;
            return static_cast<Needs>(b);
        }
    bit = 0;
    return Needs::None;
}

// ---------------------------------------------------------------------------
// El registro.
// ---------------------------------------------------------------------------

Registry &Registry::instance() {
    static Registry r;
    return r;
}

void Registry::add(const QueryDesc *rows, std::size_t n) {
    rows_.reserve(rows_.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        rows_.push_back(&rows[i]);
}

const QueryDesc *Registry::find(const std::string &name) const {
    /* `strcmp` y no medir primero: medir recorre el nombre ENTERO antes de
     * comparar nada, mientras que esto corta en el primer byte distinto -- y
     * como casi ninguna consulta empieza igual, casi siempre es un byte. */
    const char *wanted = name.c_str();
    for (const QueryDesc *d : rows_)
        if (std::strcmp(d->name, wanted) == 0) return d;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Leer los argumentos CONTRA la fila.
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief El parametro llamado @p name: lo declarado por la fila, y ademas lo
 *        que aportan sus @c Needs.
 *
 * Busca en los DOS sitios a proposito, porque lo que un manejador puede leer
 * tiene que ser exactamente lo que se publico en el esquema -- ni mas ni menos.
 * Si mirara solo la fila, `uri` seria ilegible desde el manejador aunque
 * estuviera publicado; si mirara solo los paquetes, no habria consultas con
 * parametros propios.
 *
 * Por TEXTO y no por puntero internado, al reves que el nombre de la consulta,
 * y es deliberado: aqui el conjunto es de media docena de entradas y esta en la
 * mano, asi que comparar cadenas cortas sale mas barato que tomar el cerrojo
 * del pozo una vez por parametro y por peticion.  Internar paga cuando el
 * nombre cruza una frontera y se compara contra muchos; no cuando no.
 */
const QueryParam *find_param(const QueryDesc *d, const char *name) {
    if (d == nullptr) return nullptr;
    for (const QueryParam &q : d->params)
        if (std::strcmp(q.name(), name) == 0) return &q;
    uint32_t bit = 0;
    for (Needs one = next_need(d->needs, 1, bit); bit != 0;
         one = next_need(d->needs, bit << 1, bit))
        for (const QueryParam &q : bundle_params(one))
            if (std::strcmp(q.name(), name) == 0) return &q;
    return nullptr;
}

/// El parametro, o un fallo RUIDOSO si la fila no lo declara.
const QueryParam &demand(const QueryDesc *d, const char *name) {
    const QueryParam *p = find_param(d, name);
    if (p == nullptr) {
        /* Esto NO es un error de quien pregunta: es que el manejador pide algo
         * que su propia fila no declara.  O sea que el esquema publicado miente
         * -- dice que parametros hay y hay otro --, que es justo la divergencia
         * que esta capa existe para impedir.  Devolver un defecto callado la
         * dejaria viva y sin sintoma. */
        throw BadQueryDefinition(std::string("la consulta '") +
                                 (d != nullptr ? d->name : "?") +
                                 "' lee un parametro que no declara: '" + name +
                                 "'");
    }
    return *p;
}

} // namespace

BadQueryDefinition::BadQueryDefinition(const std::string &what)
    : std::logic_error(what) {}

MissingArgument::MissingArgument(const std::string &name)
    : std::runtime_error("falta el parametro obligatorio '" + name + "'"),
      name_(name) {}

bool Args::given(const char *name) const {
    demand(desc, name); // que la fila lo declare se exige igual.
    return raw != nullptr && raw->contains(name);
}

std::string Args::str(const char *name) const {
    const QueryParam &p = demand(desc, name);
    if (raw != nullptr && raw->contains(name)) {
        const nlohmann::json &v = raw->at(name);
        if (v.is_string()) return v.get<std::string>();
    }
    if (p.presence == Presence::Required) throw MissingArgument(name);
    return p.def.s != nullptr ? p.def.s : "";
}

uint32_t Args::uint(const char *name) const {
    const QueryParam &p = demand(desc, name);
    if (raw != nullptr && raw->contains(name)) {
        const nlohmann::json &v = raw->at(name);
        if (v.is_number_unsigned()) return v.get<uint32_t>();
        /* Un entero con signo que no es negativo tambien vale: un cliente en
         * JavaScript no distingue los dos, y rechazarlo seria un error que solo
         * depende de como su runtime serializo el numero. */
        if (v.is_number_integer() && v.get<int64_t>() >= 0)
            return static_cast<uint32_t>(v.get<int64_t>());
    }
    if (p.presence == Presence::Required) throw MissingArgument(name);
    return static_cast<uint32_t>(p.def.n);
}

int64_t Args::integer(const char *name) const {
    const QueryParam &p = demand(desc, name);
    if (raw != nullptr && raw->contains(name)) {
        const nlohmann::json &v = raw->at(name);
        if (v.is_number_integer()) return v.get<int64_t>();
    }
    if (p.presence == Presence::Required) throw MissingArgument(name);
    return p.def.n;
}

bool Args::boolean(const char *name) const {
    const QueryParam &p = demand(desc, name);
    if (raw != nullptr && raw->contains(name)) {
        const nlohmann::json &v = raw->at(name);
        if (v.is_boolean()) return v.get<bool>();
    }
    if (p.presence == Presence::Required) throw MissingArgument(name);
    return p.def.b;
}

} // namespace query
