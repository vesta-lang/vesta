/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxdbg_emit.cpp
 * @brief Implementacion del traductor de tipos Vesta a entidades de depuracion.
 *
 * El orden de emision no es libre: una entidad se identifica por su contenido,
 * y su contenido incluye a quien referencia.  Para poder decir que `Lector`
 * deriva de `Flujo` hay que haber emitido `Flujo` antes, porque su huella es
 * parte de la de `Lector`.  De ahi el recorrido en profundidad por la cadena de
 * derivacion, con memoria de lo ya emitido.
 *
 * Las relaciones van de abajo a arriba: un metodo declara pertenecer a su clase
 * y la clase no lista sus metodos.  Al reves seria un ciclo -- la huella de la
 * clase dependeria de la del metodo y la del metodo de la de la clase -- y
 * ademas ya es la regla del subsistema: la relacion inversa es un indice, no un
 * dato.
 *
 * La CLAVE con la que se identifica un tipo es la del compilador, no su nombre
 * visible: es la que ya distingue `a.Vector` de `b.Vector` y una instanciacion
 * de plantilla de otra.  El nombre bonito se guarda aparte, para ensenarlo.
 */

#include "vx/vxdbg_emit.h"

#include "vx/type_checker.h"
#include "vxdbg/codec.h"
#include "vxdbg/store.h"

#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace vx {

namespace {

/**
 * @brief Como llama Vesta a algo, y de que especie es en terminos comunes.
 *
 * Tenerlos juntos evita que se separen: anadir un genero nuevo obliga a decir
 * en el mismo sitio a que especie pertenece, en vez de dejarlo para luego.
 */
struct KindPair {
    vxdbg::EntityKind kind;
    const char *lang; ///< el nombre que le da Vesta
};

/// @name Generos de Vesta
///
/// Uno por cada forma que Vesta distingue de verdad, no por cada palabra
/// clave: `@Abstract` o `@overlay` cambian lo que ES el tipo -- uno no se
/// puede instanciar, el otro no posee su memoria -- y quien lee un error tiene
/// que verlo.  Lo que solo cambia como se genera el codigo no llega aqui.
/// @{
constexpr KindPair K_STRUCT{vxdbg::EntityKind::Type, "struct"};
constexpr KindPair K_UNION{vxdbg::EntityKind::Type, "union"};
/// Vista tipada sobre memoria ajena: un valor de este tipo ES un puntero, no
/// aloca ni copia.  Confundirlo con un struct normal al explicar un fallo
/// llevaria a buscar el problema donde no esta.
constexpr KindPair K_OVERLAY{vxdbg::EntityKind::Type, "overlay"};
/// Solo sirve de base: no se instancia.
constexpr KindPair K_ABSTRACT_STRUCT{vxdbg::EntityKind::Type,
                                     "abstract struct"};
/// Tiene metodos virtuales, asi que lleva puntero a tabla y sus campos no
/// empiezan en cero.
constexpr KindPair K_POLY_STRUCT{vxdbg::EntityKind::Type, "polymorphic struct"};
constexpr KindPair K_CLASS{vxdbg::EntityKind::Type, "class"};
constexpr KindPair K_INTERFACE{vxdbg::EntityKind::Contract, "interface"};
/// Clase de aspecto: sus metodos se ejecutan alrededor de los de otras.
constexpr KindPair K_ASPECT{vxdbg::EntityKind::Type, "aspect"};
/// Predicado sobre tipos, comprobado al compilar.  Se cumple, no se instancia:
/// de ahi que sea un contrato y no un tipo.
constexpr KindPair K_CONCEPT{vxdbg::EntityKind::Contract, "concept"};
constexpr KindPair K_ENUM{vxdbg::EntityKind::Enumeration, "enum"};
/// Enum cuyos casos SON valores de otro tipo (`enum Color : u8`).
constexpr KindPair K_VALUED_ENUM{vxdbg::EntityKind::Enumeration, "valued enum"};
/// Lo que se escribe en el sitio y nadie declara: primitivos, punteros,
/// genericos instanciados sobre la marcha.
constexpr KindPair K_BUILTIN{vxdbg::EntityKind::Type, "type"};
constexpr KindPair K_FIELD{vxdbg::EntityKind::Field, "field"};
constexpr KindPair K_STATIC_FIELD{vxdbg::EntityKind::Field, "static field"};
/// Campo que solo existe mientras se compila: no ocupa nada en la instancia.
constexpr KindPair K_COMPTIME_FIELD{vxdbg::EntityKind::Field, "comptime field"};
constexpr KindPair K_METHOD{vxdbg::EntityKind::Function, "method"};
constexpr KindPair K_CONSTRUCTOR{vxdbg::EntityKind::Function, "constructor"};
constexpr KindPair K_DESTRUCTOR{vxdbg::EntityKind::Function, "destructor"};
constexpr KindPair K_VARIANT{vxdbg::EntityKind::Constant, "variant"};
constexpr KindPair K_NAMESPACE{vxdbg::EntityKind::Module, "namespace"};
/// @}

/**
 * @brief Estado del recorrido: quien ya se emitio y con que huella.
 */
class Emitter {
  public:
    /**
     * @param tc Checker con las tablas de tipos.
     * @param store Almacen destino.
     * @param stats Cuenta de lo emitido.
     */
    Emitter(const TypeChecker &tc, vxdbg::NodeStore &store,
            VxdbgEmitStats &stats)
        : tc_(tc), store_(store), stats_(stats) {}

    /// @brief Emite el nodo del fichero y lo deja como fichero en curso.
    void set_file(const std::string &path, const std::string &content);

    /// @brief Emite todos los tipos conocidos y sus miembros.
    void emit_all();

  private:
    /**
     * @brief Huella de un tipo por su clave, emitiendolo si hace falta.
     *
     * @param key Clave del compilador.
     * @return Su identificador, o uno vacio si la clave no corresponde a
     *         ningun tipo conocido.
     */
    vxdbg::LanguageEntityId entity_for(const std::string &key);

    /**
     * @brief Construye la entidad de un tipo, sin guardarla.
     *
     * Separado de la emision a proposito: es donde va a crecer todo lo que
     * queda por anadir -- posiciones, anotaciones, parametros de plantilla,
     * restricciones -- y mezclarlo con guardar y con llevar la memoria de lo ya
     * hecho convertiria una funcion en cuatro cosas a la vez.
     *
     * @param key Clave del compilador.
     * @param out Recibe la entidad.
     * @return @c false si la clave no es de ningun tipo conocido.
     */
    bool build_type(const std::string &key, vxdbg::LanguageEntity &out);

    /// @brief Igual, pero para un tipo escrito en el sitio (sin declaracion).
    vxdbg::LanguageEntityId builtin_for(const std::string &name);

    /// @brief Entidad de un espacio de nombres, por su camino.
    vxdbg::LanguageEntityId module_for(const std::string &path);

    /// @brief Emite los miembros de un struct.
    void emit_struct_members(const StructLayout &lay,
                             vxdbg::LanguageEntityId owner,
                             const std::string &owner_key);
    /// @brief Emite los miembros de una clase.
    void emit_class_members(const ClassLayout &lay,
                            vxdbg::LanguageEntityId owner,
                            const std::string &owner_key);
    /// @brief Emite las variantes de un enum.
    void emit_enum_members(const EnumLayout &lay, vxdbg::LanguageEntityId owner,
                           const std::string &owner_key);

    /**
     * @brief Emite un miembro: campo, metodo o variante.
     * @param name Nombre del miembro.
     * @param kp Genero.
     * @param owner Entidad que lo declara.
     * @param owner_key Clave de esa entidad, para componer la del miembro.
     * @param type_name Tipo al que se refiere, si tiene uno.
     */
    void emit_member(const std::string &name, const KindPair &kp,
                     vxdbg::LanguageEntityId owner,
                     const std::string &owner_key,
                     const std::string &type_name);

    /// @brief Guarda una entidad y devuelve su huella.
    vxdbg::LanguageEntityId put(const vxdbg::LanguageEntity &e);

    /**
     * @brief Nombre visible de una clave del compilador.
     *
     * Lo dice el checker, que es quien manglea: el emisor no puede deducirlo
     * partiendo la clave por un separador, porque el separador es una
     * convencion que cambia -- hoy `lib__Caja`, manana podria ser otra cosa --
     * y quedaria un nombre roto sin que nadie se enterara.
     *
     * @param key Clave.
     * @return El nombre a ensenar; la clave entera si nadie la registro.
     */
    const std::string &display_name(const std::string &key) const;

    /**
     * @brief Anade una relacion, o cuenta un destino no resuelto.
     * @param e Entidad en construccion.
     * @param kind Genero de la relacion.
     * @param target_key Clave del destino.
     */
    void relate(vxdbg::LanguageEntity &e, vxdbg::RelationKind kind,
                const std::string &target_key);

    const TypeChecker &tc_;
    vxdbg::NodeStore &store_;
    VxdbgEmitStats &stats_;
    vxdbg::FileId file_;
    /// Tipos ya emitidos, por CLAVE del compilador.  Sin esto, una jerarquia en
    /// forma de rombo emitiria la base tantas veces como caminos lleguen a ella.
    std::unordered_map<std::string, vxdbg::LanguageEntityId> done_;
    /// Tipos en curso de emision, para cortar si los datos traen un ciclo.  El
    /// checker rechaza la herencia circular, pero este recorrido no puede dar
    /// por hecho que lo que le llega esta bien formado: si lo estuviera
    /// siempre, no haria falta comprobarlo en ningun sitio.
    std::unordered_set<std::string> in_progress_;
};

void Emitter::set_file(const std::string &path, const std::string &content) {
    vxdbg::FileNode f;
    f.path = path;
    f.language = "vesta";
    f.encoding = "utf-8";
    // El resumen del contenido es lo que permite luego detectar que el fuente
    // ya no es el que se compilo, y no ensenar la linea equivocada.
    if (!content.empty())
        f.checksum = vxdbg::hash_bytes(content.data(), content.size());
    vxdbg::ContentHash h;
    if (vxdbg::store_node(store_, f, h)) file_ = vxdbg::FileId{h};
}

vxdbg::LanguageEntityId Emitter::put(const vxdbg::LanguageEntity &e) {
    vxdbg::ContentHash h;
    if (!vxdbg::store_node(store_, e, h)) return {};
    ++stats_.entities;
    return vxdbg::LanguageEntityId{h};
}

const std::string &Emitter::display_name(const std::string &key) const {
    const auto &decl = tc_.declared_ns_symbols();
    auto it = decl.find(key);
    // Lo que no se declaro dentro de un espacio de nombres no lleva prefijo:
    // su clave YA es su nombre.
    if (it == decl.end()) return key;
    return it->second.second;
}

void Emitter::relate(vxdbg::LanguageEntity &e, vxdbg::RelationKind kind,
                     const std::string &target_key) {
    if (target_key.empty()) return;
    const auto id = entity_for(target_key);
    if (id.hash.empty()) {
        // Se omite la relacion en vez de apuntar a un destino inventado: en un
        // diagnostico, un nombre equivocado es peor que un dato de menos.
        ++stats_.unresolved;
        return;
    }
    vxdbg::Relation r;
    r.kind = kind;
    r.target = id;
    e.relations.push_back(r);
}

vxdbg::LanguageEntityId Emitter::builtin_for(const std::string &name) {
    auto it = done_.find(name);
    if (it != done_.end()) return it->second;
    vxdbg::LanguageEntity e;
    e.name = name;
    e.qualified = name; // el texto del tipo ya es su identidad
    e.kind = K_BUILTIN.kind;
    e.lang_kind = K_BUILTIN.lang;
    // No se declara en ningun fichero, asi que no lleva posicion.  Sale con la
    // misma huella en todos los modulos que lo usen, que es justo lo que se
    // quiere de `i32`.
    const auto id = put(e);
    done_.emplace(name, id);
    return id;
}

vxdbg::LanguageEntityId Emitter::module_for(const std::string &path) {
    // La clave lleva delante de que es para que un espacio de nombres llamado
    // igual que un tipo no se confunda con el.
    const std::string key = "namespace " + path;
    auto it = done_.find(key);
    if (it != done_.end()) return it->second;
    vxdbg::LanguageEntity e;
    e.name = path;
    e.qualified = path;
    e.kind = K_NAMESPACE.kind;
    e.lang_kind = K_NAMESPACE.lang;
    const auto id = put(e);
    done_.emplace(key, id);
    return id;
}

bool Emitter::build_type(const std::string &key, vxdbg::LanguageEntity &e) {
    e.name = display_name(key);
    e.qualified = key;
    e.declared_at.file = file_;
    // Si se declaro dentro de un espacio de nombres, la pertenencia se dice
    // como relacion y no reconstruyendo el prefijo del nombre.
    if (auto it = tc_.declared_ns_symbols().find(key);
        it != tc_.declared_ns_symbols().end() && !it->second.first.empty()) {
        vxdbg::Relation r;
        r.kind = vxdbg::RelationKind::DeclaredIn;
        r.target = module_for(it->second.first);
        if (!r.target.hash.empty()) e.relations.push_back(r);
    }

    const auto &structs = tc_.struct_layouts();
    const auto &classes = tc_.class_layouts();
    const auto &enums = tc_.enum_layouts();

    if (auto s = structs.find(key); s != structs.end()) {
        const StructLayout &lay = s->second;
        // El orden importa: una vista sobre memoria ajena no es un struct
        // abstracto aunque lleve la marca, y no poder instanciarlo pesa mas
        // que tener metodos virtuales.
        const KindPair &kp = lay.is_overlay      ? K_OVERLAY
                             : lay.is_union      ? K_UNION
                             : lay.is_abstract   ? K_ABSTRACT_STRUCT
                             : lay.is_polymorphic ? K_POLY_STRUCT
                                                  : K_STRUCT;
        e.kind = kp.kind;
        e.lang_kind = kp.lang;
        e.byte_size = lay.size_bytes;
        e.alignment = lay.align_bytes;
        // Los conceptos que satisface un struct no estan en su layout: la
        // conformidad la lleva el checker en su propia tabla y no la expone.
        // Se anadira cuando lo haga; inventarla aqui recorriendo conceptos y
        // reevaluando predicados seria recomputar lo que ya se comprobo.
        relate(e, vxdbg::RelationKind::Derives, lay.super_name);
        return true;
    }
    if (auto c = classes.find(key); c != classes.end()) {
        const ClassLayout &lay = c->second;
        const KindPair &kp = lay.is_interface ? K_INTERFACE
                             : lay.is_aspect  ? K_ASPECT
                                              : K_CLASS;
        e.kind = kp.kind;
        e.lang_kind = kp.lang;
        e.byte_size = lay.size_bytes;
        relate(e, vxdbg::RelationKind::Derives, lay.super_name);
        for (const auto &iface : lay.interface_names)
            relate(e, vxdbg::RelationKind::Implements, iface);
        return true;
    }
    if (auto en = enums.find(key); en != enums.end()) {
        const EnumLayout &lay = en->second;
        const KindPair &kp = lay.is_valued ? K_VALUED_ENUM : K_ENUM;
        e.kind = kp.kind;
        e.lang_kind = kp.lang;
        e.byte_size = lay.size_bytes;
        // Un enum con tipo base ES un valor de ese tipo: la relacion lo dice
        // sin que quien lea tenga que saber como se representa.
        relate(e, vxdbg::RelationKind::Uses, lay.backing_type_name);
        return true;
    }
    if (auto cp = tc_.concepts().find(key); cp != tc_.concepts().end()) {
        // Un concepto no tiene tamano ni instancias: es una condicion sobre
        // tipos que se comprueba al compilar.
        e.kind = K_CONCEPT.kind;
        e.lang_kind = K_CONCEPT.lang;
        return true;
    }
    return false;
}

vxdbg::LanguageEntityId Emitter::entity_for(const std::string &key) {
    if (key.empty()) return {};
    auto it = done_.find(key);
    if (it != done_.end()) return it->second;
    // Un ciclo en la derivacion no se puede representar: la huella de cada uno
    // dependeria de la del otro.  Se corta devolviendo vacio, con lo que la
    // relacion se omite y el contador de no resueltos lo deja ver.
    if (!in_progress_.insert(key).second) return {};

    vxdbg::LanguageEntity e;
    const bool known = build_type(key, e);
    in_progress_.erase(key);
    if (!known) return {}; // no es un tipo declarado

    const auto id = put(e);
    done_.emplace(key, id);
    // Solo los TIPOS son raices: desde uno se llega a sus miembros por el
    // indice inverso, y listar tambien los miembros seria repetir el grafo
    // entero en una lista plana.
    stats_.roots.emplace_back(key, id);
    return id;
}

void Emitter::emit_member(const std::string &name, const KindPair &kp,
                          vxdbg::LanguageEntityId owner,
                          const std::string &owner_key,
                          const std::string &type_name) {
    vxdbg::LanguageEntity m;
    m.name = name;
    // Un miembro se identifica por su propietario mas su nombre: dos campos
    // `size` de dos structs distintos no son el mismo campo.
    m.qualified = owner_key + "::" + name;
    m.kind = kp.kind;
    m.lang_kind = kp.lang;
    m.declared_at.file = file_;
    {
        vxdbg::Relation r;
        r.kind = vxdbg::RelationKind::DeclaredIn;
        r.target = owner;
        m.relations.push_back(r);
    }
    if (!type_name.empty()) {
        // El tipo puede ser uno declarado o cualquier otra cosa escrita en el
        // sitio (`i32`, `u8*`, `Array<i64>`).  Los segundos se emiten como
        // entidad sin declaracion: sirven igual para decir de que es el campo.
        auto id = entity_for(type_name);
        if (id.hash.empty()) id = builtin_for(type_name);
        if (!id.hash.empty()) {
            vxdbg::Relation r;
            r.kind = vxdbg::RelationKind::Uses;
            r.target = id;
            m.relations.push_back(r);
        }
    }
    if (!put(m).hash.empty()) ++stats_.members;
}

void Emitter::emit_struct_members(const StructLayout &lay,
                                  vxdbg::LanguageEntityId owner,
                                  const std::string &owner_key) {
    for (const auto &f : lay.fields)
        emit_member(f.name, K_FIELD, owner, owner_key, type_to_string(f.type));
    for (const auto &f : lay.static_fields)
        emit_member(f.name, K_STATIC_FIELD, owner, owner_key,
                    type_to_string(f.type));
    for (const auto &f : lay.comptime_fields)
        emit_member(f.name, K_COMPTIME_FIELD, owner, owner_key,
                    type_to_string(f.type));
    for (const auto &m : lay.methods)
        emit_member(m.name, m.is_constructor ? K_CONSTRUCTOR : K_METHOD, owner,
                    owner_key, type_to_string(m.return_type));
}

void Emitter::emit_class_members(const ClassLayout &lay,
                                 vxdbg::LanguageEntityId owner,
                                 const std::string &owner_key) {
    for (const auto &f : lay.fields)
        emit_member(f.name, K_FIELD, owner, owner_key, type_to_string(f.type));
    for (const auto &f : lay.static_fields)
        emit_member(f.name, K_STATIC_FIELD, owner, owner_key,
                    type_to_string(f.type));
    for (const auto &m : lay.methods) {
        const KindPair &kp = m.is_constructor  ? K_CONSTRUCTOR
                             : m.is_destructor ? K_DESTRUCTOR
                                               : K_METHOD;
        emit_member(m.name, kp, owner, owner_key, type_to_string(m.return_type));
    }
}

void Emitter::emit_enum_members(const EnumLayout &lay,
                                vxdbg::LanguageEntityId owner,
                                const std::string &owner_key) {
    for (const auto &v : lay.variants)
        emit_member(v.name, K_VARIANT, owner, owner_key, "");
}

void Emitter::emit_all() {
    // Primero los tipos, porque los miembros apuntan a ellos y hay que tener la
    // huella del propietario antes de poder escribir un miembro.
    for (const auto &kv : tc_.struct_layouts()) entity_for(kv.first);
    for (const auto &kv : tc_.class_layouts()) entity_for(kv.first);
    for (const auto &kv : tc_.enum_layouts()) entity_for(kv.first);
    // Los conceptos van despues porque no referencian a nadie: emitirlos antes
    // no cambia nada, pero asi el recorrido queda en el orden de la tabla.
    for (const auto &kv : tc_.concepts()) entity_for(kv.first);

    for (const auto &kv : tc_.struct_layouts()) {
        const auto owner = entity_for(kv.first);
        if (!owner.hash.empty())
            emit_struct_members(kv.second, owner, kv.first);
    }
    for (const auto &kv : tc_.class_layouts()) {
        const auto owner = entity_for(kv.first);
        if (!owner.hash.empty()) emit_class_members(kv.second, owner, kv.first);
    }
    for (const auto &kv : tc_.enum_layouts()) {
        const auto owner = entity_for(kv.first);
        if (!owner.hash.empty()) emit_enum_members(kv.second, owner, kv.first);
    }
}

} // namespace

std::string default_vxdbg_dir() {
    // La misma valvula que el resto de caches del compilador: si el proyecto
    // los redirige a un sitio comun, este va con ellos y no se queda suelto en
    // la carpeta de trabajo.
    if (const char *v = std::getenv("VX_CACHE_DIR"); v && v[0])
        return std::string(v) + "/vxdbg";
    return ".cache/vxdbg";
}

bool emit_vxdbg_source(const TypeChecker &tc, const std::string &source_path,
                       const std::string &source_text,
                       const std::string &out_dir, VxdbgEmitStats &stats,
                       std::string &err) {
    (void)err;
    vxdbg::FileNodeStore store(out_dir.empty() ? default_vxdbg_dir() : out_dir);
    Emitter em(tc, store, stats);
    em.set_file(source_path, source_text);
    em.emit_all();
    return true;
}

} // namespace vx
