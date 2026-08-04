/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_vxdbg_codec.cpp
 * @brief Comprueba que los nodos van a bytes y vuelven intactos.
 *
 * Y sobre todo, la propiedad de la que depende el cache incremental: que la
 * identidad de un nodo SEA su serializacion.  Si dos nodos iguales dieran
 * huellas distintas, nada se reutilizaria nunca; si dos distintos dieran la
 * misma, el almacen serviria uno por otro.
 */

#include "vxdbg/codec.h"
#include "vxdbg/semantic.h"

#include <cstdio>
#include <string>
#include <vector>

static int fallos = 0;

/**
 * @brief Comprueba una condicion y deja constancia.
 * @param cond Lo que debe cumplirse.
 * @param que Descripcion.
 */
static void comprobar(bool cond, const char *que) {
    if (cond) {
        std::printf("  OK   %s\n", que);
    } else {
        std::printf("  FALLA %s\n", que);
        ++fallos;
    }
}

/// Un fichero cualquiera, para tener identificadores con los que trabajar.
static vxdbg::FileId un_fichero() {
    vxdbg::FileNode f;
    f.path = "lector.vx";
    f.language = "vesta";
    auto s = vxdbg::encode(f);
    return vxdbg::FileId{vxdbg::seal(s)};
}

/// La entidad del lenguaje: el nodo con mas cosas dentro.
static void probar_entidad() {
    std::printf("Entidad del lenguaje\n");

    const auto fid = un_fichero();

    vxdbg::LanguageEntity e;
    e.name = "parse";
    e.key = "std.io.Lector.parse";
    e.kind = vxdbg::EntityKind::Function;
    e.lang_kind = "method"; // lo pone el frontend; el formato no lo interpreta
    e.byte_size = 0;
    e.declared_at.file = fid;
    e.declared_at.begin_line = 31;
    e.declared_at.begin_column = 5;
    e.declared_at.end_line = 44;
    e.declared_at.end_column = 6;

    vxdbg::Relation rel;
    rel.kind = vxdbg::RelationKind::Derives;
    rel.target = vxdbg::LanguageEntityId{vxdbg::hash_bytes("Flujo", 5)};
    rel.lang_role = "base";
    e.relations.push_back(rel);

    vxdbg::Relation impl;
    impl.kind = vxdbg::RelationKind::Implements;
    impl.target = vxdbg::LanguageEntityId{vxdbg::hash_bytes("Cerrable", 8)};
    e.relations.push_back(impl);

    vxdbg::Attribute att;
    att.name = "vtable_slot";
    att.kind = vxdbg::AttributeKind::Integer;
    att.number = 7; // un numero, no un texto: por eso el atributo lleva genero
    e.attributes.push_back(att);

    auto s = vxdbg::encode(e);
    vxdbg::LanguageEntity v;
    comprobar(vxdbg::decode(s, v), "se lee lo que se escribio");
    comprobar(v.name == e.name && v.key == e.key,
              "conserva los nombres");
    comprobar(v.kind == vxdbg::EntityKind::Function, "conserva la especie");
    comprobar(v.lang_kind == "method",
              "y el genero que puso el frontend");
    comprobar(v.relations.size() == 2, "conserva las relaciones");
    comprobar(v.relations[0].kind == vxdbg::RelationKind::Derives &&
                  v.relations[0].target == rel.target &&
                  v.relations[0].lang_role == "base",
              "y cada una entera");
    comprobar(v.attributes.size() == 1 && v.attributes[0].number == 7 &&
                  v.attributes[0].kind == vxdbg::AttributeKind::Integer,
              "un atributo numerico sigue siendo numero");
    comprobar(v.declared_at.begin_line == 31 && v.declared_at.end_line == 44 &&
                  v.declared_at.file == fid,
              "conserva el tramo del fuente entero");

    // Un nodo pedido con otro genero no se interpreta: daria campos al azar.
    vxdbg::VariableNode otra;
    comprobar(!vxdbg::decode(s, otra),
              "leerlo como otro genero falla en vez de dar basura");
}

/// La propiedad que sostiene el cache incremental.
static void probar_identidad() {
    std::printf("Identidad\n");

    vxdbg::StatementNode a;
    a.lang_kind = "call";
    a.span.begin_line = 10;

    vxdbg::StatementNode b = a; // el mismo contenido

    auto sa = vxdbg::encode(a);
    auto sb = vxdbg::encode(b);
    const auto ha = vxdbg::seal(sa);
    const auto hb = vxdbg::seal(sb);
    comprobar(ha == hb, "dos nodos iguales dan la misma huella");

    // Cambiar CUALQUIER campo tiene que cambiar la huella.  Es lo que garantiza
    // que anadir un campo nuevo no deje el calculo desactualizado: la huella
    // sale de los bytes, no de una lista de campos que alguien mantiene.
    vxdbg::StatementNode c = a;
    c.span.begin_line = 11;
    auto sc = vxdbg::encode(c);
    comprobar(vxdbg::seal(sc) != ha, "cambiar la linea cambia la huella");

    vxdbg::StatementNode d = a;
    d.lang_kind = "assign";
    auto sd = vxdbg::encode(d);
    comprobar(vxdbg::seal(sd) != ha, "cambiar el genero cambia la huella");

    vxdbg::StatementNode e = a;
    e.scope = vxdbg::ScopeId{vxdbg::hash_bytes("otro", 4)};
    auto se = vxdbg::encode(e);
    comprobar(vxdbg::seal(se) != ha, "cambiar el ambito cambia la huella");
}

/// La bajada y el codigo generado.
static void probar_bajada_y_codigo() {
    std::printf("Bajada y codigo\n");

    vxdbg::LoweringMap m;
    vxdbg::LoweringEntry e1;
    e1.statement = vxdbg::StatementId{vxdbg::hash_bytes("s1", 2)};
    e1.kind = vxdbg::LoweringKind::Lowered;
    e1.ir_instrs.push_back(vxdbg::IrInstrId{vxdbg::hash_bytes("i1", 2)});
    e1.ir_instrs.push_back(vxdbg::IrInstrId{vxdbg::hash_bytes("i2", 2)});
    m.entries.push_back(e1);

    // Una sentencia que el optimizador borro: eso NO es informacion que falte.
    vxdbg::LoweringEntry e2;
    e2.statement = vxdbg::StatementId{vxdbg::hash_bytes("s2", 2)};
    e2.kind = vxdbg::LoweringKind::Eliminated;
    m.entries.push_back(e2);

    auto s = vxdbg::encode(m);
    vxdbg::LoweringMap v;
    comprobar(vxdbg::decode(s, v), "la bajada se lee");
    comprobar(v.entries.size() == 2, "con todas sus entradas");
    comprobar(v.entries[0].ir_instrs.size() == 2,
              "una sentencia puede dar varias instrucciones");
    comprobar(v.entries[1].kind == vxdbg::LoweringKind::Eliminated,
              "y se conserva que el optimizador la borro");
    comprobar(!v.statements_of(e1.ir_instrs[0]).empty(),
              "el indice inverso se reconstruye al leer");

    // Codigo: la relacion con el intermedio es de muchos a muchos.
    vxdbg::CodeDebug cd;
    cd.code = vxdbg::CodeId{vxdbg::hash_bytes("code", 4)};
    vxdbg::CodeRange rg;
    rg.begin = 0;
    rg.end = 12;
    rg.kind = vxdbg::RangeKind::Generated;
    rg.ir_instrs.push_back(vxdbg::IrInstrId{vxdbg::hash_bytes("cmp", 3)});
    rg.ir_instrs.push_back(vxdbg::IrInstrId{vxdbg::hash_bytes("jmp", 3)});
    cd.ranges.push_back(rg);
    vxdbg::CodeRange hueco;
    hueco.begin = 12;
    hueco.end = 12;
    hueco.kind = vxdbg::RangeKind::OptimizedAway;
    cd.ranges.push_back(hueco);

    auto sc = vxdbg::encode(cd);
    vxdbg::CodeDebug vc;
    comprobar(vxdbg::decode(sc, vc), "la correspondencia del codigo se lee");
    comprobar(vc.ranges.size() == 2 && vc.ranges[0].ir_instrs.size() == 2,
              "un tramo puede venir de dos instrucciones (muchos a muchos)");
    comprobar(vc.ranges[1].kind == vxdbg::RangeKind::OptimizedAway,
              "y un hueco sigue diciendo que el codigo no existe");
}

/// Las transferencias de control, incluidas las que no van a una funcion.
static void probar_transferencias() {
    std::printf("Transferencias\n");

    vxdbg::ExecutionEdge e;
    e.source = vxdbg::IrInstrId{vxdbg::hash_bytes("call", 4)};
    e.from = vxdbg::IrFunctionId{vxdbg::hash_bytes("main", 4)};
    e.kind = vxdbg::EdgeKind::Throw;
    e.to_kind = vxdbg::EndpointKind::Runtime; // lanzar no va a una funcion
    e.to_name = "runtime";
    e.dispatch = vxdbg::DispatchKind::Unknown;
    e.form = vxdbg::TransferForm::Normal;
    e.statements.push_back(vxdbg::StatementId{vxdbg::hash_bytes("s", 1)});

    auto s = vxdbg::encode(e);
    vxdbg::ExecutionEdge v;
    comprobar(vxdbg::decode(s, v), "una transferencia se lee");
    comprobar(v.kind == vxdbg::EdgeKind::Throw, "conserva por que pasa el control");
    comprobar(v.to_kind == vxdbg::EndpointKind::Runtime && v.to_name == "runtime",
              "y que el destino no era una funcion");

    // Los dos ejes son independientes: virtual y ademas incorporada.
    vxdbg::ExecutionEdge dv;
    dv.dispatch = vxdbg::DispatchKind::Virtual;
    dv.form = vxdbg::TransferForm::Inlined;
    auto sdv = vxdbg::encode(dv);
    vxdbg::ExecutionEdge vdv;
    comprobar(vxdbg::decode(sdv, vdv) &&
                  vdv.dispatch == vxdbg::DispatchKind::Virtual &&
                  vdv.form == vxdbg::TransferForm::Inlined,
              "virtual e incorporada a la vez, que es lo normal tras devirtualizar");
}

/// Guardar y recuperar pasando por el almacen.
static void probar_ida_y_vuelta_por_el_almacen() {
    std::printf("Ida y vuelta por el almacen\n");

    vxdbg::MemoryNodeStore store;

    vxdbg::LanguageEntity e;
    e.name = "Lector";
    e.kind = vxdbg::EntityKind::Type;
    e.lang_kind = "class";

    vxdbg::ContentHash h;
    comprobar(vxdbg::store_node(store, e, h), "se guarda");
    comprobar(!h.empty(), "con una huella de verdad");

    vxdbg::LanguageEntity v;
    comprobar(vxdbg::load_node(store, h, v), "se recupera");
    comprobar(v.name == "Lector" && v.lang_kind == "class", "intacto");
    comprobar(v.header.hash == h, "y sabiendo cual es su propia huella");

    // Guardar dos veces lo mismo: la propiedad que hace incremental al sistema.
    vxdbg::ContentHash h2;
    comprobar(vxdbg::store_node(store, e, h2) && h2 == h,
              "guardarlo otra vez da la misma huella");
    comprobar(store.size() == 1, "y no lo duplica");

    // Pedirlo como otro genero no cuela.
    vxdbg::StatementNode otro;
    comprobar(!vxdbg::load_node(store, h, otro),
              "pedirlo como otro genero falla");
}

/**
 * @brief El grafo semantico: claves fuera, identidades dentro.
 *
 * Que el orden lo decida el resolutor y no quien declara es lo que permite
 * anadir generos de declaracion sin revisar ningun orden, asi que se comprueba
 * dando los nodos AL REVES de como habria que emitirlos.
 */
static void grafo_semantico() {
    std::printf("Grafo semantico\n");

    vxdbg::MemoryNodeStore store;
    std::vector<vxdbg::SemanticNode> nodos;

    // El derivado PRIMERO: si el orden importara, esto no resolveria.
    vxdbg::SemanticNode derivado;
    derivado.key = "mod__Lector";
    derivado.name = "Lector";
    derivado.kind = vxdbg::EntityKind::Type;
    derivado.lang_kind = "class";
    {
        vxdbg::SemanticRelation r;
        r.kind = vxdbg::RelationKind::Derives;
        r.target = "mod__Flujo";
        derivado.relations.push_back(r);
        vxdbg::SemanticRelation f;
        f.kind = vxdbg::RelationKind::Uses;
        f.target = "no.existe";
        derivado.relations.push_back(f);
    }
    nodos.push_back(derivado);

    vxdbg::SemanticNode base;
    base.key = "mod__Flujo";
    base.name = "Flujo";
    base.kind = vxdbg::EntityKind::Type;
    base.lang_kind = "class";
    nodos.push_back(base);

    // Y uno con la clave de otro: no es un dato repetido, es una incoherencia.
    vxdbg::SemanticNode repetido;
    repetido.key = "mod__Flujo";
    repetido.name = "OtroFlujo";
    repetido.kind = vxdbg::EntityKind::Type;
    nodos.push_back(repetido);

    const auto rep = vxdbg::emit_semantic_graph(store, nodos);
    comprobar(rep.emitted == 2, "se emite un nodo por clave distinta");
    comprobar(rep.duplicates == 1, "y la clave repetida se cuenta, no se calla");
    comprobar(rep.unresolved == 1, "igual que la arista sin destino");

    // La base tiene que estar ANTES que el derivado: su identidad forma parte
    // de la de el, asi que no habria podido emitirse despues.
    size_t pos_base = 0, pos_der = 0;
    for (size_t i = 0; i < rep.ids.size(); ++i) {
        if (rep.ids[i].first == "mod__Flujo") pos_base = i;
        if (rep.ids[i].first == "mod__Lector") pos_der = i;
    }
    comprobar(pos_base < pos_der, "lo referenciado sale antes que quien lo usa");

    vxdbg::LanguageEntity leido;
    vxdbg::LanguageEntityId id_der, id_base;
    for (const auto &kv : rep.ids) {
        if (kv.first == "mod__Lector") id_der = kv.second;
        if (kv.first == "mod__Flujo") id_base = kv.second;
    }
    comprobar(vxdbg::load_node(store, id_der.hash, leido), "el derivado se lee");
    comprobar(leido.relations.size() == 1,
              "  con la arista buena y sin la rota");
    comprobar(leido.relations[0].target == id_base,
              "  apuntando de verdad a su base");
    comprobar(leido.name == "Lector" && leido.key == "mod__Lector",
              "  distinguiendo como se llama de como se le identifica");
}

int main() {
    std::printf("=== vxdbg: serializacion de los nodos ===\n");
    probar_entidad();
    probar_identidad();
    probar_bajada_y_codigo();
    probar_transferencias();
    probar_ida_y_vuelta_por_el_almacen();
    grafo_semantico();

    if (fallos == 0) {
        std::printf("=== todo correcto ===\n");
        return 0;
    }
    std::printf("=== %d comprobaciones fallidas ===\n", fallos);
    return 1;
}
