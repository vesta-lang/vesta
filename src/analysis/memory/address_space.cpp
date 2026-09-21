/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/memory/address_space.cpp
 * @brief De que memoria es una direccion cuando ha pasado por memoria.
 *
 * El contrato, el porque y lo que cuesta que falte estan en la cabecera.  Aqui
 * va COMO se resuelve, y las dos decisiones que gobiernan todo lo demas:
 *
 * 1. **Solo se AFIRMA lo que se prueba.**  Una direccion sin marca no se da
 *    por de la maquina: se queda en @c Unknown con su motivo.  Es tentador
 *    aprovechar que el intermedio trae @c NotHost por defecto y leerlo como
 *    "de la maquina", pero ese valor no distingue "se probo que es de la
 *    maquina" de "nadie dijo nada", y este analisis existe precisamente
 *    porque el segundo caso mentia.  @c Machine se prueba desde la reserva:
 *    un @c ALLOCA que no es del anfitrion deja un hueco de la maquina.
 *
 * 2. **Una escritura que no se sabe donde cae lo ENSUCIA todo.**  Si alguien
 *    guarda a traves de un puntero sin resolver, esa escritura puede haber
 *    caido en el hueco que se esta leyendo, asi que afirmar que ahi hay una
 *    direccion del anfitrion seria inventar.  Solo ensucia si lo que guardo
 *    es de OTRA memoria: guardar lo mismo que se iba a afirmar no cambia la
 *    respuesta.
 */
#include "analysis/memory/address_space.h"

#include "analysis/manager/analysis_codec.h" // lo COMUN de guardar un analisis
#include "analysis/memory/memory_access.h" // ancho de un acceso (UNICA verdad)
#include "ir/ssa_ir.h"

#include <algorithm>

namespace analysis {

const char *address_space_name(AddressSpace s) {
    switch (s) {
    case AddressSpace::Host: return "host";
    case AddressSpace::Machine: return "machine";
    case AddressSpace::Unknown: break;
    }
    return "unknown";
}

/* ---------------------------------------------------------------------------
 * Vocabulario de motivos de ESTE dominio.
 *
 * Definidos UNA vez y en el .cpp, como los nombres de productor del ASA y por
 * la misma razon: quien los compara lo hace por la DIRECCION del literal, y
 * con `constexpr` en la cabecera cada unidad de traduccion plegaria la lectura
 * a su propio literal -- los literales no se unifican entre ficheros objeto --
 * y el mismo motivo dejaria de reconocerse visto desde otro fichero.
 * ------------------------------------------------------------------------ */

const char *const kAddrWhyNotAnAddress = "as.not_an_address";
const char *const kAddrWhyIrUnknown = "as.ir_unknown";
const char *const kAddrWhyParam = "as.param";
const char *const kAddrWhyCall = "as.call";
const char *const kAddrWhyNoOracle = "as.no_points_to";
const char *const kAddrWhyAddrUnresolved = "as.addr_unresolved";
const char *const kAddrWhyNoStore = "as.no_store";
const char *const kAddrWhyStoresDiffer = "as.stores_differ";
const char *const kAddrWhyStoredValueUnknown = "as.stored_value_unknown";
const char *const kAddrWhyDirtyStore = "as.aliased_by_unknown_store";
const char *const kAddrWhyMixedOperands = "as.mixed_operands";
const char *const kAddrWhyPhiMixed = "as.phi_mixed";
const char *const kAddrWhyShape = "as.shape";
const char *const kAddrWhyBudget = "as.budget";

namespace {

/// Cuantas vueltas se le dan antes de rendirse.  Es un flujo hacia adelante
/// sobre SSA: lo unico que obliga a repetir son los PHI de un bucle, y esos
/// convergen en pocas.  El tope existe para que un intermedio raro no cuelgue
/// la compilacion, y cuando se toca SE DICE (@c kAddrWhyBudget).
constexpr int kMaxRounds = 16;

/// Una respuesta con su motivo, para no construirla a mano en cada renuncia.
AddressSpaceEntry unknown_because(asa::UnknownReason reason, const char *code,
                                  ir::IrValueId at = ir::IR_NO_VALUE) {
    AddressSpaceEntry e;
    e.space = AddressSpace::Unknown;
    e.reason = reason;
    e.reason_code = code;
    e.reason_at = at;
    return e;
}

/// Una respuesta afirmativa.  Sin motivo, que solo tiene sentido con @c
/// Unknown.
AddressSpaceEntry known(AddressSpace s) {
    AddressSpaceEntry e;
    e.space = s;
    return e;
}

/**
 * @brief Lo que se puede afirmar de dos respuestas a la vez.
 *
 * Iguales, eso; distintas, nada -- y "nada" NO es "de la maquina" --.
 */
AddressSpace join_spaces(AddressSpace a, AddressSpace b) {
    if (a == b) return a;
    return AddressSpace::Unknown;
}

/**
 * @brief Un sitio de memoria, reducido a lo que lo identifica.
 *
 * Struct con nombre y no un entero empaquetado: empaquetar tres campos de
 * anchos distintos en una clave es justo como se cuelan dos sitios que salen
 * iguales sin serlo.
 */
struct LocKey {
    uint8_t kind = 0;
    uint32_t id = 0;
    int64_t off = 0;
};

bool loc_key_less(const LocKey &a, const LocKey &b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.id != b.id) return a.id < b.id;
    return a.off < b.off;
}

bool loc_key_same(const LocKey &a, const LocKey &b) {
    return a.kind == b.kind && a.id == b.id && a.off == b.off;
}

LocKey key_of(const effects::AbstractLoc &l) {
    LocKey k;
    k.kind = static_cast<uint8_t>(l.kind);
    k.id = l.id;
    k.off = l.off;
    return k;
}

/// Que se ha guardado en un sitio, ya fusionado con el resto de escrituras
/// que caen en el mismo.
struct StoredAt {
    LocKey where;
    AddressSpace space = AddressSpace::Unknown;
    /**
     * @brief Cuantas escrituras trajeron una memoria QUE SE SABE.
     *
     * Separa dos cosas que daban el mismo @c Unknown y se arreglan de forma
     * OPUESTA.  Con menos de dos conocidas, lo que falta es saber de los
     * valores que se guardaron -- se arregla aguas arriba --; solo cuando hay
     * DOS conocidas y no coinciden lo que hay depende del camino, y entonces
     * no hay nada que arreglar: afirmar seria inventar.
     *
     * Contar las escrituras a secas no valia: dos que no se saben ninguna
     * salian como "dos caminos dicen cosas distintas", que manda a buscar una
     * rama que no existe.
     */
    uint32_t known_writes = 0;
};

bool stored_at_less(const StoredAt &a, const StoredAt &b) {
    return loc_key_less(a.where, b.where);
}

/**
 * @brief La misma cuenta, pero por RAIZ y sin mirar el desplazamiento.
 *
 * Para cuando se lee de un sitio cuyo desplazamiento no es exacto -- una
 * tabla indexada por una variable --.  Ahi el offset no dice nada, pero la
 * RAIZ si: si todas las ranuras escritas de esa region llevan direcciones de
 * la misma memoria, leas la que leas sale esa, y el indice no cambia la
 * respuesta.
 *
 * Es el patron de una tabla de punteros a funcion recorrida en un bucle, que
 * sin esto se quedaba sin contestar teniendo la respuesta delante.
 */
struct StoredInRoot {
    uint8_t kind = 0;
    uint32_t id = 0;
    AddressSpace space = AddressSpace::Unknown;
    uint32_t known_writes = 0;
};

bool stored_in_root_less(const StoredInRoot &a, const StoredInRoot &b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.id < b.id;
}

bool stored_in_root_same(const StoredInRoot &a, const StoredInRoot &b) {
    return a.kind == b.kind && a.id == b.id;
}

/**
 * @brief Es el NULO, que no es una direccion de ninguna memoria?
 *
 * Guardar un cero no contradice a quien guardo una direccion en el mismo
 * hueco: es la ausencia de una, no otra distinta.  Sin esto, la
 * inicializacion perezosa -- poner el hueco a cero y rellenarlo la primera
 * vez -- salia como dos escrituras que discrepan, y el patron es de los mas
 * comunes que hay: es el de los once envoltorios de @c std.syscall.windows.
 *
 * No es una excepcion inventada aqui: es la MISMA regla que ya aplica el
 * bajado al fusionar un PHI ("todos los args no-null comparten host-ness,
 * null=0 es indiferente").
 */
bool stores_nothing(const ir::IrFunction &fn, const IrFacts &facts,
                    ir::IrValueId v) {
    if (v >= fn.values.size()) return false;
    /* Por la DEFINICION y no solo por la bandera: `is_const` la pone la
     * propagacion de constantes, asi que un cero recien bajado no la trae
     * todavia -- y es justo el que aparece al inicializar un hueco --. */
    const ir::IrInstr *d = facts.def(v);
    if (d != nullptr && d->op == ir::IrOp::CONST && d->imm == 0) return true;
    return fn.values[v].is_const && fn.values[v].const_val == 0;
}

/**
 * @brief Se puede EMPAREJAR este sitio con otro?
 *
 * Un sitio sin raiz concreta -- @c Unknown, o la clase entera -- no identifica
 * un hueco, asi que no sirve ni para afirmar ni para ensuciar por igualdad:
 * ensucia por alias, que es otra cosa y se trata aparte.
 */
bool loc_is_precise(const effects::AbstractLoc &l) {
    return l.kind != effects::AbstractLoc::Kind::Unknown &&
           l.kind != effects::AbstractLoc::Kind::None &&
           l.id != effects::LOC_GENERIC;
}

/// El estado del resolutor.  Va en un struct para no pasar ocho parametros
/// entre las fases, que es de donde salen los que se olvidan.
struct Solver {
    const ir::IrFunction &fn;
    const IrFacts &facts;
    AddressSpaces out;

    /// La tabla points-to, si hizo falta pedirla.  Se pide UNA vez.
    const PointsTo *pt = nullptr;

    /// Lo guardado en cada sitio que se supo identificar, ordenado.
    std::vector<StoredAt> stored;
    /// Y lo mismo por RAIZ, para cuando el desplazamiento no es exacto.
    std::vector<StoredInRoot> stored_by_root;
    /// Que memorias se han escrito a traves de punteros SIN resolver.  Son las
    /// que pueden haber caido en cualquier hueco.
    bool dirty_host = false;
    bool dirty_machine = false;
    bool dirty_unknown = false;

    explicit Solver(const ir::IrFunction &f, const IrFacts &fa)
        : fn(f), facts(fa) {}

    const AddressSpaceEntry &at(ir::IrValueId v) const { return out.at(v); }

    AddressSpace space_of(ir::IrValueId v) const { return out.at(v).space; }

    /// Escribe @p e en @p v y dice si eso cambio algo.  Solo se AVANZA: una
    /// respuesta que ya era firme no se degrada a media vuelta, que es lo que
    /// haria oscilar el punto fijo.
    bool set(ir::IrValueId v, const AddressSpaceEntry &e) {
        if (v >= out.by_value.size()) return false;
        AddressSpaceEntry &cur = out.by_value[v];
        if (cur.space != AddressSpace::Unknown) return false;
        if (cur.space == e.space && cur.reason == e.reason &&
            cur.reason_code == e.reason_code)
            return false;
        cur = e;
        return e.space != AddressSpace::Unknown;
    }
};

/**
 * @brief Lo que el intermedio ya dice, antes de propagar nada.
 *
 * Las tres clases de anfitrion valen igual aqui: COMO se supo importa para
 * quien lea el hecho, no para saber en que memoria esta.  Un @c Unknown del
 * intermedio se respeta -- alguien dijo expresamente que no se sabe -- y un
 * @c NotHost NO se toma por "de la maquina", por lo dicho arriba.
 */
void seed(Solver &s) {
    s.out.by_value.assign(s.fn.values.size(), AddressSpaceEntry{});
    for (size_t i = 0; i < s.fn.values.size(); ++i) {
        const ir::IrValueId v = static_cast<ir::IrValueId>(i);
        AddressSpaceEntry &e = s.out.by_value[i];
        if (!s.facts.exists(v)) {
            /* La ranura se quedo cuando el optimizador borro lo que la
             * definia.  No es ignorancia: no hay nada ahi que resolver. */
            e = unknown_because(asa::UnknownReason::NothingToSay,
                                kAddrWhyNotAnAddress);
            continue;
        }
        const ir::MemorySpace m = s.fn.values[i].memory;
        if (m == ir::MemorySpace::Unknown) {
            e = unknown_because(asa::UnknownReason::ShapeNotRecognized,
                                kAddrWhyIrUnknown, v);
            continue;
        }
        if (s.fn.values[i].is_host_ptr()) {
            e = known(AddressSpace::Host);
            continue;
        }
        /* Queda @c NotHost, que no prueba nada: se deja sin contestar y que lo
         * gane la propagacion si puede. */
        e = unknown_because(asa::UnknownReason::NotAsked, "");
    }
}

/**
 * @brief Lo que se prueba por la OPERACION que produjo el valor.
 *
 * Son dos, y las dos son pruebas de primera mano -- las emitio el compilador
 * sabiendo lo que hacia --, no deducciones:
 *
 *  - una reserva dice en que memoria vive su hueco;
 *  - la direccion de una etiqueta es codigo DE LA MAQUINA.  Importa tanto
 *    como el otro lado: una direccion de bytecode llamada por la via nativa
 *    salta a codigo que el procesador no entiende, que es el fallo simetrico
 *    del que va todo esto.
 */
void seed_by_operation(Solver &s) {
    for (const ir::IrBlock &blk : s.fn.blocks) {
        for (const ir::IrInstr &ins : blk.instrs) {
            if (ins.dst == ir::IR_NO_VALUE) continue;
            if (ins.op == ir::IrOp::ALLOCA) {
                s.set(ins.dst, known(ins.host_alloca ? AddressSpace::Host
                                                     : AddressSpace::Machine));
            } else if (ins.op == ir::IrOp::LABEL_ADDR) {
                s.set(ins.dst, known(AddressSpace::Machine));
            }
        }
    }
}

/// Cuantas escrituras y lecturas de memoria tiene la funcion.  Decide si hay
/// que molestarse en pedir points-to: la mayoria de las funciones no guardan
/// ninguna direccion, y pedir lo vacio ya cuesta cerrojo y reservas.
struct MemoryShape {
    size_t stores = 0;
    size_t loads = 0;
};

MemoryShape memory_shape(const ir::IrFunction &fn) {
    MemoryShape sh;
    for (const ir::IrBlock &blk : fn.blocks) {
        for (const ir::IrInstr &ins : blk.instrs) {
            if (ins.op == ir::IrOp::STORE)
                ++sh.stores;
            else if (ins.op == ir::IrOp::LOAD)
                ++sh.loads;
        }
    }
    return sh;
}

/**
 * @brief Recoge, para cada sitio identificable, que memoria se guardo en el.
 *
 * Se rehace en cada vuelta porque lo guardado puede venir a su vez de una
 * lectura que aun no se habia resuelto.  Cuesta O(E log E) sobre las
 * ESCRITURAS, no sobre los valores.
 */
void collect_stores(Solver &s) {
    s.stored.clear();
    s.dirty_host = false;
    s.dirty_machine = false;
    s.dirty_unknown = false;
    if (s.pt == nullptr) return;

    for (const ir::IrBlock &blk : s.fn.blocks) {
        for (const ir::IrInstr &ins : blk.instrs) {
            if (ins.op != ir::IrOp::STORE || ins.operands.size() < 2) continue;
            const ir::IrValueId val = ins.operands[0];
            const ir::IrValueId addr = ins.operands[1];
            /* El nulo no aporta ni contradice: se salta entero, tambien
             * cuando cae por un puntero sin resolver -- ensuciar con la
             * ausencia de una direccion no tiene sentido --. */
            if (stores_nothing(s.fn, s.facts, val)) continue;
            const AddressSpace what = s.space_of(val);
            const effects::AbstractLoc l =
                loc_of(*s.pt, addr, memory_access_size(ins.type));
            if (!loc_is_precise(l)) {
                /* No se sabe donde cayo: puede haber caido en cualquier
                 * hueco.  Se apunta QUE memoria se escribio, porque solo
                 * ensucia a quien iba a afirmar otra distinta. */
                if (what == AddressSpace::Host)
                    s.dirty_host = true;
                else if (what == AddressSpace::Machine)
                    s.dirty_machine = true;
                else
                    s.dirty_unknown = true;
                continue;
            }
            const uint32_t known = (what == AddressSpace::Unknown) ? 0u : 1u;
            StoredAt entry;
            entry.where = key_of(l);
            entry.space = what;
            entry.known_writes = known;
            s.stored.push_back(entry);

            StoredInRoot per_root;
            per_root.kind = entry.where.kind;
            per_root.id = entry.where.id;
            per_root.space = what;
            per_root.known_writes = known;
            s.stored_by_root.push_back(per_root);
        }
    }

    std::sort(s.stored.begin(), s.stored.end(), stored_at_less);
    /* Se funden las que caen en el mismo sitio: si no coinciden, lo que hay
     * ahi depende de por donde se haya pasado y no se afirma nada. */
    size_t w = 0;
    for (size_t i = 0; i < s.stored.size();) {
        size_t j = i + 1;
        AddressSpace acc = s.stored[i].space;
        uint32_t known = s.stored[i].known_writes;
        while (j < s.stored.size() &&
               loc_key_same(s.stored[j].where, s.stored[i].where)) {
            acc = join_spaces(acc, s.stored[j].space);
            known += s.stored[j].known_writes;
            ++j;
        }
        s.stored[w].where = s.stored[i].where;
        s.stored[w].space = acc;
        s.stored[w].known_writes = known;
        ++w;
        i = j;
    }
    s.stored.resize(w);

    /* Y el mismo plegado por raiz, que es lo que contesta cuando el
     * desplazamiento no es exacto. */
    std::sort(s.stored_by_root.begin(), s.stored_by_root.end(),
              stored_in_root_less);
    size_t wr = 0;
    for (size_t i = 0; i < s.stored_by_root.size();) {
        size_t j = i + 1;
        AddressSpace acc = s.stored_by_root[i].space;
        uint32_t known = s.stored_by_root[i].known_writes;
        while (j < s.stored_by_root.size() &&
               stored_in_root_same(s.stored_by_root[j], s.stored_by_root[i])) {
            acc = join_spaces(acc, s.stored_by_root[j].space);
            known += s.stored_by_root[j].known_writes;
            ++j;
        }
        s.stored_by_root[wr] = s.stored_by_root[i];
        s.stored_by_root[wr].space = acc;
        s.stored_by_root[wr].known_writes = known;
        ++wr;
        i = j;
    }
    s.stored_by_root.resize(wr);
}

/// Lo guardado en @p k, o nullptr si nadie escribio ahi de forma
/// identificable.  Busqueda binaria sobre el vector ordenado.
/// Lo guardado en TODA la raiz @p kind/@p id, o nullptr si nadie escribio ahi.
const StoredInRoot *stored_in_root(const Solver &s, uint8_t kind, uint32_t id) {
    StoredInRoot probe;
    probe.kind = kind;
    probe.id = id;
    const auto it =
        std::lower_bound(s.stored_by_root.begin(), s.stored_by_root.end(),
                         probe, stored_in_root_less);
    if (it == s.stored_by_root.end() || !stored_in_root_same(*it, probe))
        return nullptr;
    return &*it;
}

const StoredAt *stored_at(const Solver &s, const LocKey &k) {
    StoredAt probe;
    probe.where = k;
    const auto it = std::lower_bound(s.stored.begin(), s.stored.end(), probe,
                                     stored_at_less);
    if (it == s.stored.end() || !loc_key_same(it->where, k)) return nullptr;
    return &*it;
}

/// Puede una escritura sin resolver haber dejado otra cosa en el hueco?
bool dirtied_by_unknown_store(const Solver &s, AddressSpace candidate) {
    if (s.dirty_unknown) return true;
    if (candidate == AddressSpace::Host) return s.dirty_machine;
    if (candidate == AddressSpace::Machine) return s.dirty_host;
    return false;
}

/// Lo que se puede decir del resultado de una lectura.
AddressSpaceEntry resolve_load(const Solver &s, const ir::IrInstr &ins) {
    if (ins.operands.empty())
        return unknown_because(asa::UnknownReason::ShapeNotRecognized,
                               kAddrWhyShape, ins.dst);
    if (s.pt == nullptr)
        return unknown_because(asa::UnknownReason::MissingDependency,
                               kAddrWhyNoOracle, ins.dst);

    const ir::IrValueId addr = ins.operands[0];
    const effects::AbstractLoc l =
        loc_of(*s.pt, addr, memory_access_size(ins.type));
    if (!loc_is_precise(l))
        return unknown_because(asa::UnknownReason::MissingDependency,
                               kAddrWhyAddrUnresolved, addr);

    /* Desplazamiento que no se probo: @c loc_of degrada a la region entera y
     * lo marca poniendo el ancho a cero.  Entonces el offset no dice nada,
     * pero la RAIZ si -- si todas las ranuras escritas de esa region llevan
     * direcciones de la misma memoria, leas la que leas sale esa --.  Es el
     * caso de una tabla de punteros a funcion recorrida en un bucle. */
    if (l.width == 0) {
        const StoredInRoot *whole = stored_in_root(s, key_of(l).kind, l.id);
        if (whole == nullptr)
            return unknown_because(asa::UnknownReason::MissingDependency,
                                   kAddrWhyNoStore, addr);
        if (whole->space == AddressSpace::Unknown)
            return unknown_because(
                whole->known_writes < 2 ? asa::UnknownReason::MissingDependency
                                        : asa::UnknownReason::RuntimeDependent,
                whole->known_writes < 2 ? kAddrWhyStoredValueUnknown
                                        : kAddrWhyStoresDiffer,
                addr);
        if (dirtied_by_unknown_store(s, whole->space))
            return unknown_because(asa::UnknownReason::MissingDependency,
                                   kAddrWhyDirtyStore, addr);
        return known(whole->space);
    }

    const StoredAt *found = stored_at(s, key_of(l));
    if (found == nullptr)
        return unknown_because(asa::UnknownReason::MissingDependency,
                               kAddrWhyNoStore, addr);
    if (found->space == AddressSpace::Unknown) {
        /* UNA sola escritura cuyo valor no se sabe no es una discrepancia:
         * lo que falta es saber de ESE valor, y eso se arregla aguas arriba.
         * Darle el motivo de "dos caminos dicen cosas distintas" mandaba a
         * buscar una rama que no existe -- es el caso de los envoltorios de
         * `std.syscall.windows`, donde el unico que escribe ahi es el
         * llamado, rellenando su buffer de retorno --. */
        if (found->known_writes < 2)
            return unknown_because(asa::UnknownReason::MissingDependency,
                                   kAddrWhyStoredValueUnknown, addr);
        return unknown_because(asa::UnknownReason::RuntimeDependent,
                               kAddrWhyStoresDiffer, addr);
    }
    if (dirtied_by_unknown_store(s, found->space))
        return unknown_because(asa::UnknownReason::MissingDependency,
                               kAddrWhyDirtyStore, addr);
    return known(found->space);
}

/// Lo que se puede decir de una derivacion: sumar o restar un desplazamiento a
/// una direccion no la saca de su memoria.
AddressSpaceEntry resolve_derived(const Solver &s, const ir::IrInstr &ins) {
    AddressSpace acc = AddressSpace::Unknown;
    bool any = false;
    for (const ir::IrValueId op : ins.operands) {
        const AddressSpace sp = s.space_of(op);
        if (sp == AddressSpace::Unknown) continue;
        if (!any) {
            acc = sp;
            any = true;
        } else if (sp != acc) {
            /* Dos direcciones de memorias distintas sumadas: el resultado no
             * es de ninguna de las dos, y decir que si lo es seria inventar. */
            return unknown_because(asa::UnknownReason::RuntimeDependent,
                                   kAddrWhyMixedOperands, ins.dst);
        }
    }
    if (!any)
        return unknown_because(asa::UnknownReason::MissingDependency,
                               kAddrWhyShape, ins.dst);
    return known(acc);
}

/// Lo que se puede decir de un PHI: lo que coincida en todas sus entradas.
AddressSpaceEntry resolve_phi(const Solver &s, const ir::IrInstr &ins) {
    AddressSpace acc = AddressSpace::Unknown;
    bool any = false;
    for (const ir::IrPhiArg &a : ins.phi_args) {
        const AddressSpace sp = s.space_of(a.value);
        if (sp == AddressSpace::Unknown)
            return unknown_because(asa::UnknownReason::MissingDependency,
                                   kAddrWhyPhiMixed, a.value);
        if (!any) {
            acc = sp;
            any = true;
        } else if (sp != acc) {
            return unknown_because(asa::UnknownReason::RuntimeDependent,
                                   kAddrWhyPhiMixed, a.value);
        }
    }
    if (!any)
        return unknown_because(asa::UnknownReason::NothingToSay,
                               kAddrWhyNotAnAddress, ins.dst);
    return known(acc);
}

/// Una vuelta del punto fijo.  @return si algo avanzo.
bool one_round(Solver &s) {
    bool changed = false;
    for (const ir::IrBlock &blk : s.fn.blocks) {
        for (const ir::IrInstr &ins : blk.instrs) {
            if (ins.dst == ir::IR_NO_VALUE) continue;
            if (s.at(ins.dst).space != AddressSpace::Unknown) continue;
            switch (ins.op) {
            case ir::IrOp::LOAD:
                changed |= s.set(ins.dst, resolve_load(s, ins));
                break;
            case ir::IrOp::ADD:
            case ir::IrOp::SUB:
            case ir::IrOp::GEP:
            case ir::IrOp::BITCAST:
                changed |= s.set(ins.dst, resolve_derived(s, ins));
                break;
            case ir::IrOp::PHI:
                changed |= s.set(ins.dst, resolve_phi(s, ins));
                break;
            case ir::IrOp::CALL:
            case ir::IrOp::CALLN:
            case ir::IrOp::CALLIND:
                /* No se cruzan llamadas a proposito -- lo dice la cabecera --,
                 * asi que lo que devuelva otra funcion se contesta "no se"
                 * diciendo por que.  Si el intermedio ya lo marco, la siembra
                 * lo cogio antes de llegar aqui. */
                s.set(ins.dst,
                      unknown_because(asa::UnknownReason::OpaqueBoundary,
                                      kAddrWhyCall, ins.dst));
                break;
            default: break;
            }
        }
    }
    return changed;
}

/// Lo que queda sin contestar cuando se acaban las vueltas, con su motivo.
void close_open_questions(Solver &s, bool ran_out) {
    for (size_t i = 0; i < s.out.by_value.size(); ++i) {
        AddressSpaceEntry &e = s.out.by_value[i];
        if (e.space != AddressSpace::Unknown) continue;
        if (e.reason != asa::UnknownReason::NotAsked) continue;
        const ir::IrValueId v = static_cast<ir::IrValueId>(i);
        if (ran_out) {
            e = unknown_because(asa::UnknownReason::BudgetExceeded,
                                kAddrWhyBudget, v);
            continue;
        }
        if (s.facts.param_index(v) >= 0) {
            /* Viene de fuera: lo sabe quien llama, y cruzar hasta el es lo que
             * este analisis no hace. */
            e = unknown_because(asa::UnknownReason::OpaqueBoundary,
                                kAddrWhyParam, v);
            continue;
        }
        e = unknown_because(asa::UnknownReason::NothingToSay,
                            kAddrWhyNotAnAddress, v);
    }
}

} // namespace

AddressSpaces compute_address_spaces(const ir::IrFunction &fn,
                                     const IrFacts &facts, PointsToOracle pt) {
    Solver s(fn, facts);
    seed(s);
    seed_by_operation(s);

    /* La pereza POR DENTRO: points-to solo se pide si la funcion de verdad
     * guarda algo en memoria y lo lee de vuelta.  Sin las dos cosas no hay
     * ningun eslabon que emparejar, y preguntar costaria sin devolver nada. */
    const MemoryShape shape = memory_shape(fn);
    if (shape.stores > 0 && shape.loads > 0 && pt.valid())
        s.pt = &pt.ask(pt.ctx, fn);

    bool ran_out = true;
    for (int round = 0; round < kMaxRounds; ++round) {
        collect_stores(s);
        if (!one_round(s)) {
            ran_out = false;
            break;
        }
    }
    close_open_questions(s, ran_out);
    return s.out;
}

// ===========================================================================
//  Guardarla y recuperarla entre compilaciones
// ===========================================================================

const char *const kAddressSpaceAnalysisName = "address_space";

char AddressSpaceAnalysis::ID = 0;

namespace {

/// Lo que ocupa una entrada como minimo, para comprobar que cabe antes de
/// leerla: espacio (1) + motivo (1) + indice de codigo (4) + valor (4).
constexpr size_t kEntryBytes = 10;

void write_entry(util::ByteWriter &w, const AddressSpaceEntry &e,
                 uint32_t code_index) {
    w.u8(static_cast<uint8_t>(e.space));
    w.u8(static_cast<uint8_t>(e.reason));
    w.u32(code_index);
    w.u32(static_cast<uint32_t>(e.reason_at));
}

AddressSpaceEntry read_entry(util::ByteReader &r,
                             const std::vector<const char *> &codes) {
    AddressSpaceEntry e;
    e.space = static_cast<AddressSpace>(r.u8());
    e.reason = static_cast<asa::UnknownReason>(r.u8());
    const uint32_t code_index = r.u32();
    e.reason_at = static_cast<ir::IrValueId>(r.u32());
    e.reason_code = code_index < codes.size() ? codes[code_index] : "";
    return e;
}

} // namespace

std::vector<uint8_t> serialize_address_spaces(const AddressSpaces &as) {
    /* Los codigos se recogen mientras se escriben las entradas y la tabla sale
     * DETRAS: no se sabe cuales hay hasta haberlas recorrido todas. */
    CodeTable codes;
    util::ByteWriter body;
    body.u32(static_cast<uint32_t>(as.by_value.size()));
    for (const AddressSpaceEntry &e : as.by_value)
        write_entry(body, e, codes.index_of(e.reason_code));

    util::ByteWriter w;
    write_analysis_header(w, kAddressSpaceAnalysisName, kAddressSpaceFormat);
    codes.write(w);
    const std::vector<uint8_t> body_bytes = body.take();
    if (!body_bytes.empty()) w.raw(body_bytes.data(), body_bytes.size());
    return w.take();
}

bool deserialize_address_spaces(const uint8_t *data, size_t n, size_t n_values,
                                AddressSpaces &out) {
    if (data == nullptr || n == 0) return false;
    util::ByteReader r(data, n);
    if (!read_analysis_header(r, kAddressSpaceAnalysisName,
                              kAddressSpaceFormat))
        return false;

    std::vector<const char *> codes;
    if (!CodeTable::read(r, codes)) return false;

    /* Se arma APARTE y solo se entrega al final: unos bytes cortados dejarian
     * al que pregunta una tabla a medias, o sea respuestas sobre memoria que
     * nadie ha resuelto. */
    AddressSpaces as;
    const uint32_t count = r.u32();
    if (!r.ok() || !fits_in(r, count, kEntryBytes)) return false;
    as.by_value.resize(count);
    for (uint32_t i = 0; i < count && r.ok(); ++i)
        as.by_value[i] = read_entry(r, codes);
    if (!r.ok()) return false;

    /* Coherencia con la funcion que se esta mirando, aparte de la clave del
     * almacen: servir la tabla de OTRA funcion no daria un error, daria
     * respuestas sobre huecos que no existen. */
    if (as.by_value.size() != n_values) return false;

    out = std::move(as);
    return true;
}

} // namespace analysis
