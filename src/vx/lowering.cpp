/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file lowering.cpp
 * @brief Implementacion del pase AST -> ir::IrModule de Vesta.
 */

#include "util/env_flags.h"
#include "vx/lowering.h"
#include "lowering/lowering_internal.h" // helpers que comparten las unidades del lowering
#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType

#include "loader/oop_types.h" // ADVICE_*: el orden de la cadena
#include <algorithm>
#include <chrono>
#include <iostream>
#include "ffi/virtual_lib_registry.h" // lookup_virtual_fn (bug 161: MC.23)
#include "vx/asm/asm_effects.h"       // inferencia de clobbers ( AS inc.4)
#include "vx/asm/asm_diag.h"      // diagnosticos estructurales del asm (ASA.2)
#include "vx/asm/asm_lift_emit.h" // lift de patrones atomicos a IR tipado (ASA.3)
#include "vx/asm/asm_lift_general.h" // lift general straight-line entero a IR real
#include "vx/asm/asm_lift_micro.h"
#include "vx/asm/asm_lift_registro.h"
#include "vx/asm/asm_phys_reg.h" // asm_body_subst_greedy // lift de asm opaco sin operandos -> ASM_MICRO
#include "vx/asm/instr_db.h"    // reschedule_asm (reoptimizador de asm, ASA)
#include "vx/asm/asm_backend.h" // validacion de sintaxis via Keystone (inc.4b)
#include "vx/collection_intrinsics.h"        // tabla de tipos coleccion
#include "vx/comptime/comptime_introspect.h" // helpers compartidos rama A
#include "vx/generics/concepts.h"      // conceptos como predicado -> CONST bool
#include "vx/generics/generic_clone.h" // clone_expr (custom print to_string)
#include "vx/lexer.h"                  // parse de fragments para @Macro
#include "vx/parser.h"                 // parse_one_expr para @Macro
#include "ir/ir_optimizer.h"           // register_pure_new_helper

#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>

/* El tamano en bytes de un tipo lo contesta el vocabulario unico
 * (ir/ir_type_info.h) -- aqui vivia una copia mas de esa tabla, y su comentario
 * explicaba por que: "mantener una copia es preferible a exponer el helper del
 * emisor para no acoplar el frontend al detalle de codegen".  El razonamiento
 * era bueno cuando la unica fuente era el emisor; el vocabulario no es codegen,
 * es el idioma del propio IR, asi que la razon de la copia ya no existe.
 *
 * El eje es el de ACCESO, que es lo que esta copia contestaba (un handle mide
 * sus 4 bytes de dato). */

namespace vx {
// =====================================================================
//  Mini-ensamblador de bloques `asm` ( AOT 16/32-bit).
//
//  Keystone (KS_OPT_SYNTAX_NASM) ensambla instrucciones + labels intra-
//  bloque + `db`, PERO no soporta `$`/`$$`/`times`.  Para permitir mezclar
//  codigo y datos crudos en UN solo bloque `asm` (como en NASM), partimos
//  el cuerpo por lineas: las INSTRUCCIONES se acumulan y se mandan a
//  Keystone (un flush por cada directiva de datos, para preservar el orden
//  y resolver labels dentro del run); las directivas de DATOS (db/dw/dd/dq/
//  times) las evaluamos nosotros con $ = offset actual del bloque y $$ = 0.
//
//  Limitacion: un label definido antes de una directiva de datos NO es
//  visible para instrucciones que vengan DESPUES (Keystone resuelve labels
//  dentro de un solo run).  Para boot sectors (codigo contiguo + padding +
//  firma al final) esto no es un problema.
// =====================================================================

/**
 * @brief Recorre un sub-arbol AST acumulando los nombres de variables
 *        que aparecen como destino de AssignExpr o operandos de ++/--.
 *
 * Lo necesita el lower_while() para construir los PHI nodes del bloque
 * header al estilo Braun.  No incluye las variables declaradas dentro
 * del propio sub-arbol (esas son locales al loop y no necesitan PHI);
 * el filtrado real se hace en el caller, que descarta cualquier nombre
 * que no exista en el scope justo antes del loop.
 *
 * @param n   Nodo a inspeccionar (puede ser nullptr).
 * @param out Set destino al que se añaden los nombres.
 */
void collect_assigned_vars(const ast::Node *n,
                                  std::set<std::string> &out) {
    if (!n) return;
    switch (n->kind) {
    case ast::NodeKind::AssignExpr: {
        auto *a = static_cast<const ast::AssignExpr *>(n);
        if (a->target && a->target->kind == ast::NodeKind::IdentExpr) {
            out.insert(
                static_cast<const ast::IdentExpr *>(a->target.get())->name);
        }
        collect_assigned_vars(a->value.get(), out);
        return;
    }
    case ast::NodeKind::UnaryExpr: {
        auto *u = static_cast<const ast::UnaryExpr *>(n);
        const bool mutates =
            (u->op == ast::UnOp::PreInc || u->op == ast::UnOp::PostInc ||
             u->op == ast::UnOp::PreDec || u->op == ast::UnOp::PostDec);
        if (mutates && u->operand &&
            u->operand->kind == ast::NodeKind::IdentExpr) {
            out.insert(
                static_cast<const ast::IdentExpr *>(u->operand.get())->name);
        }
        collect_assigned_vars(u->operand.get(), out);
        return;
    }
    case ast::NodeKind::BinaryExpr: {
        auto *b = static_cast<const ast::BinaryExpr *>(n);
        collect_assigned_vars(b->lhs.get(), out);
        collect_assigned_vars(b->rhs.get(), out);
        return;
    }
    case ast::NodeKind::CallExpr: {
        auto *c = static_cast<const ast::CallExpr *>(n);
        collect_assigned_vars(c->callee.get(), out);
        for (auto &a : c->args)
            collect_assigned_vars(a.get(), out);
        return;
    }
    case ast::NodeKind::BlockStmt: {
        auto *b = static_cast<const ast::BlockStmt *>(n);
        for (auto &s : b->body)
            collect_assigned_vars(s.get(), out);
        return;
    }
    case ast::NodeKind::ExprStmt: {
        auto *es = static_cast<const ast::ExprStmt *>(n);
        collect_assigned_vars(es->expr.get(), out);
        return;
    }
    case ast::NodeKind::IfStmt: {
        auto *is_ = static_cast<const ast::IfStmt *>(n);
        collect_assigned_vars(is_->cond.get(), out);
        collect_assigned_vars(is_->then_branch.get(), out);
        collect_assigned_vars(is_->else_branch.get(), out);
        return;
    }
    case ast::NodeKind::WhileStmt: {
        auto *w = static_cast<const ast::WhileStmt *>(n);
        collect_assigned_vars(w->cond.get(), out);
        collect_assigned_vars(w->body.get(), out);
        return;
    }
    case ast::NodeKind::DoWhileStmt: {
        auto *d = static_cast<const ast::DoWhileStmt *>(n);
        collect_assigned_vars(d->body.get(), out);
        collect_assigned_vars(d->cond.get(), out);
        return;
    }
    case ast::NodeKind::ForStmt: {
        auto *f = static_cast<const ast::ForStmt *>(n);
        collect_assigned_vars(f->init.get(), out);
        collect_assigned_vars(f->cond.get(), out);
        collect_assigned_vars(f->step.get(), out);
        collect_assigned_vars(f->body.get(), out);
        return;
    }
    case ast::NodeKind::ReturnStmt: {
        auto *r = static_cast<const ast::ReturnStmt *>(n);
        collect_assigned_vars(r->value.get(), out);
        return;
    }
    case ast::NodeKind::TryStmt: {
        auto *t = static_cast<const ast::TryStmt *>(n);
        collect_assigned_vars(t->body.get(), out);
        for (auto &cc : t->catches) {
            collect_assigned_vars(cc.body.get(), out);
        }
        collect_assigned_vars(t->finally_body.get(), out);
        return;
    }
    case ast::NodeKind::SynchronizedStmt: {
        auto *ss = static_cast<const ast::SynchronizedStmt *>(n);
        collect_assigned_vars(ss->target.get(), out);
        collect_assigned_vars(ss->body.get(), out);
        return;
    }
    case ast::NodeKind::ThrowStmt: {
        auto *ts = static_cast<const ast::ThrowStmt *>(n);
        collect_assigned_vars(ts->value.get(), out);
        return;
    }
    case ast::NodeKind::MatchExpr: {
        auto *me = static_cast<const ast::MatchExpr *>(n);
        collect_assigned_vars(me->scrutinee.get(), out);
        for (auto &arm : me->arms) {
            collect_assigned_vars(arm.body.get(), out);
        }
        return;
    }
    case ast::NodeKind::SpawnExpr: {
        auto *se = static_cast<const ast::SpawnExpr *>(n);
        collect_assigned_vars(se->body.get(), out);
        return;
    }
    case ast::NodeKind::RSpawnExpr: {
        auto *re = static_cast<const ast::RSpawnExpr *>(n);
        collect_assigned_vars(re->node_idx.get(), out);
        collect_assigned_vars(re->body.get(), out);
        return;
    }
    /* LabelStmt es solo un marker; el stmt siguiente vive en el
     * BlockStmt enclosing y se procesa por iteracion normal. */
    case ast::NodeKind::VarDeclStmt: {
        auto *v = static_cast<const ast::VarDeclStmt *>(n);
        // VarDeclStmt introduce una variable nueva: NO entra en el
        // set como mutacion (es definicion).  Pero su initializer
        // puede contener asignaciones a variables externas.
        collect_assigned_vars(v->init.get(), out);
        return;
    }
    default: return;
    }
}

// ---------------------------------------------------------------------
// Constructor.
// ---------------------------------------------------------------------

Lowering::Lowering(ast::ModuleNode &mod, const TypeChecker &tc,
                   Diagnostics &diags)
    : mod_(mod), tc_(tc), diags_(diags) {
    // Reservar capacidad razonable para el scope chain.  Programas
    // tipicos no suelen pasar de 5 scopes anidados.
    scopes_.reserve(8);
}

// ---------------------------------------------------------------------
// Helpers de tipo.
// ---------------------------------------------------------------------

ir::IrType Lowering::ir_type_from_primitive(PrimitiveKind p) noexcept {
    // Mapeo directo.  Se usa una tabla constexpr indexada por enum
    // (PrimitiveKind y IrType comparten posiciones logicas pero los
    // valores numericos no coinciden, asi que este switch es la
    // version mantenible).
    switch (p) {
    case PrimitiveKind::VOID: return ir::IrType::VOID;
    case PrimitiveKind::BOOL: return ir::IrType::BOOL;
    // CHAR no tiene contraparte directa en ir::IrType; mapeamos a U8
    // (mismo ancho, semanticamente equivalente porque el frontend
    // solo lo usa como entero pequenyo sin codepoint awareness).
    case PrimitiveKind::CHAR: return ir::IrType::U8;
    case PrimitiveKind::I8: return ir::IrType::I8;
    case PrimitiveKind::I16: return ir::IrType::I16;
    case PrimitiveKind::I32: return ir::IrType::I32;
    case PrimitiveKind::I64: return ir::IrType::I64;
    case PrimitiveKind::U8: return ir::IrType::U8;
    case PrimitiveKind::U16: return ir::IrType::U16;
    case PrimitiveKind::U32: return ir::IrType::U32;
    case PrimitiveKind::U64: return ir::IrType::U64;
    case PrimitiveKind::F32: return ir::IrType::F32;
    case PrimitiveKind::F64: return ir::IrType::F64;
    case PrimitiveKind::PTR: return ir::IrType::PTR;
    // Para STRUCT y ARRAY no hay un IrType directo; en el lowering
    // ambos se representan via su PUNTERO base (PTR).  Cuando un
    // caller pasa STRUCT/ARRAY a este helper esperando un IrType
    // unitario, devolvemos PTR como aproximacion mas razonable.
    case PrimitiveKind::STRUCT: return ir::IrType::PTR;
    case PrimitiveKind::ARRAY: return ir::IrType::PTR;
    // CLASS es reference type: la "variable" guarda un puntero a
    // ObjectHeader, asi que el IrType subyacente es PTR.
    case PrimitiveKind::CLASS: return ir::IrType::PTR;
    // Optional/Result builtins: la variable guarda un puntero
    // (PTR) al buffer en stack alocado por Some/Ok/etc.  16 bytes
    // para Optional, 24 para Result; el lowering emite ALLOCA.
    case PrimitiveKind::OPTIONAL: return ir::IrType::PTR;
    case PrimitiveKind::RESULT: return ir::IrType::PTR;
    // string es un GcHandle opaco i64.
    case PrimitiveKind::STRING: return ir::IrType::I64;
    // tipos primitivos de coleccion: i64 handle host pointer.
    // Cero overhead vs llamar el plugin directo (sin wrapping).
    case PrimitiveKind::ARRAYLIST: return ir::IrType::I64;
    case PrimitiveKind::HASHMAP: return ir::IrType::I64;
    case PrimitiveKind::HASHSET: return ir::IrType::I64;
    case PrimitiveKind::QUEUE: return ir::IrType::I64;
    case PrimitiveKind::DEQUE: return ir::IrType::I64;
    case PrimitiveKind::TREEMAP: return ir::IrType::I64;
    case PrimitiveKind::TREESET: return ir::IrType::I64;
    case PrimitiveKind::STACK: return ir::IrType::I64;
    // Smart pointers: slot stack con host_ptr (8 bytes).
    case PrimitiveKind::UNIQUE_PTR: return ir::IrType::PTR;
    case PrimitiveKind::SHARED_PTR: return ir::IrType::PTR;
    // Borrows: host_ptr de 8 bytes (zero overhead vs T* raw).
    case PrimitiveKind::BORROW: return ir::IrType::PTR;
    case PrimitiveKind::BORROW_MUT: return ir::IrType::PTR;
    // Future<T>: handle i64.
    case PrimitiveKind::FUTURE: return ir::IrType::I64;
    // FUNCTION: par (fn_addr, env_addr); usamos PTR como aproximacion.
    case PrimitiveKind::FUNCTION: return ir::IrType::PTR;
    case PrimitiveKind::COUNT: return ir::IrType::VOID;
    }
    return ir::IrType::VOID;
}

// ---------------------------------------------------------------------
// Run principal.
// ---------------------------------------------------------------------

// Tamano en bytes de un global array nativo @c T[N] (N constante en
// compile-time).  Devuelve 0 si @p tn no es un array de tamano fijo con
// elemento dimensionable (entonces no se le reserva storage estatico).
// Habilita buffers estaticos globales (p.ej. heap de un allocator bump en
// codigo bare-metal): @c u8[4096] g_heap; -> slot de 4096 bytes en .data.
/**
 * @brief Bits IEEE de @p d listos para grabarse en el slot de un global float.
 *
 * El ancho lo manda el TIPO DECLARADO, no el literal: los literales de coma
 * flotante se parsean como @c double, pero el slot de un @c f32 guarda un
 * binary32 y su LOAD lee 4 bytes.  Grabar ahi los bits de un double deja en
 * esos 4 bytes el resto de la mantisa (para 0.5 son ceros -> el global valia
 * 0).  El resultado va en los bytes bajos del qword del slot.
 *
 * @param d      Valor del literal (ya parseado como double).
 * @param is_f32 true si el global se declaro @c f32; false si @c f64.
 * @return Patron de bits a grabar en el slot.
 */
static uint64_t float_bits_for_global(double d, bool is_f32) {
    uint64_t bits = 0;
    if (is_f32) {
        const float f = static_cast<float>(d);
        uint32_t u32 = 0;
        std::memcpy(&u32, &f, sizeof(u32));
        bits = u32;
    } else {
        std::memcpy(&bits, &d, sizeof(d));
    }
    return bits;
}

static uint64_t vx_global_array_bytes(const ast::TypeNode *tn,
                                      const TypeChecker &tc) {
    if (!tn || tn->kind != ast::NodeKind::ArrayTypeNode) return 0;
    auto *at = static_cast<const ast::ArrayTypeNode *>(tn);
    if (!at->element_type || !at->size_expr)
        return 0; // T[] decay = sin storage
    if (at->size_expr->kind != ast::NodeKind::IntLitExpr) return 0;
    const uint64_t count =
        static_cast<const ast::IntLitExpr *>(at->size_expr.get())->value;
    uint64_t esz = 0;
    if (at->element_type->kind == ast::NodeKind::PrimitiveTypeNode)
        esz = primitive_size_bytes(
            static_cast<const ast::PrimitiveTypeNode *>(at->element_type.get())
                ->prim);
    else if (at->element_type->kind == ast::NodeKind::PointerTypeNode)
        esz = 8;
    else if (at->element_type->kind == ast::NodeKind::NamedTypeNode) {
        // Elemento newtype (typedef-new, p.ej. `uintptr[256]`): tamano del
        // primitivo subyacente (accesor const del type checker).
        const auto *nt =
            static_cast<const ast::NamedTypeNode *>(at->element_type.get());
        if (const Type *u = tc.newtype_underlying(nt->name))
            esz = primitive_size_bytes(u->kind);
        else {
            // Elemento `@overlay struct` (p.ej. `Foo[4] g_hs;`): el valor de
            // una vista ES un puntero de 8 bytes -> el array guarda N punteros.
            // Sin esto esz=0 y el global se quedaba SIN storage estatico.
            auto sit = tc.struct_layouts().find(nt->name);
            if (sit != tc.struct_layouts().end() && sit->second.is_overlay)
                esz = 8;
        }
    }
    if (esz == 0 || count == 0) return 0;
    return count * esz;
}

// Computa el intervalo DFS [lo,hi] de cada clase sobre el bosque de herencia
// (super_name).  El preorden numera lo; hi = max lo del subarbol.  Asi
// is-a(A,B) <=> B.lo <= A.lo <= B.hi (A es B o un descendiente de B).  Orden de
// hijos deterministico (alfabetico) para estabilidad cross-build.  Defensa
// contra ciclos via marca de visitados (el type checker ya rechaza ciclos de
// herencia, pero el DFS no debe colgarse si reaparece uno).
void Lowering::compute_type_intervals() {
    type_intervals_.clear();
    const auto &layouts = tc_.class_layouts();
    if (layouts.empty()) return;
    // children[S] = clases cuya super es S (S es a su vez una clase del
    // modulo).
    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> roots;
    for (const auto &kv : layouts) {
        const std::string &nm = kv.first;
        const std::string &sup = kv.second.super_name;
        if (!sup.empty() && layouts.count(sup))
            children[sup].push_back(nm);
        else
            roots.push_back(nm);
    }
    for (auto &kv : children)
        std::sort(kv.second.begin(), kv.second.end());
    std::sort(roots.begin(), roots.end());
    uint32_t counter = 0;
    std::unordered_set<std::string> visiting;
    std::function<uint32_t(const std::string &)> dfs =
        [&](const std::string &nm) -> uint32_t {
        const uint32_t lo = counter++;
        uint32_t hi = lo;
        if (visiting.insert(nm).second) {
            // defensa anti-ciclo
            auto it = children.find(nm);
            if (it != children.end())
                for (const std::string &ch : it->second)
                    hi = std::max(hi, dfs(ch));
        }
        type_intervals_[nm] = {lo, hi};
        return hi;
    };
    for (const std::string &r : roots)
        dfs(r);
}

// Forward-decl del chequeo de lowereabilidad de macros (definido mas abajo) +
// definicion del contexto force-lower, para que el pre-pase de run() los use.
static std::string macro_body_unsupported_reason(const TypeChecker &tc,
                                                 const ast::Stmt *s);

static thread_local std::unordered_set<std::string> *g_macro_force_lower =
    nullptr;
static thread_local std::unordered_set<std::string> *g_macro_visiting = nullptr;

void Lowering::register_fn_ret_info(const std::string &name, PrimitiveKind kind,
                                    const std::string &enum_struct_name,
                                    bool is_async) {
    ir::IrType rt =
        (kind == PrimitiveKind::VOID || kind == PrimitiveKind::COUNT)
            ? ir::IrType::VOID
            : ir_type_from_primitive(kind);

    // Enum de usuario: se modela como STRUCT cuyo struct_name esta en
    // enum_layouts_.  Es SRET (retbuf del tamano del layout).
    bool is_user_enum = false;
    if (kind == PrimitiveKind::STRUCT && !enum_struct_name.empty()) {
        const auto &elays = tc_.enum_layouts();
        is_user_enum = (elays.find(enum_struct_name) != elays.end());
    }
    // (gap O): funciones que devuelven FUNCTION.  Mismo patron que los
    // enums: el tipo IR pasa a VOID y el caller pasa un retbuf hidden de
    // 16 bytes (slot del function value).
    const bool is_function_ret = (kind == PrimitiveKind::FUNCTION);
    if (is_function_ret) fn_returns_function_.insert(name);
    // Smart pointers: SRET para `unique<T>` / `shared<T>`.  Sin esto,
    // devolver un smart pointer seria inseguro (su slot vive en el stack
    // del callee y muere al RET).  Con SRET el caller aloca el slot y el
    // callee copia los bytes ahi.
    const bool is_smartptr_ret = (kind == PrimitiveKind::UNIQUE_PTR ||
                                  kind == PrimitiveKind::SHARED_PTR);
    if (is_smartptr_ret) fn_returns_smartptr_.insert(name);
    // Vesta Embed (native_poo_): `string` es value-type de 24 bytes ->
    // retorno por valor via SRET (igual que un struct).  Solo en native;
    // en Full/JIT `string` es un handle i64.
    const bool is_str_value_ret =
        (native_poo_ && kind == PrimitiveKind::STRING);
    if (is_str_value_ret) fn_returns_str_value_.insert(name);
    // STRUCT por valor: MISMO motivo que el smart pointer de arriba -- el
    // buffer del struct vive en el frame del callee y muere al RET.  Era el
    // unico agregado sin SRET: devolvia el puntero a esa memoria muerta y
    // funcionaba solo si el caller la copiaba antes de tocar la pila.
    // Los enums (STRUCT con enum_layout) ya salen por `is_user_enum`, y un
    // `@overlay struct` es un puntero de 8 bytes -> por registro, correcto.
    bool is_struct_ret = false;
    if (kind == PrimitiveKind::STRUCT && !is_user_enum &&
        !enum_struct_name.empty()) {
        const auto &slays = tc_.struct_layouts();
        auto it_s = slays.find(enum_struct_name);
        if (it_s != slays.end() && !it_s->second.is_overlay) {
            is_struct_ret = true;
            fn_ret_struct_name_[name] = enum_struct_name;
        }
    }

    // sret: estas funciones tienen ret_type IR = VOID y un retbuf hidden
    // como primer param.  Sin este ajuste, fn_return_types_ apuntaria a PTR
    // y los callers crearian un dst SSA "huerfano" que el emisor intentaria
    // escribir desde la salida (que no existe).
    if (kind == PrimitiveKind::OPTIONAL || kind == PrimitiveKind::RESULT ||
        is_user_enum || is_function_ret || is_smartptr_ret ||
        is_str_value_ret || is_struct_ret) {
        rt = ir::IrType::VOID;
    }
    // Item 9: @Async wrapper retorna i64 (Future handle), no T.  El tipo
    // logico T se preserva como Future<T> en el sig del type checker para
    // el `await fut`, pero el bytecode del wrapper devuelve i64 raw en R0.
    // Sin este ajuste, el lowering del call site marca dst con tipo T (e.g.
    // f64), emite un cast i64->f64 erroneo (FTOI cambia el valor), y await
    // opera sobre un handle corrupto.
    if (is_async) {
        rt = ir::IrType::I64;
        kind = PrimitiveKind::I64;
    }

    fn_return_types_[name] = rt;
    fn_ret_kind_[name] = kind;
    // Guardamos el nombre del enum para que el caller pueda buscar su
    // size_bytes al alocar el retbuf.
    if (is_user_enum) fn_ret_enum_name_[name] = enum_struct_name;
}

bool Lowering::run(ir::IrModule &out_module, const std::string &module_name) {
    /* Reparto del coste de la bajada.  Es la fase mas cara del frontend en un
     * fichero de un solo modulo -- 1,0 s de los 1,2 que costaba uno de 5.700
     * lineas -- y hasta ahora se publicaba como un solo numero, que no dice si
     * el trabajo esta en las funciones o en lo que se prepara antes. */
    const bool medir_bajada = util::flag_on(util::FlagId::Times);
    using RelojBajada = std::chrono::steady_clock;
    const auto marca_run = RelojBajada::now();
    long us_previo = 0, n_bajadas = 0;

    const size_t initial_errors = diags_.error_count();
    out_module.name = module_name;
    out_module.format = "velb";
    // Guardar puntero al modulo de salida para que los lowering de
    // expresiones (StringLitExpr, builtins FFI) puedan registrar
    // datos estaticos y imports nativos sin pasar el modulo en cada
    // signature.
    out_mod_ = &out_module;

    // AOT: precomputar los intervalos de tipo (encoding nested-set) para el
    // type matching de catch.  Barato y necesario antes de bajar cualquier
    // try/throw.
    if (native_poo_) compute_type_intervals();

    /* La cadena de aspectos de cada metodo, EN ORDEN.  Se recoge ANTES de bajar
     * ningun cuerpo por dos motivos: cada sitio de llamada la consulta para
     * decidir si puede especular, y el `proceed()` de un `@Around` necesita
     * saber a que llama, que depende de su posicion en la cadena.
     *
     * El orden es el de declaracion, que es el mismo en que `__module_init`
     * llama a `addadvice` y por tanto el que tendra la cadena en ejecucion. */
    for (auto &decl : mod_.decls) {
        if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
        auto *cd_asp = static_cast<ast::ClassDecl *>(decl.get());
        for (auto &m_uptr : cd_asp->methods) {
            auto *m = m_uptr.get();
            if (!m || m->advice_kind == 0) continue;
            const std::string &t = m->advice_target;
            const size_t p = t.find('.');
            /* Un pointcut mal formado lo diagnostica la emision del advice;
             * aqui basta con NO poder atribuirlo, que es lo prudente. */
            if (p == std::string::npos || p == 0 || p + 1 >= t.size()) {
                all_advices_attributed_ = false;
                continue;
            }
            const std::string target = t.substr(0, p) + "__" + t.substr(p + 1);
            ir::IrModule::ChainedAdvice entry;
            entry.kind = static_cast<uint8_t>(m->advice_kind - 1);
            entry.method_ir_name = cd_asp->name + "__" + m->name;
            advice_chains_[target].push_back(std::move(entry));
        }
    }
    /* A que llama el `proceed()` de cada `@Around`.
     *
     * Un `@Around` envuelve al SIGUIENTE de su cadena, y el ultimo al metodo.
     * Como un advice tiene un solo objetivo -- el pointcut es `Clase.metodo`
     * exacto -- su `proceed` tiene UN destino, conocido aqui.  Eso es lo que
     * permite llamarlo directo en vez de por el marco. */
    for (const auto &kv : advice_chains_) {
        const std::string *prev = nullptr;
        for (const auto &a : kv.second) {
            if (a.kind != loader::ADVICE_AROUND) continue;
            if (prev != nullptr) proceed_target_[*prev] = a.method_ir_name;
            prev = &a.method_ir_name;
        }
        /* El mas interno llama al metodo. */
        if (prev != nullptr) proceed_target_[*prev] = kv.first;
    }
    out_module.advice_chains = advice_chains_;
    out_module.all_advices_attributed = all_advices_attributed_;

    // AOT / Embed (native_poo_): el AOP (@Aspect + advice) se registra en
    // RUNTIME (MethodInfo::advice_chain via addadvice en __module_init), que
    // native_poo NO emite.  Sin esto, el advice se ignoraria SILENCIOSAMENTE y
    // los metodos correrian sin sus before/after/around -> resultado erroneo
    // (16_aop daba 1 en vez de 99).  Rechazar en compile-time es lo correcto:
    // un fallo ruidoso es mejor que un resultado incorrecto.
    if (native_poo_) {
        for (const auto &kv : tc_.class_layouts()) {
            if (kv.second.is_aspect) {
                error_at(SourceLoc{},
                         "AOP (@Aspect '" + kv.first +
                             "') no soportado en compilacion nativa "
                             "(--target bare/embed): el advice se registra en "
                             "runtime y se ignoraria. Usa --target full o "
                             "elimina los aspectos.");
                return false;
            }
        }
    }

    // Inferir el fichero fuente del primer AST node con loc.file no
    // vacio.  Esto se usa en warnings emitidos por @c cast_if_needed
    // que solo recibe @c source_line.  Sin esta inferencia, los
    // warnings se imprimirian sin nombre de fichero.
    for (auto &d : mod_.decls) {
        if (!d) continue;
        if (!d->loc.file.empty()) {
            current_file_ = d->loc.file;
            break;
        }
    }

    // EMITIR PRIMERO los IntrospectInfo chunks
    // y poblar @c introspect_idx_by_name_ ANTES de bajar funciones,
    // para que @c find_type("Literal") pueda resolver el indice del
    // chunk en compile-time durante el lowering de main / otras
    // funciones.  Los layouts ya estan calculados por el type checker.
    emit_introspect_info_chunks();

    // Pase 1: registrar el tipo de retorno de cada funcion para validar
    // las llamadas.  Esto en un programa real ya esta en el type checker,
    // pero lo replicamos aqui para no acoplar la API.
    //
    // Adicionalmente registramos el PrimitiveKind semantico (OPTIONAL/
    // RESULT/...) en @c fn_ret_kind_ para que @c lower_call detecte
    // las funciones sret y aloque el retbuf en el caller.
    for (auto &decl : mod_.decls) {
        if (!decl) continue;
        if (decl->kind == ast::NodeKind::FunctionDecl) {
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            PrimitiveKind kind = PrimitiveKind::VOID;
            if (fd->return_type &&
                fd->return_type->kind == ast::NodeKind::PrimitiveTypeNode &&
                static_cast<ast::PrimitiveTypeNode *>(fd->return_type.get())
                        ->prim != PrimitiveKind::GC_PTR) {
                auto *pt = static_cast<ast::PrimitiveTypeNode *>(
                    fd->return_type.get());
                kind = pt->prim;
            } else if (fd->return_type) {
                // NOTA gc<T>: `gc<unique<i64>>` es un PrimitiveTypeNode(GC_PTR)
                // con type_args, pero su tipo REAL de retorno es el inner T
                // (UNIQUE_PTR/SHARED_PTR/CLASS/...) con gc_managed=true -- ver
                // TypeChecker::type_from_node.  Debemos resolverlo por
                // `resolve_type_node` (NO quedarnos en GC_PTR) para que la
                // deteccion de SRET del CALLER (fn_returns_smartptr_, etc.)
                // coincida con la del CALLEE (lower_function, que usa
                // resolve_type_node).  Sin esto, una fn que devuelve
                // gc<unique<T>>/gc<shared<T>> es SRET en el callee (retbuf)
                // pero el caller no pasa retbuf -> escritura a puntero basura
                // (SEGV en AOT, bug 248).  Por eso GC_PTR se excluye de la
                // rama PrimitiveTypeNode de arriba y cae aqui.
                // Para tipos no-primitivos (NamedTypeNode con CLASS,
                // Optional<T>, Result<V,E>, ARRAY, PTR, alias), usar
                // el tipo semantico resuelto.  Sin esto, las llamadas
                // a funciones que devuelven Result/Optional pierden
                // su PTR de retorno y la asignacion al var-decl falla.
                const Type sem = tc_.resolve_type_node(fd->return_type.get());
                if (sem.kind != PrimitiveKind::COUNT &&
                    sem.kind != PrimitiveKind::VOID) {
                    kind = sem.kind;
                }
            }
            // El nombre del tipo cuando el retorno es STRUCT: sirve para
            // distinguir un enum de usuario (SRET) de un struct normal.
            std::string enum_struct_name;
            if (kind == PrimitiveKind::STRUCT && fd->return_type) {
                const Type sem_check =
                    tc_.resolve_type_node(fd->return_type.get());
                if (sem_check.kind == PrimitiveKind::STRUCT)
                    enum_struct_name = sem_check.struct_name;
            }
            // El registro (incluida la decision de SRET) vive en un unico
            // helper compartido con las funciones importadas -- ver
            // register_fn_ret_info.
            register_fn_ret_info(fd->name, kind, enum_struct_name,
                                 fd->is_async);
        } else if (decl->kind == ast::NodeKind::ExternFnDecl) {
            // FFI declarativo: registrar tipo de retorno y
            // mapeo nombre -> libreria nativa para que @c lower_call
            // emita CALLN @Method("<lib>:<name>") en vez de CALLVM.
            auto *efd = static_cast<ast::ExternFnDecl *>(decl.get());
            ir::IrType rt = ir::IrType::VOID;
            if (efd->return_type &&
                efd->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
                auto *pt = static_cast<ast::PrimitiveTypeNode *>(
                    efd->return_type.get());
                if (pt->prim != PrimitiveKind::VOID) {
                    rt = ir_type_from_primitive(pt->prim);
                }
            } else if (efd->return_type) {
                const Type sem = tc_.resolve_type_node(efd->return_type.get());
                if (sem.kind != PrimitiveKind::COUNT &&
                    sem.kind != PrimitiveKind::VOID) {
                    rt = ir_type_from_primitive(sem.kind);
                }
            }
            fn_return_types_[efd->name] = rt;
            extern_lib_by_fn_name_[efd->name] = efd->lib;
        }
    }

    // Pase 1b: registrar el retorno de las funciones IMPORTADAS de otro
    // modulo.  No estan en @c mod_.decls (el modulo actual solo ve su propio
    // AST); llegan como @c FunctionSig inyectada desde el .vxi, con su Type
    // ya reconstruido por @c resolve_type_string.  Sin este pase el caller
    // no sabria que una fn cross-modulo devuelve Optional/Result/enum/... y
    // omitiria el retbuf hidden de la convencion SRET: el callee escribiria
    // en lo que hubiera en el registro del primer argumento (el primer arg
    // real) -> escritura a puntero basura -> SEGV.  Se usa el MISMO helper
    // que las locales, asi que caller y callee no pueden divergir.
    for (const auto &kv : tc_.function_sigs_by_name()) {
        const std::string &fname = kv.first;
        // Las locales (y las extern declaradas aqui) ya estan registradas
        // arriba con su AST, que es la fuente mas precisa.
        if (fn_return_types_.find(fname) != fn_return_types_.end()) continue;
        const FunctionSig *sig = tc_.function_sig_by_name(fname);
        if (!sig) continue;
        // FFI nativo: convencion CALLN propia (valor en R0), nunca SRET.
        if (!sig->extern_lib.empty()) continue;
        register_fn_ret_info(fname, sig->return_type.kind,
                             sig->return_type.struct_name,
                             /*is_async=*/false);
    }

    // Pase 2: bajar cada funcion.
    //
    // ORDEN IMPORTANTE: el emisor IR (ir_emitter.cpp::ir_emit_module)
    // marca como "entry point" la PRIMERA funcion del modulo, lo que
    // hace que esa funcion termine con 'hlt' (detiene la VM) en lugar
    // de 'ret'.  Por tanto si dejamos las funciones en el orden en que
    // aparecen en el .vx, una funcion como 'factorial' que se declara
    // antes de 'main' acabaria como entry point y la primera llamada
    // recursiva detendria la VM.  Solucion: bajamos 'main' primero
    // (si existe), luego el resto en orden de declaracion.
    ast::FunctionDecl *main_decl = nullptr;
    for (auto &decl : mod_.decls) {
        if (decl && decl->kind == ast::NodeKind::FunctionDecl) {
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            if (fd->name == "main") {
                main_decl = fd;
                break;
            }
        }
    }
    // L2.2: pre-scan global runtime vars y reservar slots ANTES de
    // bajar main.  Sin esto, lower_ident("g") en main encuentra
    // runtime_global_slots_ vacio y emite "nombre no resuelto".
    for (auto &decl : mod_.decls) {
        if (!decl || decl->kind != ast::NodeKind::GlobalVarDecl) continue;
        auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
        if (gv->is_const || gv->is_comptime) continue;
        // thread_local: almacenamiento por-hilo (TLS NATIVO).  Su plantilla va
        // a una seccion SHF_TLS (.tdata) con SD_FLAG_TLS; el acceso usa el
        // thread pointer (fs/gs + TPOFF) que emite el codegen AOT.  El init
        // debe ser una constante (literal entero o ausente = 0): es la
        // plantilla estatica que el cargador copia por-hilo, no un store en
        // __module_init.
        if (gv->is_thread_local) {
            uint64_t nbytes = 8;
            uint16_t talign = 8;
            PrimitiveKind prim_kind = PrimitiveKind::I64;
            if (gv->type &&
                gv->type->kind == ast::NodeKind::PrimitiveTypeNode) {
                auto *pt =
                    static_cast<ast::PrimitiveTypeNode *>(gv->type.get());
                prim_kind = pt->prim;
                nbytes = primitive_size_bytes(pt->prim);
                if (nbytes == 0) nbytes = 8;
                talign = static_cast<uint16_t>(nbytes);
            }
            const bool is_f64 = (prim_kind == PrimitiveKind::F64);
            const bool is_f32 = (prim_kind == PrimitiveKind::F32);
            // Valor inicial: constante (literal entero/float/bool/char, negado,
            // o una referencia a un `comptime` const).  Es la plantilla
            // estatica que el cargador copia por-hilo.
            uint64_t init_val = 0;
            bool init_ok = true;
            if (gv->init) {
                const ast::Expr *ie = gv->init.get();
                int64_t sign = 1;
                if (ie->kind == ast::NodeKind::UnaryExpr) {
                    auto *u = static_cast<const ast::UnaryExpr *>(ie);
                    if (u->op == ast::UnOp::Neg && u->operand &&
                        (u->operand->kind == ast::NodeKind::IntLitExpr ||
                         u->operand->kind == ast::NodeKind::FloatLitExpr)) {
                        sign = -1;
                        ie = u->operand.get();
                    }
                }
                if (ie->kind == ast::NodeKind::FloatLitExpr) {
                    // Empaquetar los bits IEEE 754 (f64 o f32) de la plantilla.
                    double d =
                        sign *
                        static_cast<const ast::FloatLitExpr *>(ie)->value;
                    if (is_f32) {
                        float f = static_cast<float>(d);
                        uint32_t u32;
                        std::memcpy(&u32, &f, 4);
                        init_val = u32;
                    } else {
                        std::memcpy(&init_val, &d, 8);
                    }
                } else if (ie->kind == ast::NodeKind::IntLitExpr) {
                    int64_t iv =
                        sign *
                        static_cast<int64_t>(
                            static_cast<const ast::IntLitExpr *>(ie)->value);
                    // i64-literal en un thread_local float -> convertir a IEEE.
                    if (is_f64) {
                        double d = static_cast<double>(iv);
                        std::memcpy(&init_val, &d, 8);
                    } else if (is_f32) {
                        float f = static_cast<float>(iv);
                        uint32_t u32;
                        std::memcpy(&u32, &f, 4);
                        init_val = u32;
                    } else {
                        init_val = static_cast<uint64_t>(iv);
                    }
                } else if (ie->kind == ast::NodeKind::BoolLitExpr) {
                    init_val = static_cast<const ast::BoolLitExpr *>(ie)->value
                                   ? 1
                                   : 0;
                } else if (ie->kind == ast::NodeKind::CharLitExpr) {
                    init_val = static_cast<uint64_t>(
                        static_cast<const ast::CharLitExpr *>(ie)->codepoint);
                } else if (ie->kind == ast::NodeKind::IdentExpr) {
                    // Referencia a un `comptime` const entero -> su valor.
                    const auto &cgv = tc_.comptime_const_values();
                    auto cit =
                        cgv.find(static_cast<const ast::IdentExpr *>(ie)->name);
                    if (cit != cgv.end() && !cit->second.is_str &&
                        !cit->second.is_struct) {
                        int64_t cv = sign * cit->second.value;
                        if (is_f64) {
                            double d = static_cast<double>(cv);
                            std::memcpy(&init_val, &d, 8);
                        } else if (is_f32) {
                            float f = static_cast<float>(cv);
                            uint32_t u32;
                            std::memcpy(&u32, &f, 4);
                            init_val = u32;
                        } else {
                            init_val = static_cast<uint64_t>(cv);
                        }
                    } else {
                        init_ok = false;
                    }
                } else {
                    init_ok = false;
                }
            }
            if (!init_ok) {
                diags_.error(
                    gv->loc,
                    "thread_local '" + gv->name +
                        "': el inicializador debe ser una constante (literal "
                        "entero/float/bool/char, o un `comptime` const)");
                continue;
            }
            const uint64_t tls_slot = get_or_create_tls_global_slot(
                gv->name, nbytes, init_val, talign);
            // Init != 0: registrar para el TLS callback __vx_tls_init (la
            // plantilla a cero no necesita store -- el bloque ya esta a cero).
            if (init_val != 0)
                tls_nonzero_inits_.push_back({tls_slot, init_val});
            continue;
        }
        // Global de tipo STRUCT: reservar un slot de `size_bytes`, igual que un
        // array.  Sin esto no habia storage y cualquier uso daba "nombre no
        // resuelto" -- un struct simplemente no podia ser global, aunque un
        // array de structs si.  El caso natural (un contador compartido, una
        // config, un registro de estado) es justo una global.
        //
        // Un `@overlay struct` NO entra: su valor runtime es un puntero de 8
        // bytes y lo cubre la rama de primitivos de abajo (lo trata como PTR).
        if (gv->type && !gv->is_const && !gv->is_comptime &&
            gv->type->kind == ast::NodeKind::NamedTypeNode) {
            const Type gt = tc_.resolve_type_node(gv->type.get());
            if (gt.kind == PrimitiveKind::STRUCT && !gt.struct_name.empty()) {
                auto sit = tc_.struct_layouts().find(gt.struct_name);
                if (sit != tc_.struct_layouts().end() &&
                    !sit->second.is_overlay && sit->second.size_bytes > 0) {
                    (void)get_or_create_runtime_global_slot(
                        gv->name, (uint64_t)sit->second.size_bytes);
                    continue;
                }
            }
        }
        // Global array nativo T[N]: reservar slot de N*sizeof(T) bytes.
        if (gv->type && gv->type->kind == ast::NodeKind::ArrayTypeNode) {
            const uint64_t ab = vx_global_array_bytes(gv->type.get(), tc_);
            if (ab > 0) {
                const uint64_t slot =
                    get_or_create_runtime_global_slot(gv->name, ab);
                // Init-list constante `= {e0, e1, ...}`: grabar los bytes
                // directamente en el slot .data (en AOT no corre
                // __module_init).  Solo elementos enteros constantes.
                auto *at = static_cast<ast::ArrayTypeNode *>(gv->type.get());
                uint64_t esz = 8;
                if (at->element_type &&
                    at->element_type->kind == ast::NodeKind::PrimitiveTypeNode)
                    esz = primitive_size_bytes(
                        static_cast<ast::PrimitiveTypeNode *>(
                            at->element_type.get())
                            ->prim);
                if (gv->init && gv->init->kind == ast::NodeKind::InitListExpr &&
                    slot < out_mod_->static_data.entries.size() && esz > 0) {
                    auto *il = static_cast<ast::InitListExpr *>(gv->init.get());
                    const uint32_t base_off =
                        out_mod_->static_data.entries[slot].byte_offset;
                    for (size_t ei = 0; ei < il->elements.size(); ++ei) {
                        uint64_t cval = 0;
                        const ast::Expr *ie = il->elements[ei].get();
                        bool have = false;
                        if (ie && ie->kind == ast::NodeKind::IntLitExpr) {
                            cval =
                                static_cast<const ast::IntLitExpr *>(ie)->value;
                            have = true;
                        } else if (ie && ie->kind == ast::NodeKind::UnaryExpr) {
                            auto *u = static_cast<const ast::UnaryExpr *>(ie);
                            if (u->op == ast::UnOp::Neg && u->operand &&
                                u->operand->kind == ast::NodeKind::IntLitExpr) {
                                cval = (uint64_t)(-(int64_t)static_cast<
                                                       const ast::IntLitExpr *>(
                                                       u->operand.get())
                                                       ->value);
                                have = true;
                            }
                        } else if (ie &&
                                   ie->kind == ast::NodeKind::CharLitExpr) {
                            cval = static_cast<const ast::CharLitExpr *>(ie)
                                       ->codepoint;
                            have = true;
                        } else if (ie &&
                                   ie->kind == ast::NodeKind::BoolLitExpr) {
                            cval =
                                static_cast<const ast::BoolLitExpr *>(ie)->value
                                    ? 1u
                                    : 0u;
                            have = true;
                        }
                        if (!have) continue;
                        const uint64_t eoff = base_off + ei * esz;
                        for (uint64_t k = 0; k < esz; ++k)
                            out_mod_->static_data.bytes[eoff + k] =
                                (uint8_t)((cval >> (8 * k)) & 0xFF);
                    }
                }
            }
            continue;
        }
        // Tipo primitivo directo O newtype (typedef-new) que resuelve a un
        // primitivo (p.ej. `uintptr` -> u64): en ambos casos pre-creamos el
        // slot del global para que TODAS las funciones (no solo la que lo
        // escribe primero) resuelvan su lectura/escritura al mismo slot.
        // Sin esto, un global de tipo std.types leido/escrito desde otra
        // funcion daba "nombre no resuelto" o leia 0.
        if (!gv->type || (gv->type->kind != ast::NodeKind::PrimitiveTypeNode &&
                          gv->type->kind != ast::NodeKind::NamedTypeNode))
            continue;
        PrimitiveKind pt_prim =
            (gv->type->kind == ast::NodeKind::PrimitiveTypeNode)
                ? static_cast<ast::PrimitiveTypeNode *>(gv->type.get())->prim
                : tc_.resolve_type_node(gv->type.get()).kind;
        // Un global de tipo overlay (`@overlay struct`) tiene como VALOR
        // runtime un puntero al bloque host (8 bytes) -> darle slot como un
        // PTR.
        if (pt_prim == PrimitiveKind::STRUCT &&
            gv->type->kind == ast::NodeKind::NamedTypeNode) {
            Type rt = tc_.resolve_type_node(gv->type.get());
            auto sit = tc_.struct_layouts().find(rt.struct_name);
            if (sit != tc_.struct_layouts().end() && sit->second.is_overlay)
                pt_prim = PrimitiveKind::PTR;
        }
        switch (pt_prim) {
        case PrimitiveKind::STRING:
        case PrimitiveKind::I8:
        case PrimitiveKind::I16:
        case PrimitiveKind::I32:
        case PrimitiveKind::I64:
        case PrimitiveKind::U8:
        case PrimitiveKind::U16:
        case PrimitiveKind::U32:
        case PrimitiveKind::U64:
        case PrimitiveKind::F32:
        case PrimitiveKind::F64:
        case PrimitiveKind::BOOL:
        case PrimitiveKind::CHAR:
        case PrimitiveKind::PTR:
            (void)get_or_create_runtime_global_slot(gv->name);
            break;
        default: break;
        }
    }
    // Globals IMPORTADOS de otro modulo: mismo pre-pase.  Tiene que ser AQUI y
    // no al primer uso, porque el prologo de `main` decide si llama a
    // `__module_init` mirando si hay algun slot -- y un modulo que solo USA
    // globals de sus deps no tendria ninguno todavia, asi que el init no
    // correria y el global se leeria a cero.  Como el merge los unifica con los
    // del dep por `shared_key`, pre-crearlos no cuesta storage.
    for (const auto &kv : tc_.imported_global_storage())
        (void)ensure_imported_global_slot(kv.first);
    // Los que se usan cualificados (`lib.counter`) no estan en esa tabla: viven
    // en el namespace importado.  Mismo criterio (kind=1 = variable/constante,
    // sin valor inlineable, y no un string que se materializa desde su blob).
    //
    // OJO: la tabla de namespaces incluye tambien los DECLARADOS en este mismo
    // modulo (`namespace app;` registra sus propios simbolos para el acceso
    // cualificado).  Esos son locales: su storage ya lo decidio el bucle de
    // arriba, con el tipo delante -- y hay tipos que NO llevan slot (un global
    // de tipo funcion se resuelve como closure).  Darles uno aqui los
    // desviaria a la ruta de global plano y romperia su uso.
    for (auto &decl : mod_.decls) {
        if (decl && decl->kind == ast::NodeKind::GlobalVarDecl)
            local_global_names_.insert(
                static_cast<ast::GlobalVarDecl *>(decl.get())->name);
    }
    for (const auto &ns : tc_.imported_namespaces()) {
        for (const auto &sym : ns.symbols) {
            if (sym.kind != 1 || sym.has_const_value ||
                sym.mangled_label.empty())
                continue;
            if (sym.var_type.kind == PrimitiveKind::STRING) continue;
            if (local_global_names_.count(sym.mangled_label) != 0) continue;
            (void)shared_global_slot_for(sym.mangled_label, sym.var_type);
        }
    }
    // AOT (native_poo_): los campos estaticos de clase se mapean a globales
    // planos (slot __static_<Clase>_<campo>).  Pre-grabamos su inicializador
    // constante en los bytes del slot (no hay __module_init en bare).  Las
    // rutas de lectura/escritura usan el mismo slot via get_or_create.
    if (native_poo_) {
        for (auto &decl : mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            for (const auto &fld : cd->fields) {
                if (!fld.is_static) continue;
                const uint64_t slot = get_or_create_runtime_global_slot(
                    "__static_" + cd->name + "_" + fld.name, 8);
                if (!fld.init) continue;
                uint64_t cval = 0;
                bool have = false;
                const ast::Expr *ie = fld.init.get();
                if (ie->kind == ast::NodeKind::IntLitExpr) {
                    cval = static_cast<const ast::IntLitExpr *>(ie)->value;
                    have = true;
                } else if (ie->kind == ast::NodeKind::BoolLitExpr) {
                    cval = static_cast<const ast::BoolLitExpr *>(ie)->value
                               ? 1u
                               : 0u;
                    have = true;
                } else if (ie->kind == ast::NodeKind::CharLitExpr) {
                    cval = static_cast<const ast::CharLitExpr *>(ie)->codepoint;
                    have = true;
                } else if (ie->kind == ast::NodeKind::UnaryExpr) {
                    auto *u = static_cast<const ast::UnaryExpr *>(ie);
                    if (u->op == ast::UnOp::Neg && u->operand &&
                        u->operand->kind == ast::NodeKind::IntLitExpr) {
                        cval = (uint64_t)(-(int64_t)static_cast<
                                               const ast::IntLitExpr *>(
                                               u->operand.get())
                                               ->value);
                        have = true;
                    }
                }
                if (have && slot < out_module.static_data.entries.size()) {
                    uint32_t off =
                        out_module.static_data.entries[slot].byte_offset;
                    for (int k = 0; k < 8; ++k)
                        out_module.static_data.bytes[off + (size_t)k] =
                            (uint8_t)((cval >> (8 * k)) & 0xFF);
                }
            }
        }
    }
    /* PRE-PASE force-lower: determinar que comptime fns hay que bajar a runtime
     * porque un @Macro (o comptime fn con asm) lowereable las referencia
     * (transitivamente).  Sin esto, el `__macro_<X>` que llama a un helper
     * comptime emitiria un `callvm code.<helper>` colgante (los comptime
     * helpers no se bajan por defecto).  Poblamos @c
     * comptime_fns_to_force_lower_ ANTES del lowering para que el orden de
     * bajada de decls sea irrelevante. */
    {
        std::unordered_set<std::string> visiting;
        g_macro_force_lower = &comptime_fns_to_force_lower_;
        g_macro_visiting = &visiting;
        for (auto &decl : mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::FunctionDecl) continue;
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            if (!fd->body || fd->is_imported_comptime) continue;
            const bool is_lowerable_comptime =
                (fd->is_comptime && fd->is_macro) ||
                (fd->is_comptime && !fd->is_macro &&
                 comptime_fn_needs_vm(tc_, fd));
            if (!is_lowerable_comptime) continue;
            visiting.clear();
            // Efecto colateral: recolecta los helpers lowereables.  Si el macro
            // NO es lowereable, no pasa nada (sus helpers no se fuerzan; el
            // macro caera a AST-only en lower_function como antes).
            if (macro_body_unsupported_reason(tc_, fd->body.get()).empty()) {
                // macro lowereable: sus helpers ya estan en el set.
            } else {
                // No lowereable: quitar cualquier helper que solo el aportara
                // seria complejo; es inocuo dejarlos (una comptime fn lowerada
                // de mas es dead code si nadie la llama en runtime).  Los
                // helpers recolectados de un macro no-lowereable igual pueden
                // ser referenciados por otro macro lowereable.
            }
        }
        /* Los METODOS comptime (un constructor comptime, por ejemplo) tambien
         * llaman a helpers, y sin recorrerlos el helper no entra al set: su
         * llamada acababa rechazada como "no es comptime-evaluable" pese a
         * estar dentro de un cuerpo que se ejecuta al compilar. */
        auto scan_methods = [&](const auto &methods) {
            for (const auto &m : methods) {
                if (!m || !m->body || !m->is_comptime) continue;
                visiting.clear();
                (void)macro_body_unsupported_reason(tc_, m->body.get());
            }
        };
        for (auto &decl : mod_.decls) {
            if (!decl) continue;
            if (decl->kind == ast::NodeKind::StructDecl)
                scan_methods(
                    static_cast<ast::StructDecl *>(decl.get())->methods);
            else if (decl->kind == ast::NodeKind::ClassDecl)
                scan_methods(
                    static_cast<ast::ClassDecl *>(decl.get())->methods);
        }
        g_macro_force_lower = nullptr;
        g_macro_visiting = nullptr;
    }

    us_previo =
        static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(
                              RelojBajada::now() - marca_run)
                              .count());
    const auto marca_fns = RelojBajada::now();

    if (main_decl) {
        lower_function(main_decl, out_module);
        ++n_bajadas;
    }

    for (auto &decl : mod_.decls) {
        if (!decl) continue;
        if (decl->kind == ast::NodeKind::FunctionDecl) {
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            if (fd == main_decl) continue; // ya bajada
            ++n_bajadas;
            if (fd->is_async) {
                lower_async_function(fd, out_module);
            } else {
                lower_function(fd, out_module);
            }
        } else if (decl->kind == ast::NodeKind::GlobalVarDecl) {
            // Las variables globales con storage real no estan soportadas
            // en el frontend Vesta actual.  Pero `const T NAME = lit;` SI
            // funciona porque @c lower_ident las inlinea como CONST en
            // cada uso (no necesitan storage).  Solo avisamos para las
            // globales NO-const o las que tienen inicializador no-literal
            // (que efectivamente se ignoran).
            auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
            bool literal_const =
                gv->is_const && gv->init &&
                (gv->init->kind == ast::NodeKind::IntLitExpr ||
                 gv->init->kind ==
                     ast::NodeKind::StringLitExpr // 2026-05-23 const string
                 // global
                 || (gv->init->kind == ast::NodeKind::UnaryExpr &&
                     static_cast<ast::UnaryExpr *>(gv->init.get())->op ==
                         ast::UnOp::Neg &&
                     static_cast<ast::UnaryExpr *>(gv->init.get())->operand &&
                     static_cast<ast::UnaryExpr *>(gv->init.get())
                             ->operand->kind == ast::NodeKind::IntLitExpr));
            /* A.38/A.39: `comptime const` y `static_assert` (que se
             * envuelve como GlobalVarDecl dummy con type=void) no
             * tienen storage runtime y NO necesitan warning. */
            bool is_comptime_silent =
                gv->is_comptime ||
                (gv->type &&
                 gv->type->kind == ast::NodeKind::PrimitiveTypeNode &&
                 static_cast<ast::PrimitiveTypeNode *>(gv->type.get())->prim ==
                     PrimitiveKind::VOID);
            // L2.2: globales runtime no-const obtienen storage real
            // via slot en static_data inicializado por __module_init.
            // Solo se reserva si tiene tipo basico soportado: STRING o
            // enteros/floats que caben en 8 bytes.
            bool runtime_global_supported = false;
            // Global array nativo T[N]: ya tiene slot (pre-pase); soportado.
            if (!gv->is_const && !is_comptime_silent && gv->type &&
                gv->type->kind == ast::NodeKind::ArrayTypeNode &&
                vx_global_array_bytes(gv->type.get(), tc_) > 0) {
                runtime_global_supported = true;
            }
            // Tipo primitivo directo O un newtype (typedef-new) que resuelve a
            // un primitivo de <=8 bytes (p.ej. `uintptr` -> u64).  Resolvemos
            // via resolve_type_node para que los tipos semanticos de std.types
            // tengan storage global igual que su underlying.
            if (!gv->is_const && !is_comptime_silent && gv->type &&
                (gv->type->kind == ast::NodeKind::PrimitiveTypeNode ||
                 gv->type->kind == ast::NodeKind::NamedTypeNode)) {
                PrimitiveKind gpk =
                    (gv->type->kind == ast::NodeKind::PrimitiveTypeNode)
                        ? static_cast<ast::PrimitiveTypeNode *>(gv->type.get())
                              ->prim
                        : tc_.resolve_type_node(gv->type.get()).kind;
                // Global de tipo overlay: su valor runtime es un puntero (8
                // bytes)
                // -> tratarlo como PTR (slot de 8 bytes, init por asignacion).
                if (gpk == PrimitiveKind::STRUCT &&
                    gv->type->kind == ast::NodeKind::NamedTypeNode) {
                    Type rt = tc_.resolve_type_node(gv->type.get());
                    auto sit = tc_.struct_layouts().find(rt.struct_name);
                    if (sit != tc_.struct_layouts().end() &&
                        sit->second.is_overlay)
                        gpk = PrimitiveKind::PTR;
                }
                switch (gpk) {
                case PrimitiveKind::STRING:
                case PrimitiveKind::I8:
                case PrimitiveKind::I16:
                case PrimitiveKind::I32:
                case PrimitiveKind::I64:
                case PrimitiveKind::U8:
                case PrimitiveKind::U16:
                case PrimitiveKind::U32:
                case PrimitiveKind::U64:
                case PrimitiveKind::F32:
                case PrimitiveKind::F64:
                case PrimitiveKind::BOOL:
                case PrimitiveKind::CHAR:
                case PrimitiveKind::PTR:
                    runtime_global_supported = true;
                    {
                        uint64_t slot =
                            get_or_create_runtime_global_slot(gv->name);
                        // AOT/bare: __module_init NO se ejecuta (el entry es
                        // main/kmain directo).  Para que el global tenga su
                        // valor inicial sin depender de __module_init, si el
                        // init es una constante la grabamos DIRECTAMENTE en los
                        // bytes de .data.  El STORE de __module_init (VM/JIT)
                        // re-escribe el mismo valor; en AOT esos bytes son la
                        // unica fuente.  (Inits no-constantes -- p.ej. llamadas
                        // -- siguen necesitando __module_init: no soportado en
                        // AOT puro, pero raro en codigo bare.)
                        uint64_t cval = 0;
                        bool have = false;
                        // Un `f32 g = 0.5` guarda los bits de un binary32 (4
                        // bytes), NO los de un double: el LOAD lee 4 bytes y
                        // con los bits de f64 solo veria el resto de la
                        // mantisa (0.5 en f64 tiene los 4 bytes bajos a cero
                        // -> el global salia 0).
                        const bool g_is_f32 = (gpk == PrimitiveKind::F32);
                        const ast::Expr *ie = gv->init.get();
                        if (ie) {
                            switch (ie->kind) {
                            case ast::NodeKind::IntLitExpr:
                                cval = static_cast<const ast::IntLitExpr *>(ie)
                                           ->value;
                                have = true;
                                break;
                            case ast::NodeKind::BoolLitExpr:
                                cval = static_cast<const ast::BoolLitExpr *>(ie)
                                               ->value
                                           ? 1u
                                           : 0u;
                                have = true;
                                break;
                            case ast::NodeKind::CharLitExpr:
                                cval = static_cast<const ast::CharLitExpr *>(ie)
                                           ->codepoint;
                                have = true;
                                break;
                            case ast::NodeKind::FloatLitExpr: {
                                const double d =
                                    static_cast<const ast::FloatLitExpr *>(ie)
                                        ->value;
                                cval = float_bits_for_global(d, g_is_f32);
                                have = true;
                                break;
                            }
                            case ast::NodeKind::UnaryExpr: {
                                auto *u =
                                    static_cast<const ast::UnaryExpr *>(ie);
                                if (u->op == ast::UnOp::Neg && u->operand &&
                                    u->operand->kind ==
                                        ast::NodeKind::IntLitExpr) {
                                    cval =
                                        (uint64_t)(-(int64_t)static_cast<
                                                        const ast::IntLitExpr
                                                            *>(u->operand.get())
                                                        ->value);
                                    have = true;
                                } else if (u->op == ast::UnOp::Neg &&
                                           u->operand &&
                                           u->operand->kind ==
                                               ast::NodeKind::FloatLitExpr) {
                                    const double d =
                                        -static_cast<const ast::FloatLitExpr *>(
                                             u->operand.get())
                                             ->value;
                                    cval = float_bits_for_global(d, g_is_f32);
                                    have = true;
                                }
                                break;
                            }
                            default: break;
                            }
                        }
                        if (have &&
                            slot < out_mod_->static_data.entries.size()) {
                            uint32_t off =
                                out_mod_->static_data.entries[slot].byte_offset;
                            for (int k = 0; k < 8; ++k)
                                out_mod_->static_data.bytes[off + (size_t)k] =
                                    (uint8_t)((cval >> (8 * k)) & 0xFF);
                        }
                    }
                    break;
                default: break;
                }
            }
            if (!literal_const && !is_comptime_silent &&
                !runtime_global_supported) {
                diags_.warning(decl->loc,
                               "variable global no-const ignorada (sin storage "
                               "real en este frontend)");
            }
        }
    }

    // Bajar metodos de clases al final.  Vienen DESPUES de las
    // funciones top-level para no tomar la posicion de "entry point"
    // del emisor IR (que termina la primera funcion con hlt).  Cada
    // metodo se compila como IrFunction con nombre <Class>__<method>
    // y un primer parametro implicito 'this' de tipo PTR.
    for (auto &decl : mod_.decls) {
        if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
        auto *cd = static_cast<ast::ClassDecl *>(decl.get());
        lower_class_methods(cd, out_module);
    }

    // Bajar metodos de structs (value-types, dispatch estatico).  Cada
    // uno se compila como funcion libre <Struct>__<metodo> con un
    // primer parametro implicito 'this' (PTR a la direccion del struct).
    for (auto &decl : mod_.decls) {
        if (!decl || decl->kind != ast::NodeKind::StructDecl) continue;
        auto *sd = static_cast<ast::StructDecl *>(decl.get());
        lower_struct_methods(sd, out_module);
    }

    // NS.6-ext: metodos de extension / impl (funciones libres <clave>__metodo).
    lower_extension_methods(out_module);

    // Generar funciones auxiliares de POO:
    //  - __new_<X>(args) por cada clase: encapsula findclass+newobj+ctor.
    //  - __module_init(): registra todas las clases via defclass+...
    // Estas se añaden al modulo despues de las funciones de usuario;
    // el prologo de main incluye una llamada a __module_init para
    // garantizar que las clases esten registradas antes del cuerpo.
    generate_new_helpers(out_module);
    // Thunks para `&extern` usado como cfn (se rellenan durante el lowering).
    generate_extern_cfn_thunks(out_module);
    // Helper runtime __vx_free_uniq para el reassign-free de campos unique<T>.
    generate_free_uniq_helper(out_module);
    //  AOT.2.b: en POO nativa no hay ClassRegistry -> no se genera
    // __module_init (las clases son layout estatico compile-time).
    if (!native_poo_) generate_module_init_function(out_module);

    // Exportar metadata POO al @c IrModule para que el port transpiler
    // (port-C, etc.) emita codigo POO eficiente sin reconstruir las
    // clases desde @c __module_init.  Llamar tras lower_class_methods
    // para que los @c IrMethod::ir_fn_name apunten a IrFunctions ya
    // emitidas en @c out_module.functions.
    export_classes_to_ir(out_module);

    // volcar las funciones sinteticas de spawn DESPUES de las
    // de usuario y POO.  Asi main sigue siendo la primera funcion del
    // modulo (entry point con hlt) y los helpers de spawn quedan al
    // final como funciones normales (cierran con ret, pero el body
    // siempre incluye un hlt explicito antes del fin del bloque).
    for (auto &h : pending_spawn_helpers_) {
        propagate_is_gc_object_through_phis(h);
        out_module.add_function(std::move(h));
    }
    pending_spawn_helpers_.clear();

    // Bloques `bytes name { db/dw/dd/dq/times }` (datos crudos NASM, AOT):
    // se internan como entradas de static_data en su @section (default
    // .rodata) y se marcan FORCE_EMIT para que el emisor AOT las coloque
    // aunque ningun codigo las referencie (firmas, tablas, boot sectors).
    // NON_DEDUP evita que el dedup post-merge colapse dos bloques con los
    // mismos bytes en secciones distintas.
    for (auto &decl : mod_.decls) {
        if (!decl || decl->kind != ast::NodeKind::BytesDecl) continue;
        auto *bd = static_cast<ast::BytesDecl *>(decl.get());

        // Bloque `asm`: ensamblar el cuerpo NASM via Keystone a la bitness
        // indicada (@bits) y colocarlo en su seccion como datos crudos.
        // Las directivas $/$$/times NO las soporta Keystone; el usuario usa
        // un bloque `bytes` con @at/times para padding/firma.
        if (bd->is_asm) {
            std::vector<uint8_t> asm_bytes;
            std::string aerr;
            // Mini-ensamblador: instrucciones via Keystone + db/dw/dd/dq/
            // times/$/$$ propios, intercalados en orden (estilo NASM).  Los
            // call/jmp a un simbolo (funcion Vesta) salen como sym_refs REL32.
            std::vector<ir::IrModule::StaticDataMeta::SymRef> asm_syms;
            if (!asmblk_assemble(bd->asm_body, bd->asm_bits, asm_bytes, aerr,
                                 &asm_syms)) {
                diags_.error(bd->loc, "bloque asm '" + bd->name + "': " + aerr);
                continue;
            }
            const size_t idx =
                out_module.static_data.push_back(std::move(asm_bytes));
            auto &m = out_module.static_data.meta_at(idx);
            m.section_name =
                bd->attr_section.empty() ? ".text" : bd->attr_section;
            m.section_perms = bd->attr_section_perms;
            m.section_at = bd->attr_at;
            m.section_order = bd->attr_order;
            m.sym_refs = std::move(asm_syms); // call/jmp -> funcion Vesta
            //  NR / dev-OS: exportar el nombre del bloque como simbolo
            // resoluble por otros bloques (cross-block jmp/call/dd).
            m.symbol_name = bd->name;
            m.flags |= ir::IrModule::SD_FLAG_FORCE_EMIT |
                       ir::IrModule::SD_FLAG_NON_DEDUP;
            continue;
        }

        // Reconstruir el blob resolviendo los operandos identificador.  Un
        // identificador puede ser:
        //   (a) comptime const entero -> literal del ancho de la directiva.
        //   (b) comptime array        -> sus elementos (cada uno del ancho).
        //   (c) simbolo de funcion     -> reloc ABS64 (requiere dq=8).
        // Los sym_refs vienen en orden de offset creciente (el parser los
        // añade segun avanza); reconstruimos de izquierda a derecha.
        const auto &ccv = tc_.comptime_const_values();
        std::vector<uint8_t> rebuilt;
        std::vector<ir::IrModule::StaticDataMeta::SymRef> kept;
        rebuilt.reserve(bd->data.size());
        size_t cursor = 0;
        bool ok = true;
        for (const auto &sr : bd->sym_refs) {
            if (sr.offset > bd->data.size()) {
                ok = false;
                break;
            }
            // Bytes literales que preceden a este operando.
            rebuilt.insert(rebuilt.end(), bd->data.begin() + cursor,
                           bd->data.begin() + sr.offset);
            auto cit = ccv.find(sr.sym);
            if (cit != ccv.end()) {
                const auto &cc = cit->second;
                if (cc.is_str || cc.is_struct || cc.is_type) {
                    diags_.error(
                        bd->loc,
                        "bytes: comptime '" + sr.sym +
                            "' no es entero ni array; no es embebible");
                    ok = false;
                    break;
                }
                if (cc.is_array) {
                    for (const auto &ev : cc.array_vals) {
                        if (!ev || ev->is_str || ev->is_array ||
                            ev->is_struct) {
                            diags_.error(bd->loc,
                                         "bytes: el array comptime '" + sr.sym +
                                             "' tiene elementos no enteros");
                            ok = false;
                            break;
                        }
                        const uint64_t v = (uint64_t)ev->value;
                        for (int i = 0; i < sr.width; ++i)
                            rebuilt.push_back((uint8_t)(v >> (8 * i)));
                    }
                    if (!ok) break;
                } else {
                    const uint64_t v = (uint64_t)cc.value;
                    for (int i = 0; i < sr.width; ++i)
                        rebuilt.push_back((uint8_t)(v >> (8 * i)));
                }
            } else {
                // Simbolo (funcion u otro bloque) -> reloc absoluta.  Se
                // admite `dq` (ABS64) y `dd` (ABS32): un dev-OS pone la base
                // de un GDTR / un puntero far de 32 bits con `dd gdt`, donde
                // la direccion cabe en 32 bits (binario plano bajo 4GB).
                if (sr.width != 8 && sr.width != 4) {
                    diags_.error(
                        bd->loc,
                        "bytes: la referencia al simbolo '" + sr.sym +
                            "' requiere 'dd' (32 bits) o 'dq' (64 bits)");
                    ok = false;
                    break;
                }
                ir::IrModule::StaticDataMeta::SymRef d;
                d.offset = (uint32_t)rebuilt.size(); // offset en el blob nuevo
                d.sym = sr.sym;
                d.width = sr.width; // 4 -> ABS32, 8 -> ABS64
                d.is_rel = sr.is_rel ? 1 : 0;
                kept.push_back(std::move(d));
                for (int i = 0; i < sr.width; ++i)
                    rebuilt.push_back(0); // placeholder
            }
            cursor =
                (size_t)sr.offset + sr.width; // saltar el placeholder original
        }
        if (!ok) continue; // error ya emitido; saltar este bloque
        // Resto de bytes literales tras el ultimo operando.
        if (cursor <= bd->data.size())
            rebuilt.insert(rebuilt.end(), bd->data.begin() + cursor,
                           bd->data.end());

        const size_t idx = out_module.static_data.push_back(std::move(rebuilt));
        auto &m = out_module.static_data.meta_at(idx);
        m.section_name =
            bd->attr_section.empty() ? ".rodata" : bd->attr_section;
        m.section_perms = bd->attr_section_perms;
        m.section_at = bd->attr_at;
        m.section_order = bd->attr_order;
        //  NR / dev-OS: exportar el nombre del bloque bytes como simbolo
        // resoluble cross-block (p.ej. `lgdt [gdtr]` / `dd gdt` desde otro).
        m.symbol_name = bd->name;
        m.flags |=
            ir::IrModule::SD_FLAG_FORCE_EMIT | ir::IrModule::SD_FLAG_NON_DEDUP;
        // Solo las refs de funcion sobreviven como relocs (las comptime
        // consts ya se materializaron como bytes).
        m.sym_refs = std::move(kept);
    }

    // CPU dispatch (cimiento): si algun cpu_features() se uso, prepender
    // `call __vx_cpu_init` al ENTRY de main para que la deteccion corra UNA
    // VEZ antes de cualquier codigo del usuario.  Se hace AQUI (post-lowering)
    // y no en lower_function porque main se baja ANTES que el resto: un
    // cpu_features() en una funcion no-main marca cpu_features_used_ DESPUES
    // de cerrar main.  Solo en native_poo_ (AOT): el helper usa INLINE_ASM
    // (PURE_NATIVE) + el wiring no toca el stub _start.
    // AUTO multiversion (--float-isa auto): si main tiene ops VEC_*,
    // renombrarlo a __vx_main_body + sintetizar un main que despacha por cpuid.
    // Debe correr ANTES del wiring de inits (necesita que main exista como el
    // wrapper para prepender alli el call __vx_auto_init).
    ensure_auto_multiversion(out_module);

    if (native_poo_ && (cpu_features_used_ || cpu_dispatch_used_)) {
        // Asegurar que el global de features + el helper __vx_cpu_init existan
        // (idempotente).  El cpuid corre primero: el dispatch lee el bitmask.
        (void)ensure_cpu_features_global();
        // Cada init se prepone SOLO si su mecanismo de dispatch se emitio
        // (evita arrastrar la maquinaria memcpy a un programa que solo usa
        // strcmp/strlen, y viceversa).  Inc 5a: el strdisp_init setea los fp
        // de strcmp/strlen (override del usuario o baseline; sin cpuid).
        const bool mc_disp = memcpy_helpers_emitted_;
        const bool sd_disp = strdisp_emitted_;
        // Localizar main y prepender las CALL a su bloque de entrada.  El
        // ORDEN final de ejecucion debe ser:  __vx_cpu_init (cpuid) ->
        // __vx_memcpy_init -> __vx_strdisp_init -> codigo del usuario.
        // insert(begin()) prepende, asi que insertamos en orden inverso:
        // strdisp_init, luego memcpy_init, luego cpu_init (queda de primero).
        for (auto &f : out_module.functions) {
            if (f.name != "main") continue;
            if (f.blocks.empty()) break;
            auto &ins = f.blocks[0].instrs;
            if (sd_disp) {
                ir::IrInstr call_sd{};
                call_sd.op = ir::IrOp::CALL;
                call_sd.type = ir::IrType::VOID;
                call_sd.dst = ir::IR_NO_VALUE;
                call_sd.func_name = "__vx_strdisp_init";
                call_sd.source_line = 0;
                ins.insert(ins.begin(), std::move(call_sd));
            }
            if (mc_disp) {
                ir::IrInstr call_mc{};
                call_mc.op = ir::IrOp::CALL;
                call_mc.type = ir::IrType::VOID;
                call_mc.dst = ir::IR_NO_VALUE;
                call_mc.func_name = "__vx_memcpy_init";
                call_mc.source_line = 0;
                ins.insert(ins.begin(), std::move(call_mc));
            }
            if (auto_dispatch_emitted_) {
                // AUTO: el dispatch del main (setea __vx_main_body$fp).  Debe
                // ir DESPUES de cpu_init (lee el bitmask) y ANTES del CALLIND
                // del wrapper (que lee el fp).  Se inserta aqui (antes que
                // cpu_init) para quedar justo tras el en el orden final.
                ir::IrInstr call_auto{};
                call_auto.op = ir::IrOp::CALL;
                call_auto.type = ir::IrType::VOID;
                call_auto.dst = ir::IR_NO_VALUE;
                call_auto.func_name = "__vx_auto_init";
                call_auto.source_line = 0;
                ins.insert(ins.begin(), std::move(call_auto));
            }
            ir::IrInstr call_init{};
            call_init.op = ir::IrOp::CALL;
            call_init.type = ir::IrType::VOID;
            call_init.dst = ir::IR_NO_VALUE;
            call_init.func_name = "__vx_cpu_init";
            call_init.source_line = 0;
            ins.insert(ins.begin(), std::move(call_init));
            break;
        }
    }

    // TLS callback (thread_local PE): si el modulo tiene thread_local con init
    // != 0, sintetizar __vx_tls_init -- la funcion que el cargador de Windows
    // llama en cada attach de hilo (registrada en AddressOfCallBacks del
    // IMAGE_TLS_DIRECTORY).  Escribe la plantilla a la copia por-hilo (el
    // cargador no siempre la copia para el TLS de una .dll en un consumidor
    // minimal sin CRT).  Reusa el acceso TLS (STR_LIT_ADDR is_tls -> store),
    // que el driver baja a gs:[0x58]+secrel.  Idempotente y barato (N stores
    // por attach; N = thread_local con init != 0).
    if (native_poo_ && !tls_nonzero_inits_.empty()) {
        ir::IrFunction ti;
        ti.name = "__vx_tls_init";
        // Devuelve i64 1 (TRUE): __vx_tls_init es el ENTRY POINT (DllMain) de
        // la .dll -- el cargador lo llama en cada attach de hilo y aqui
        // aplicamos la plantilla por-hilo (ntdll no la copia para el TLS de una
        // .dll sin un entry que dispare su init).  DllMain debe devolver TRUE o
        // la carga falla.  (Tambien queda registrado como TLS callback, que
        // ignora el retorno.)
        ti.ret_type = ir::IrType::I64;
        const ir::IrBlockId e = ti.new_block("entry");
        for (const auto &pr : tls_nonzero_inits_) {
            const uint64_t slot = pr.first;
            const uint64_t val = pr.second;
            // %addr = &tls_var (STR_LIT_ADDR del slot; is_tls lo deriva el
            // driver desde SD_FLAG_TLS -> acceso por thread pointer).
            const ir::IrValueId v_addr = ti.new_value(ir::IrType::PTR);
            ti.values[v_addr].is_host_ptr = true;
            {
                ir::IrInstr a{};
                a.op = ir::IrOp::STR_LIT_ADDR;
                a.type = ir::IrType::PTR;
                a.dst = v_addr;
                a.imm = slot;
                ti.append(e, std::move(a));
            }
            // %v = CONST val (8B); el slot esta padded a 8 -> store uniforme
            // i64.
            const ir::IrValueId v_val = ti.new_value(ir::IrType::I64);
            {
                ir::IrInstr c{};
                c.op = ir::IrOp::CONST;
                c.type = ir::IrType::I64;
                c.dst = v_val;
                c.imm = val;
                ti.append(e, std::move(c));
            }
            {
                ir::IrInstr s{};
                s.op = ir::IrOp::STORE;
                s.type = ir::IrType::I64;
                s.operands = {v_val, v_addr};
                ti.append(e, std::move(s));
            }
        }
        // return 1 (TRUE) -- DllMain debe devolver no-cero o la carga falla.
        const ir::IrValueId v_one = ti.new_value(ir::IrType::I64);
        {
            ir::IrInstr c{};
            c.op = ir::IrOp::CONST;
            c.type = ir::IrType::I64;
            c.dst = v_one;
            c.imm = 1;
            ti.append(e, std::move(c));
        }
        {
            ir::IrInstr r{};
            r.op = ir::IrOp::RET;
            r.type = ir::IrType::I64;
            r.operands = {v_one};
            ti.append(e, std::move(r));
        }
        out_module.add_function(std::move(ti));
    }

    // gc<T> opt-in: si el modulo usa gc<T> (CLASE, unique, shared o primitivo),
    // generar __vxgc_init que (1) llama vx_gc_init -> construye el heap E
    // INSTALA el runner nativo de finalizadores, y (2) registra los stackmaps
    // AOT (seccion .vxgc_smap) en el GC al arranque, inyectando un CALL a el al
    // INICIO de main -> el scan preciso ve los frames nativos y los gc<T> vivos
    // sobreviven la coleccion.  El driver emite la seccion .vxgc_smap tras el
    // layout (con relocs a cada funcion).
    //
    // El gate no puede limitarse a `classes_used_gc_` (gc<Clase>): un
    // gc<unique<T>>/gc<shared<T>> NO es una clase pero SI aloca via vx_gc_* y
    // registra un finalizador -- sin vx_gc_init su runner no se instala y el
    // finalizador se descarta (deleter/dtor no corre -> FUGA en AOT, bugs
    // 248).  Detectamos el uso REAL de gc<T> escaneando si alguna funcion
    // emitida referencia un simbolo `vx_gc_*` (uniforme para clase/unique/
    // shared/primitivo).
    bool module_uses_gc =
        !classes_used_gc_.empty() || module_has_gc_finalizers_;
    if (native_poo_ && !module_uses_gc) {
        for (const auto &f : out_module.functions) {
            for (const auto &b : f.blocks) {
                for (const auto &ins : b.instrs)
                    if (ins.func_name.rfind("vx_gc_", 0) == 0) {
                        module_uses_gc = true;
                        break;
                    }
                if (module_uses_gc) break;
            }
            if (module_uses_gc) break;
        }
    }
    if (native_poo_ && module_uses_gc) {
        ir::IrFunction gi;
        gi.name = "__vxgc_init";
        gi.ret_type = ir::IrType::VOID;
        const ir::IrBlockId e = gi.new_block("entry");
        // CALL vx_gc_init(): construye el heap global E INSTALA el runner
        // nativo de finalizadores (gc_finalizer_run_native).  Debe correr antes
        // del primer alloc/register_finalizer para que los finalizadores de
        // objetos escapados se ejecuten (deleter/dtor nativo) al colectar/exit.
        {
            ir::IrInstr ci{};
            ci.op = ir::IrOp::CALL;
            ci.type = ir::IrType::VOID;
            ci.dst = ir::IR_NO_VALUE;
            ci.func_name = "vx_gc_init";
            ci.is_call_site = true;
            gi.append(e, std::move(ci));
        }
        // %start = section_start(".vxgc_smap")  (PTR)
        const ir::IrValueId v_start = gi.new_value(ir::IrType::PTR);
        gi.values[v_start].is_host_ptr = true;
        {
            ir::IrInstr r{};
            r.op = ir::IrOp::SECTION_REF;
            r.type = ir::IrType::PTR;
            r.dst = v_start;
            r.func_name = ".vxgc_smap";
            r.imm = 0; // START
            gi.append(e, std::move(r));
        }
        // call vx_gc_register_aot_stackmaps(%start)  -- el tamanño total va
        // EMBEBIDO en el header de la seccion (section_size seria una reloc
        // SIZE no soportada en .obj/.o; section_start es una ADDR normal).
        {
            ir::IrInstr c{};
            c.op = ir::IrOp::CALL;
            c.type = ir::IrType::VOID;
            c.dst = ir::IR_NO_VALUE;
            c.func_name = "vx_gc_register_aot_stackmaps";
            c.operands = {v_start};
            gi.append(e, std::move(c));
        }
        {
            ir::IrInstr r{};
            r.op = ir::IrOp::RET;
            r.type = ir::IrType::VOID;
            gi.append(e, std::move(r));
        }
        out_module.add_function(std::move(gi));
        // Inyectar CALL __vxgc_init al inicio de main (antes de todo, incl. los
        // inits de cpu): el registro debe correr antes del primer gc<T> alloc.
        for (auto &f : out_module.functions) {
            if (f.name != "main" || f.blocks.empty()) continue;
            ir::IrInstr cg{};
            cg.op = ir::IrOp::CALL;
            cg.type = ir::IrType::VOID;
            cg.dst = ir::IR_NO_VALUE;
            cg.func_name = "__vxgc_init";
            f.blocks[0].instrs.insert(f.blocks[0].instrs.begin(),
                                      std::move(cg));
            break;
        }
        // Shutdown-time: inyectar CALL vx_gc_finalize_all ANTES de cada RET de
        // main.  Garantiza cero fuga del recurso interno de objetos gc<T> con
        // finalizador que ESCAPARON su scope y el sweep no colecto todavia (el
        // finalizador corre su deleter/dtor nativo antes del exit).  El valor
        // de retorno de main (RET %v) se preserva: el CALL se inserta ANTES del
        // RET pero no toca su operando.  Solo si el modulo registra
        // finalizadores (algun gc<T> con recurso interno): si no, es no-op
        // inofensivo.
        if (module_has_gc_finalizers_) {
            for (auto &f : out_module.functions) {
                if (f.name != "main") continue;
                for (auto &blk : f.blocks) {
                    for (size_t i = 0; i < blk.instrs.size(); ++i) {
                        if (blk.instrs[i].op != ir::IrOp::RET) continue;
                        ir::IrInstr cf{};
                        cf.op = ir::IrOp::CALL;
                        cf.type = ir::IrType::VOID;
                        cf.dst = ir::IR_NO_VALUE;
                        cf.func_name = "vx_gc_finalize_all";
                        cf.is_call_site = true;
                        blk.instrs.insert(blk.instrs.begin() + i,
                                          std::move(cf));
                        ++i; // saltar el RET recien desplazado
                    }
                }
                break;
            }
        }
    }

    if (medir_bajada) {
        const long us_total = static_cast<long>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                RelojBajada::now() - marca_run)
                .count());
        const long us_fns = static_cast<long>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                RelojBajada::now() - marca_fns)
                .count());
        std::cerr << "[bajada] " << n_bajadas << " funciones | preparar "
                  << us_previo << " us | bajar+resto " << us_fns
                  << " us | total " << us_total << " us\n";
    }
    return diags_.error_count() == initial_errors;
}

// ---------------------------------------------------------------------
// Sprint 4 (A.37.s4): IntrospectInfo POD chunks.
//
// Para cada layout marcado @Introspect emitimos UN chunk en
// static_data con este layout self-contained (todas las direcciones
// son offsets relativos al inicio del chunk, asi no hace falta
// relocation cross-chunk):
//
// HEADER (24 bytes):
//   +0   u32 kind         (0=Prim, 1=Class, 2=Struct, 3=Enum)
//   +4   u32 size_bytes
//   +8   u32 align_bytes
//   +12  u32 field_count
//   +16  u32 name_off     -- offset interno a los bytes del nombre
//   +20  u32 name_len
//
// FIELDS array (16 bytes cada uno):  ofset 24 + i*16
//   +0   u32 offset       -- offset del field DENTRO de la instancia
//   +4   u32 size_bytes
//   +8   u32 name_off     -- offset interno
//   +12  u32 name_len
//
// NAMES area: empieza tras los FIELDS.  Bytes raw, sin NUL
// terminator (la longitud va en name_len; el accesor type_info_name
// construye un StringObject via STRMAKE con name_addr + name_len).
// ---------------------------------------------------------------------
void Lowering::emit_introspect_info_chunks() {
    auto build_chunk =
        [this](
            const std::string &name, uint32_t kind, uint32_t size_bytes,
            uint32_t align_bytes,
            const std::vector<
                std::pair<std::string, std::pair<uint32_t, uint32_t>>> &fields)
        -> std::vector<uint8_t> {
        const uint32_t field_count = static_cast<uint32_t>(fields.size());
        const size_t header_sz = 24;
        const size_t fields_sz = field_count * 16;
        /* Calculamos los offsets de los nombres tras la tabla de fields. */
        uint32_t name_off = static_cast<uint32_t>(header_sz + fields_sz);
        uint32_t name_len = static_cast<uint32_t>(name.size());
        std::vector<std::pair<uint32_t, uint32_t>> field_name_ranges;
        field_name_ranges.reserve(fields.size());
        uint32_t cur = name_off + name_len;
        for (auto &f : fields) {
            const uint32_t flen = static_cast<uint32_t>(f.first.size());
            field_name_ranges.push_back({cur, flen});
            cur += flen;
        }
        /* Reservamos el buffer con tamano exacto y rellenamos. */
        std::vector<uint8_t> buf;
        buf.resize(cur, 0);
        auto put_u32 = [&buf](size_t at, uint32_t v) {
            buf[at + 0] = static_cast<uint8_t>(v & 0xFF);
            buf[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            buf[at + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            buf[at + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        };
        put_u32(0, kind);
        put_u32(4, size_bytes);
        put_u32(8, align_bytes);
        put_u32(12, field_count);
        put_u32(16, name_off);
        put_u32(20, name_len);
        for (size_t i = 0; i < fields.size(); ++i) {
            const size_t base = header_sz + i * 16;
            put_u32(base + 0, fields[i].second.first);       /* offset */
            put_u32(base + 4, fields[i].second.second);      /* size */
            put_u32(base + 8, field_name_ranges[i].first);   /* name_off */
            put_u32(base + 12, field_name_ranges[i].second); /* name_len */
        }
        /* Copiar bytes del nombre del tipo. */
        for (size_t i = 0; i < name.size(); ++i)
            buf[name_off + i] = static_cast<uint8_t>(name[i]);
        /* Copiar bytes de los nombres de fields. */
        for (size_t i = 0; i < fields.size(); ++i) {
            const auto &nm = fields[i].first;
            const uint32_t pos = field_name_ranges[i].first;
            for (size_t j = 0; j < nm.size(); ++j) {
                buf[pos + j] = static_cast<uint8_t>(nm[j]);
            }
        }
        return buf;
    };
    /* El nombre almacenado en el chunk (lo que devuelve type_info_name)
     * debe ser el nombre PUBLICO del tipo -- el ultimo segmento tras el
     * separador de namespace "__".  La clave del indice sigue siendo el
     * nombre mangled (lo que find_type resuelve en compile-time). */
    auto public_seg = [](const std::string &mangled) -> std::string {
        const size_t p = mangled.rfind("__");
        return (p == std::string::npos) ? mangled : mangled.substr(p + 2);
    };

    /* Structs marcados @Introspect. */
    for (const auto &kv : tc_.struct_layouts()) {
        const auto &lay = kv.second;
        if (!lay.is_introspect) continue;
        std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> fs;
        fs.reserve(lay.fields.size());
        for (const auto &f : lay.fields) {
            fs.push_back({f.name, {f.offset, f.size}});
        }
        std::vector<uint8_t> chunk =
            build_chunk(public_seg(lay.name), /*Struct=*/2, lay.size_bytes,
                        lay.align_bytes, fs);
        const uint64_t idx = out_mod_->intern_static_data(std::move(chunk));
        introspect_idx_by_name_[lay.name] = idx;
    }
    /* Clases marcadas @Introspect.  No emitimos metodos por ahora
     * (Sprint 4 MVP cubre solo fields; Sprint 5 añade methods). */
    for (const auto &kv : tc_.class_layouts()) {
        const auto &lay = kv.second;
        if (!lay.is_introspect) continue;
        std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> fs;
        fs.reserve(lay.fields.size());
        for (const auto &f : lay.fields) {
            fs.push_back({f.name, {f.offset, f.size}});
        }
        std::vector<uint8_t> chunk = build_chunk(
            public_seg(lay.name), /*Class=*/1, lay.size_bytes, /*align=*/8, fs);
        const uint64_t idx = out_mod_->intern_static_data(std::move(chunk));
        introspect_idx_by_name_[lay.name] = idx;
    }
    /* Enums marcados @Introspect.  Listamos variantes como "fields"
     * sinteticos con offset=tag, size=0 -- conveccion para el MVP;
     * el usuario sabe que el campo offset en realidad es el tag. */
    for (const auto &kv : tc_.enum_layouts()) {
        const auto &lay = kv.second;
        if (!lay.is_introspect) continue;
        std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> fs;
        fs.reserve(lay.variants.size());
        for (const auto &v : lay.variants) {
            fs.push_back({v.name, {v.tag, 0}});
        }
        std::vector<uint8_t> chunk = build_chunk(
            public_seg(lay.name), /*Enum=*/3, lay.size_bytes, /*align=*/8, fs);
        const uint64_t idx = out_mod_->intern_static_data(std::move(chunk));
        introspect_idx_by_name_[lay.name] = idx;
    }
}

// ---------------------------------------------------------------------
// Lowering de una funcion.
// ---------------------------------------------------------------------

/**
 * @brief  MC.1 -- detecta si el body de un @Macro contiene
 * caracteristicas que el IR runtime NO soporta todavia.
 *
 * Devuelve la primera razon encontrada (string descriptivo) o cadena
 * vacia si el body es lowerable.  Used by @c lower_function para
 * decidir si lowear o saltar el body al IR.
 *
 * Patrones detectados como NO soportados (todavia):
 *   - Calls a builtins comptime-only (`comptime_concat`, `to_str`,
 *     `gensym`, `comptime_compile`, etc.).
 *   - Calls con type_args (introspect: `sizeof<T>`, `field_name<T>`,
 *     `comptime_type<T>`, etc.).
 *   - VarDeclStmt con `is_comptime=true` (comptime var/const) --
 *     requiere puente de memoria compartida (MC.5).
 *   - ExprStmt con AssignExpr a IdentExpr global comptime --
 *     mismo motivo que arriba.
 *
 * En sprints posteriores (MC.4, MC.5) cada categoria se vuelve
 * "soportada" anadiendo un FFI runtime + bridge de memoria.
 */
static std::string macro_body_unsupported_reason(const TypeChecker &tc,
                                                 const ast::Stmt *s);

static std::string macro_body_unsupported_reason_expr(const TypeChecker &tc,
                                                      const ast::Expr *e);

/* Force-lower de comptime helpers: cuando @c g_macro_force_lower != nullptr, el
 * chequeo de lowereabilidad NO rechaza las llamadas a comptime fns no-macro,
 * sino que RECURRE en su body (chequeo transitivo) y, si son lowereables,
 * recolecta su nombre en @c g_macro_force_lower para que @c lower_function las
 * baje a runtime (`code.<helper>`), permitiendo que el `__macro_<X>` que las
 * llama resuelva.  @c g_macro_visiting es la guarda de ciclos.  thread_local
 * porque M8 compila modulos en paralelo (cada thread con su propio contexto).
 * (Definidos arriba, antes de Lowering::run.) */

static std::string macro_body_unsupported_reason_expr(const TypeChecker &tc,
                                                      const ast::Expr *e) {
    if (!e) return "";
    switch (e->kind) {
    case ast::NodeKind::IdentExpr: {
        /*  MC.17.2: refs a `comptime const` (INMUTABLES)
         * globales de tipo int SE ACEPTAN -- se materializan
         * como slot @c static_data de 8 bytes, leidos via
         * LOAD i64.  El valor es fijo, no hay divergencia
         * posible con el AST evaluator.
         *
         * `comptime var` (MUTABLES) siguen rechazados porque
         * la VM y el AST evaluator mantendrian copias separadas
         * que se desincronizarian con @Pure memoization
         * (test 156).  Soportarlos requiere shared memory
         * cross-AST/VM (deferred). */
        const auto *id = static_cast<const ast::IdentExpr *>(e);
        auto cit = tc.comptime_const_values().find(id->name);
        if (cit != tc.comptime_const_values().end()) {
            if (cit->second.is_str) {
                return "ref a comptime global string '" + id->name + "'";
            }
            if (cit->second.is_mutable) {
                /* comptime var MUTABLE global: se comparte entre lectores
                 * AST-eval (p.ej. `static_assert(g == 3)` top-level, otros
                 * comptime blocks) y el macro.  Si el macro VM-evaluara y
                 * mutara un slot static_data de la VM mientras el static_assert
                 * lee la copia AST (comptime_const_values_) -> DESYNC (el
                 * assert ve el valor viejo).  Por eso el macro que referencia
                 * un mutable global se deja AST-eval (misma copia que los
                 * lectores) -- arquitectural, no un gap de codegen.  Los
                 * mutables SOLO se podrian VM-evaluar si TODO lector comptime
                 * (incl. static_assert) leyera el slot de la VM, lo que
                 * exigiria ejecutar la VM en cada eval comptime -- fuera de
                 * alcance. */
                return "ref a comptime var (mutable) global '" + id->name + "'";
            }
            /* comptime const int (INMUTABLE) OK: slot static_data read-only. */
            return "";
        }
        return "";
    }
    case ast::NodeKind::CallExpr: {
        const auto *ce = static_cast<const ast::CallExpr *>(e);
        /* Calls con type-args -> introspect: NO soportado v1. */
        if (!ce->type_args.empty()) {
            return "introspect builtin con type_args (sizeof<T>, etc.)";
        }
        /* Calls a builtins comptime-only por nombre. */
        if (ce->callee && ce->callee->kind == ast::NodeKind::IdentExpr) {
            const auto *id =
                static_cast<const ast::IdentExpr *>(ce->callee.get());
            /*  MC.15B+C: los builtins que YA estan aliasados a
             * sus equivalentes runtime str_* en @c lower_call NO
             * deben rechazarse aqui -- el lowering los soporta.
             * Los demas siguen siendo comptime-only.
             *
             * Lowereables (MC.15B: concat/streq/strlen; MC.15C:
             * to_str/chr/ord/substr/gensym):
             *   comptime_concat  -> STRCAT
             *   comptime_streq   -> STRCMP + cmp
             *   comptime_strlen  -> STRLEN
             *   comptime_to_str  -> CALLN(vio_int_to_vmbuf) + STRMAKE
             *   comptime_chr     -> CALLN(vio_char_to_vmbuf) + STRMAKE
             *   comptime_ord     -> STRRAW + LOAD u8
             *   comptime_substr  -> STRSLICE
             *   gensym           -> CALLN(vio_gensym)
             *
             * Restantes (MC.15D futuro): repeat, replace, contains,
             * compile, emit_expr, type, print/ct_print.
             */
            /* Restantes comptime-only tras MC.18+MC.20:
             *   comptime_compile / compile      -- generacion de codigo
             * dinamica comptime_emit_expr / emit_expr  -- splice de AST en
             * compile-time comptime_type                   -- type-as-value
             *
             * `comptime_print`, `ct_print` -> `println` (MC.18).
             * `static_assert` -> virtual lib `vesta_comptime`
             * via FFI bridge (MC.20).  El lowering emite CALLN
             * a "vesta_comptime:static_assert" que el Loader
             * resuelve via @c lookup_virtual_fn al cargar el
             * .velb. */
            static const std::unordered_set<std::string> COMPTIME_ONLY = {
                "comptime_compile", "comptime_emit_expr", "comptime_type",
                "compile",          "emit_expr",
            };
            if (COMPTIME_ONLY.count(id->name)) {
                return "builtin comptime-only '" + id->name + "'";
            }
            /* MC.23 fix (bug 161): los nombres registrados como virtual
             * comptime fns bajo `vesta_comptime`
             * (comptime_type_sizeof/alignof/kind, comptime_compile,
             * static_assert) NO tienen simbolo de bytecode real -- solo existen
             * in-process en el compilador.  Bajar el body del macro a IR
             * emitiria un `callvm code.<nombre>` colgante que el linker no
             * resuelve (RelocationError).  Se fuerza a que el macro corra en
             * comptime (AST/VM eval), que SI resuelve el nombre via
             * lookup_virtual_fn y embebe el resultado como literal. */
            /* Las type-metadata (`comptime_type_sizeof/alignof/kind`) con arg
             * LITERAL string son CONSTANTES compile-time: el lowering las
             * pliega a un CONST (ver lower_call), asi que NO rechazan el macro.
             * El resto de virtual fns (static_assert, comptime_compile) sin
             * simbolo bytecode siguen forzando AST/VM-eval del call site. */
            static const std::unordered_set<std::string> FOLDABLE_TYPE_META = {
                "comptime_type_sizeof", "comptime_type_alignof",
                "comptime_type_kind"};
            if (ffi::lookup_virtual_fn("vesta_comptime", id->name) &&
                !(FOLDABLE_TYPE_META.count(id->name) && ce->args.size() == 1 &&
                  ce->args[0] &&
                  ce->args[0]->kind == ast::NodeKind::StringLitExpr)) {
                return "virtual comptime fn '" + id->name + "'";
            }
            /*  MC.17.3: calls a @Macros user-defined SE ACEPTAN
             * (la callee tambien se baja a IR con nombre
             * `__macro_<callee>`, asi que emitimos CALLVM regular
             * a esa label).  Calls a comptime fns NO-@Macro
             * siguen rechazados (necesitarian inline o lower
             * propio que no esta hecho). */
            auto fn_it = tc.comptime_fns().find(id->name);
            if (fn_it != tc.comptime_fns().end()) {
                if (fn_it->second && fn_it->second->is_macro) {
                    /* Aceptamos.  El callee macro tambien sera
                     * lowereado por el linker (al final del
                     * pase).  Si su body resulta no-lowereable,
                     * el __macro_<callee> no existira y la
                     * CALLVM fallara en runtime -- ese caso
                     * cae al fallback AST por inconsistencia. */
                    for (const auto &a : ce->args) {
                        auto ra =
                            macro_body_unsupported_reason_expr(tc, a.get());
                        if (!ra.empty()) return ra;
                    }
                    return "";
                }
                /* Llamada a una comptime fn NO-macro.  Con force-lower activo
                 * (g_macro_force_lower != null): recurrir en su body; si es
                 * lowereable, recolectarla para bajarla a runtime y ACEPTAR la
                 * llamada.  Sin force-lower (call sites legacy): rechazar
                 * (AST-only), comportamiento previo. */
                if (g_macro_force_lower && fn_it->second &&
                    fn_it->second->body) {
                    const std::string &hn = fn_it->first; // nombre registrado
                    if (g_macro_visiting->count(hn)) {
                        return ""; // ciclo: asumir OK (el otro nivel decide)
                    }
                    g_macro_visiting->insert(hn);
                    std::string sub = macro_body_unsupported_reason(
                        tc, fn_it->second->body.get());
                    g_macro_visiting->erase(hn);
                    if (sub.empty()) {
                        g_macro_force_lower->insert(hn);
                        /* Seguir recorriendo los ARGS de esta llamada: pueden
                         * contener llamadas anidadas a OTRAS comptime fns
                         * (p.ej. `bf_emit(bf_classify(x))`) que tambien hay que
                         * recolectar para el force-lower.  Sin esto, el callee
                         * del argumento quedaba fuera del set -> su CALL
                         * emitiria "no es comptime-evaluable (argumento
                         * runtime?)". */
                        for (const auto &a : ce->args) {
                            auto ra =
                                macro_body_unsupported_reason_expr(tc, a.get());
                            if (!ra.empty()) return ra;
                        }
                        return "";
                    }
                    return "helper comptime no-lowereable '" + id->name +
                           "': " + sub;
                }
                return "call a comptime fn user-defined '" + id->name + "'";
            }
        }
        /* Recurse en args. */
        for (const auto &a : ce->args) {
            auto r = macro_body_unsupported_reason_expr(tc, a.get());
            if (!r.empty()) return r;
        }
        auto r = macro_body_unsupported_reason_expr(tc, ce->callee.get());
        if (!r.empty()) return r;
        return "";
    }
    case ast::NodeKind::BinaryExpr: {
        const auto *bn = static_cast<const ast::BinaryExpr *>(e);
        auto r = macro_body_unsupported_reason_expr(tc, bn->lhs.get());
        if (!r.empty()) return r;
        return macro_body_unsupported_reason_expr(tc, bn->rhs.get());
    }
    case ast::NodeKind::StringLitExpr: {
        /* Un string interpolado `"... ${expr} ..."` (o triple-quoted) que un
         * @Macro devuelve puede llevar en su interpolacion llamadas a otras
         * comptime fns (p.ej. `"() => { ${bf_compile_body(src)} }"`).  Recorrer
         * las exprs de interpolacion para que esos callees entren al set de
         * force-lower; sin esto la interpolacion emitia un CALLVM colgante y el
         * macro no era comptime-evaluable a string (solo funcionaba con concat
         * `"a" + f(x) + "b"`, que si se recorre por el case BinaryExpr). */
        const auto *sl = static_cast<const ast::StringLitExpr *>(e);
        for (const auto &ie : sl->interp_exprs) {
            auto r = macro_body_unsupported_reason_expr(tc, ie.get());
            if (!r.empty()) return r;
        }
        return "";
    }
    case ast::NodeKind::InitListExpr: {
        /* Init list `{a, b, c}` de un array local: el lowering del macro lo
         * soporta via el var-decl tipado (`i64 xs[N] = {...}`) que hace ALLOCA
         * + STOREs.  Recurrir en los elementos por si alguno no es lowereable
         * (p.ej. un init list de structs, que si requiere layout). */
        const auto *il = static_cast<const ast::InitListExpr *>(e);
        for (const auto &el : il->elements) {
            auto r = macro_body_unsupported_reason_expr(tc, el.get());
            if (!r.empty()) return r;
        }
        return "";
    }
    case ast::NodeKind::IndexExpr: {
        /* Array indexing `arr[i]`: lowereable en macro body cuando `arr` es un
         * array local tipado (el lowering conoce el elem type via el var-decl).
         * Recurrir en base + index. */
        const auto *ix = static_cast<const ast::IndexExpr *>(e);
        auto r = macro_body_unsupported_reason_expr(tc, ix->base.get());
        if (!r.empty()) return r;
        return macro_body_unsupported_reason_expr(tc, ix->index.get());
    }
    case ast::NodeKind::UnaryExpr: {
        const auto *un = static_cast<const ast::UnaryExpr *>(e);
        return macro_body_unsupported_reason_expr(tc, un->operand.get());
    }
    case ast::NodeKind::TernaryExpr: {
        const auto *te = static_cast<const ast::TernaryExpr *>(e);
        auto r = macro_body_unsupported_reason_expr(tc, te->cond.get());
        if (!r.empty()) return r;
        r = macro_body_unsupported_reason_expr(tc, te->then_expr.get());
        if (!r.empty()) return r;
        return macro_body_unsupported_reason_expr(tc, te->else_expr.get());
    }
    case ast::NodeKind::AssignExpr: {
        const auto *ae = static_cast<const ast::AssignExpr *>(e);
        auto r = macro_body_unsupported_reason_expr(tc, ae->target.get());
        if (!r.empty()) return r;
        return macro_body_unsupported_reason_expr(tc, ae->value.get());
    }
    default: return "";
    }
}

static std::string macro_body_unsupported_reason(const TypeChecker &tc,
                                                 const ast::Stmt *s) {
    if (!s) return "";
    switch (s->kind) {
    case ast::NodeKind::BlockStmt: {
        const auto *bs = static_cast<const ast::BlockStmt *>(s);
        for (const auto &st : bs->body) {
            auto r = macro_body_unsupported_reason(tc, st.get());
            if (!r.empty()) return r;
        }
        return "";
    }
    case ast::NodeKind::VarDeclStmt: {
        const auto *vd = static_cast<const ast::VarDeclStmt *>(s);
        /*   (1/3): `comptime var/const` LOCALES dentro
         * de un macro body ya no se rechazan.  El lowering los
         * trata como vars runtime regulares (en `lower_var_decl`
         * detectamos el flag y descartamos la rama comptime
         * cuando current_fn_is_macro_=true).  El VM computa el
         * init en cada invocacion -- mismo resultado semantico
         * que la evaluacion AST que ocurria one-time.
         *
         * Validamos solo el init si esta presente. */
        /* Vars locales de tipo array nativo `T[N]` o struct
         * nominal NO son lowereables en este path (requeriria
         * ALLOCA + sizeof del elemento + path completo de
         * struct layout).  Fallback al AST evaluator que SI
         * maneja arrays/structs comptime (A.41+A.42). */
        if (vd->type) {
            const auto *t = vd->type.get();
            if (t->kind == ast::NodeKind::ArrayTypeNode) {
                /* Array local `T[N]`: el lowering hace ALLOCA + init (STOREs) y
                 * el indexing usa el elem type del var-decl.  Validar solo el
                 * init. */
                if (vd->init)
                    return macro_body_unsupported_reason_expr(tc,
                                                              vd->init.get());
                return "";
            }
            if (t->kind == ast::NodeKind::NamedTypeNode) {
                /* Si el nombre matchea un struct declarado, es
                 * un struct value-type que no lowereamos en el
                 * body del macro. */
                const auto *nt = static_cast<const ast::NamedTypeNode *>(t);
                if (tc.struct_layouts().find(nt->name) !=
                    tc.struct_layouts().end()) {
                    return "var local de tipo struct '" + nt->name +
                           "' en macro body (usa AST eval)";
                }
            }
        }
        if (vd->init) {
            return macro_body_unsupported_reason_expr(tc, vd->init.get());
        }
        return "";
    }
    case ast::NodeKind::ExprStmt: {
        const auto *es = static_cast<const ast::ExprStmt *>(s);
        return macro_body_unsupported_reason_expr(tc, es->expr.get());
    }
    case ast::NodeKind::ReturnStmt: {
        const auto *rs = static_cast<const ast::ReturnStmt *>(s);
        return macro_body_unsupported_reason_expr(tc, rs->value.get());
    }
    case ast::NodeKind::IfStmt: {
        const auto *is = static_cast<const ast::IfStmt *>(s);
        auto r = macro_body_unsupported_reason_expr(tc, is->cond.get());
        if (!r.empty()) return r;
        r = macro_body_unsupported_reason(tc, is->then_branch.get());
        if (!r.empty()) return r;
        if (is->else_branch) {
            return macro_body_unsupported_reason(tc, is->else_branch.get());
        }
        return "";
    }
    case ast::NodeKind::WhileStmt: {
        const auto *ws = static_cast<const ast::WhileStmt *>(s);
        auto r = macro_body_unsupported_reason_expr(tc, ws->cond.get());
        if (!r.empty()) return r;
        return macro_body_unsupported_reason(tc, ws->body.get());
    }
    case ast::NodeKind::DoWhileStmt: {
        const auto *ds = static_cast<const ast::DoWhileStmt *>(s);
        auto r = macro_body_unsupported_reason_expr(tc, ds->cond.get());
        if (!r.empty()) return r;
        return macro_body_unsupported_reason(tc, ds->body.get());
    }
    case ast::NodeKind::ForStmt: {
        const auto *fs = static_cast<const ast::ForStmt *>(s);
        if (fs->init) {
            auto r = macro_body_unsupported_reason(tc, fs->init.get());
            if (!r.empty()) return r;
        }
        if (fs->cond) {
            auto r = macro_body_unsupported_reason_expr(tc, fs->cond.get());
            if (!r.empty()) return r;
        }
        if (fs->step) {
            auto r = macro_body_unsupported_reason_expr(tc, fs->step.get());
            if (!r.empty()) return r;
        }
        return macro_body_unsupported_reason(tc, fs->body.get());
    }
    case ast::NodeKind::ComptimeBlockStmt:
    case ast::NodeKind::ComptimeForStmt:
        return "comptime block/for en macro body (requiere MC.5)";
    default: return "";
    }
}

/* Detecta si el body de un @Macro FORWARDEA un expr-capture: llama a una
 * comptime fn que tiene un parametro `expr` (p.ej. `source(e)` / `inject(e)`).
 * Esos casos NO pueden correr en la ComptimeVM porque el helper necesita
 * re-capturar el texto en SU sitio de llamada (no una representacion runtime);
 * se dejan a AST-eval.  Un macro con `expr` param que solo lo usa como string
 * (p.ej. `bf_compile_body(src)`) NO forwardea y SI va a la VM. */
static bool macro_body_forwards_expr_capture_expr(const TypeChecker &tc,
                                                  const ast::Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case ast::NodeKind::CallExpr: {
        const auto *ce = static_cast<const ast::CallExpr *>(e);
        if (ce->callee && ce->callee->kind == ast::NodeKind::IdentExpr) {
            const std::string &n =
                static_cast<const ast::IdentExpr *>(ce->callee.get())->name;
            auto it = tc.comptime_fns().find(n);
            if (it != tc.comptime_fns().end() && it->second) {
                for (const auto &p : it->second->params)
                    if (p && p->is_expr_capture) return true;
            }
        }
        for (const auto &a : ce->args)
            if (macro_body_forwards_expr_capture_expr(tc, a.get())) return true;
        return macro_body_forwards_expr_capture_expr(tc, ce->callee.get());
    }
    case ast::NodeKind::BinaryExpr: {
        const auto *bn = static_cast<const ast::BinaryExpr *>(e);
        return macro_body_forwards_expr_capture_expr(tc, bn->lhs.get()) ||
               macro_body_forwards_expr_capture_expr(tc, bn->rhs.get());
    }
    case ast::NodeKind::StringLitExpr: {
        const auto *sl = static_cast<const ast::StringLitExpr *>(e);
        for (const auto &ie : sl->interp_exprs)
            if (macro_body_forwards_expr_capture_expr(tc, ie.get()))
                return true;
        return false;
    }
    case ast::NodeKind::TernaryExpr: {
        const auto *te = static_cast<const ast::TernaryExpr *>(e);
        return macro_body_forwards_expr_capture_expr(tc, te->cond.get()) ||
               macro_body_forwards_expr_capture_expr(tc, te->then_expr.get()) ||
               macro_body_forwards_expr_capture_expr(tc, te->else_expr.get());
    }
    default: return false;
    }
}

static bool macro_body_forwards_expr_capture(const TypeChecker &tc,
                                             const ast::Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
    case ast::NodeKind::BlockStmt: {
        const auto *bs = static_cast<const ast::BlockStmt *>(s);
        for (const auto &st : bs->body)
            if (macro_body_forwards_expr_capture(tc, st.get())) return true;
        return false;
    }
    case ast::NodeKind::ReturnStmt: {
        const auto *rs = static_cast<const ast::ReturnStmt *>(s);
        return macro_body_forwards_expr_capture_expr(tc, rs->value.get());
    }
    case ast::NodeKind::ExprStmt: {
        const auto *es = static_cast<const ast::ExprStmt *>(s);
        return macro_body_forwards_expr_capture_expr(tc, es->expr.get());
    }
    case ast::NodeKind::VarDeclStmt: {
        const auto *vd = static_cast<const ast::VarDeclStmt *>(s);
        return macro_body_forwards_expr_capture_expr(tc, vd->init.get());
    }
    case ast::NodeKind::IfStmt: {
        const auto *is = static_cast<const ast::IfStmt *>(s);
        return macro_body_forwards_expr_capture_expr(tc, is->cond.get()) ||
               macro_body_forwards_expr_capture(tc, is->then_branch.get()) ||
               macro_body_forwards_expr_capture(tc, is->else_branch.get());
    }
    case ast::NodeKind::WhileStmt: {
        const auto *ws = static_cast<const ast::WhileStmt *>(s);
        return macro_body_forwards_expr_capture_expr(tc, ws->cond.get()) ||
               macro_body_forwards_expr_capture(tc, ws->body.get());
    }
    case ast::NodeKind::ForStmt: {
        const auto *fs = static_cast<const ast::ForStmt *>(s);
        return macro_body_forwards_expr_capture(tc, fs->init.get()) ||
               macro_body_forwards_expr_capture_expr(tc, fs->cond.get()) ||
               macro_body_forwards_expr_capture_expr(tc, fs->step.get()) ||
               macro_body_forwards_expr_capture(tc, fs->body.get());
    }
    default: return false;
    }
}

/* Pre-pase de annotation de tipos para body de @Macro.
 *
 * Los macros NO pasan por `check_functions` (los saltea porque su
 * body se interpreta solo al call site).  Pero MC.1 los baja a IR para
 * que la VM eval pueda ejecutarlos.  Sin annotation de tipos, los
 * IdentExpr en el body tienen result_type=VOID -- `lower_binary` no
 * detecta el caso `code == "OK"` con `code: string` y emite cmpjmp
 * directo sobre los handles en lugar de STRCMP runtime.
 *
 * Este walker recorre el body y annota result_type de los IdentExpr
 * cuyo nombre matchee un param del macro.  Es minimal -- solo cubre
 * el caso de params; otras vars locales se annotan al llamarlas via
 * lower_expr (que internamente usa el scope del lowering).  */
static void annotate_macro_param_idents(
    ast::Stmt *s, const std::unordered_map<std::string, Type> &param_types) {
    if (!s) return;
    std::function<void(ast::Expr *)> walk_expr = [&](ast::Expr *e) {
        if (!e) return;
        if (e->kind == ast::NodeKind::IdentExpr) {
            auto *id = static_cast<ast::IdentExpr *>(e);
            auto it = param_types.find(id->name);
            if (it != param_types.end()) {
                /* Siempre sobreescribir: dentro del body del macro
                 * los IdentExpr no fueron type-checkeados; el campo
                 * puede tener un default heredado del parser. */
                id->result_type = it->second;
            }
            return;
        }
        if (e->kind == ast::NodeKind::BinaryExpr) {
            auto *bn = static_cast<ast::BinaryExpr *>(e);
            walk_expr(bn->lhs.get());
            walk_expr(bn->rhs.get());
            return;
        }
        if (e->kind == ast::NodeKind::UnaryExpr) {
            auto *un = static_cast<ast::UnaryExpr *>(e);
            walk_expr(un->operand.get());
            return;
        }
        if (e->kind == ast::NodeKind::CallExpr) {
            auto *ce = static_cast<ast::CallExpr *>(e);
            walk_expr(ce->callee.get());
            for (auto &a : ce->args)
                walk_expr(a.get());
            return;
        }
        if (e->kind == ast::NodeKind::AssignExpr) {
            auto *ae = static_cast<ast::AssignExpr *>(e);
            walk_expr(ae->target.get());
            walk_expr(ae->value.get());
            return;
        }
        if (e->kind == ast::NodeKind::TernaryExpr) {
            auto *te = static_cast<ast::TernaryExpr *>(e);
            walk_expr(te->cond.get());
            walk_expr(te->then_expr.get());
            walk_expr(te->else_expr.get());
            return;
        }
        if (e->kind == ast::NodeKind::IndexExpr) {
            auto *ix = static_cast<ast::IndexExpr *>(e);
            walk_expr(ix->base.get());
            walk_expr(ix->index.get());
            return;
        }
        if (e->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e);
            walk_expr(fa->base.get());
            return;
        }
        if (e->kind == ast::NodeKind::CastExpr) {
            auto *ca = static_cast<ast::CastExpr *>(e);
            walk_expr(ca->operand.get());
            return;
        }
        /* Otros tipos de expresion: no necesitan recursion para el
         * caso de annotation de params (literals, ThisExpr, etc.). */
    };
    switch (s->kind) {
    case ast::NodeKind::BlockStmt: {
        auto *bs = static_cast<ast::BlockStmt *>(s);
        for (auto &st : bs->body)
            annotate_macro_param_idents(st.get(), param_types);
        break;
    }
    case ast::NodeKind::VarDeclStmt: {
        auto *vd = static_cast<ast::VarDeclStmt *>(s);
        if (vd->init) walk_expr(vd->init.get());
        break;
    }
    case ast::NodeKind::ExprStmt: {
        auto *es = static_cast<ast::ExprStmt *>(s);
        walk_expr(es->expr.get());
        break;
    }
    case ast::NodeKind::ReturnStmt: {
        auto *rs = static_cast<ast::ReturnStmt *>(s);
        if (rs->value) walk_expr(rs->value.get());
        break;
    }
    case ast::NodeKind::IfStmt: {
        auto *ifs = static_cast<ast::IfStmt *>(s);
        walk_expr(ifs->cond.get());
        annotate_macro_param_idents(ifs->then_branch.get(), param_types);
        if (ifs->else_branch)
            annotate_macro_param_idents(ifs->else_branch.get(), param_types);
        break;
    }
    case ast::NodeKind::WhileStmt: {
        auto *ws = static_cast<ast::WhileStmt *>(s);
        walk_expr(ws->cond.get());
        annotate_macro_param_idents(ws->body.get(), param_types);
        break;
    }
    case ast::NodeKind::DoWhileStmt: {
        auto *ds = static_cast<ast::DoWhileStmt *>(s);
        walk_expr(ds->cond.get());
        annotate_macro_param_idents(ds->body.get(), param_types);
        break;
    }
    case ast::NodeKind::ForStmt: {
        auto *fs = static_cast<ast::ForStmt *>(s);
        if (fs->init) annotate_macro_param_idents(fs->init.get(), param_types);
        if (fs->cond) walk_expr(fs->cond.get());
        if (fs->step) walk_expr(fs->step.get());
        annotate_macro_param_idents(fs->body.get(), param_types);
        break;
    }
    default: break;
    }
}

void Lowering::lower_function(ast::FunctionDecl *fd, ir::IrModule &out) {
    // Bug fix 2026-05-23: forward declarations no tienen body -- skip.
    if (fd->is_forward_decl || !fd->body) return;
    // Cross-module: una comptime/macro fn re-parseada de un dep NO se re-baja
    // aqui (el dep ya bajo su `__macro_<X>` + helpers, que el importer mergea).
    // Solo el AST se conserva para AST-eval al invocarla.
    if (fd->is_imported_comptime) return;
    // Templates genericos (con type_params) y especializaciones (#7) se
    // omiten: sus monomorphizaciones concretas (que SI aparecen en
    // mod_.decls) se bajan normalmente.
    if (!fd->type_params.empty() || fd->is_specialization) return;
    /* A.39: comptime fn (no-macro) NO se baja a IR.  Su body solo
     * se evalua en compile-time cuando es invocada desde un contexto
     * comptime.
     *
     *  MC.1 (A.43.22): @Macro bodies SI se lowean al IR (con
     * nombre `__macro_<original>`) cuando el body es lowerable.
     * Esto valida que la pipeline IR -> bytecode soporta el codigo
     * del macro; futuros sprints MC.2+ ejecutan ese bytecode via
     * una ComptimeVM para acelerar la metaprogramacion ~10-1000x.
     * Por ahora el IR queda en el modulo como dead code; el call
     * site del macro sigue usando el evaluator AST. */
    /* F1: una `comptime fn` (no-macro) con inline asm se baja a IR y se
     * ejecuta en el ComptimeVM (JIT + interp fallback).  Funciona en .velb
     * (interp/JIT) y en AOT: ambos hacen el two- que compila el codigo
     * comptime a un `.velb` cacheado y lo carga, asi que los call sites
     * comptime invocan la VM y el valor se pliega a constante. */
    const bool is_vm_comptime_fn =
        fd->is_comptime && !fd->is_macro && comptime_fn_needs_vm(tc_, fd);
    /* Force-lower: una comptime fn (no-macro) que un @Macro lowereable
     * referencia (recolectada en el pre-pase de run()) SI se baja, como fn
     * runtime normal (nombre plano `fd->name`), para que el `callvm code.<X>`
     * del macro resuelva. */
    const bool is_force_lowered_comptime =
        fd->is_comptime && !fd->is_macro &&
        comptime_fns_to_force_lower_.count(fd->name) != 0;
    if (fd->is_comptime && !fd->is_macro) {
        /* comptime fn (no-macro): por defecto NO se baja (se evalua en
         * compile-time y se elide).  Solo-LSP: con emit_comptime_fns_ la
         * bajamos como funcion normal para poder inspeccionar su codegen
         * (JIT/AOT/bytecode del hover).  No pasa por el setup de macro.
         * F1: si usa asm, SI se baja (para ejecutar en el ComptimeVM).
         * Force-lower: si un macro la referencia, tambien se baja. */
        if (!emit_comptime_fns_ && !is_vm_comptime_fn &&
            !is_force_lowered_comptime)
            return;
    } else if (fd->is_comptime) {
        /* @Macro con un param `expr`: el parser tipa `expr` como STRING
         * (captura el texto crudo del call site como StringLitExpr).  Es
         * VM-evaluable como cualquier macro con param string -> se baja a
         * `__macro_<X>` y corre en la ComptimeVM (interp/JIT), marshalando el
         * texto como StringObject.  El unico caso que NO puede ir a la VM es el
         * FORWARDING del expr a un helper expr-capture
         * (`source(e)`/`inject(e)`), donde el texto debe re-capturarse en el
         * sitio del helper: esos SI se dejan a AST-eval.  (Antes se forzaba
         * AST-eval para TODO expr-param macro; el usuario exige "nada de
         * AST-eval, todo interp/JIT".) */
        bool has_expr_param = false;
        for (const auto &p : fd->params)
            if (p && p->is_expr_capture) {
                has_expr_param = true;
                break;
            }
        if (has_expr_param &&
            macro_body_forwards_expr_capture(tc_, fd->body.get())) {
            ++macro_skipped_count_;
            macro_skip_reasons_.emplace_back(
                fd->name,
                "forwarding de `expr` a helper expr-capture (AST-eval)");
            return;
        }
        /* @Macro: intentar lowear el body al IR.  Si contiene
         * caracteristicas no soportadas todavia (introspect,
         * comptime var, builtins comptime-only), saltar limpiamente
         * y dejar que el evaluator AST haga el trabajo.
         *
         * Activamos el contexto force-lower para que las llamadas a comptime
         * fns lowereables NO se rechacen (se bajaran junto al macro). */
        g_macro_force_lower = &comptime_fns_to_force_lower_;
        std::unordered_set<std::string> ml_visiting;
        g_macro_visiting = &ml_visiting;
        const std::string reason =
            macro_body_unsupported_reason(tc_, fd->body.get());
        g_macro_force_lower = nullptr;
        g_macro_visiting = nullptr;
        if (!reason.empty()) {
            /* No soportado -- fallback silencioso al AST eval.
             * Capturamos el reason para diagnostico via
             * VESTA_MC_VERBOSE (el usuario lo ve como
             * "[mc-lower] M_xxx: AST-only (usa Y)"). */
            ++macro_skipped_count_;
            macro_skip_reasons_.emplace_back(fd->name, reason);
            return;
        }
        /* Pre-pase de annotation: los macros no pasan por
         * `check_functions` asi que los IdentExpr en el body tienen
         * result_type=VOID.  Anotamos los IdentExpr que matcheen
         * params del macro para que `lower_binary` detecte el caso
         * `code == "OK"` con `code: string` y emita STRCMP runtime.
         *
         * Bug en demo 162: comparaciones de string dentro del body
         * del macro emitian `cmpjmp` directo sobre los handles GC
         * sin invocar STRMAKE/STRCMP -> resultados incorrectos. */
        std::unordered_map<std::string, Type> macro_param_types;
        for (auto &p : fd->params) {
            if (p && p->type) {
                macro_param_types[p->name] =
                    tc_.resolve_type_node(p->type.get());
            }
        }
        if (!macro_param_types.empty()) {
            annotate_macro_param_idents(fd->body.get(), macro_param_types);
        }
        /* Continuar al lowering normal con nombre prefijado. */
    }
    /*  MC.17.1: setear flag para que lower_var_decl trate
     * `comptime var/const` LOCALES como vars runtime regulares.
     * Reset al salir de la funcion. */
    const bool prev_is_macro = current_fn_is_macro_;
    /* P1: fn-VM comparte modo macro. */
    current_fn_is_macro_ =
        (fd->is_comptime && fd->is_macro) || is_vm_comptime_fn;
    struct ScopeGuard {
        bool *flag;
        bool saved;

        ~ScopeGuard() { *flag = saved; }
    } macro_flag_guard{&current_fn_is_macro_, prev_is_macro};

    ir::IrFunction fn;
    /*  MC.1: nombre prefijado para macros lowered al IR.
     * Asi no colisionan con funciones runtime y son identificables
     * por el TypeChecker para invocacion desde ComptimeVM (MC.2). */
    if ((fd->is_macro && fd->is_comptime) || is_vm_comptime_fn) {
        /* @Macro, o comptime fn con asm (F1): nombre prefijado + registro en
         * el ComptimeRuntime para invocacion via VM.  El prefijo `__macro_`
         * identifica "codigo comptime lowered" (macro o fn). */
        fn.name = "__macro_" + fd->name;
        fn.is_macro_compiled = true;
        ++macro_lowered_count_;
        /* Registrar el nombre en el ComptimeRuntime para que el chequeo de
         * tipos sepa que el macro EXISTE y pueda intentar invocarlo mas
         * adelante.  La direccion todavia no se sabe -- aqui no hay bytecode --
         * asi que va el centinela: `0` no vale como marcador porque es una
         * direccion legitima (la primera funcion del artefacto vive ahi). */
        const_cast<TypeChecker &>(tc_).comptime_runtime().register_macro(
            fn.name, ComptimeRuntime::kPcUnresolved);
    } else {
        fn.name = fd->name;
    }
    // Igual que con los metodos: el vinculo se anota donde se crea el nombre.
    // Sin esto, un fallo dentro de una funcion libre salia con el nombre a
    // secas
    // -- sin firma, sin fichero -- porque el mapa del artefacto no la tenia.
    note_emitted_function(fn.name, fd->name);

    // @fp(strict|fast): politica de contraccion FMA de la funcion.  El pase
    // ir_pass_fuse_fma solo contrae si fn.fp_contract; @fp(strict) -> false.
    fn.fp_contract = fd->fp_contract;

    // AOT 2b (dev OS): seccion de salida del codigo + permisos.  Metadata
    // pura para el codegen AOT; el interp/JIT la ignoran.
    fn.section = fd->attr_section;
    fn.section_perms = fd->attr_section_perms;
    fn.section_at = fd->attr_at;
    fn.section_order = fd->attr_order;
    //  NR: @Naked -- el codegen suprime prologo/epilogo/ret.
    fn.is_naked = fd->is_naked;
    fn.no_idiom = fd->is_no_idiom;

    /* Hasta donde llega lo que se puede afirmar de ella.  De una funcion
     * privada el modulo tiene TODOS los sitios de llamada, asi que lo que
     * aportan es todo lo que le llega; de una publica puede llamarla cualquiera
     * desde otro sitio, y entonces no haberlo visto no es que no exista. */
    fn.is_public = fd->is_public;

    // Subsistema de coste (modo --analyze): propagar el contrato
    // @complexity del AST al IR.  Metadata pura -- el codegen la ignora;
    // solo la consume el analizador estatico analyze::bigo.
    fn.complexity_expr = fd->complexity_expr;
    fn.complexity_vars = fd->complexity_vars;
    fn.complexity_partial_pre = fd->complexity_partial_pre;
    fn.complexity_partial_post = fd->complexity_partial_post;
    fn.complexity_total_pre = fd->complexity_total_pre;
    fn.complexity_total_post = fd->complexity_total_post;
    // Contratos de huella (recurso/efecto): metadata para la verificacion.

    // Tipo de retorno.  Aceptamos tipos primitivos directamente o
    // pasamos por resolve_type_node para PointerTypeNode/ArrayTypeNode
    // (mapeados a IrType::PTR via ir_type_from_primitive).
    Type sem_ret = fd->return_type
                       ? tc_.resolve_type_node(fd->return_type.get())
                       : Type{PrimitiveKind::VOID};
    // sret: si la funcion declara devolver Optional<T>,
    // Result<V,E> o un enum declarado por usuario, internamente la
    // convertimos en void + un parametro hidden retbuf:ptr al inicio.
    // El callee escribe el resultado en el buffer del caller, evitando
    // heap allocation y leaks.
    const auto &elays_check = tc_.enum_layouts();
    const bool sret_enum =
        sem_ret.kind == PrimitiveKind::STRUCT &&
        elays_check.find(sem_ret.struct_name) != elays_check.end();
    // (gap O): SRET para funciones que retornan FUNCTION.  El
    // slot del function value tiene 16 bytes (fn_addr + env_addr).
    const bool sret_function = (sem_ret.kind == PrimitiveKind::FUNCTION);
    // Smart pointers: SRET de 8 bytes para `unique<T>` / `shared<T>`.
    const bool sret_smartptr = (sem_ret.kind == PrimitiveKind::UNIQUE_PTR ||
                                sem_ret.kind == PrimitiveKind::SHARED_PTR);
    // Vesta Embed (native_poo_): `string` value-type de 24 bytes -> SRET.
    const bool sret_str_value =
        (native_poo_ && sem_ret.kind == PrimitiveKind::STRING);
    // STRUCT por valor -> SRET.  Era el UNICO agregado que no lo usaba, y por
    // eso estaba roto: `return r` devolvia un PUNTERO al buffer de `r`, que
    // vive en el frame del callee -- muerto tras el `ret`.  El caller leia esa
    // memoria despues, y lo que hubiera pasado por la pila entre medias (el
    // propio restore de registros del call) la pisaba.  Funcionaba de milagro
    // cuando el caller copiaba antes de tocar la pila; con un `println` de por
    // medio, el struct llegaba a ceros (medido).
    //
    // Un `@overlay struct` NO entra: su valor ES un puntero de 8 bytes a
    // memoria ajena, asi que devolverlo por registro es correcto.
    const auto &slays_check = tc_.struct_layouts();
    auto it_slay_ret = slays_check.find(sem_ret.struct_name);
    const bool sret_struct = sem_ret.kind == PrimitiveKind::STRUCT &&
                             !sret_enum && it_slay_ret != slays_check.end() &&
                             !it_slay_ret->second.is_overlay;
    const bool sret =
        (sem_ret.kind == PrimitiveKind::OPTIONAL ||
         sem_ret.kind == PrimitiveKind::RESULT || sret_enum || sret_function ||
         sret_smartptr || sret_str_value || sret_struct);
    if (fd->return_type &&
        fd->return_type->kind == ast::NodeKind::PrimitiveTypeNode && !sret) {
        auto *pt = static_cast<ast::PrimitiveTypeNode *>(fd->return_type.get());
        fn.ret_type = ir_type_from_primitive(pt->prim);
    } else if (fd->return_type) {
        if (sret) {
            fn.ret_type = ir::IrType::VOID;
        } else {
            fn.ret_type = (sem_ret.kind != PrimitiveKind::COUNT &&
                           sem_ret.kind != PrimitiveKind::VOID)
                              ? ir_type_from_primitive(sem_ret.kind)
                              : ir::IrType::VOID;
        }
    } else {
        fn.ret_type = ir::IrType::VOID;
    }

    // Parametros: cada uno es un IrValue con is_param=true.
    std::vector<std::pair<std::string, ir::IrValueId>> param_bindings;
    param_bindings.reserve(fd->params.size() + (sret ? 1 : 0));
    // ABI custom por funcion (register("rXX") en un param): materializamos
    // param_abi_regs SOLO si al menos un param lo declara -> alineado con
    // fn.params (retbuf/vacount = "" = ABI estandar).  push_abi() lo mantiene
    // en sincronia con cada fn.params.push_back().
    bool has_custom_abi = false;
    for (const auto &pp : fd->params)
        if (pp && !pp->abi_reg.empty()) {
            has_custom_abi = true;
            break;
        }
    auto push_abi = [&](const std::string &r) {
        // Canonicalizar a 64 bits (eax->rax): el prologo del callee
        // (canon_gp_to_mreg) reconoce igual x86-64 y x86-32.  "" (ABI estandar)
        // se mantiene "".
        if (has_custom_abi)
            fn.param_abi_regs.push_back(r.empty() ? std::string()
                                                  : asm_canonical_reg(r));
    };
    // register() en param -> variable register() mutable (desugar tras el
    // entry). El param llega en su registro por la ABI custom (caller+callee);
    // la variable register() reutiliza el modelo de `cas`: STORE inicial = IN,
    // LOAD (return) = OUT read-back -> `register("rax") id; asm{syscall};
    // return id` devuelve rax POST-asm (el resultado), no el valor de entrada.
    struct CustomAbiParam {
        std::string name;
        ir::IrValueId vid;
        std::string reg;
        ir::IrType pt;
    };
    std::vector<CustomAbiParam> custom_abi_params;
    // Hidden retbuf param para sret (si aplica): primero en la lista.
    ir::IrValueId v_retbuf = ir::IR_NO_VALUE;
    if (sret) {
        v_retbuf = fn.new_value(ir::IrType::PTR, "%__retbuf");
        fn.values[v_retbuf].is_param = true;
        // BugFix sret-cross-mem (2026-06-04): SOLO marcar host_ptr para
        // SRET de Optional/Result/enum (donde el caller aloca host).
        // Para FUNCTION/smart-ptr el callee tiene su propio manejo y
        // marcarlo host rompe el copia in-place.
        // El value-string (native_poo_) vive en host stack (ALLOCA host)
        // -> su retbuf tambien es host_ptr para que las copias usen `movh`.
        // El retbuf de un agregado vive en host, como el propio agregado
        // (ver lower_var_decl): el `return` copia ahi con `movh`.
        const bool sret_optres_like =
            (sem_ret.kind == PrimitiveKind::OPTIONAL ||
             sem_ret.kind == PrimitiveKind::RESULT || sret_enum ||
             sret_str_value || sret_struct);
        if (sret_optres_like) {
            fn.values[v_retbuf].is_host_ptr = true;
        }
        fn.params.push_back(v_retbuf);
        push_abi(""); // retbuf SRET: ABI estandar (primer arg-reg)
    }
    for (auto &p : fd->params) {
        ir::IrType pt = ir::IrType::I64;
        bool param_is_class = false;
        bool param_is_host_ptr = false;
        if (p->type && p->type->kind == ast::NodeKind::PrimitiveTypeNode) {
            auto *ptn = static_cast<ast::PrimitiveTypeNode *>(p->type.get());
            pt = ir_type_from_primitive(ptn->prim);
            // Vesta Embed (native_poo_): un param `string` es value-type
            // (24 bytes); el caller pasa un PTR HOST al value-string en
            // su stack.  Marcar host_ptr para que los LOAD del callee
            // (s.length(), s.cstr(), concat operand) usen `movh` (host).
            // En Full/JIT `string` es un GcHandle i64 -> NO host_ptr.
            if (native_poo_ && ptn->prim == PrimitiveKind::STRING) {
                param_is_host_ptr = true;
            }
        } else if (p->type) {
            // Para tipos compuestos (PointerTypeNode, ArrayTypeNode,
            // NamedTypeNode resuelto) usamos el helper de tipos del
            // checker para obtener el Type semantico y mapear su kind.
            const Type sem = tc_.resolve_type_node(p->type.get());
            if (sem.kind != PrimitiveKind::COUNT &&
                sem.kind != PrimitiveKind::VOID) {
                pt = ir_type_from_primitive(sem.kind);
            }
            if (sem.kind == PrimitiveKind::CLASS) param_is_class = true;
            // Punteros raw (`T*`) y arrays (`T[]`) consultan @c is_virtual
            // del Type para decidir naturaleza del SSA value:
            //   T*               (is_virtual=false) -> host_ptr=true
            //   VirtualPtr<T>    (is_virtual=true)  -> host_ptr=false
            //   T[N] (decay)     (is_virtual=true)  -> host_ptr=false
            // Sin esta propagacion, indexar @c bdat[i] en parametros
            // emite mov (memoria VM) para tipos host -> garbage.
            if ((sem.kind == PrimitiveKind::PTR ||
                 sem.kind == PrimitiveKind::ARRAY) &&
                !sem.is_virtual) {
                param_is_host_ptr = true;
            }
            // Overlay: un valor overlay ES un puntero (host) de 8 bytes a la
            // memoria ajena.  Pasado como parametro se recibe como PTR host;
            // sin esto los accesos `v.campo`/`v.arr[i]` dentro del callee
            // emiten mov/loadz (VM) en vez de movh/loadzh -> memoria erronea.
            if (sem.kind == PrimitiveKind::STRUCT && !sem.struct_name.empty()) {
                auto ovit = tc_.struct_layouts().find(sem.struct_name);
                if (ovit != tc_.struct_layouts().end() &&
                    ovit->second.is_overlay) {
                    pt = ir::IrType::PTR;
                }
                // Un agregado se pasa por su DIRECCION y vive en memoria HOST
                // (ver lower_var_decl): struct, enum/ADT (que tambien son
                // PrimitiveKind::STRUCT) y overlay (puntero host ajeno).
                param_is_host_ptr = true;
            }
            // BugFix sret-cross-mem (2026-06-04): los parametros de
            // tipo Optional<T>/Result<V,E> son PTRs al buffer SRET
            // alocado por el caller (que ahora siempre es host_alloca).
            // Sin marcar is_host_ptr=true, isOk/value/error en el callee
            // emiten LOAD con `mov` (VM mem) en lugar de `movh` (host)
            // -> Result llega zeroed al usar dentro del callee.
            if (sem.kind == PrimitiveKind::OPTIONAL ||
                sem.kind == PrimitiveKind::RESULT) {
                param_is_host_ptr = true;
            }
            // Bug host-vs-VM (2026-07-15): la ambiguedad historica de `T[]`
            // como parametro (array dinamico host de `new T[N]` vs stack-decay
            // de un `T[N]` local, que vivia en la pila VM) ESTA CERRADA: desde
            // que @c ir_pass_promote_local_allocas promueve con force_all, todo
            // `T[N]` local es tambien host.  Con ambos origenes en host, un
            // `T[]` NO virtual es siempre una direccion host y se marca como
            // tal arriba junto con `T*`.  `VirtualPtr<T>` (is_virtual=true)
            // sigue siendo la unica forma de nombrar una direccion VM.
        }
        // Variadico CRUDO (`...` pelado): PASS-THROUGH.  El compilador NO
        // empaqueta los args -- ocupan los arg-regs del ABI segun la convencion
        // de llamada, y el cuerpo asm (para @Naked) los accede directamente. No
        // se crea binding ni __vacount: no hay array ni vacount().  Es el
        // equivalente a una `F(a, ...)` en C, que acepta N args crudos.
        if (p->is_raw_variadic) {
            continue;
        }
        // Variadico (`T... name`): el callee lo recibe como `T*` (puntero host
        // al array empaquetado por el caller), NO como T.  El count va en un
        // param i64 OCULTO que se anñade tras el loop (leido con vacount()).
        if (p->is_variadic) {
            pt = ir::IrType::PTR;
            param_is_host_ptr = true;
            param_is_class = false;
        }
        const ir::IrValueId vid = fn.new_value(pt, "%" + p->name);
        fn.values[vid].is_param = true;
        if (param_is_class) {
            fn.values[vid].is_host_ptr = true;
            fn.values[vid].is_gc_object = true;
        } else if (param_is_host_ptr) {
            fn.values[vid].is_host_ptr = true;
        }
        fn.params.push_back(vid);
        push_abi(p->abi_reg); // ABI custom del param (o "" si estandar)
        param_bindings.emplace_back(p->name, vid);
        if (!p->abi_reg.empty())
            custom_abi_params.push_back({p->name, vid, p->abi_reg, pt});
    }
    // Variadicos: param OCULTO i64 del count, tras el `T*` del ultimo param.
    // `vacount()` en el body resuelve a este binding.  (Un variadico CRUDO
    // `...` no empaqueta ni tiene count: los args pasan crudos en los
    // arg-regs.)
    if (!fd->params.empty() && fd->params.back()->is_variadic &&
        !fd->params.back()->is_raw_variadic) {
        const ir::IrValueId vcnt = fn.new_value(ir::IrType::I64, "%__vacount");
        fn.values[vcnt].is_param = true;
        fn.params.push_back(vcnt);
        push_abi(""); // count oculto de variadico: ABI estandar
        param_bindings.emplace_back("__vacount", vcnt);
    }

    // Bloque entry.
    const ir::IrBlockId entry = fn.new_block("entry");
    // Conectar el estado del lowering al de esta funcion.
    fn_ = &fn;
    current_block_ = entry;
    block_terminated_ = false;
    scopes_.clear();
    push_scope();
    for (auto &kv : param_bindings)
        bind(kv.first, kv.second);

    // register() en params: desugar a variable register() mutable ligada al
    // reg. Se hace AQUI (no en el bucle de params) porque el entry block y
    // current_block_ ya existen.  Reutiliza el modelo de las vars register()
    // (asm_reg_bindings + STORE inicial + LOAD read-back en lower_asm): el body
    // y el/los asm{} usan la variable, y `return id` lee el registro POST-asm.
    for (const auto &cp : custom_abi_params) {
        const size_t bytes = ir::type_access_bytes(cp.pt);
        const ir::IrValueId addr = fn.new_value(ir::IrType::PTR);
        ir::IrInstr ai{};
        ai.op = ir::IrOp::ALLOCA;
        ai.type = ir::IrType::I8; // unidad: 1 byte
        ai.dst = addr;
        ai.imm = static_cast<uint64_t>(bytes < 8 ? 8 : bytes);
        ai.host_alloca = true;
        fn.values[addr].is_host_ptr = true;
        ai.source_line = fd->loc.line;
        fn.append(current_block_, std::move(ai));
        const bool is_vec =
            cp.reg.rfind("xmm", 0) == 0 || cp.reg.rfind("ymm", 0) == 0 ||
            cp.reg.rfind("zmm", 0) == 0 || cp.reg.rfind("XMM", 0) == 0 ||
            cp.reg.rfind("YMM", 0) == 0 || cp.reg.rfind("ZMM", 0) == 0;
        {
            // La clase con la que se declaro: aqui el registro es CONCRETO, asi
            // que la clase es el registro mismo.  Es lo que dira su ancho a
            // quien lo pregunte despues.
            ir::AsmRegBinding b{addr, cp.reg, cp.pt, is_vec, cp.name};
            b.reg_class = cp.reg;
            fn.asm_reg_bindings.push_back(std::move(b));
        }
        // La variable vive en un ALLOCA (como cualquier var register()): marcar
        // address-taken para que read_local emita un LOAD del slot en cada uso
        // (p.ej. `return id`) en lugar de devolver la DIRECCION del alloca. Sin
        // esto, un callee standalone con param register retornaba el puntero de
        // pila (el inline lo ocultaba porque elimina el desugar).
        address_taken_locals_.insert(cp.name);
        // STORE inicial: variable register() = param (que llega en ese registro
        // por la ABI custom; el mov reg,reg resultante es no-op).
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = cp.pt;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {cp.vid, addr};
        st.source_line = fd->loc.line;
        fn.append(current_block_, std::move(st));
        // Re-bind: el body y el asm usan la VARIABLE (su ALLOCA), no el param.
        bind(cp.name, addr);
    }

    // sret: configurar el contexto de la funcion actual.  Si
    // declara devolver Optional/Result, retbuf es el primer param
    // hidden y todos los `return` copiaran al buffer del caller en
    // vez de devolver un valor.
    sret_active_ = sret;
    sret_retbuf_ = v_retbuf;
    if (sret_enum) {
        // Tamano dinamico segun el enum declarado.
        auto it_e = elays_check.find(sem_ret.struct_name);
        sret_buf_size_ = it_e != elays_check.end()
                             ? static_cast<uint64_t>(it_e->second.size_bytes)
                             : 16ULL;
    } else if (sret_function) {
        // el slot del function value es siempre 16 bytes.
        sret_buf_size_ = 16ULL;
    } else if (sret_smartptr) {
        // Smart pointer slot.  unique<T> usa Tier 1 (16 bytes: ptr + deleter).
        // shared<T> usa 8 bytes (host_ptr al control block; deleter
        // vive en el control block del GcHeap).  No tenemos forma
        // simple de discriminar aqui (sem_ret.kind UNIQUE vs SHARED);
        // usamos 16 para unique y 8 para shared.
        sret_buf_size_ =
            (sem_ret.kind == PrimitiveKind::UNIQUE_PTR) ? 16ULL : 8ULL;
    } else if (sret_str_value) {
        // value-string: {ptr,len,cap} = 3 qwords = 24 bytes.
        sret_buf_size_ = 24ULL;
    } else if (sret_struct) {
        // Tamano del struct declarado, redondeado a multiplo de 8: la copia al
        // retbuf va qword a qword (`sret_buf_size_ / 8`), asi que un struct de
        // 12 bytes copiaria solo 8 y perderia el ultimo campo.  El caller aloca
        // con ESTE mismo redondeo, asi que copiar el qword de mas es escribir
        // en su propio buffer.
        sret_buf_size_ =
            (static_cast<uint64_t>(it_slay_ret->second.size_bytes) + 7ULL) &
            ~7ULL;
    } else if (sret) {
        sret_buf_size_ = (sem_ret.kind == PrimitiveKind::OPTIONAL
                              ? (uint64_t)optional_buf_bytes(sem_ret)
                              : 24ULL);
    } else {
        sret_buf_size_ = 0ULL;
    }
    // (gap O): activar el modo "env en heap" para todos los
    // lambdas creados dentro del body de esta funcion.  Asi el env
    // sobrevive al RET y el caller puede invocar la closure sin
    // use-after-free.  Se restaura al salir de @c lower_function.
    const bool prev_returns_fn = current_fn_returns_function_;
    current_fn_returns_function_ = sret_function;
    // Para `string get_x() { return "lit"; }` -- propaga al
    // lower_return para que detecte el literal y lo promueva via
    // STRMAKE en vez de devolver el ptr crudo.
    const bool prev_returns_str = current_fn_returns_string_;
    current_fn_returns_string_ = (sem_ret.kind == PrimitiveKind::STRING);
    // native_poo_: marca que el return de `string` baja por SRET de
    // value-type (24 bytes) -> lower_return construye el value-string.
    const bool prev_sret_str_value = current_fn_sret_str_value_;
    current_fn_sret_str_value_ = sret_str_value;

    // nonnull en parametros: por cada parametro declarado con
    // `T !!name` (o `nonnull T name`), inyectamos un `unwrap` al
    // entry de la funcion.  Si el caller pasa null, la excepcion
    // NullPointerException se lanza inmediatamente con stack trace
    // apuntando al entry del callee, lo que da diagnosticos
    // tempranos en vez de fallos lejanos al primer uso del param.
    for (size_t pi = 0; pi < fd->params.size(); ++pi) {
        const auto &p = fd->params[pi];
        if (!p || !p->type || !p->type->is_nonnull) continue;
        const ir::IrValueId v_old = param_bindings[pi].second;
        const ir::IrType t_old = fn_->values[v_old].type;
        const ir::IrValueId v_new = fn_->new_value(t_old);
        // raw_asm-elim 2026-05-28: nonnull param check via IrOp::UNWRAP
        // (lanza FATAL_NULL_POINTER si src==0).  Reemplaza RAW_ASM.
        ir::IrInstr uw{};
        uw.op = ir::IrOp::UNWRAP;
        uw.type = t_old;
        uw.dst = v_new;
        uw.operands = {v_old};
        uw.source_line = p->loc.line;
        emit(current_block_, std::move(uw));
        // Re-bind: futuros usos de p->name resuelven al valor unwrapped.
        if (fn_->values[v_old].is_host_ptr) {
            fn_->values[v_new].is_host_ptr = true;
        }
        // Sustituir el binding del scope (push_scope nuevo + el viejo
        // se reemplaza re-bindeando con bind() que sobrescribe).
        bind(p->name, v_new);
    }

    // Pre-pase: identificar variables locales cuya direccion se toma con
    // '&'.  Influye en lower_var_decl (ALLOCA en lugar de SSA) y en
    // read_local / write_local (LOAD/STORE).
    address_taken_locals_.clear();
    host_bearing_locals_.clear();
    // `static` locals: mapa nombre->slot global, unico por funcion.
    static_local_slots_.clear();
    // Limpiar mapa de labels de goto (per-funcion).
    goto_labels_.clear();
    if (fd->body) scan_address_taken(fd->body.get());
    // Los params con ABI custom (register) viven en un ALLOCA (desugar mas
    // arriba, ANTES de este pre-pase).  El clear() de address_taken_locals_
    // recien borro el marcado que el desugar puso -> re-insertarlo AQUI para
    // que read_local emita un LOAD del slot en cada uso (`return id`) en vez de
    // devolver la DIRECCION del alloca (un callee retornaba el puntero de
    // pila).
    for (const auto &cp : custom_abi_params)
        address_taken_locals_.insert(cp.name);
    // fix9 - eliminados los pre-pases scan_try / scan_loops.
    // Las flags `current_fn_has_try_` y `current_fn_has_loops_` solo
    // se usaban para decidir si emitir el cleanup RAW_ASM de fix
    // / fix5.  Tras fix8 (GC stack scanning conservativo),
    // esos cleanups ya no se emiten; los handles sin roots los colecta
    // el major_gc automaticamente.  Las flags quedan declaradas pero
    // siempre false, para minimizar el delta del header (eliminarlas
    // requiere actualizar miembros que pueden estar referenciados en
    // codigo no escaneado).
    current_fn_has_try_ = false;
    current_fn_has_loops_ = false;
    current_fn_no_idiom_ = fd->is_no_idiom;
    // escape detection para colecciones primitivas: detectar
    //  locales cuyo handle se devuelve, asigna a campo o se almacena en
    //  memoria.  Los marcados quedan fuera del cleanup automatico.
    const_str_locals_.clear();
    escaping_locals_.clear();
    reassigned_locals_.clear();
    if (fd->body) scan_escaping_locals(fd->body.get());
    // Los deleters estaticos por-variable son por-funcion (los nombres de
    // variables se reusan entre funciones); limpiar al entrar a una nueva.
    unique_var_deleter_.clear();

    // CRITICO: los IDs de SSA value son POR-FUNCION; ssa_concrete_class_ (mapa
    // vid->clase concreta para devirt nativa) DEBE limpiarse entre funciones o
    // un vid de la funcion anterior (p.ej. %1 = new Square en main) colisiona
    // con un param de esta (b = %1 en total) -> devirt al tipo equivocado.
    // (Bug AOT-especifico: solo native_poo devirta clases via este mapa.)
    ssa_concrete_class_.clear();

    // limpiar el stack de cleanups (synchronized activos) al
    // entrar a una nueva funcion.  Cada funcion arranca sin cleanups;
    // las acciones se acumulan al bajar synchronized y se consumen al
    // emitir return o al cerrar el scope normalmente.
    cleanup_stack_.clear();

    // Si esta es 'main' y el modulo declara clases, insertar prologo
    // que invoca __module_init para registrarlas en el ClassRegistry
    // antes de ejecutar el cuerpo del usuario.
    if (fd->name == "main") {
        bool any_class = false;
        for (auto &decl : mod_.decls) {
            if (decl && decl->kind == ast::NodeKind::ClassDecl) {
                any_class = true;
                break;
            }
        }
        //  M6.b L.6: el root puede no declarar clases pero importar
        // alguna de un dep via `import "lib" only Counter;`.  En ese
        // caso, class_layouts() del TypeChecker contiene la clase
        // importada y necesitamos llamar a __module_init (que el merge
        // trae del dep) para registrarla en el ClassRegistry runtime.
        //
        // IMPORTANTE: filtrar las clases runtime-predefined (FatalError
        // etc.) que SIEMPRE estan en class_layouts y no requieren
        // __module_init.  Tambien filtrar clases declaradas localmente
        // en mod_.decls (ya cubiertas por el check de any_class arriba).
        if (!any_class) {
            std::unordered_set<std::string> local_class_names;
            for (auto &decl : mod_.decls) {
                if (decl && decl->kind == ast::NodeKind::ClassDecl) {
                    local_class_names.insert(
                        static_cast<const ast::ClassDecl *>(decl.get())->name);
                }
            }
            for (const auto &kv : tc_.class_layouts()) {
                if (kv.second.is_runtime_predefined) continue;
                if (local_class_names.count(kv.first)) continue;
                // Clase no-local + no-runtime = importada de un dep.
                any_class = true;
                break;
            }
        }
        // L2.2: tambien llamar __module_init si hay globals runtime
        // que requieren inicializacion (string="lit" etc.).
        //  AOT.2.b: en POO nativa NO hay ClassRegistry -> main no
        // llama a __module_init (las clases son layout estatico).
        bool need_init = any_class || !runtime_global_slots_.empty();
        if (need_init && !native_poo_) {
            ir::IrInstr call_init{};
            call_init.op = ir::IrOp::CALL;
            call_init.type = ir::IrType::VOID;
            call_init.dst = ir::IR_NO_VALUE;
            call_init.func_name = "__module_init";
            call_init.source_line = fd->loc.line;
            fn.append(current_block_, std::move(call_init));
        }
    }

    // Instrumentacion: vx_trace:enter al inicio.  Solo para funciones
    // de usuario (saltamos __module_init, __new_*, __async_*, __lambda_*,
    // __spawn_* y wrappers internos).  El bytecode VM, JIT y ports
    // heredan la instrumentacion porque vive en el IR.
    if (instrument_mode_ != "none" && instrument_mode_ != "" &&
        fd->name != "__module_init" && fd->name.compare(0, 6, "__new_") != 0 &&
        fd->name.compare(0, 8, "__async_") != 0 &&
        fd->name.compare(0, 9, "__lambda_") != 0 &&
        fd->name.compare(0, 8, "__spawn_") != 0) {
        emit_instrument_enter(fd->name, fd->loc.line);
    }

    // C-3: dentro del cuerpo de la PROPIA fn override desactivar el
    // ruteo, o un `a + b` / `str_concat(a, b)` en su body se rutearia a
    // si mismo (recursion infinita).  Se restaura al cerrar la funcion.
    const std::string saved_concat_ovr = string_concat_override_;
    const std::string saved_eq_ovr = string_eq_override_;
    if (!string_concat_override_.empty() && fd->name == string_concat_override_)
        string_concat_override_.clear();
    if (!string_eq_override_.empty() && fd->name == string_eq_override_)
        string_eq_override_.clear();

    // Cuerpo.
    if (fd->body) {
        lower_block(fd->body.get());
    }
    string_concat_override_ = saved_concat_ovr;
    string_eq_override_ = saved_eq_ovr;

    // Cerrar la funcion: si la ultima instruccion no es terminador,
    // añadir RET con valor por defecto (0) en funciones no-void, o
    // RET sin valor en void.
    if (!block_terminated_) {
        // Multihilo AOT: join-all implicito de los hilos de `spawn` en el RET
        // por caida-al-final de main (sin return explicito).
        if (native_poo_ && vx_thread_used_ && fd->name == "main") {
            ir::IrInstr jc{};
            jc.op = ir::IrOp::CALL;
            jc.type = ir::IrType::VOID;
            jc.dst = ir::IR_NO_VALUE;
            jc.func_name = "__vx_thread_join_all";
            jc.is_call_site = true;
            jc.source_line = fd->loc.line;
            fn.append(current_block_, std::move(jc));
        }
        // emitir cleanups de auto-free de colecciones antes
        // del RET implicito.  Garantiza liberacion incluso si la
        // funcion cae al final sin un return explicito.
        emit_cleanups_all();
        // Instrumentacion: vx_trace:exit antes del RET implicito.
        if (instrument_mode_ != "none" && instrument_mode_ != "" &&
            fd->name != "__module_init" &&
            fd->name.compare(0, 6, "__new_") != 0 &&
            fd->name.compare(0, 8, "__async_") != 0 &&
            fd->name.compare(0, 9, "__lambda_") != 0 &&
            fd->name.compare(0, 8, "__spawn_") != 0) {
            emit_instrument_exit(fd->name, ir::IR_NO_VALUE, fd->loc.line);
        }
        ir::IrInstr ret{};
        ret.op = ir::IrOp::RET;
        ret.type = fn.ret_type;
        // RET sintetico de caida-al-final (no proviene de un `return`
        // explicito): en @Naked el codegen NO lo materializa (el asm provee
        // ret/iretq).
        ret.ret_implicit = true;
        if (fn.ret_type != ir::IrType::VOID) {
            const ir::IrValueId zero = emit_const(fn.ret_type, 0, fd->loc.line);
            ret.operands.push_back(zero);
        }
        ret.source_line = fd->loc.line;
        fn.append(current_block_, std::move(ret));
        block_terminated_ = true;
    }

    pop_scope();
    // (gap O): restaurar el flag de "funcion retorna FUNCTION".
    current_fn_returns_function_ = prev_returns_fn;
    current_fn_returns_string_ = prev_returns_str;
    current_fn_sret_str_value_ = prev_sret_str_value;
    // Validar que todas las labels referenciadas por gotos esten
    // declaradas; si alguna se quedo sin declarar es uso de una
    // label inexistente (`goto missing_label`).
    for (const auto &kv : goto_labels_) {
        if (!kv.second.declared) {
            error_at(kv.second.first_use_loc,
                     std::string("label '") + kv.first +
                         "' usada en goto pero nunca declarada");
        }
    }
    propagate_is_gc_object_through_phis(fn);
    out.add_function(std::move(fn));
    fn_ = nullptr;
}

// Bug D fix: propagar is_gc_object a traves de todos los PHI nodes hasta
// punto fijo.  Cualquier IrValue cuya origen sea un host_ptr GC-managed
// (clase) debe heredar el flag para que save_live_regs del IR emitter
// convierta a gchandle pre-CALL (estable a evacuacion del GC) en lugar
// de pushar host_ptr crudo.  Sin esta propagacion, los PHIs de
// loops/if-merges con NULL inicial + valor real en back-edge perdian el
// flag, causando segfaults tras cualquier CALL (e.g. str_make) que
// disparara GC con head/tail vivos en regs.
void Lowering::propagate_is_gc_object_through_phis(ir::IrFunction &fn) {
    bool gc_changed = true;
    while (gc_changed) {
        gc_changed = false;
        for (auto &blk : fn.blocks) {
            for (auto &ins : blk.instrs) {
                if (ins.op != ir::IrOp::PHI) continue;
                if (ins.dst == ir::IR_NO_VALUE) continue;
                if (static_cast<size_t>(ins.dst) >= fn.values.size()) continue;
                if (fn.values[ins.dst].is_gc_object) continue;
                for (const auto &arg : ins.phi_args) {
                    if (static_cast<size_t>(arg.value) < fn.values.size() &&
                        fn.values[arg.value].is_gc_object) {
                        fn.values[ins.dst].is_gc_object = true;
                        gc_changed = true;
                        break;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------
// Statements.
// ---------------------------------------------------------------------


void Lowering::emit_zero_fill(ir::IrValueId addr, uint64_t size_bytes,
                              uint32_t line) {
    // Emite STORE 0 en trozos decrecientes (8/4/2/1) sin desbordar el rango.
    auto store_zero = [&](ir::IrType ty, uint64_t o) {
        ir::IrValueId v_addr = addr;
        if (o > 0) {
            ir::IrValueId v_off = emit_const(ir::IrType::I64, o, line);
            v_addr = fn_->new_value(ir::IrType::PTR);
            // Heredar la naturaleza host/VM de la base (AOT native_poo aloca el
            // struct en la pila NATIVA -> el STORE debe ir a host, no a
            // vm_mem).
            fn_->values[v_addr].is_host_ptr = fn_->values[addr].is_host_ptr;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_addr;
            ad.operands = {addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        ir::IrValueId v_zero = emit_const(ty, 0, line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ty;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_zero, v_addr};
        st.source_line = line;
        emit(current_block_, std::move(st));
    };
    /* A partir de cierto tamano se EMITE EL HECHO (`memset`) en vez de
     * desplegarlo.  Desplegar destruye la semantica "esta region se pone a
     * cero" y ningun nivel inferior puede reconstruirla: medido, `i32[8192]
     * arr;` -- una DECLARACION -- generaba 16397 instrucciones, 86 KB de
     * codigo y 1,7 s de compilacion, con un solo bloque basico de 16405
     * instrucciones que hacia estallar el scheduler (O(n^2) por bloque).
     * Crecia lineal (~2n+13) SIN umbral, a cualquier tamano.
     *
     * Por debajo del umbral se siguen emitiendo stores: para unos pocos bytes
     * "store 0" ES la forma optima y no se pierde nada -- ningun backend
     * tendria algo mejor que hacer con la informacion.  El umbral es de FORMA,
     * no de politica: quien decide COMO rellenar (bucle, `rep stosb`, SIMD, o
     * el `memset` que el programa haya puesto en su lugar) es el backend. */
    static const uint64_t kInlineZeroMax = 64; // bytes desplegados en linea.
    if (size_bytes > kInlineZeroMax) {
        ir::IrValueId v_val = emit_const(ir::IrType::I64, 0, line);
        ir::IrValueId v_len = emit_const(ir::IrType::I64, size_bytes, line);
        ir::IrInstr ms{};
        ms.op = ir::IrOp::MEMSET;
        ms.type = ir::IrType::VOID;
        ms.dst = ir::IR_NO_VALUE;
        ms.operands = {addr, v_val, v_len};
        ms.source_line = line;
        emit(current_block_, std::move(ms));
        return;
    }

    uint64_t off = 0;
    while (size_bytes - off >= 8) {
        store_zero(ir::IrType::I64, off);
        off += 8;
    }
    if (size_bytes - off >= 4) {
        store_zero(ir::IrType::I32, off);
        off += 4;
    }
    if (size_bytes - off >= 2) {
        store_zero(ir::IrType::I16, off);
        off += 2;
    }
    if (size_bytes - off >= 1) {
        store_zero(ir::IrType::I8, off);
        off += 1;
    }
}

// =========================================================================
//  Transcodificacion de literales en tiempo de compilacion
// =========================================================================

bool Lowering::transcode_literal(const std::string &utf8, int enc,
                                 std::vector<uint8_t> &out) {
    out.clear();
    // ENC_ANSI (1) NO se pliega: la codepage es del sistema donde se EJECUTA,
    // no donde se compila.  Plegarlo produciria bytes correctos solo en las
    // maquinas que compartan codepage con la de compilacion.
    if (enc == 1) return false;

    // Decodificar la forma canonica (UTF-8) a code points.
    std::vector<uint32_t> cps;
    cps.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        const uint8_t b = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = 0;
        size_t n = 1;
        if ((b & 0x80) == 0) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1Fu;
            n = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0Fu;
            n = 3;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07u;
            n = 4;
        } else {
            return false; // byte inicial invalido
        }
        if (i + n > utf8.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const uint8_t c = static_cast<uint8_t>(utf8[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        cps.push_back(cp);
        i += n;
    }

    switch (enc) {
    case 0: // ENC_ASCII: solo representable si TODO cae por debajo de 0x80.
        for (uint32_t cp : cps) {
            if (cp >= 0x80) return false;
            out.push_back(static_cast<uint8_t>(cp));
        }
        out.push_back(0);
        return true;
    case 2: // ENC_UTF8: la forma canonica ya lo es.
        out.assign(utf8.begin(), utf8.end());
        out.push_back(0);
        return true;
    case 3: {
        // ENC_UTF16 (LE), con pares sustitutos.
        auto put16 = [&out](uint16_t u) {
            out.push_back(static_cast<uint8_t>(u & 0xFFu));
            out.push_back(static_cast<uint8_t>((u >> 8) & 0xFFu));
        };
        for (uint32_t cp : cps) {
            if (cp < 0x10000u) {
                put16(static_cast<uint16_t>(cp));
            } else {
                const uint32_t v = cp - 0x10000u;
                put16(static_cast<uint16_t>(0xD800u + (v >> 10)));
                put16(static_cast<uint16_t>(0xDC00u + (v & 0x3FFu)));
            }
        }
        put16(0);
        return true;
    }
    case 4: // ENC_UTF32 (LE).
        for (uint32_t cp : cps) {
            out.push_back(static_cast<uint8_t>(cp & 0xFFu));
            out.push_back(static_cast<uint8_t>((cp >> 8) & 0xFFu));
            out.push_back(static_cast<uint8_t>((cp >> 16) & 0xFFu));
            out.push_back(static_cast<uint8_t>((cp >> 24) & 0xFFu));
        }
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        return true;
    default: return false;
    }
}

ir::IrValueId Lowering::emit_folded_string_blob(const std::string &utf8,
                                                int enc, uint32_t line) {
    std::vector<uint8_t> bytes;
    if (!transcode_literal(utf8, enc, bytes)) return ir::IR_NO_VALUE;

    const auto key = std::make_pair(utf8, enc);
    auto it = folded_str_blobs_.find(key);
    uint64_t slot;
    if (it != folded_str_blobs_.end()) {
        slot = it->second;
    } else {
        // push_back directo (no intern): el intern deduplica POR CONTENIDO y
        // podria devolver un slot ya existente de la seccion `data` (memoria
        // VM), que al marcarlo host romperia a quien lo use como direccion VM.
        slot = static_cast<uint64_t>(
            out_mod_->static_data.push_back(bytes.data(), bytes.size()));
        auto &m = out_mod_->static_data.meta_at(slot);
        m.flags |= ir::IrModule::SD_FLAG_NON_DEDUP;
        // `.data` es lo que enruta el slot a la seccion `gdata` (memoria HOST):
        // el blob tiene que ser direccionable por una API nativa.
        m.section_name = ".data";
        // UTF-16 y UTF-32 exigen alineacion propia para leerse como u16/u32.
        m.alignment = (enc == 3) ? 2 : ((enc == 4) ? 4 : 1);
        folded_str_blobs_[key] = slot;
    }

    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    ir::IrInstr is{};
    is.op = ir::IrOp::STR_LIT_ADDR;
    is.type = ir::IrType::PTR;
    is.dst = v;
    is.imm = slot;
    is.source_line = line;
    emit(current_block_, std::move(is));
    fn_->values[v].is_host_ptr = true; // gdata vive en memoria host
    return v;
}

void Lowering::emit_struct_field_defaults(ir::IrValueId base_addr,
                                          const StructLayout &lay,
                                          uint32_t line,
                                          bool only_non_comptime) {
    for (const auto &fi : lay.fields) {
        // Filtro: saltar los defaults que la imagen comptime ya trae.
        if (only_non_comptime && fi.default_init && fi.bit_width == 0) {
            const ComptimeEvalResult dv =
                comptime_eval_expr(tc_, fi.default_init);
            if (dv.ok && !dv.deferred && !dv.is_str && !dv.is_array &&
                !dv.is_struct && !dv.is_type)
                continue;
        }
        // Direccion del campo (base + offset), heredando naturaleza host/VM.
        auto field_addr = [&]() -> ir::IrValueId {
            if (fi.offset == 0) return base_addr;
            ir::IrValueId v_off =
                emit_const(ir::IrType::I64, (uint64_t)fi.offset, line);
            ir::IrValueId v_a = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_a].is_host_ptr = fn_->values[base_addr].is_host_ptr;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_a;
            ad.operands = {base_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
            return v_a;
        };
        if (!fi.default_init) {
            // Campo struct anidado SIN default propio: si su TIPO declara
            // defaults, aplicarlos recursivamente en la sub-direccion.
            if (fi.type.kind == PrimitiveKind::STRUCT) {
                auto it = tc_.struct_layouts().find(fi.type.struct_name);
                if (it != tc_.struct_layouts().end()) {
                    bool any_def = false;
                    for (const auto &sf : it->second.fields)
                        if (sf.default_init) {
                            any_def = true;
                            break;
                        }
                    if (any_def)
                        emit_struct_field_defaults(field_addr(), it->second,
                                                   line);
                }
            }
            continue;
        }
        // Campo escalar con default comptime-constante: lower + STORE.
        ir::IrValueId v_val = lower_expr(fi.default_init);
        if (v_val == ir::IR_NO_VALUE) continue;
        const ir::IrType ir_ft = ir_type_from_primitive(fi.type.kind);
        v_val = cast_if_needed(v_val, fn_->values[v_val].type, ir_ft, line,
                               /*is_explicit=*/true);
        ir::IrValueId v_addr = field_addr();
        if (fi.bit_width > 0) {
            // Bit field con default: por ahora se ignora (raro); el zero-fill
            // deja el campo a 0.  Un default de bit field requeriria RMW.
            continue;
        }
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir_ft;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_addr};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
}

ir::IrValueId Lowering::materialize_comptime_struct(const ComptimeEvalResult &r,
                                                    const StructLayout &lay,
                                                    uint32_t line) {
    // Alocar el buffer del struct en memoria host (es un value-type).
    const uint64_t buf_bytes =
        (static_cast<uint64_t>(lay.size_bytes) + 7ULL) & ~7ULL;
    const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
    ir::IrInstr al{};
    al.op = ir::IrOp::ALLOCA;
    al.type = ir::IrType::I8;
    al.imm = buf_bytes;
    al.dst = v_buf;
    al.host_alloca = true;
    al.source_line = line;
    emit(current_block_, std::move(al));
    fn_->values[v_buf].is_host_ptr = true;
    fill_comptime_struct_into(v_buf, r, lay, line);
    return v_buf;
}

void Lowering::fill_comptime_struct_into(ir::IrValueId base_addr,
                                         const ComptimeEvalResult &r,
                                         const StructLayout &lay,
                                         uint32_t line) {
    for (const auto &fi : lay.fields) {
        auto it = r.struct_fields.find(fi.name);
        if (it == r.struct_fields.end() || !it->second) continue;
        const ComptimeValue &cv = *it->second;
        // Direccion del campo (base + offset), heredando la naturaleza host/VM.
        ir::IrValueId v_addr = base_addr;
        if (fi.offset != 0) {
            const ir::IrValueId v_off =
                emit_const(ir::IrType::I64, (uint64_t)fi.offset, line);
            v_addr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_addr].is_host_ptr =
                fn_->values[base_addr].is_host_ptr;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_addr;
            ad.operands = {base_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        if (fi.type.kind == PrimitiveKind::STRUCT && cv.is_struct) {
            // Campo struct anidado: rellenar recursivamente en su direccion.
            auto its = tc_.struct_layouts().find(fi.type.struct_name);
            if (its != tc_.struct_layouts().end()) {
                const ComptimeEvalResult sub = result_from_value(cv);
                fill_comptime_struct_into(v_addr, sub, its->second, line);
            }
            continue;
        }
        // Campo escalar: constante + STORE en la direccion del campo.
        const ir::IrType ir_ft = ir_type_from_primitive(fi.type.kind);
        const ir::IrValueId v_val = emit_const(ir_ft, (uint64_t)cv.value, line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir_ft;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_addr};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
}

void Lowering::emit_struct_init_fields(ir::IrValueId base_addr,
                                       const StructLayout &lay,
                                       ast::InitListExpr *il, uint32_t line) {
    // Aplicar primero los valores por defecto de los campos; el init-list
    // explicito de abajo sobrescribe los campos que liste (DSE limpia lo
    // muerto).
    emit_struct_field_defaults(base_addr, lay, line);
    for (size_t i = 0; i < il->elements.size(); ++i) {
        const StructFieldInfo *fi = nullptr;
        if (il->is_designated) {
            const std::string &fname = il->field_names[i];
            for (const auto &f : lay.fields) {
                if (f.name == fname) {
                    fi = &f;
                    break;
                }
            }
            if (!fi) {
                error_at(il->loc, "lowering: campo '" + fname +
                                      "' no existe en struct '" + lay.name +
                                      "'");
                continue;
            }
        } else {
            if (i >= lay.fields.size()) {
                error_at(il->loc,
                         "lowering: init list excede campos del struct");
                break;
            }
            fi = &lay.fields[i];
        }
        // Direccion del campo destino (base + offset).
        ir::IrValueId v_addr = base_addr;
        if (fi->offset > 0) {
            ir::IrValueId v_off =
                emit_const(ir::IrType::I64, (uint64_t)fi->offset, line);
            v_addr = fn_->new_value(ir::IrType::PTR);
            // `base + off` sigue apuntando a la MISMA memoria que `base`: la
            // naturaleza (host / VM) se hereda.  Sin esto, un struct en host
            // inicializado con una init-list anidada escribia sus campos con
            // `mov` (VM) sobre una direccion host -> basura.
            fn_->values[v_addr].is_host_ptr =
                fn_->values[base_addr].is_host_ptr;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_addr;
            ad.operands = {base_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        ast::Expr *elem = il->elements[i].get();
        // Campo de tipo STRUCT inicializado con un init-list ANIDADO
        // (`{.min = {.x=.., .y=..}}` o `{.min = Punto{...}}`): se rellena
        // RECURSIVAMENTE in-place en la direccion del campo.  lower_expr no
        // baja un InitListExpr como valor, por eso hay que tratarlo aqui.
        if (fi->type.kind == PrimitiveKind::STRUCT &&
            elem->kind == ast::NodeKind::InitListExpr) {
            auto it_sl = tc_.struct_layouts().find(fi->type.struct_name);
            if (it_sl == tc_.struct_layouts().end()) {
                error_at(il->loc, "lowering: struct '" + fi->type.struct_name +
                                      "' sin layout (init anidado)");
                continue;
            }
            emit_struct_init_fields(v_addr, it_sl->second,
                                    static_cast<ast::InitListExpr *>(elem),
                                    line);
            continue;
        }
        ir::IrValueId v_val = lower_expr(elem);
        if (v_val == ir::IR_NO_VALUE) continue;
        // Campo AGREGADO inline (struct/array) desde una EXPRESION (otra
        // variable, llamada, ...): copia memberwise desde la direccion origen
        // (no un STORE escalar, que guardaria la direccion como puntero).
        // Un campo de tipo `@overlay struct` NO es un agregado inline: guarda
        // el HANDLE de la vista (8 bytes) -> STORE escalar del puntero (abajo).
        if ((fi->type.kind == PrimitiveKind::STRUCT &&
             !type_is_overlay(fi->type)) ||
            fi->type.kind == PrimitiveKind::ARRAY) {
            uint64_t sz = size_of_type(fi->type);
            if (sz == 0 && fi->type.kind == PrimitiveKind::STRUCT) {
                auto it_sl = tc_.struct_layouts().find(fi->type.struct_name);
                if (it_sl != tc_.struct_layouts().end())
                    sz = (uint64_t)it_sl->second.size_bytes;
            }
            if (sz == 0) sz = 8;
            emit_memberwise_copy(v_addr, v_val, sz, line);
            if (fi->type.kind == PrimitiveKind::STRUCT) {
                auto it_sl = tc_.struct_layouts().find(fi->type.struct_name);
                if (it_sl != tc_.struct_layouts().end() &&
                    it_sl->second.has_copy_hook) {
                    emit_struct_method_on_host_field(
                        v_addr, fi->type.struct_name,
                        fi->type.struct_name + "____clone__", line);
                }
            }
            continue;
        }
        const ir::IrType ir_ft = ir_type_from_primitive(fi->type.kind);
        const bool elem_is_literal =
            elem->kind == ast::NodeKind::IntLitExpr ||
            elem->kind == ast::NodeKind::FloatLitExpr ||
            elem->kind == ast::NodeKind::BoolLitExpr ||
            elem->kind == ast::NodeKind::CharLitExpr ||
            elem->kind == ast::NodeKind::NullLitExpr;
        v_val = cast_if_needed(v_val, fn_->values[v_val].type, ir_ft, line,
                               /*is_explicit=*/elem_is_literal);
        if (fi->bit_width > 0) {
            error_at(il->loc, "lowering: init list no soporta bit fields aun");
            continue;
        }
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir_ft;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_addr};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
}

void Lowering::lower_static_local(ast::VarDeclStmt *vd, const Type &sem_type) {
    // Nombre unico por funcion: dos wrappers con `static ctx` no colisionan.
    const std::string fn_name = fn_ ? fn_->name : std::string("?");
    const std::string mangled = fn_name + "$static$" + vd->name;
    const bool aggregate = (sem_type.kind == PrimitiveKind::STRUCT ||
                            sem_type.kind == PrimitiveKind::ARRAY);
    uint64_t nbytes = static_cast<uint64_t>(size_of_type(sem_type));
    if (nbytes < 8) nbytes = 8; // minimo un qword
    const uint64_t slot = get_or_create_runtime_global_slot(mangled, nbytes);
    const ir::IrType ld =
        aggregate ? ir::IrType::PTR : ir_type_from_primitive(sem_type.kind);
    static_local_slots_[vd->name] = {slot, ld, aggregate};

    // Sin init: el slot ya es zero-init (get_or_create_runtime_global_slot).
    if (!vd->init) return;

    // Init cero constante: el slot ya vale 0 -> nada que emitir.
    if (!aggregate && vd->init->kind == ast::NodeKind::IntLitExpr &&
        static_cast<const ast::IntLitExpr *>(vd->init.get())->value == 0) {
        return;
    }

    // Agregado (struct): el init-once emite los valores por defecto de los
    // campos en el slot.  Un array o un struct sin layout queda zero-init.
    const StructLayout *agg_lay = nullptr;
    if (aggregate) {
        if (sem_type.kind != PrimitiveKind::STRUCT)
            return; // array -> zero-init
        auto it_l = tc_.struct_layouts().find(sem_type.struct_name);
        if (it_l == tc_.struct_layouts().end()) return;
        agg_lay = &it_l->second;
    }

    // Init-once: un booleano global guarda si ya se corrio el init.  La
    // PRIMERA ejecucion de la funcion baja el init y marca done=1; las
    // siguientes lo saltan -> estado persistente entre llamadas.
    const uint64_t done_slot =
        get_or_create_runtime_global_slot(mangled + "$done", 8);
    const int ln = vd->loc.line;

    auto emit_addr = [&](uint64_t s) -> ir::IrValueId {
        ir::IrValueId a = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr is{};
        is.op = ir::IrOp::STR_LIT_ADDR;
        is.type = ir::IrType::PTR;
        is.dst = a;
        is.imm = s;
        is.source_line = ln;
        emit(current_block_, std::move(is));
        fn_->values[a].is_host_ptr = true; // gdata vive en memoria host
        return a;
    };

    // done_val = LOAD i64 [done_slot]
    const ir::IrValueId addr_done = emit_addr(done_slot);
    const ir::IrValueId done_val = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr l{};
        l.op = ir::IrOp::LOAD;
        l.type = ir::IrType::I64;
        l.dst = done_val;
        l.operands = {addr_done};
        l.source_line = ln;
        emit(current_block_, std::move(l));
    }
    // cond = (done_val == 0)
    const ir::IrValueId zero = emit_const(ir::IrType::I64, 0, ln);
    const ir::IrValueId cond = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr c{};
        c.op = ir::IrOp::CMP_EQ;
        c.type = ir::IrType::BOOL;
        c.dst = cond;
        c.operands = {done_val, zero};
        c.source_line = ln;
        emit(current_block_, std::move(c));
    }
    const ir::IrBlockId init_bb = fn_->new_block("static_init");
    const ir::IrBlockId cont_bb = fn_->new_block("static_cont");
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands.push_back(cond);
        br.target_block = init_bb;
        br.false_block = cont_bb;
        br.source_line = ln;
        emit(current_block_, std::move(br));
    }
    fn_->blocks[current_block_].succs.push_back(init_bb);
    fn_->blocks[current_block_].succs.push_back(cont_bb);
    fn_->blocks[init_bb].preds.push_back(current_block_);
    fn_->blocks[cont_bb].preds.push_back(current_block_);

    // init_bb: correr el init -> STORE al slot + STORE done=1 -> BR cont.
    current_block_ = init_bb;
    block_terminated_ = false;
    if (aggregate) {
        // Struct: zero-fill + init.
        const ir::IrValueId var_addr = emit_addr(slot);
        emit_zero_fill(var_addr, static_cast<uint64_t>(agg_lay->size_bytes),
                       ln);
        // El inicializador (p.ej. un ctor comptime `T(...)`) se DESCARTABA:
        // el slot solo recibia los defaults, asi que el ctor no se aplicaba
        // nunca a un `static`.  Ahora se baja y se copia su imagen; despues se
        // emiten SOLO los defaults que esa imagen no puede llevar (una
        // referencia a funcion necesita una direccion resuelta al enlazar).
        // `T()` sobre un struct SIN ningun constructor que case es
        // value-init, no una llamada: bajarlo emitiria una CALL a un simbolo
        // inexistente (`code.T`).  En ese caso basta con los defaults.
        bool init_is_bare_value_init = false;
        if (vd->init->kind == ast::NodeKind::CallExpr) {
            auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
            if (ce->callee && ce->callee->kind == ast::NodeKind::IdentExpr &&
                static_cast<ast::IdentExpr *>(ce->callee.get())->name ==
                    agg_lay->name) {
                bool tiene_ctor = false;
                for (const auto &m : agg_lay->methods)
                    if (m.is_constructor &&
                        m.param_types.size() == ce->args.size()) {
                        tiene_ctor = true;
                        break;
                    }
                init_is_bare_value_init = !tiene_ctor;
            }
        }
        const ir::IrValueId v_src = init_is_bare_value_init
                                        ? ir::IR_NO_VALUE
                                        : lower_expr(vd->init.get());
        if (v_src != ir::IR_NO_VALUE) {
            const bool src_is_host = fn_->values[v_src].is_host_ptr;
            const uint64_t qwords =
                (static_cast<uint64_t>(agg_lay->size_bytes) + 7) / 8;
            for (uint64_t qi = 0; qi < qwords; ++qi) {
                const ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, static_cast<int64_t>(qi * 8), ln);
                const ir::IrValueId v_s = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_s].is_host_ptr = src_is_host;
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = v_s;
                    ad.operands = {v_src, v_off};
                    ad.source_line = ln;
                    emit(current_block_, std::move(ad));
                }
                const ir::IrValueId v_w = fn_->new_value(ir::IrType::I64);
                {
                    ir::IrInstr l2{};
                    l2.op = ir::IrOp::LOAD;
                    l2.type = ir::IrType::I64;
                    l2.dst = v_w;
                    l2.operands = {v_s};
                    l2.source_line = ln;
                    emit(current_block_, std::move(l2));
                }
                const ir::IrValueId v_d = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_d].is_host_ptr = true; // gdata = memoria host
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = v_d;
                    ad.operands = {var_addr, v_off};
                    ad.source_line = ln;
                    emit(current_block_, std::move(ad));
                }
                {
                    ir::IrInstr st2{};
                    st2.op = ir::IrOp::STORE;
                    st2.type = ir::IrType::I64;
                    st2.operands = {v_w, v_d};
                    st2.source_line = ln;
                    emit(current_block_, std::move(st2));
                }
            }
            emit_struct_field_defaults(var_addr, *agg_lay, ln,
                                       /*only_non_comptime=*/true);
        } else {
            emit_struct_field_defaults(var_addr, *agg_lay, ln);
        }
        if (agg_lay->is_polymorphic)
            emit_struct_vptr_init(var_addr, *agg_lay, ln);
    } else {
        const ir::IrValueId iv = lower_expr(vd->init.get());
        if (iv != ir::IR_NO_VALUE) {
            const ir::IrValueId var_addr = emit_addr(slot);
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ld;
            st.operands = {iv, var_addr};
            st.source_line = ln;
            emit(current_block_, std::move(st));
        }
    }
    {
        const ir::IrValueId one = emit_const(ir::IrType::I64, 1, ln);
        const ir::IrValueId addr_done2 = emit_addr(done_slot);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {one, addr_done2};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    }
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR;
        br.target_block = cont_bb;
        br.source_line = ln;
        emit(current_block_, std::move(br));
    }
    fn_->blocks[init_bb].succs.push_back(cont_bb);
    fn_->blocks[cont_bb].preds.push_back(init_bb);

    current_block_ = cont_bb;
    block_terminated_ = false;
}

void Lowering::lower_var_decl(ast::VarDeclStmt *vd) {
    // Propagacion de literal para el plegado de str_cstr/str_wstr: una
    // `const string p = "x";` deja el texto accesible por nombre.  Solo const
    // (no se puede reasignar); si el nombre se redeclara con otro texto se
    // descarta, para no equivocarse con el sombreado entre ambitos.
    if (vd && vd->is_const && vd->init &&
        vd->init->kind == ast::NodeKind::StringLitExpr &&
        !static_cast<ast::StringLitExpr *>(vd->init.get())->is_interpolated()) {
        const Type vt = vd->type ? tc_.resolve_type_node(vd->type.get())
                                 : vd->init->result_type;
        if (vt.kind == PrimitiveKind::STRING) {
            const std::string &txt =
                static_cast<ast::StringLitExpr *>(vd->init.get())->value;
            auto it = const_str_locals_.find(vd->name);
            if (it != const_str_locals_.end() && it->second != txt) {
                const_str_locals_.erase(it); // redeclarado: ambiguo
            } else {
                const_str_locals_[vd->name] = txt;
            }
        }
    }

    // Resolver el Type semantico (aplicando aliases y structs).
    // A.43.7: con `auto`/`var` (vd->infer_type), el AST no tiene
    // TypeNode -> el type checker ya computo y guardo el tipo en
    // `vd->init->result_type` durante check_expr.  Lo reusamos sin
    // re-evaluar el init.
    Type sem_type = vd->type ? tc_.resolve_type_node(vd->type.get())
                             : (vd->init ? vd->init->result_type : Type{});

    // `static T x = init;` local: duracion estatica (gdata) + init-once.
    // Se desvia por completo del camino ALLOCA (stack).
    if (vd->is_static) {
        lower_static_local(vd, sem_type);
        return;
    }

    // `T c = T();` constructor por defecto de struct (sin ctor declarado, 0
    // args): equivale a `T c;` -- cada campo toma su valor por defecto.  Se
    // descarta el init para que el camino de struct sin init emita los
    // defaults (emit_struct_field_defaults).  No aplica a agregados static
    // (los maneja lower_static_local con su propio init-once).
    if (sem_type.kind == PrimitiveKind::STRUCT && vd->init &&
        vd->init->kind == ast::NodeKind::CallExpr &&
        static_cast<ast::CallExpr *>(vd->init.get())->is_default_struct_ctor) {
        vd->init.reset();
    }

    // Overlay F1: `PEB peb = PEB(ptr);`.  Un overlay ES un puntero (la vista);
    // NO se aloca buffer.  Bajamos el init (que produce el puntero base host) y
    // bindeamos la variable directamente a ese valor.  El acceso a campos
    // (lower_field_access) reusa el camino de struct (base + offset + LOAD) con
    // is_host_ptr=true -> loads/stores host.
    if (sem_type.kind == PrimitiveKind::STRUCT) {
        auto it = tc_.struct_layouts().find(sem_type.struct_name);
        if (it != tc_.struct_layouts().end() && it->second.is_overlay) {
            if (!vd->init) {
                error_at(vd->loc, "un overlay '" + sem_type.struct_name +
                                      "' requiere un puntero base en su "
                                      "declaracion");
                return;
            }
            ir::IrValueId base = lower_expr(vd->init.get());
            if (base == ir::IR_NO_VALUE) return;
            fn_->values[base].is_host_ptr = true;
            bind(vd->name, base);
            return;
        }
    }

    //  Z.6: propagar el modificador @c shared del var-decl al
    // @c NewExpr del init.  Si el init es `new T(...)` y el var-decl
    // tiene `shared`, el `new` debe alocar en el SharedHeap en lugar
    // del gc_heap local.  El @c lower_new_expr detecta la marca y
    // emite `__new_<Class>_shared` (que internamente usa @c newobjs).
    if (vd->is_shared && vd->init && vd->init->kind == ast::NodeKind::NewExpr) {
        auto *ne = static_cast<ast::NewExpr *>(vd->init.get());
        ne->is_shared = true;
        // Registrar la clase como usada en modo shared para que
        // generate_new_helpers genere su variante `__new_<X>_shared`.
        if (!ne->class_name.empty()) {
            classes_used_shared_.insert(ne->class_name);
        }
    }

    //  Z.9: si el var-decl tiene `shared`, registrar el nombre en
    // @c shared_locals_ para que el escape analyzer en spawn capture
    // no genere warning (es shared explicitamente).
    if (vd->is_shared) {
        shared_locals_.insert(vd->name);
    }

    // gc<T> opt-in: si el var-decl es `gc<Class>` (sem_type.gc_managed) y el
    // init es `new Class(...)`, marcar el NewExpr para que el lowering despache
    // a __new_<Class>_gc (vx_gc_alloc) y registrar la clase para generar ese
    // helper.  El valor es un host_ptr GC-managed (marcado is_gc_object); NO se
    // registra cleanup RAII (el GC colecta).
    if (native_poo_ && sem_type.gc_managed && vd->init &&
        vd->init->kind == ast::NodeKind::NewExpr) {
        auto *ne = static_cast<ast::NewExpr *>(vd->init.get());
        ne->is_gc = true;
        if (!ne->class_name.empty()) classes_used_gc_.insert(ne->class_name);
    }

    //  AS inc.3: si el var-decl tiene storage-class register("reg"),
    // forzar el camino ALLOCA (slot estable) marcando el nombre como
    // address-taken.  Sin esto, un primitivo register-bound se baja a un
    // SSA value efimero que el optimizer pliega/elimina (el body asm es
    // una string opaca que no referencia SSA values).  Con el ALLOCA + el
    // INLINE_ASM listandolo como operando (op no-safe -> escapa), el slot
    // sobrevive y el backend lo cablea al registro fisico.  El registro
    // real en @c fn_->asm_reg_bindings se hace en la rama ALLOCA de abajo.
    if (!vd->reg_binding.empty()) {
        address_taken_locals_.insert(vd->name);
    }

    // Tracking para fix #1 newInstance: si el tipo declarado es alias
    // `Class` y el init es `Class.forName("X")` con X literal, registrar
    // var_name -> "X" para que `cls.newInstance()` luego pueda emitir
    // `new X()` directo (con ctor invocado).  Detectamos via
    // FieldAccessExpr con property_kind=100 (forName) que el type
    // checker ya marco.
    if (vd->type && vd->type->kind == ast::NodeKind::NamedTypeNode) {
        const auto *nt =
            static_cast<const ast::NamedTypeNode *>(vd->type.get());
        const bool is_class_alias = (nt->name == "Class");
        if (is_class_alias && vd->init &&
            vd->init->kind == ast::NodeKind::CallExpr) {
            auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
            if (ce->callee &&
                ce->callee->kind == ast::NodeKind::FieldAccessExpr) {
                auto *fa =
                    static_cast<ast::FieldAccessExpr *>(ce->callee.get());
                // property_kind 100 = forName (estatico, sin self).
                if (fa->property_kind == 100 && ce->args.size() == 1 &&
                    ce->args[0] &&
                    ce->args[0]->kind == ast::NodeKind::StringLitExpr) {
                    auto *slit =
                        static_cast<ast::StringLitExpr *>(ce->args[0].get());
                    if (!slit->is_interpolated()) {
                        class_origin_of_local_[vd->name] = slit->value;
                    }
                }
            }
        } else if (is_class_alias) {
            // Init no-trackeable -> borrar entrada previa por seguridad.
            class_origin_of_local_.erase(vd->name);
        }
    }

    // Array init C-style: `i32 arr[N] = {e0, e1, ...};`.
    if (sem_type.kind == PrimitiveKind::ARRAY && vd->init &&
        vd->init->kind == ast::NodeKind::InitListExpr) {
        auto *il = static_cast<ast::InitListExpr *>(vd->init.get());
        if (il->is_designated) {
            error_at(vd->loc,
                     "lowering: init designado '.field=' no aplica a arrays");
            return;
        }
        const Type elem_t = sem_type.pointee ? *sem_type.pointee : Type{};
        const uint32_t elem_sz = (uint32_t)primitive_size_bytes(elem_t.kind);
        if (elem_sz == 0) {
            error_at(vd->loc, "lowering: tipo del elemento sin sizeof");
            return;
        }
        const uint32_t arr_size = sem_type.array_size > 0
                                      ? (uint32_t)sem_type.array_size
                                      : (uint32_t)il->elements.size();
        if (il->elements.size() > arr_size) {
            error_at(vd->loc, "lowering: init list excede tamano de array");
            return;
        }
        ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = addr;
        al.imm = (uint64_t)arr_size * elem_sz;
        al.source_line = vd->loc.line;
        /* Buffer en memoria HOST, como en las demas rutas de array local:
         * todo lo que lo consume (`a` decaido a `T*`, `&a[i]`, la funcion que
         * lo recibe) emite accesos de host, asi que dejarlo en la pila de la
         * VM mata el proceso en cuanto se recorre. */
        al.host_alloca = true;
        fn_->values[addr].is_host_ptr = true;
        emit(current_block_, std::move(al));
        const ir::IrType ir_elem = ir_type_from_primitive(elem_t.kind);
        for (size_t i = 0; i < il->elements.size(); ++i) {
            ir::IrValueId v_val = lower_expr(il->elements[i].get());
            if (v_val == ir::IR_NO_VALUE) continue;
            // Suprimir warning de narrowing si el elemento es literal
            // (`{10, 20, ...}` con i64-defaulted literals encajando en
            // el tipo de elemento).  Mismo razonamiento que en
            // var-decl con init literal.
            const bool elem_is_literal =
                il->elements[i]->kind == ast::NodeKind::IntLitExpr ||
                il->elements[i]->kind == ast::NodeKind::FloatLitExpr ||
                il->elements[i]->kind == ast::NodeKind::BoolLitExpr ||
                il->elements[i]->kind == ast::NodeKind::CharLitExpr ||
                il->elements[i]->kind == ast::NodeKind::NullLitExpr;
            v_val = cast_if_needed(v_val, fn_->values[v_val].type, ir_elem,
                                   vd->loc.line,
                                   /*is_explicit=*/elem_is_literal);
            ir::IrValueId v_addr_i = addr;
            if (i > 0) {
                ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, (uint64_t)(i * elem_sz), vd->loc.line);
                v_addr_i = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_addr_i;
                ad.operands = {addr, v_off};
                ad.source_line = vd->loc.line;
                emit(current_block_, std::move(ad));
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir_elem;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_val, v_addr_i};
            st.source_line = vd->loc.line;
            emit(current_block_, std::move(st));
        }
        bind(vd->name, addr);
        return;
    }

    // Struct init C-style: `Point p = {.x=1, .y=2};` o
    // posicional `Point p = {1, 2};`.
    if (sem_type.kind == PrimitiveKind::STRUCT && vd->init &&
        vd->init->kind == ast::NodeKind::InitListExpr) {
        auto *il = static_cast<ast::InitListExpr *>(vd->init.get());
        const auto &layouts = tc_.struct_layouts();
        auto it_l = layouts.find(sem_type.struct_name);
        if (it_l == layouts.end()) {
            error_at(vd->loc, "lowering: struct '" + sem_type.struct_name +
                                  "' sin layout");
            return;
        }
        const StructLayout &lay = it_l->second;
        ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = addr;
        al.imm = (uint64_t)lay.size_bytes;
        // Host SIEMPRE: ver el comentario extenso de la rama sin init-list.
        al.host_alloca = true;
        fn_->values[addr].is_host_ptr = true;
        al.source_line = vd->loc.line;
        emit(current_block_, std::move(al));
        // Seguridad: zero-inicializar TODO el struct antes de escribir los
        // campos listados.  Asi los campos NO presentes en el init-list quedan
        // a 0 (no basura de la pila).  Subsume el zero de los bit fields.
        emit_zero_fill(addr, (uint64_t)lay.size_bytes, vd->loc.line);
        // Valores por defecto de los campos (`u8 a = 0x10`); el init-list
        // explicito de abajo sobrescribe los campos que liste.
        emit_struct_field_defaults(addr, lay, vd->loc.line);
        // @Virtual: fijar el vptr del struct polimorfico a su vtable (tras el
        // zero_fill; el init-list solo escribe campos, no el vptr en offset 0).
        if (lay.is_polymorphic) emit_struct_vptr_init(addr, lay, vd->loc.line);
        // Zero los storage words de bit fields antes del
        // loop para evitar que el RMW lea basura del ALLOCA.  Los
        // unique (offset, size) ya estan en lay.fields para bit
        // fields; emit STORE 0 una sola vez por word.
        std::set<std::pair<uint32_t, uint32_t>> zeroed_bf;
        for (const auto &f : lay.fields) {
            if (f.bit_width == 0) continue;
            auto key = std::make_pair(f.offset, f.size);
            if (!zeroed_bf.insert(key).second) continue;
            ir::IrType ft_zero = ir_type_from_primitive(f.type.kind);
            ir::IrValueId v_zero = emit_const(ft_zero, 0, vd->loc.line);
            ir::IrValueId v_addr_w = addr;
            if (f.offset > 0) {
                ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, (uint64_t)f.offset, vd->loc.line);
                v_addr_w = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_addr_w;
                ad.operands = {addr, v_off};
                ad.source_line = vd->loc.line;
                emit(current_block_, std::move(ad));
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ft_zero;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_zero, v_addr_w};
            st.source_line = vd->loc.line;
            emit(current_block_, std::move(st));
        }
        for (size_t i = 0; i < il->elements.size(); ++i) {
            const StructFieldInfo *fi = nullptr;
            if (il->is_designated) {
                const std::string &fname = il->field_names[i];
                for (const auto &f : lay.fields) {
                    if (f.name == fname) {
                        fi = &f;
                        break;
                    }
                }
                if (!fi) {
                    error_at(vd->loc,
                             "lowering: campo '" + fname + "' no existe");
                    continue;
                }
            } else {
                if (i >= lay.fields.size()) {
                    error_at(vd->loc, "lowering: init list excede campos");
                    break;
                }
                fi = &lay.fields[i];
            }
            // Campo STRUCT inicializado con un init-list ANIDADO
            // (`{.min = {.x=.., .y=..}}` o `{.min = Punto{...}}`): se rellena
            // RECURSIVAMENTE in-place en la direccion del campo.  lower_expr no
            // baja un InitListExpr como valor -> hay que tratarlo aqui.
            if (fi->type.kind == PrimitiveKind::STRUCT &&
                il->elements[i]->kind == ast::NodeKind::InitListExpr) {
                ir::IrValueId v_faddr = addr;
                if (fi->offset > 0) {
                    ir::IrValueId v_off = emit_const(
                        ir::IrType::I64, (uint64_t)fi->offset, vd->loc.line);
                    v_faddr = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = v_faddr;
                    ad.operands = {addr, v_off};
                    ad.source_line = vd->loc.line;
                    emit(current_block_, std::move(ad));
                }
                auto it_sl = tc_.struct_layouts().find(fi->type.struct_name);
                if (it_sl == tc_.struct_layouts().end()) {
                    error_at(vd->loc, "lowering: struct '" +
                                          fi->type.struct_name +
                                          "' sin layout (init anidado)");
                    continue;
                }
                emit_struct_init_fields(
                    v_faddr, it_sl->second,
                    static_cast<ast::InitListExpr *>(il->elements[i].get()),
                    vd->loc.line);
                continue;
            }
            ir::IrValueId v_val = lower_expr(il->elements[i].get());
            if (v_val == ir::IR_NO_VALUE) continue;
            const ir::IrType ir_ft = ir_type_from_primitive(fi->type.kind);
            const bool elem_is_literal =
                il->elements[i]->kind == ast::NodeKind::IntLitExpr ||
                il->elements[i]->kind == ast::NodeKind::FloatLitExpr ||
                il->elements[i]->kind == ast::NodeKind::BoolLitExpr ||
                il->elements[i]->kind == ast::NodeKind::CharLitExpr ||
                il->elements[i]->kind == ast::NodeKind::NullLitExpr;
            v_val = cast_if_needed(v_val, fn_->values[v_val].type, ir_ft,
                                   vd->loc.line,
                                   /*is_explicit=*/elem_is_literal);
            ir::IrValueId v_addr = addr;
            if (fi->offset > 0) {
                ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, (uint64_t)fi->offset, vd->loc.line);
                v_addr = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_addr;
                ad.operands = {addr, v_off};
                ad.source_line = vd->loc.line;
                emit(current_block_, std::move(ad));
            }
            // Campo AGREGADO inline (struct/array value-type): @c v_val es la
            // DIRECCION del agregado origen -> copia memberwise (qword a
            // qword) sus bytes al campo, NO un STORE escalar (que guardaria la
            // direccion origen).  Sin esto un `Outer o = {.w = inner}`
            // guardaba &inner en o.w y leer o.w.v devolvia la direccion
            // (bug struct-en-struct, value-type anidado).
            // Un campo de tipo `@overlay struct` NO es un agregado inline:
            // guarda el HANDLE de la vista (8 bytes) -> STORE escalar (abajo).
            if ((fi->type.kind == PrimitiveKind::STRUCT &&
                 !type_is_overlay(fi->type)) ||
                fi->type.kind == PrimitiveKind::ARRAY) {
                uint64_t sz = size_of_type(fi->type);
                if (sz == 0 && fi->type.kind == PrimitiveKind::STRUCT) {
                    auto it_sl =
                        tc_.struct_layouts().find(fi->type.struct_name);
                    if (it_sl != tc_.struct_layouts().end())
                        sz = (uint64_t)it_sl->second.size_bytes;
                }
                if (sz == 0) sz = 8;
                emit_memberwise_copy(v_addr, v_val, sz, vd->loc.line);
                if (fi->type.kind == PrimitiveKind::STRUCT) {
                    auto it_sl =
                        tc_.struct_layouts().find(fi->type.struct_name);
                    if (it_sl != tc_.struct_layouts().end() &&
                        it_sl->second.has_copy_hook) {
                        emit_struct_method_on_host_field(
                            v_addr, fi->type.struct_name,
                            fi->type.struct_name + "____clone__", vd->loc.line);
                    }
                }
                continue;
            }
            // Bit field en init list: read-modify-write.
            // El ALLOCA inicial deja basura; debemos LOAD el storage
            // word actual, limpiar los bits del rango con AND ~mask,
            // OR con (val<<offset), STORE.  Igual que en lower_assign
            // para bit fields.
            if (fi->bit_width > 0) {
                ir::IrValueId v_old = fn_->new_value(ir_ft);
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = ir_ft;
                ld.dst = v_old;
                ld.operands = {v_addr};
                ld.source_line = vd->loc.line;
                emit(current_block_, std::move(ld));
                const uint64_t mask =
                    (fi->bit_width == 64)
                        ? UINT64_MAX
                        : ((uint64_t(1) << fi->bit_width) - 1);
                const uint64_t inv_mask = ~(mask << fi->bit_offset);
                ir::IrValueId v_inv = emit_const(ir_ft, inv_mask, vd->loc.line);
                ir::IrValueId v_clr = fn_->new_value(ir_ft);
                {
                    ir::IrInstr an{};
                    an.op = ir::IrOp::AND;
                    an.type = ir_ft;
                    an.dst = v_clr;
                    an.operands = {v_old, v_inv};
                    an.source_line = vd->loc.line;
                    emit(current_block_, std::move(an));
                }
                ir::IrValueId v_msk = emit_const(ir_ft, mask, vd->loc.line);
                ir::IrValueId v_tr = fn_->new_value(ir_ft);
                {
                    ir::IrInstr an{};
                    an.op = ir::IrOp::AND;
                    an.type = ir_ft;
                    an.dst = v_tr;
                    an.operands = {v_val, v_msk};
                    an.source_line = vd->loc.line;
                    emit(current_block_, std::move(an));
                }
                ir::IrValueId v_sh = v_tr;
                if (fi->bit_offset > 0) {
                    ir::IrValueId v_amt = emit_const(
                        ir_ft, (uint64_t)fi->bit_offset, vd->loc.line);
                    v_sh = fn_->new_value(ir_ft);
                    ir::IrInstr sh{};
                    sh.op = ir::IrOp::SHL;
                    sh.type = ir_ft;
                    sh.dst = v_sh;
                    sh.operands = {v_tr, v_amt};
                    sh.source_line = vd->loc.line;
                    emit(current_block_, std::move(sh));
                }
                ir::IrValueId v_new = fn_->new_value(ir_ft);
                {
                    ir::IrInstr or_{};
                    or_.op = ir::IrOp::OR;
                    or_.type = ir_ft;
                    or_.dst = v_new;
                    or_.operands = {v_clr, v_sh};
                    or_.source_line = vd->loc.line;
                    emit(current_block_, std::move(or_));
                }
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir_ft;
                st.dst = ir::IR_NO_VALUE;
                st.operands = {v_new, v_addr};
                st.source_line = vd->loc.line;
                emit(current_block_, std::move(st));
                continue;
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir_ft;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_val, v_addr};
            st.source_line = vd->loc.line;
            emit(current_block_, std::move(st));
        }
        bind(vd->name, addr);
        return;
    }

    // Caso 1: variable de tipo struct.  Reservamos memoria local con
    // ALLOCA del IR (el emisor lo baja a 'subsp rsp, N + readcur') y
    // guardamos el IrValueId del puntero como "current value" de la
    // variable en scope.  El acceso a campos via FieldAccessExpr
    // calcula offsets desde este puntero base.
    if (sem_type.kind == PrimitiveKind::STRUCT) {
        const auto &layouts = tc_.struct_layouts();
        auto it = layouts.find(sem_type.struct_name);
        // ADTs: si NO esta en struct_layouts, puede ser un enum
        // (compartimos PrimitiveKind::STRUCT para reusar el camino
        // de value-type).  Buscar en enum_layouts_ y alocar slot
        // de @c size_bytes (8 + 8*max_payload_fields).
        if (it == layouts.end()) {
            const auto &elays = tc_.enum_layouts();
            auto ite = elays.find(sem_type.struct_name);
            if (ite != elays.end()) {
                const EnumLayout &elay = ite->second;
                const ir::IrValueId eaddr = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr eal{};
                eal.op = ir::IrOp::ALLOCA;
                eal.type = ir::IrType::I8;
                eal.dst = eaddr;
                eal.imm = static_cast<uint64_t>(elay.size_bytes);
                // Todo agregado (struct Y enum) vive en memoria HOST en los
                // tres modos.  Ver la rama STRUCT: si unos acaban en host y
                // otros en la pila VM, el callee -- que solo recibe una
                // direccion -- lee unos u otros como basura.
                eal.host_alloca = true;
                eal.source_line = vd->loc.line;
                emit(current_block_, std::move(eal));
                fn_->values[eaddr].is_host_ptr = true;
                // La variable es un value-type: se bindea a un SLOT ESTABLE
                // (@c eaddr, ALLOCA en VM stack) y el inicializador se COPIA
                // qword-by-qword al slot -- MISMO modelo que un struct
                // (ver rama STRUCT abajo).  Antes se bindeaba la variable al
                // slot del constructor (repunte del puntero); eso rompia con
                // una asignacion condicional (`if { t = X }` / arm de match):
                // el var-decl apuntaba a un slot (p.ej. GC-host) y el assign
                // a otro (ALLOCA VM), un PHI mezclaba punteros de naturaleza
                // distinta y el LOAD del tag del `match t` usaba movh sobre
                // una direccion VM -> SIGSEGV.  Con el slot estable, `t`
                // tiene UNA sola direccion (VM) y el match lee siempre con mov.
                bind(vd->name, eaddr);
                if (vd->init) {
                    const ir::IrValueId init_addr = lower_expr(vd->init.get());
                    if (init_addr != ir::IR_NO_VALUE) {
                        emit_enum_copy(eaddr, init_addr,
                                       fn_->values[init_addr].is_host_ptr,
                                       elay.size_bytes, vd->loc.line);
                    }
                }
                return;
            }
            error_at(vd->loc, "lowering: struct/enum desconocido '" +
                                  sem_type.struct_name + "'");
            return;
        }
        const StructLayout &lay = it->second;
        // ALLOCA del IR reserva count * sizeof(T) bytes; pasamos
        // tipo i8 para que count sea exactamente size_bytes.  El
        // emisor lo traduce a 'subsp rsp, N' + 'readcur rDst'.
        const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::ALLOCA;
        ins.type = ir::IrType::I8; // unidad: 1 byte
        ins.dst = addr;
        ins.imm = (uint64_t)lay.size_bytes;
        ins.source_line = vd->loc.line;
        // AOT bare (native_poo_): NO hay VM stack -> el struct debe vivir
        // en la pila nativa (host_alloca).  Sin esto, un struct que
        // escapa (p.ej. se pasa por puntero a un metodo s.metodo()) se
        // aloca con ALLOCA_VM ([rbx+0x40]); el .exe standalone no tiene
        // ProcessVM en rbx -> SIGSEGV.
        //
        // Bug host-vs-VM (2026-07-15): tambien en interp/JIT cuando se toma
        // `&p`.  El comentario anterior daba por bueno que "los escapantes
        // usan VM stack que el runtime mapea", pero el consumidor del `P*`
        // resultante (param, campo o elemento) lo deref-ea con movh por la
        // convencion `T*`=host -> movh sobre direccion VM -> SIGSEGV.  Solo
        // pasaba inadvertido mientras el inliner borraba la llamada.  Un
        // struct que NO se address-takea se queda en la pila VM (y el
        // ir_optimizer ya lo promueve por perf si no escapa).
        // Host SIEMPRE, no solo en AOT ni solo si se toma la direccion.  Mismo
        // criterio (y mismo motivo) que el buffer de un `T[N]` local.
        //
        // Que fuera CONDICIONAL era el bug: cualquier cosa que marcara UN local
        // lo mandaba a host y dejaba al de al lado en la pila VM.  El callee no
        // puede distinguirlos -- recibe una direccion y punto -- asi que leia
        // uno de los dos como basura, con el otro funcionando (que es lo que lo
        // hacia dificil de ver).  Medido: dos structs y un metodo con arg ->
        // this=(0,0) y o=(1,2).  Con TODOS los agregados en host, caller y
        // callee coinciden siempre.  Memoria VM explicita = `VirtualPtr<T>`.
        ins.host_alloca = true;
        const bool struct_is_host = ins.host_alloca;
        emit(current_block_, std::move(ins));
        if (struct_is_host) fn_->values[addr].is_host_ptr = true;
        bind(vd->name, addr);
        // Seguridad + RAII: zero-inicializar SIEMPRE el buffer del struct.  Un
        // struct local en pila NO se zeroea solo (a diferencia de un objeto
        // GC); sin esto, (a) los campos no asignados exponen basura de la pila
        // (seguridad), y (b) un campo shared/unique/closure sin asignar tendria
        // un ctrl/slot basura y su dtor haria free de basura (UAF).  El
        // init-list / copy posterior sobrescribe los campos que toque.
        emit_zero_fill(addr, (uint64_t)lay.size_bytes, vd->loc.line);
        // Declaracion sin init (`P p;`): aplicar los valores por defecto de los
        // campos.  Si hay init (copia de otro struct/llamada) el copy de abajo
        // sobrescribe todo, asi que los defaults solo aplican sin init.
        if (!vd->init) emit_struct_field_defaults(addr, lay, vd->loc.line);
        // @Virtual: un struct polimorfico recien construido apunta su vptr
        // (offset 0) a la vtable de SU tipo declarado.  Va tras el zero_fill
        // (que dejo el vptr en 0) y los defaults.  Para `Derivado d;` fija
        // vptr=vtable_Derivado, de modo que un dispatch posterior por `Base*`
        // resuelve al metodo del derivado (dispatch dinamico correcto).
        if (lay.is_polymorphic && !vd->init)
            emit_struct_vptr_init(addr, lay, vd->loc.line);
        // Ownership ruta B (copy-hook): `S b = a;` donde S declara `__clone__`
        // y `a` es un lvalue struct existente (IdentExpr) es una COPIA.  Modelo
        // (estilo Rust Clone): memcpy bit a bit a->b (abajo) y DESPUES
        // `b.__clone__()` (CALL <S>____clone__(b)) que aplica el efecto sobre
        // la copia (p.ej. ++refcount de su recurso).  Opera sobre `this`=b
        // (misma memory class que cualquier metodo de struct -> sin mismatch
        // host/VM). `S b = move(a)` o `S b = call()` (valor
        // fresco/transferencia) NO entran.
        const bool do_copy_hook =
            vd->init && vd->init->kind == ast::NodeKind::IdentExpr &&
            lay.has_copy_hook &&
            escaping_locals_.find(vd->name) == escaping_locals_.end();
        // B3 fix: si hay inicializador, lower-lo como PTR al struct
        // origen y copiar qword-by-qword al slot ALLOCA recien creado.
        // Soporta:
        //   - Call result: `Punto v = funcion_que_devuelve_struct(...)`
        //   - read_borrow: `Punto v = read_borrow(b)` (B2 pass-through)
        //   - Otros SSA values PTR a struct.
        // El init list (que SI estaba soportado) se maneja en la rama
        // de mas arriba antes de llegar aqui (linea 1837).
        if (vd->init) {
            const ir::IrValueId v_src = lower_expr(vd->init.get());
            if (v_src != ir::IR_NO_VALUE) {
                // Heredar is_host_ptr del source para los LOADs.  Si
                // el src viene de read_borrow / ptr_of (unique), es
                // host_ptr; si viene de un struct stack ALLOCA es VM.
                const bool src_is_host = fn_->values[v_src].is_host_ptr;
                // Copia qword-by-qword (size_bytes redondeado a 8).
                const uint64_t qwords = (lay.size_bytes + 7) / 8;
                for (uint64_t qi = 0; qi < qwords; ++qi) {
                    const uint64_t off = qi * 8;
                    const ir::IrValueId v_off =
                        emit_const(ir::IrType::I64, static_cast<int64_t>(off),
                                   vd->loc.line);
                    // src + off
                    const ir::IrValueId v_src_at =
                        fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_src_at].is_host_ptr = src_is_host;
                    {
                        ir::IrInstr ad{};
                        ad.op = ir::IrOp::ADD;
                        ad.type = ir::IrType::I64;
                        ad.dst = v_src_at;
                        ad.operands = {v_src, v_off};
                        ad.source_line = vd->loc.line;
                        emit(current_block_, std::move(ad));
                    }
                    // LOAD i64 from src+off
                    const ir::IrValueId v_word =
                        fn_->new_value(ir::IrType::I64);
                    {
                        ir::IrInstr ld{};
                        ld.op = ir::IrOp::LOAD;
                        ld.type = ir::IrType::I64;
                        ld.dst = v_word;
                        ld.operands = {v_src_at};
                        ld.source_line = vd->loc.line;
                        emit(current_block_, std::move(ld));
                    }
                    // dst slot + off.  Su naturaleza se HEREDA del slot: dar
                    // por hecho que es VM hacia que la copia escribiera con
                    // `mov` sobre una direccion host -> el struct se quedaba a
                    // ceros (y su copy-hook/dtor operaban sobre basura).
                    const ir::IrValueId v_dst_at =
                        fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_dst_at].is_host_ptr =
                        fn_->values[addr].is_host_ptr;
                    {
                        ir::IrInstr ad{};
                        ad.op = ir::IrOp::ADD;
                        ad.type = ir::IrType::I64;
                        ad.dst = v_dst_at;
                        ad.operands = {addr, v_off};
                        ad.source_line = vd->loc.line;
                        emit(current_block_, std::move(ad));
                    }
                    // STORE i64 [dst+off] = word
                    {
                        ir::IrInstr st{};
                        st.op = ir::IrOp::STORE;
                        st.type = ir::IrType::I64;
                        st.operands = {v_word, v_dst_at};
                        st.source_line = vd->loc.line;
                        emit(current_block_, std::move(st));
                    }
                }
            }
        }
        // Copy-hook: tras el memcpy, `b.__clone__()` aplica el efecto de copia
        // sobre la nueva copia (this = addr = b).
        if (do_copy_hook) {
            ir::IrInstr cc{};
            cc.op = ir::IrOp::CALL;
            cc.type = ir::IrType::VOID;
            cc.dst = ir::IR_NO_VALUE;
            cc.operands = {addr}; // this = b (la copia)
            cc.func_name = sem_type.struct_name + "__" + "__clone__";
            cc.source_line = vd->loc.line;
            emit(current_block_, std::move(cc));
        }
        // Fase 2a interop C / ownership: destructor automatico (RAII) del
        // struct value-type local con `~Struct()` declarado y que NO escapa.
        // CALL directo a <Struct>__dtor(addr) al exit del scope (dispatch
        // estatico, sin vtable; inlineable -> un dtor trivial cuesta ~0).  Si
        // el struct ESCAPA (return/store -> escaping_locals_), se SUPRIME el
        // cleanup: move-on-return (el caller re-registra el dtor de su copia
        // -> un solo free).  Cero overhead para structs sin `~Struct()`.
        if (escaping_locals_.find(vd->name) == escaping_locals_.end()) {
            bool has_dtor = false;
            for (const auto &mi : lay.methods)
                if (mi.is_destructor) {
                    has_dtor = true;
                    break;
                }
            if (has_dtor) {
                CleanupAction act;
                act.kind = CleanupAction::Kind::STRUCT_DTOR;
                act.operands = {addr};
                act.source_line = vd->loc.line;
                act.refresh_name = vd->name;
                // Naming de lower_struct_methods: <Struct>__ + __dtor.
                act.func_name = sem_type.struct_name + "__" + "__dtor";
                cleanup_stack_.push_back(std::move(act));
            }
            // Ownership escape-sensitive: si el struct tiene campos closure
            // (lambda con captura) y su valor llega POR MOVE desde una call
            // (init = CallExpr) que retorna un struct con closure escapado, su
            // env vive en HEAP y este consumidor es el unico responsable de
            // liberarlo al exit del scope (el productor suprimio su cleanup via
            // escaping_locals_ al hacer return).  Registramos CLOSURE_ENV_FREE
            // con los offsets de los campos fn.  El caso local-no-escapa NO
            // entra aqui (su env vive en stack, sin liberacion).
            if (vd->init && vd->init->kind == ast::NodeKind::CallExpr) {
                std::vector<uint32_t> fn_offs;
                for (const auto &f : lay.fields)
                    if (f.type.kind == PrimitiveKind::FUNCTION &&
                        !f.type.fn_is_raw)
                        fn_offs.push_back(f.offset);
                if (!fn_offs.empty()) {
                    CleanupAction act;
                    act.kind = CleanupAction::Kind::CLOSURE_ENV_FREE;
                    act.operands = {addr};
                    act.source_line = vd->loc.line;
                    act.refresh_name = vd->name;
                    act.closure_field_offsets = std::move(fn_offs);
                    cleanup_stack_.push_back(std::move(act));
                }
            }
        }
        return;
    }

    // C-style string init para arrays byte-like: `u8[N] arr = "literal"`.
    // Detecta el patron y emite STOREs byte-a-byte del contenido del
    // string literal, con zerificacion del resto si N > strlen.  Si
    // strlen > N reporta error (truncation, comportamiento C).
    // No se promueve el literal a StringObject (es array de bytes
    // crudo, sin GC).  Aceptamos solo literales no interpolados.
    if (sem_type.kind == PrimitiveKind::ARRAY && vd->init &&
        vd->init->kind == ast::NodeKind::StringLitExpr && sem_type.pointee &&
        (sem_type.pointee->kind == PrimitiveKind::U8 ||
         sem_type.pointee->kind == PrimitiveKind::I8 ||
         sem_type.pointee->kind == PrimitiveKind::CHAR)) {
        auto *sl = static_cast<ast::StringLitExpr *>(vd->init.get());
        if (sl->is_interpolated()) {
            error_at(vd->loc,
                     "init de array con string no acepta interpolacion");
            return;
        }
        const std::string &bytes = sl->value;
        const uint32_t str_n = (uint32_t)bytes.size();
        const uint32_t arr_n =
            sem_type.array_size > 0 ? (uint32_t)sem_type.array_size : str_n;
        if (str_n > arr_n) {
            error_at(vd->loc, "literal de string (" + std::to_string(str_n) +
                                  " bytes) mas grande que el array (" +
                                  std::to_string(arr_n) + ")");
            return;
        }
        const Type elem_t = *sem_type.pointee;
        const ir::IrType ir_elem = ir_type_from_primitive(elem_t.kind);
        const uint32_t elem_sz = (uint32_t)primitive_size_bytes(elem_t.kind);
        // ALLOCA del array (siempre arr_n elementos).
        ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr al{};
            al.op = ir::IrOp::ALLOCA;
            al.type = ir::IrType::I8;
            al.dst = addr;
            al.imm = (uint64_t)arr_n * elem_sz;
            al.source_line = vd->loc.line;
            /* Buffer HOST: ver la nota de las otras rutas de array local. */
            al.host_alloca = true;
            fn_->values[addr].is_host_ptr = true;
            emit(current_block_, std::move(al));
        }
        // STORE byte-a-byte del string.
        for (uint32_t i = 0; i < str_n; ++i) {
            ir::IrValueId v_val =
                emit_const(ir_elem, (uint64_t)(uint8_t)bytes[i], vd->loc.line);
            ir::IrValueId v_addr_i = addr;
            if (i > 0) {
                ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, (uint64_t)i * elem_sz, vd->loc.line);
                v_addr_i = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_addr_i;
                ad.operands = {addr, v_off};
                ad.source_line = vd->loc.line;
                emit(current_block_, std::move(ad));
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir_elem;
            st.operands = {v_val, v_addr_i};
            st.source_line = vd->loc.line;
            emit(current_block_, std::move(st));
        }
        // Zerificar el resto (semantica C: padding a cero).
        for (uint32_t i = str_n; i < arr_n; ++i) {
            ir::IrValueId v_zero = emit_const(ir_elem, 0, vd->loc.line);
            ir::IrValueId v_off = emit_const(
                ir::IrType::I64, (uint64_t)i * elem_sz, vd->loc.line);
            ir::IrValueId v_addr_i = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_addr_i;
            ad.operands = {addr, v_off};
            ad.source_line = vd->loc.line;
            emit(current_block_, std::move(ad));
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir_elem;
            st.operands = {v_zero, v_addr_i};
            st.source_line = vd->loc.line;
            emit(current_block_, std::move(st));
        }
        bind(vd->name, addr);
        return;
    }

    // Array init C-style: `i32 arr[N] = {e0, e1, ...};`.
    // Detectamos InitListExpr en el inicializador y emitimos:
    //   ALLOCA del array (igual que sin init).
    //   Por cada elemento: STORE val a (base + i * sizeof(T)).
    //   bind nombre al PTR base.
    // Solo positional (sin .field=); reportamos error si is_designated.
    if (sem_type.kind == PrimitiveKind::ARRAY && vd->init &&
        vd->init->kind == ast::NodeKind::InitListExpr) {
        auto *il = static_cast<ast::InitListExpr *>(vd->init.get());
        if (il->is_designated) {
            error_at(vd->loc,
                     "lowering: init designado '.field=' no aplica a arrays");
            return;
        }
        const Type elem_t = sem_type.pointee ? *sem_type.pointee : Type{};
        const uint32_t elem_sz = (uint32_t)primitive_size_bytes(elem_t.kind);
        if (elem_sz == 0) {
            error_at(vd->loc, "lowering: tipo del elemento sin sizeof");
            return;
        }
        const uint32_t arr_size = sem_type.array_size > 0
                                      ? (uint32_t)sem_type.array_size
                                      : (uint32_t)il->elements.size();
        if (il->elements.size() > arr_size) {
            error_at(vd->loc, "lowering: init list mas elementos que el array");
            return;
        }
        // ALLOCA arr_size * elem_sz bytes.
        ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = addr;
        al.imm = (uint64_t)arr_size * elem_sz;
        al.source_line = vd->loc.line;
        /* El buffer va a memoria HOST, igual que el de un array local SIN
         * inicializador (ver la otra rama y su nota de 2026-07-15).  Este
         * camino -- el de `T[N] a = {...}` -- se quedo sin marcar, asi que el
         * array acababa en la pila de la VM mientras todo lo que lo consume
         * (`a` decaido a `T*`, `&a[i]`, la funcion que lo recibe) emitia
         * accesos de HOST.  Leer una direccion VM como si fuera host mata el
         * proceso, y solo se notaba al RECORRERLO con indice variable: con
         * indices constantes el optimizador resolvia los accesos antes. */
        al.host_alloca = true;
        fn_->values[addr].is_host_ptr = true;
        emit(current_block_, std::move(al));
        // STORE de cada elemento.
        const ir::IrType ir_elem = ir_type_from_primitive(elem_t.kind);
        for (size_t i = 0; i < il->elements.size(); ++i) {
            ir::IrValueId v_val = lower_expr(il->elements[i].get());
            if (v_val == ir::IR_NO_VALUE) continue;
            // Suprimir warning de narrowing si el elemento es literal
            // (`{10, 20, ...}` con i64-defaulted literals encajando en
            // el tipo de elemento).  Mismo razonamiento que en
            // var-decl con init literal.
            const bool elem_is_literal =
                il->elements[i]->kind == ast::NodeKind::IntLitExpr ||
                il->elements[i]->kind == ast::NodeKind::FloatLitExpr ||
                il->elements[i]->kind == ast::NodeKind::BoolLitExpr ||
                il->elements[i]->kind == ast::NodeKind::CharLitExpr ||
                il->elements[i]->kind == ast::NodeKind::NullLitExpr;
            v_val = cast_if_needed(v_val, fn_->values[v_val].type, ir_elem,
                                   vd->loc.line,
                                   /*is_explicit=*/elem_is_literal);
            ir::IrValueId v_addr_i = addr;
            if (i > 0) {
                ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, (uint64_t)(i * elem_sz), vd->loc.line);
                v_addr_i = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_addr_i;
                ad.operands = {addr, v_off};
                ad.source_line = vd->loc.line;
                emit(current_block_, std::move(ad));
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir_elem;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_val, v_addr_i};
            st.source_line = vd->loc.line;
            emit(current_block_, std::move(st));
        }
        bind(vd->name, addr);
        return;
    }

    // Struct init C-style: `Point p = {.x = 1, .y = 2};`
    // o `Point p = {1, 2};` (positional).  ALLOCA del struct + STORE
    // de cada campo en su offset.
    if (sem_type.kind == PrimitiveKind::STRUCT && vd->init &&
        vd->init->kind == ast::NodeKind::InitListExpr) {
        auto *il = static_cast<ast::InitListExpr *>(vd->init.get());
        const auto &layouts = tc_.struct_layouts();
        auto it_l = layouts.find(sem_type.struct_name);
        if (it_l == layouts.end()) {
            error_at(vd->loc, "lowering: struct '" + sem_type.struct_name +
                                  "' sin layout");
            return;
        }
        const StructLayout &lay = it_l->second;
        // ALLOCA del struct.
        ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = addr;
        al.imm = (uint64_t)lay.size_bytes;
        // Bug host-vs-VM (2026-07-15): si se toma `&p`, el slot vive en host
        // (`T*` = direccion host por convencion).  Ver el comentario extenso
        // en la rama del struct sin init-list.
        // Host SIEMPRE: ver el comentario extenso de la rama sin init-list.
        al.host_alloca = true;
        fn_->values[addr].is_host_ptr = true;
        al.source_line = vd->loc.line;
        emit(current_block_, std::move(al));
        // STORE cada campo en su offset (recursivo para structs anidados).
        emit_struct_init_fields(addr, lay, il, vd->loc.line);
        bind(vd->name, addr);
        return;
    }

    // Array nativo T[N]: identico a struct desde la optica del lowering.
    // Reservamos N*sizeof(T) bytes en stack y guardamos la direccion base
    // como "valor" de la variable.  Los accesos arr[i] se desugan a
    // ADD(addr, i*sizeof(T)) + LOAD/STORE igual que para T*; el tipo del
    // pointee se obtiene del propio sem_type para escalar el offset.
    if (sem_type.kind == PrimitiveKind::ARRAY) {
        // bug4: array dinamico `T[]` con init `new T[N]` o assigned
        // desde otro host_ptr.  El slot guarda el host_ptr al buffer
        // alocado en heap.  Cuando array_size == 0 y hay init, bindeo
        // el local al SSA value del init (host_ptr) sin ALLOCA stack.
        if (!sem_type.pointee || sem_type.array_size == 0) {
            if (vd->init) {
                const ir::IrValueId v_init = lower_expr(vd->init.get());
                if (v_init != ir::IR_NO_VALUE) {
                    // Mark is_host_ptr para que LOAD/STORE indirectos
                    // emitan movh.  El IR del new T[N] ya lo marca.
                    bind(vd->name, v_init);
                    return;
                }
            }
            error_at(
                vd->loc,
                "lowering: array sin tamano fijo requiere init con `new T[N]`");
            return;
        }
        const size_t bytes = size_of_type(sem_type);
        if (bytes == 0) {
            error_at(vd->loc, "lowering: tamano del array es 0 (tipo de "
                              "elemento desconocido?)");
            return;
        }
        const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::ALLOCA;
        ins.type = ir::IrType::I8;
        ins.dst = addr;
        ins.imm = (uint64_t)bytes;
        ins.source_line = vd->loc.line;
        // Bug host-vs-VM (2026-07-15): el buffer de un `T[N]` local va SIEMPRE
        // a memoria host, no solo en AOT.  Su tipo es un array NO virtual (ver
        // @c type_from_node), asi que `arr` decaido a `T[]`/`T*` y `&arr[i]`
        // son direcciones host y sus consumidores emiten movh.  Cuando el
        // buffer se quedaba en la pila VM (interp/JIT) las dos rutas
        // discrepaban: `sum_array(arr, n)` leia el array con movh sobre una
        // direccion VM.  Un array VM explicito se nombra con `VirtualPtr<T>`.
        ins.host_alloca = true;
        emit(current_block_, std::move(ins));
        fn_->values[addr].is_host_ptr = true;
        bind(vd->name, addr);
        // Zero-inicializar SIEMPRE el buffer del array local (mismo motivo que
        // los structs, ~L4415): un array en pila NO se zeroea solo.  El interp
        // daba 0 solo porque su VM stack esta a cero; el JIT (pila HOST) leia
        // basura -> DIVERGENCIA interp/JIT (un `i32[N] a` leido sin escribir, o
        // `a[i]++` sobre un elemento no inicializado, daba garbage en JIT).
        // Ademas es seguridad: sin esto el array expone basura de la pila.
        emit_zero_fill(addr, (uint64_t)bytes, vd->loc.line);
        if (vd->init) {
            error_at(vd->loc, "lowering: inicializador de array aun no "
                              "soportado en esta ruta");
        }
        return;
    }

    // Caso 2: tipos primitivos / PTR (camino tradicional).
    ir::IrType vt = ir::IrType::I64;
    if (vd->type && vd->type->kind == ast::NodeKind::PrimitiveTypeNode) {
        auto *pt = static_cast<ast::PrimitiveTypeNode *>(vd->type.get());
        vt = ir_type_from_primitive(pt->prim);
    } else if (sem_type.kind != PrimitiveKind::COUNT &&
               sem_type.kind != PrimitiveKind::VOID) {
        // Alias resuelto a primitivo / PTR.
        vt = ir_type_from_primitive(sem_type.kind);
    }
    // bug6 - gc<T> para T CUALQUIERA (primitivo / smart ptr / anidado): la
    // variable guarda un host_ptr al box GC (gc_box).  El IR type debe ser PTR
    // (no el tipo interno T), si no un cast_if_needed insertaria un bitcast
    // PTR->T que descartaria is_host_ptr -> el `*g` posterior leeria memoria
    // VM en lugar del box host y devolveria basura.  Para gc<Clase> el tipo ya
    // era CLASS->PTR; esto extiende el mismo modelo a T no-clase.
    if (sem_type.gc_managed) {
        vt = ir::IrType::PTR;
    }

    // variable address-taken (&x aparece en algun sitio del body).
    // Reservamos memoria local con ALLOCA y emitimos un STORE inicial
    // si hay inicializador.  El scope guarda la DIRECCION (no el valor),
    // y read_local/write_local hacen LOAD/STORE para todos los usos.
    if (address_taken_locals_.count(vd->name)) {
        const size_t bytes = ir::type_access_bytes(vt); // tamano del tipo escalar
        const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr ai{};
        ai.op = ir::IrOp::ALLOCA;
        ai.type = ir::IrType::I8; // unidad: 1 byte
        ai.dst = addr;
        ai.imm = (uint64_t)bytes;
        // Bug host-vs-VM (2026-07-15): si se toma `&x`, el puntero resultante
        // es un `T*` y por convencion del lenguaje un `T*` es una direccion
        // HOST: el callee que reciba `T*`, o el campo/elemento `T*` donde se
        // guarde, lo deref-earan con movh.  Por tanto el slot debe vivir en
        // memoria host desde el principio.  Antes se quedaba en la pila VM y
        // solo "funcionaba" mientras el inliner o el store-to-load forwarding
        // borraban el deref; en cuanto el puntero cruzaba una frontera real
        // (callee no inlineado, o guardado en un struct/array y releido) se
        // hacia movh sobre una direccion VM -> SIGSEGV.
        //
        // Es el mismo criterio que @c ir_pass_promote_callned_allocas aplica a
        // los args de CALLN.  Se marca AQUI (y no en un pase del IR) porque
        // solo el lowering distingue un consumidor host (`T*`) de uno VM: los
        // structs de params de los opcodes meta viven en la pila VM y NO se
        // address-takean desde el codigo del usuario, asi que no se ven
        // afectados.  `VirtualPtr<T>` sigue siendo la via para memoria VM.
        ai.host_alloca = true;
        fn_->values[addr].is_host_ptr = true;
        ai.source_line = vd->loc.line;
        emit(current_block_, std::move(ai));
        bind(vd->name, addr);

        //  AS inc.3: registrar el binding register("reg") -> slot.
        // El backend port-C materializa este ALLOCA como una variable C
        // con register-pin de GCC y traduce sus LOAD/STORE a accesos
        // directos a la variable.
        if (!vd->reg_binding.empty()) {
            const std::string &rb = vd->reg_binding;
            const bool is_vec =
                rb.rfind("xmm", 0) == 0 || rb.rfind("ymm", 0) == 0 ||
                rb.rfind("zmm", 0) == 0 || rb.rfind("XMM", 0) == 0 ||
                rb.rfind("YMM", 0) == 0 || rb.rfind("ZMM", 0) == 0;
            ir::AsmRegBinding b{addr, rb, vt, is_vec, vd->name};
            b.reg_class = rb; // registro concreto: la clase es el registro.
            fn_->asm_reg_bindings.push_back(std::move(b));
        }

        // Store del valor inicial (o 0 si no hay init).
        ir::IrValueId v0 = ir::IR_NO_VALUE;
        if (vd->init) {
            v0 = lower_expr(vd->init.get());
            if (v0 != ir::IR_NO_VALUE) {
                const ir::IrType vfrom = fn_->values[v0].type;
                // Suprimir el warning de cast implicito cuando el
                // init es un literal: `u8 init = 0` no merece
                // alarma porque el valor es estatico y conocido en
                // compile-time; es un patron habitual y el type
                // checker ya valida el rango.
                const bool init_is_literal =
                    vd->init->kind == ast::NodeKind::IntLitExpr ||
                    vd->init->kind == ast::NodeKind::FloatLitExpr ||
                    vd->init->kind == ast::NodeKind::BoolLitExpr ||
                    vd->init->kind == ast::NodeKind::CharLitExpr ||
                    vd->init->kind == ast::NodeKind::NullLitExpr;
                v0 = cast_if_needed(v0, vfrom, vt, vd->init->loc,
                                    /*is_explicit=*/init_is_literal);
            }
        }
        if (v0 == ir::IR_NO_VALUE) v0 = emit_const(vt, 0, vd->loc.line);

        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = vt;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v0, addr};
        st.source_line = vd->loc.line;
        emit(current_block_, std::move(st));
        return;
    }

    ir::IrValueId v = ir::IR_NO_VALUE;
    if (vd->init) {
        // ----- Smart pointer move: unique/shared = move(p) -----
        // Patron especial: si el tipo destino es unique<T>/shared<T>
        // y el init es CallExpr(IdentExpr("move"), [p]), transferimos
        // ownership via mvtake (1 instr VM: copia + zero source).
        //
        // Lowering:
        //   1. lower p -> v_src_slot (SSA value que es la direccion
        //                            del slot stack del origen).
        //   2. ALLOCA 8 bytes -> v_dst_slot.
        //   3. Emit `mvtake [dst], [src]` via RAW_ASM.
        //   4. Marcar pointee_is_host_ptr en v_dst_slot.
        //
        // El cleanup del origen (registrado al declarar p) seguira
        // ejecutandose al exit del scope; vera 0 en el slot (zerificado
        // por mvtake) y RAW_FREE(0) sera no-op limpio.
        if ((sem_type.kind == PrimitiveKind::UNIQUE_PTR ||
             sem_type.kind == PrimitiveKind::SHARED_PTR) &&
            vd->init->kind == ast::NodeKind::CallExpr) {
            auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
            if (ce->callee && ce->callee->kind == ast::NodeKind::IdentExpr &&
                ce->args.size() == 1) {
                auto *cid = static_cast<ast::IdentExpr *>(ce->callee.get());
                if (cid->name == "move") {
                    const ir::IrValueId v_src = lower_expr(ce->args[0].get());
                    if (v_src != ir::IR_NO_VALUE) {
                        // unique<T> Tier 1: slot = 16 bytes (ptr + deleter).
                        // shared<T>: slot = 8 bytes (ctrl_block_ptr).
                        const uint32_t slot_bytes =
                            (sem_type.kind == PrimitiveKind::UNIQUE_PTR) ? 16
                                                                         : 8;
                        // ALLOCA para el slot destino.
                        const ir::IrValueId v_dst =
                            fn_->new_value(ir::IrType::PTR);
                        {
                            ir::IrInstr al{};
                            al.op = ir::IrOp::ALLOCA;
                            al.type = ir::IrType::I8;
                            al.dst = v_dst;
                            al.imm = slot_bytes;
                            al.source_line = vd->loc.line;
                            emit(current_block_, std::move(al));
                        }
                        // Emit mvtake [v_dst+0], [v_src+0] (ptr).
                        // Para unique<T> tambien emit mvtake [v_dst+8],
                        // [v_src+8] (deleter).
                        emit_mvtake(v_dst, v_src, vd->loc.line);
                        if (slot_bytes == 16) {
                            // Segundo qword: deleter.  Calculamos los dos
                            // punteros +8 y emitimos otro mvtake.
                            const ir::IrValueId v_eight =
                                emit_const(ir::IrType::I64, 8, vd->loc.line);
                            const ir::IrValueId v_dst8 =
                                fn_->new_value(ir::IrType::PTR);
                            const ir::IrValueId v_src8 =
                                fn_->new_value(ir::IrType::PTR);
                            {
                                ir::IrInstr add{};
                                add.op = ir::IrOp::ADD;
                                add.type = ir::IrType::I64;
                                add.dst = v_dst8;
                                add.operands = {v_dst, v_eight};
                                add.source_line = vd->loc.line;
                                emit(current_block_, std::move(add));
                            }
                            {
                                ir::IrInstr add{};
                                add.op = ir::IrOp::ADD;
                                add.type = ir::IrType::I64;
                                add.dst = v_src8;
                                add.operands = {v_src, v_eight};
                                add.source_line = vd->loc.line;
                                emit(current_block_, std::move(add));
                            }
                            emit_mvtake(v_dst8, v_src8, vd->loc.line);
                        }
                        fn_->values[v_dst].pointee_is_host_ptr = true;
                        v = v_dst;
                        goto bind_and_cleanup;
                    }
                }
            }
        }
        // Vesta Embed Inc 0: en modo native_poo_ (AOT) el tipo `string`
        // es VALUE-TYPE (struct {ptr,len,cap} de 24 bytes en stack,
        // HEAP-ALWAYS, RAII), NO un StringObject GC.  Dos casos:
        //   (a) `string s = "literal"`  -> construir el repr (ALLOCA +
        //       RAW_ALLOC + copia + nul + STOREs).
        //   (b) `string b = a`          -> MOVE: copiar los 24 bytes del
        //       slot de `a` al de `b` y ZERAR el ptr@0 del slot fuente
        //       (para que su cleanup sea no-op, sin doble-free).
        // El path Full/JIT/interp (sin native_poo_) NO entra aqui: cae a
        // la promocion StringObject GC de abajo.
        if (native_poo_ && sem_type.kind == PrimitiveKind::STRING && vd->init) {
            // Caso (a): literal NO interpolado (Inc 0 no cubre
            // interpolacion ni concat -- esos son Inc 1-4).
            if (vd->init->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = static_cast<ast::StringLitExpr *>(vd->init.get());
                if (!slit->is_interpolated()) {
                    v = build_native_string_from_literal(slit, vd->loc.line);
                    bind(vd->name, v);
                    // Una cadena que sale de un literal no tiene buffer propio
                    // -- o cabe inline, o es una vista sobre .rodata -- asi que
                    // mientras nadie la reasigne no hay nada que liberar.
                    // Registrar la limpieza igualmente no era gratis: emite un
                    // `free`, y ESO es lo que hacia que cualquier programa con
                    // una cadena constante enlazara el asignador entero.
                    const bool puede_acabar_siendo_suyo =
                        reassigned_locals_.count(vd->name) != 0;
                    // RAII: liberar el buffer al exit del scope, salvo
                    // que el string escape (return/asignacion a campo).
                    if (puede_acabar_siendo_suyo &&
                        escaping_locals_.find(vd->name) ==
                            escaping_locals_.end()) {
                        CleanupAction act;
                        act.kind = CleanupAction::Kind::STRING_FREE;
                        act.operands = {v};
                        act.source_line = vd->loc.line;
                        act.refresh_name = vd->name;
                        cleanup_stack_.push_back(std::move(act));
                    }
                    return;
                }
            }
            // Caso (b): MOVE desde otra variable string (IdentExpr).
            if (vd->init->kind == ast::NodeKind::IdentExpr &&
                vd->init->result_type.kind == PrimitiveKind::STRING) {
                const ir::IrValueId v_src = lower_expr(vd->init.get());
                if (v_src != ir::IR_NO_VALUE) {
                    // Nuevo slot de 24 bytes para `b` (host stack en native).
                    const ir::IrValueId v_slot =
                        fn_->new_value(ir::IrType::PTR);
                    if (native_poo_) fn_->values[v_slot].is_host_ptr = true;
                    {
                        ir::IrInstr al{};
                        al.op = ir::IrOp::ALLOCA;
                        al.type = ir::IrType::I8;
                        al.dst = v_slot;
                        al.imm = 24;
                        al.host_alloca = native_poo_;
                        al.source_line = vd->loc.line;
                        emit(current_block_, std::move(al));
                    }
                    // String Inc 5 (SSO): copiar los 24 bytes via MEMCPY
                    // (no 3 LOAD/STORE i64) -> evita el store-forwarding
                    // sobre qword2 que perdia la longitud SSO.
                    emit_native_str_move_copy(v_slot, v_src, vd->loc.line);
                    // Invalidar el slot fuente (move-out).  Inc 5 (SSO):
                    // si era HEAP -> ptr@0=0 (su cleanup hara free(0)=
                    // no-op; el buffer ahora lo posee `b`); si era SSO ->
                    // sin cambio (data inline, no hay buffer compartido).
                    emit_native_str_invalidate_moved(v_src, vd->loc.line);
                    bind(vd->name, v_slot);
                    // RAII para `b` (poseedor del buffer tras el move).
                    if (escaping_locals_.find(vd->name) ==
                        escaping_locals_.end()) {
                        CleanupAction act;
                        act.kind = CleanupAction::Kind::STRING_FREE;
                        act.operands = {v_slot};
                        act.source_line = vd->loc.line;
                        act.refresh_name = vd->name;
                        cleanup_stack_.push_back(std::move(act));
                    }
                    return;
                }
            }
            // Caso (c): init es una EXPRESION que produce un value-string
            // owned (concat `a + b`, str_concat(a, b), o futuras ops Inc
            // 2+).  lower_expr devuelve el PTR al slot nativo.  Bind +
            // RAII STRING_FREE (el resultado de un concat es owned).
            {
                const ir::IrValueId v_slot = lower_expr(vd->init.get());
                if (v_slot != ir::IR_NO_VALUE) {
                    bind(vd->name, v_slot);
                    if (escaping_locals_.find(vd->name) ==
                        escaping_locals_.end()) {
                        CleanupAction act;
                        act.kind = CleanupAction::Kind::STRING_FREE;
                        act.operands = {v_slot};
                        act.source_line = vd->loc.line;
                        act.refresh_name = vd->name;
                        cleanup_stack_.push_back(std::move(act));
                    }
                    return;
                }
            }
        }
        // Lazy promotion: si el tipo destino es STRING y el
        // init es un string literal puro (StringLitExpr), promover
        // a StringObject GC-managed via STRMAKE.  Asi `string s =
        // "hola"` aloca 1 vez; `print("hola")` (sin var-decl) sigue
        // sin alocar.
        if (sem_type.kind == PrimitiveKind::STRING && vd->init &&
            vd->init->kind == ast::NodeKind::StringLitExpr) {
            // Tanto literales puros como interpolados se promueven
            // a StringObject GC-managed; el helper detecta el caso
            // y emite STRMAKE simple o cadena de STRMAKE+STRCAT
            // segun corresponda.
            auto *slit = static_cast<ast::StringLitExpr *>(vd->init.get());
            v = lower_string_literal_to_string_object(slit);
            bind(vd->name, v);
            return;
        }
        v = lower_expr(vd->init.get());
        if (v != ir::IR_NO_VALUE) {
            const ir::IrType vfrom = fn_->values[v].type;
            // Misma supresion de warning que en la rama
            // address-taken: literales no merecen alarma de
            // narrowing porque el valor es compile-time conocido.
            const bool init_is_literal =
                vd->init->kind == ast::NodeKind::IntLitExpr ||
                vd->init->kind == ast::NodeKind::FloatLitExpr ||
                vd->init->kind == ast::NodeKind::BoolLitExpr ||
                vd->init->kind == ast::NodeKind::CharLitExpr ||
                vd->init->kind == ast::NodeKind::NullLitExpr;
            v = cast_if_needed(v, vfrom, vt, vd->init->loc,
                               /*is_explicit=*/init_is_literal);
        }
    } else {
        // Sin init: defecto 0.  Las variables sin init son raras
        // en uso normal pero el type checker no las prohibe.
        v = emit_const(vt, 0, vd->loc.line);
    }
bind_and_cleanup:
    bind(vd->name, v);

    // auto-free de colecciones primitivas.  Si el tipo del var
    // es uno de los tipos coleccion (ARRAYLIST, HASHMAP, etc), registrar
    // un cleanup en cleanup_stack_ que llame al free fn correspondiente
    // del plugin nativo al exit del scope/funcion.  El cleanup se emite
    // como RAW_ASM (consistente con synchronized) que prepara R1=handle,
    // R15=1, y emite calln al @Method del free.  Cero overhead en el
    // hot path (solo se emite al exit; CALL clean exits sin frame).
    //
    // Limitacion: si el handle se devuelve (return xs) o se asigna a
    // otra variable que vive mas, el free aqui dejaria al caller con
    // un handle invalido.  El escape analysis basico marca esos
    // locales en @c escaping_locals_ y omite el cleanup para ellos;
    // los locales realmente locales si reciben el free automatico.

    // FINALIZADOR CLASS_DTOR: un gc<Clase> con ~Clase() que POSEE un recurso
    // (campo owned, file handle, etc.) no tiene cleanup determinista (el GC lo
    // colecta).  Sin finalizador, ~Clase() nunca corre -> FUGA del recurso.
    // Registramos un finalizador GC que invoca el <Clase>____dtor CONCRETO
    // (dispatch estatico, CALL directo) cuando el sweep colecte el objeto.
    // Aplica ESCAPE o NO-escape indistintamente (el GC colecta cada objeto
    // exactamente una vez -> el dtor corre exactamente una vez; no hay cleanup
    // determinista que pudiera duplicarlo).  interp/JIT via gcfinalc; AOT via
    // CALL vx_gc_register_finalizer (emit_gc_set_finalizer bifurca por target).
    if (v != ir::IR_NO_VALUE && sem_type.kind == PrimitiveKind::CLASS &&
        sem_type.gc_managed && vd->init &&
        vd->init->kind == ast::NodeKind::NewExpr) {
        const auto &class_layouts = tc_.class_layouts();
        auto it_cls = class_layouts.find(sem_type.struct_name);
        if (it_cls != class_layouts.end()) {
            const ClassLayout &lay = it_cls->second;
            const ClassMethodInfo *dtor = nullptr;
            for (const auto &mi : lay.methods)
                if (mi.is_destructor) {
                    dtor = &mi;
                    break;
                }
            // Solo registrar si el dtor NO es polimorfico (dispatch estatico:
            // sin super, sin interfaces, sin subclases).  Si fuera virtual, el
            // finalizador tendria que resolver por vtable (fuera del contrato
            // "CALL directo"); ese caso (gc<Clase> polimorfica con dtor +
            // escape) queda como incremento futuro (fallback: se colecta sin
            // dtor).
            if (dtor != nullptr) {
                bool has_vtable =
                    !lay.super_name.empty() || !lay.interface_names.empty();
                if (!has_vtable)
                    for (const auto &kv : class_layouts)
                        if (kv.second.super_name == sem_type.struct_name) {
                            has_vtable = true;
                            break;
                        }
                if (!has_vtable) {
                    const std::string owner = dtor->defining_class.empty()
                                                  ? sem_type.struct_name
                                                  : dtor->defining_class;
                    const std::string dtor_label =
                        owner + "__" + dtor->name; // <Clase>____dtor
                    // vaddr del dtor via LABEL_ADDR (dispatch estatico).
                    const ir::IrValueId v_dtor =
                        emit_label_addr(dtor_label, vd->loc.line);
                    emit_gc_set_finalizer(v, /*CLASS_DTOR*/ 3, vd->loc.line,
                                          v_dtor);
                }
            }
        }
    }

    // Destructor automatico (RAII) para instancias locales de
    // clase Vesta que tienen `~ClassName()` declarado y NO escapan.
    // Emite CALLVIRT al destructor al exit del scope/funcion via
    // cleanup_stack_, mismo mecanismo que el auto-free de colecciones.
    // gc<Clase>: se EXCLUYE -- su ~Clase() lo corre el finalizador GC
    // (CLASS_DTOR, registrado arriba), no el cleanup determinista de scope.
    // Sin esta exclusion, un gc<Clase> no-escape ejecutaria el dtor DOS veces
    // (CALL_DTOR del scope + finalizador GC al colectar) -> doble-free.
    if (v != ir::IR_NO_VALUE && sem_type.kind == PrimitiveKind::CLASS &&
        !sem_type.gc_managed &&
        escaping_locals_.find(vd->name) == escaping_locals_.end()) {
        const auto &class_layouts = tc_.class_layouts();
        auto it_cls = class_layouts.find(sem_type.struct_name);
        if (it_cls != class_layouts.end()) {
            const ClassLayout &lay = it_cls->second;
            const ClassMethodInfo *dtor = nullptr;
            for (const auto &mi : lay.methods) {
                if (mi.is_destructor) {
                    dtor = &mi;
                    break;
                }
            }
            //  AOT.2.b/c/d: POO nativa -> al exit del scope, para una
            // instancia HEAP (`= new`): invocar `~T()` (si existe) y luego
            // liberar la memoria (RAW_FREE).  RAII determinista, sin GC, sin
            // leak.  Una instancia STACK (`Rect r;` -> alloca) NO se libera
            // (la pila se reclama sola).  El dtor se llama DIRECTO al tipo
            // estatico (`<owner>__<dtor>`); el resto del cuerpo del dtor (que
            // libera recursos propios, p.ej. free de un campo malloc) corre.
            const bool is_heap_new =
                vd->init && vd->init->kind == ast::NodeKind::NewExpr;
            // gc<T>: NO registrar cleanup RAII -- el GC (libvesta_gc) colecta
            // el objeto cuando deja de ser alcanzable (incl. ciclos). Liberarlo
            // por RAII seria un double-free (el GC ya lo gestiona).
            if (native_poo_ && is_heap_new && !sem_type.gc_managed) {
                CleanupAction act;
                act.kind = CleanupAction::Kind::NATIVE_FREE;
                act.operands = {v};
                act.source_line = vd->loc.line;
                act.refresh_name = vd->name;
                if (dtor) {
                    // AOT.2.d: nombre IR del dtor del tipo estatico ->
                    // se invoca antes del free.
                    const std::string owner = dtor->defining_class.empty()
                                                  ? sem_type.struct_name
                                                  : dtor->defining_class;
                    act.func_name =
                        owner + "__" + dtor->name; // <Class>____dtor
                    // AOT.2.d (4): dtor polimorfico.  Si la clase estatica
                    // tiene vtable (es base/derivada o implementa interfaz),
                    // el dtor es virtual -> despachar por la vtable de la
                    // INSTANCIA (no por el tipo estatico) para que
                    // `Base b = new Derived()` ejecute ~Derived().  Misma
                    // deteccion de needs_vtable que __new_<Class>.
                    bool has_vtable =
                        !lay.super_name.empty() || !lay.interface_names.empty();
                    if (!has_vtable)
                        for (const auto &kv : class_layouts)
                            if (kv.second.super_name == sem_type.struct_name) {
                                has_vtable = true;
                                break;
                            }
                    if (has_vtable) {
                        act.native_dtor_virtual = true;
                        act.dtor_vtable_index = dtor->vtable_index;
                    }
                }
                cleanup_stack_.push_back(std::move(act));
            } else if (dtor) {
                // cleanup CALL_DTOR: el regalloc ve un CALL/CALLVIRT
                // real y preserva los regs vivos del scope (incluido el
                // reg de v_ret en lower_return).  refresh_name garantiza
                // que el cleanup vea el binding actual del local si fue
                // reasignado tras el var-decl.
                CleanupAction act;
                act.kind = CleanupAction::Kind::CALL_DTOR;
                act.operands = {v};
                act.source_line = vd->loc.line;
                act.refresh_name = vd->name;
                act.dtor_vtable_index = dtor->vtable_index;
                // Dispatch estatico cuando el dtor NO es polimorfico: el tipo
                // declarado del contenedor coincide con el dinamico (no hay
                // super, ni interfaces, ni subclases) -> el dtor sintetizado se
                // resuelve en compile-time.  Emitir CALL DIRECTO al
                // `<owner>____dtor` en lugar de CALLVIRT: mas rapido (sin
                // vtable lookup) en interp/JIT y compilable en AOT
                // --target=bare (que no tiene vtable runtime).  Misma deteccion
                // de vtable que NATIVE_FREE / __new_<Class>.  Solo cuando
                // existe vtable (herencia/interfaz real) se conserva el
                // CALLVIRT.
                bool has_vtable =
                    !lay.super_name.empty() || !lay.interface_names.empty();
                if (!has_vtable)
                    for (const auto &kv : class_layouts)
                        if (kv.second.super_name == sem_type.struct_name) {
                            has_vtable = true;
                            break;
                        }
                if (!has_vtable) {
                    const std::string owner = dtor->defining_class.empty()
                                                  ? sem_type.struct_name
                                                  : dtor->defining_class;
                    act.func_name =
                        owner + "__" + dtor->name; // <Class>____dtor
                }
                cleanup_stack_.push_back(std::move(act));
            }
            // fix9 - eliminado el cleanup RAW_ASM `gchandle+drop`
            // para CLASS sin destructor (era el fix).  Ya no
            // necesario tras fix8 (GC stack scanning conservativo
            // con interior scan en OldGen): los handles que no aparecen
            // en stack/regs/external_refs son barridos automaticamente
            // por el major_gc.  Las restricciones que el fix antiguo
            // imponia (scopes_.size()<=2, !current_fn_has_try_) ya no
            // aplican.
        }
    }

    // fix9 - eliminado el cleanup RAW_ASM para `i64 obj =
    // newInstance(cls)` (era el fix2).  Mismo razonamiento que
    // el caso CLASS sin destructor: el GC stack scanning fix8
    // colecta automaticamente cualquier handle que no aparezca en
    // stack/regs vivos, sin importar si el var-decl es CLASS o I64
    // ni si la funcion tiene try/catch.

    // BugFix R9: SOLO registrar cleanup si el init es directamente un
    // constructor de coleccion (`arraylist(n)`, `hashmap(n)`, etc.).
    // Otras formas (cast, asignacion de otra var, return de func)
    // son ALIAS del mismo handle -> el owner original ya tiene cleanup;
    // duplicarlo causa double-free al exit.  Ejemplo: `ArrayList l1 =
    // (ArrayList)groups.get(1)` crea un alias del handle ya owned por
    // list1; sin este check, l1 se libera al exit Y list1 tambien
    // -> corrupcion del heap (exit 127).
    bool init_is_col_ctor = false;
    if (vd->init && vd->init->kind == ast::NodeKind::CallExpr) {
        auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
        if (ce->callee && ce->callee->kind == ast::NodeKind::IdentExpr) {
            auto *id = static_cast<ast::IdentExpr *>(ce->callee.get());
            if (find_col_ctor(id->name) != nullptr) {
                init_is_col_ctor = true;
            }
        }
    }
    if (v != ir::IR_NO_VALUE && is_col_kind(sem_type.kind) &&
        init_is_col_ctor &&
        escaping_locals_.find(vd->name) == escaping_locals_.end()) {
        // solo registramos cleanup si el local NO escapa
        // (ni return ni asignacion a campo/slot/deref).  Si escapa,
        // el caller toma posesion del handle y lo libera.
        const ColType *ct = find_col_type(sem_type.kind);
        if (ct) {
            // elegir variante *_free_gc cuando la coleccion
            // retiene refs GC (e.g. ArrayList<string>).  El frontend
            // setea pointee/pointee2 en sem_type al resolver el tipo
            // declarado; col_needs_gc_aware decide.
            PrimitiveKind elem_k = PrimitiveKind::VOID;
            PrimitiveKind val_k = PrimitiveKind::VOID;
            if (sem_type.pointee) elem_k = sem_type.pointee->kind;
            if (sem_type.pointee2) val_k = sem_type.pointee2->kind;
            // En native_poo (AOT) no hay VM -> no hay getproc ni el gc_addref/
            // release del runtime de la VM.  Usamos la variante NO-GC (libera
            // solo el almacenamiento de la coleccion); el lifetime de los
            // elementos lo gestiona el modelo nativo (RAII / gc<T>), no el
            // refcount de la VM.
            const bool gc_aware =
                (ct->native_free_fn_gc != nullptr) && !native_poo_ &&
                col_needs_gc_aware(sem_type.kind, elem_k, val_k);
            const char *fn_name =
                gc_aware ? ct->native_free_fn_gc : ct->native_free_fn;
            out_mod_->register_native_import(COL_NATIVE_LIB, fn_name);
            CleanupAction act;
            act.kind = CleanupAction::Kind::CALLN_FREE;
            act.operands = {v};
            act.source_line = vd->loc.line;
            act.refresh_name = vd->name;
            act.func_name = std::string(COL_NATIVE_LIB) + ":" + fn_name;
            act.needs_proc = gc_aware;
            cleanup_stack_.push_back(std::move(act));
        }
    }

    // ---- Smart pointers (H3 inc-on-copy): `shared<T> b = a` es una COPIA ----
    // (init es un IdentExpr de otro shared, no `shared_box`/`move`/factory).
    // Incrementamos el refcount del bloque de control: cada copia es un dueno
    // mas, y su SHAREDPTR_REL al exit lo decrementa.  Asi use_count es correcto
    // y (con free-when-0) el bloque se libera tras el ultimo dueno.  El move
    // (CallExpr `move`) y la construccion (`shared_box`) NO incrementan.
    if (sem_type.kind == PrimitiveKind::SHARED_PTR && v != ir::IR_NO_VALUE &&
        vd->init && vd->init->kind == ast::NodeKind::IdentExpr &&
        vd->init->result_type.kind == PrimitiveKind::SHARED_PTR) {
        emit_shared_refcount_inc(v, vd->loc.line);
    }

    // ---- Smart pointers: registrar cleanup automatico al scope exit ----
    //
    // Para @c unique<T>: SMARTPTR_FREE con literal_deleter="free" (default
    // Tier 0) o nombre de funcion deleter custom (set por unique_with).
    // Para @c shared<T>: SHAREDPTR_REL (refcount--; GC libera).
    //
    // Solo se registra si el local NO escapa (escaping_locals_).  Si
    // escapa, el caller toma posesion (return) o lo guarda
    // (asignacion a field/slot/deref), por lo que NO se debe liberar
    // aqui.
    if (v != ir::IR_NO_VALUE &&
        (sem_type.kind == PrimitiveKind::UNIQUE_PTR ||
         sem_type.kind == PrimitiveKind::SHARED_PTR) &&
        escaping_locals_.find(vd->name) == escaping_locals_.end()) {
        CleanupAction act;
        act.operands = {v};
        act.source_line = vd->loc.line;
        act.refresh_name = vd->name;
        if (sem_type.kind == PrimitiveKind::UNIQUE_PTR) {
            act.kind = CleanupAction::Kind::SMARTPTR_FREE;
            // Decision del literal_deleter (cleanup mas eficiente
            // posible segun la info compile-time disponible):
            //
            //   pending_smartptr_deleter_ no vacio
            //     -> init fue unique_with(_, deleter) -> usar ese deleter.
            //
            //   init es CallExpr (factory que devuelve unique<T>)
            //     -> dejar literal_deleter vacio -> dispatch dinamico
            //        via slot+8 al runtime (lee deleter del slot).
            //
            //   otro (init es unique_box, IdentExpr, etc.)
            //     -> usar "free" (Tier 1 con sentinel; el slot[+8]=0).
            if (!pending_smartptr_deleter_.empty()) {
                act.literal_deleter = pending_smartptr_deleter_;
            } else if (vd->init && vd->init->kind == ast::NodeKind::CallExpr) {
                auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
                bool is_factory_call = false;
                bool is_move_call = false;
                std::string move_src_name;
                if (ce->callee &&
                    ce->callee->kind == ast::NodeKind::IdentExpr) {
                    auto *cid = static_cast<ast::IdentExpr *>(ce->callee.get());
                    // Si el callee no es un builtin de smart pointer
                    // (unique_box/unique_with/shared_box/shared_with/move),
                    // asumimos factory de usuario y usamos dispatch dinamico.
                    const std::string &n = cid->name;
                    is_factory_call =
                        (n != "unique_box" && n != "unique_with" &&
                         n != "shared_box" && n != "shared_with" &&
                         n != "move");
                    is_move_call = (n == "move");
                    if (is_move_call && !ce->args.empty() &&
                        ce->args[0]->kind == ast::NodeKind::IdentExpr) {
                        move_src_name =
                            static_cast<ast::IdentExpr *>(ce->args[0].get())
                                ->name;
                    }
                }
                if (is_move_call) {
                    // bug4: `unique<T> b = move(a)`.  El move copia el deleter
                    // de `a` al slot `b[+8]`.  Resolvemos el deleter
                    // ESTATICAMENTE por el tipo/origen conocido de `a` (caso
                    // comun: `a` es una variable local con deleter conocido en
                    // compile-time) para emitir un cleanup DIRECTO (free / CALL
                    // <fn> / CALLN extern) sin dispatch dinamico ni lectura de
                    // slot+8 en runtime. Solo si `a` es opaca (deleter
                    // desconocido) caemos al dispatch dinamico ("").
                    auto it_del = unique_var_deleter_.find(move_src_name);
                    if (!move_src_name.empty() &&
                        it_del != unique_var_deleter_.end()) {
                        act.literal_deleter = it_del->second; // estatico
                    } else {
                        act.literal_deleter = ""; // dispatch dinamico (opaco)
                    }
                } else if (is_factory_call) {
                    act.literal_deleter = ""; // dispatch dinamico
                } else {
                    act.literal_deleter = "free";
                }
            } else {
                act.literal_deleter = "free"; // Tier 1 con sentinel
            }
            act.slot_size = 16; // Tier 1
            // Registrar el deleter estatico de esta variable para que un
            // futuro `move(<esta var>)` lo resuelva sin dispatch dinamico.
            // Solo cuando es conocido (no vacio -> no opaco).
            if (!act.literal_deleter.empty())
                unique_var_deleter_[vd->name] = act.literal_deleter;

            // Bug fix bug2: si el inner T es una CLASS Vesta con destructor,
            // registrar el vtable_index para que el cleanup invoque
            // `~T()` sobre el objeto contenido ANTES de liberar el slot.
            // Sin esto, `unique_box(new Recurso(1))` perdia el destructor
            // al exit del scope -- el slot se RAW_FREE'aba pero el
            // Recurso quedaba huerfano (eventual GC pero sin ~Recurso).
            if (sem_type.pointee &&
                sem_type.pointee->kind == PrimitiveKind::CLASS) {
                const auto &cls_layouts = tc_.class_layouts();
                auto it_cls = cls_layouts.find(sem_type.pointee->struct_name);
                if (it_cls != cls_layouts.end()) {
                    // Marcar siempre como inner GC class para que el
                    // cleanup NO haga RAW_FREE del host_ptr (que es un
                    // host_ptr a un objeto GC, no a memoria RAW_ALLOC).
                    act.inner_is_gc_class = true;
                    const ClassLayout &ilay = it_cls->second;
                    for (const auto &mi : ilay.methods) {
                        if (mi.is_destructor) {
                            act.inner_dtor_vtable_index = mi.vtable_index;
                            // Nombre directo del dtor (<owner>__<dtor>) para
                            // CALL directo en native_poo (AOT).
                            const std::string owner =
                                mi.defining_class.empty()
                                    ? sem_type.pointee->struct_name
                                    : mi.defining_class;
                            act.inner_dtor_func_name = owner + "__" + mi.name;
                            // Polimorfico si la clase inner tiene vtable
                            // (super/interfaz o alguna subclase) -> mismo
                            // criterio que __new_<Class> / NATIVE_FREE.
                            bool has_vtable = !ilay.super_name.empty() ||
                                              !ilay.interface_names.empty();
                            if (!has_vtable)
                                for (const auto &kv : cls_layouts)
                                    if (kv.second.super_name ==
                                        sem_type.pointee->struct_name) {
                                        has_vtable = true;
                                        break;
                                    }
                            act.inner_dtor_virtual = has_vtable;
                            break;
                        }
                    }
                }
            }
        } else {
            act.kind = CleanupAction::Kind::SHAREDPTR_REL;
            act.slot_size = 8;
        }
        cleanup_stack_.push_back(std::move(act));

        // gc<unique<T>>/gc<shared<T>> NO-escape (AOT): el box lleva un
        // finalizador GC (registrado por gc_box) para el caso ESCAPE, pero este
        // var NO escapa -> el cleanup RAII determinista de arriba libera el
        // recurso.  Si ademas corriera el finalizador al `gc_finalize_all` del
        // exit, el bloque de control se liberaria DOS VECES (RAW_FREE del
        // cleanup + free del finalizador; ademas con allocadores distintos:
        // slab vx_mem vs libc) -> corrupcion de heap (bug 245,
        // gc<shared<unique<i64>>> anidado).  Desregistramos el finalizador aqui
        // para que finalize_all lo salte (anti-doble-free, el modelo que la doc
        // de emit_gc_set_finalizer ya describia).  Solo en native_poo_ (AOT):
        // en interp/JIT el finalizador y el cleanup conviven sin corrupcion (la
        // memoria del box es del VM y su liberacion es idempotente).
        if (native_poo_ && sem_type.gc_managed) {
            ir::IrInstr ur{};
            ur.op = ir::IrOp::CALL;
            ur.type = ir::IrType::VOID;
            ur.dst = ir::IR_NO_VALUE;
            ur.func_name = "vx_gc_unregister_finalizer";
            ur.operands = {v};
            ur.is_call_site = true;
            ur.source_line = vd->loc.line;
            emit(current_block_, std::move(ur));
        }
    }
    // Limpiar pending_smartptr_deleter_ tras consumirlo (o si el
    // var-decl no era smart pointer pero hubo un unique_with previo
    // sin var-decl asociado, evitar contaminacion del siguiente).
    pending_smartptr_deleter_.clear();
}


// =========================================================================
//  Instrumentacion (vx_trace:enter / vx_trace:exit)
// =========================================================================
//
// Como el lowering emite CALLN a un nombre @c "vx_trace:enter" /
// @c "vx_trace:exit", todos los backends (bytecode VM, JIT, port C,
// futuros) heredan la instrumentacion automaticamente.  Cada backend
// resuelve el simbolo a su forma:
//   - bytecode VM: CALLN se resuelve via stdlib/native/runtime/vx_trace.dll
//   - JIT: idem (mismo CALLN dispatch)
//   - port C: emit_native_call lo bridgea a fprintf stderr (default)
//             o el usuario provee su propia implementacion.

void Lowering::emit_instrument_enter(const std::string &fn_name,
                                     uint32_t line) {
    if (!fn_ || !out_mod_) return;
    // 1. Internar el nombre como literal en static_data.  Incluye nul
    //    terminator para que sea NUL-terminated C string utilizable
    //    por strdup/printf en cualquier backend hosted.
    std::vector<uint8_t> bytes(fn_name.begin(), fn_name.end());
    bytes.push_back(0);
    const uint64_t name_idx = out_mod_->intern_static_data(std::move(bytes));

    // 2. STR_LIT_ADDR: cargar ptr al literal en un SSA value.
    const ir::IrValueId v_name = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr sa{};
        sa.op = ir::IrOp::STR_LIT_ADDR;
        sa.type = ir::IrType::PTR;
        sa.dst = v_name;
        sa.imm = name_idx;
        sa.source_line = line;
        emit(current_block_, std::move(sa));
    }

    // 3. CALLN void a "vx_trace:enter"(proc_ptr, name_ptr).
    //    El proc_ptr lo obtenemos via @c getproc; el plugin nativo
    //    lo usa para @c vm_read_bytes del nombre.  En port C el
    //    bridge ignora el proc_ptr.
    const ir::IrValueId v_proc = emit_getproc(line);
    ir::IrInstr call{};
    call.op = ir::IrOp::CALLN;
    call.type = ir::IrType::VOID;
    call.dst = ir::IR_NO_VALUE;
    // El @c lib_path incluye el subdir bajo @c stdlib/native/ para
    // que el loader pueda resolver la DLL via path relativo al
    // @c vm.exe (igual convencion que vesta_io / vesta_math).
    call.func_name = "stdlib/native/runtime/vx_trace:enter";
    call.operands = {v_proc, v_name};
    call.source_line = line;
    emit(current_block_, std::move(call));

    // 4. Registrar el import nativo para que el linker .velb
    //    incluya la libreria.
    out_mod_->register_native_import("stdlib/native/runtime/vx_trace", "enter");
}

void Lowering::emit_instrument_exit(const std::string &fn_name,
                                    ir::IrValueId v_ret, uint32_t line) {
    if (!fn_ || !out_mod_) return;
    std::vector<uint8_t> bytes(fn_name.begin(), fn_name.end());
    bytes.push_back(0);
    const uint64_t name_idx = out_mod_->intern_static_data(std::move(bytes));

    const ir::IrValueId v_name = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr sa{};
        sa.op = ir::IrOp::STR_LIT_ADDR;
        sa.type = ir::IrType::PTR;
        sa.dst = v_name;
        sa.imm = name_idx;
        sa.source_line = line;
        emit(current_block_, std::move(sa));
    }

    // Si la funcion es void, pasar 0 como return value placeholder.
    ir::IrValueId v_val = v_ret;
    if (v_val == ir::IR_NO_VALUE) {
        v_val = emit_const(ir::IrType::I64, 0, line);
    }

    const ir::IrValueId v_proc = emit_getproc(line);
    ir::IrInstr call{};
    call.op = ir::IrOp::CALLN;
    call.type = ir::IrType::VOID;
    call.dst = ir::IR_NO_VALUE;
    // Usamos @c leave en lugar de @c exit para evitar colision con la
    // libc @c exit() cuando el port C emite @c extern declarations.
    call.func_name = "stdlib/native/runtime/vx_trace:leave";
    call.operands = {v_proc, v_name, v_val};
    call.source_line = line;
    emit(current_block_, std::move(call));

    out_mod_->register_native_import("stdlib/native/runtime/vx_trace", "leave");
}

// Forward decls de helpers definidos mas abajo en el TU.  Necesarias
// porque lower_try y try_lower_builtin_call los usan.

/// usado por lower_class_methods para emitir el CALLVIRT a
/// destructores de fields destructibles del contenedor.
// ---------------------------------------------------------------------
// try / catch / throw.
//
// Estrategia: usamos las instrucciones bytecode existentes
// tryenter / tryleave / throw.  El IR no tiene un nodo dedicado para
// exception frames; emitimos RAW_ASM con substitucion {dst}/{srcN}
// para colocar el handler PC y el ClassInfo* en registros.
//
// Layout de bloques (1 catch, sin finally):
//   current      -> RAW_ASM: findclass exc + tryenter handler, type
//                  -> br body
//   body         -> lower(try body)
//                  -> RAW_ASM: tryleave + jmp merge
//   handler      -> bind r0 a var (si la hay) + lower(catch body)
//                  -> br merge
//   merge        -> continuacion
//
// Multi-catch / finally: pendientes (deferidos en MVP).
//
// El handler PC se obtiene como @Absolute("code.<fn>_<handler.name>")
// donde <handler.name> incluye el sufijo numerico que new_block añade.
// Asi el linker resuelve la referencia sin necesitar metadata extra.
// ---------------------------------------------------------------------


// ---------------------------------------------------------------------
// foreach: for (T x : col) body  -- desazucarado a counted loop.
//
// Patron sintetizado para `for (T x : arr) body` con arr: T[N]:
//   {
//     i32 __idx = 0;
//     while (__idx < N) {
//       T x = arr[__idx];
//       body
//       __idx = __idx + 1;
//     }
//   }
//
// Para requerimos N conocido en compile time (T[N]).  Para
// T[] (decay-to-pointer) sin tamano se necesita un parametro de
// longitud explicito o un objeto Array<T> managed (deferido).
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// emit_cleanups_all: emite todos los cleanups activos en orden inverso
// (mas reciente primero).  No modifica el stack: el caller (e.g.,
// lower_synchronized) hace su pop por flujo normal.
// ---------------------------------------------------------------------
void Lowering::emit_cleanups_all() {
    emit_cleanups_range(0, cleanup_stack_.size());
}

void Lowering::emit_cleanups_range(size_t start, size_t end) {
    if (end > cleanup_stack_.size()) end = cleanup_stack_.size();
    if (start >= end) return;
    // Recorrer [start, end) en orden INVERSO (LIFO).  El cleanup mas
    // reciente (top del stack) se ejecuta primero, igual que destructores
    // C++ en el orden inverso a su construccion.
    for (size_t k = end; k-- > start;) {
        const CleanupAction *it = &cleanup_stack_[k];
        std::vector<ir::IrValueId> opnds = it->operands;
        // refresh: sustituir operands[0] con el binding ACTUAL
        // del local (permite dispose(xs)+cleanup idempotente, etc.).
        if (!it->refresh_name.empty() && !opnds.empty()) {
            const ir::IrValueId v_now = lookup(it->refresh_name);
            if (v_now != ir::IR_NO_VALUE) {
                opnds[0] = v_now;
            }
        }
        switch (it->kind) {
        case CleanupAction::Kind::CALL_DTOR: {
            if (!it->func_name.empty()) {
                // Dispatch estatico: el dtor sintetizado del contenedor se
                // resuelve por el tipo declarado (no polimorfico).  CALL
                // DIRECTO -> mas rapido (sin vtable lookup) y compilable en
                // AOT --target=bare.  El regalloc lo trata como CALL y
                // preserva los regs vivos del scope (incluido v_ret).
                ir::IrInstr cd{};
                cd.op = ir::IrOp::CALL;
                cd.type = ir::IrType::VOID;
                cd.dst = ir::IR_NO_VALUE;
                cd.operands = std::move(opnds);
                cd.func_name = it->func_name;
                cd.source_line = it->source_line;
                emit(current_block_, std::move(cd));
                break;
            }
            // Dtor polimorfico (herencia/interfaz): emitir CALLVIRT real
            // para que el regalloc lo trate como CALL y preserve regs
            // caller-saved vivos (especialmente el reg que lleva v_ret en
            // lower_return).
            ir::IrInstr cv{};
            cv.op = ir::IrOp::CALLVIRT;
            cv.type = ir::IrType::VOID;
            cv.dst = ir::IR_NO_VALUE;
            cv.operands = std::move(opnds);
            cv.imm = static_cast<uint64_t>(it->dtor_vtable_index);
            cv.source_line = it->source_line;
            emit(current_block_, std::move(cv));
            break;
        }
        case CleanupAction::Kind::STRUCT_DTOR: {
            // CALL directo a <Struct>__dtor(addr): dispatch estatico (los
            // structs no tienen vtable).  IrOp::CALL -> CALLVM en interp/JIT,
            // call nativo en AOT; el inliner puede inlinearlo (dtor trivial =
            // coste ~0).  El regalloc lo trata como CALL y preserva los regs
            // vivos del scope (incluido el reg de v_ret en lower_return).
            ir::IrInstr cd{};
            cd.op = ir::IrOp::CALL;
            cd.type = ir::IrType::VOID;
            cd.dst = ir::IR_NO_VALUE;
            cd.operands = std::move(opnds);
            cd.func_name = it->func_name;
            cd.source_line = it->source_line;
            emit(current_block_, std::move(cd));
            break;
        }
        case CleanupAction::Kind::CLOSURE_ENV_FREE: {
            // Ownership: liberar el env+slot heap de cada campo closure del
            // struct (move-on-return: el productor suprimio su cleanup via
            // escaping_locals_, asi que este consumidor es el unico que
            // libera).  opnds[0] = PTR al struct (refrescado).  Por campo,
            // reusa emit_free_closure_env_field (null-guard interno).
            if (!opnds.empty()) {
                for (uint32_t off : it->closure_field_offsets) {
                    emit_free_closure_env_field(opnds[0], off, it->source_line);
                }
            }
            break;
        }
        case CleanupAction::Kind::NATIVE_FREE: {
            //  AOT.2.d: invocar `~T()` (CALL directo al dtor del
            // tipo estatico) ANTES de liberar -> el dtor libera sus
            // recursos propios (RAII).  Luego RAW_FREE de la instancia
            // (host_ptr de calloc) -> aot_lower lo baja a call<free>.
            // RAW_FREE(0)/free(NULL) es no-op -> seguro si fue movido.
            if (it->native_dtor_virtual) {
                // AOT.2.d (4): dtor polimorfico via vtable de la
                // instancia.  %vt = LOAD [obj+0]; %fn = LOAD
                // [%vt + idx*8]; CALLIND %fn(obj).  Asi una ref base
                // que posee una instancia derivada ejecuta el dtor
                // DERIVADO (la vtable de obj[0] la puso __new_<Derived>).
                const ir::IrValueId obj = opnds[0];
                const uint32_t idx = it->dtor_vtable_index;
                ir::IrValueId v_vt = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_vt].is_host_ptr = true;
                {
                    ir::IrInstr ld{};
                    ld.op = ir::IrOp::LOAD;
                    ld.type = ir::IrType::I64;
                    ld.dst = v_vt;
                    ld.operands = {obj};
                    ld.source_line = it->source_line;
                    emit(current_block_, std::move(ld));
                }
                ir::IrValueId v_slot = v_vt;
                if (idx != 0) {
                    const ir::IrValueId v_off = emit_const(
                        ir::IrType::I64, static_cast<uint64_t>(idx) * 8u,
                        it->source_line);
                    v_slot = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_slot].is_host_ptr = true;
                    {
                        ir::IrInstr ad{};
                        ad.op = ir::IrOp::ADD;
                        ad.type = ir::IrType::PTR;
                        ad.dst = v_slot;
                        ad.operands = {v_vt, v_off};
                        ad.source_line = it->source_line;
                        emit(current_block_, std::move(ad));
                    }
                }
                ir::IrValueId v_fn = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_fn].is_host_ptr = true;
                {
                    ir::IrInstr ld2{};
                    ld2.op = ir::IrOp::LOAD;
                    ld2.type = ir::IrType::I64;
                    ld2.dst = v_fn;
                    ld2.operands = {v_slot};
                    ld2.source_line = it->source_line;
                    emit(current_block_, std::move(ld2));
                }
                ir::IrInstr ci{};
                ci.op = ir::IrOp::CALLIND;
                ci.type = ir::IrType::VOID;
                ci.dst = ir::IR_NO_VALUE;
                ci.func_ptr = v_fn;
                ci.operands = {obj};
                ci.source_line = it->source_line;
                emit(current_block_, std::move(ci));
            } else if (!it->func_name.empty()) {
                ir::IrInstr dc{};
                dc.op = ir::IrOp::CALL;
                dc.type = ir::IrType::VOID;
                dc.dst = ir::IR_NO_VALUE;
                dc.func_name = it->func_name;
                dc.operands = opnds; // this
                dc.source_line = it->source_line;
                emit(current_block_, std::move(dc));
            }
            ir::IrInstr rf{};
            rf.op = ir::IrOp::RAW_FREE;
            rf.type = ir::IrType::VOID;
            rf.dst = ir::IR_NO_VALUE;
            rf.operands = std::move(opnds);
            rf.source_line = it->source_line;
            emit(current_block_, std::move(rf));
            break;
        }
        case CleanupAction::Kind::STRING_FREE: {
            // Vesta Embed Inc 0 / Inc 5 (SSO): liberar el buffer de un
            // string value-type al exit del scope.  opnds[0] = PTR al slot
            // de 24 bytes.  emit_native_str_free_if_heap libera SOLO si el
            // slot esta en modo HEAP (la data SSO es inline, no se libera)
            // y free(0) es no-op -> seguro tras un move-out.
            emit_native_str_free_if_heap(opnds[0], it->source_line);
            break;
        }
        case CleanupAction::Kind::RAW_ASM: {
            // raw_asm-elim wave 2: dead code.  Todas las creaciones
            // de CleanupAction setean su @c kind explicitamente a un
            // valor especifico (CALL_DTOR/CALLN_FREE/SMARTPTR_FREE/
            // SHAREDPTR_REL/SYNC_EXIT).  Si esta rama se alcanza, es
            // un bug del frontend que olvido setear el kind; emitir
            // diagnostico claro en lugar de raw_asm opaco.
            error_at(
                SourceLoc{"", it->source_line, 1},
                "internal: CleanupAction con Kind::RAW_ASM (default) "
                "alcanzado al exit del scope; el frontend debe setear "
                "un kind especifico (CALL_DTOR/CALLN_FREE/SMARTPTR_FREE/etc.)");
            break;
        }
        case CleanupAction::Kind::SYNC_EXIT: {
            // Sprint 6.C: tryleave + monexit como IR ops puros.
            // AOT (native_poo_): el frame de excepcion es setjmp/longjmp ->
            // se popea con __vx_pop_frame (no TRYLEAVE op, que el backend
            // nativo no soporta); el monitor se libera con __vx_monexit.
            if (native_poo_) {
                ir::IrInstr cp{};
                cp.op = ir::IrOp::CALL;
                cp.type = ir::IrType::VOID;
                cp.dst = ir::IR_NO_VALUE;
                cp.func_name = "__vx_pop_frame";
                cp.source_line = it->source_line;
                emit(current_block_, std::move(cp));
            } else {
                ir::IrInstr tl{};
                tl.op = ir::IrOp::TRYLEAVE;
                tl.type = ir::IrType::VOID;
                tl.dst = ir::IR_NO_VALUE;
                tl.source_line = it->source_line;
                emit(current_block_, std::move(tl));
            }
            if (!opnds.empty()) {
                emit_monitor_op(opnds[0], /*enter=*/false, it->source_line);
            }
            break;
        }
        case CleanupAction::Kind::CALLN_FREE: {
            // CALLN al free nativo de la coleccion (variante GC
            // o no-GC).  Para la variante *_gc prependemos un
            // GETPROC como primer argumento; el
            // regalloc trata el CALLN como call normal y preserva
            // los regs vivos del caller.
            std::vector<ir::IrValueId> args;
            if (it->needs_proc) {
                args.reserve(opnds.size() + 1);
                args.push_back(emit_getproc(it->source_line));
            } else {
                args.reserve(opnds.size());
            }
            for (auto vid : opnds)
                args.push_back(vid);
            ir::IrInstr cf{};
            cf.op = ir::IrOp::CALLN;
            cf.type = ir::IrType::VOID;
            cf.dst = ir::IR_NO_VALUE;
            cf.func_name = it->func_name;
            cf.operands = std::move(args);
            cf.source_line = it->source_line;
            emit(current_block_, std::move(cf));
            break;
        }
        case CleanupAction::Kind::SMARTPTR_FREE: {
            // Cleanup de @c unique<T> en scope exit.
            //
            // Tier 1 layout: slot[+0]=ptr, slot[+8]=deleter_addr.
            //   deleter_addr == 0 -> sentinel: RAW_FREE(ptr).
            //   deleter_addr != 0 -> CALLVMR(deleter_addr, ptr).
            //
            // Si literal_deleter esta poblado (caso comun:
            // var-decl con init = unique_box/unique_with), usamos
            // ese conocimiento compile-time para emitir el cleanup
            // mas eficiente (RAW_FREE directo, CALLVM @Absolute
            // fijo, o CALLN @Method para extern wrappers).
            //
            // Si NO esta poblado (caso SRET: el unique vino de
            // una funcion que lo creo internamente), leemos el
            // deleter_addr del slot+8 y dispatchamos dinamicamente.
            const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ptr].is_host_ptr = true;
            {
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = ir::IrType::I64;
                ld.dst = v_ptr;
                ld.operands = opnds; // [v_slot]
                ld.source_line = it->source_line;
                emit(current_block_, std::move(ld));
            }

            // Bug fix bug2: si el inner T es una CLASS Vesta con
            // destructor, invocar `~T()` ANTES del free.  El
            // CALLVIRT requiere host_ptr no nulo; emitimos guard
            // implicito via skip si v_ptr == 0 (no debe ocurrir
            // tras unique_box(new T()), pero defensive).
            //
            // Bug fix adicional: si inner_is_gc_class, el host_ptr
            // que vive en el slot apunta a un objeto GC-managed
            // (no a RAW_ALLOC memory).  Hacer RAW_FREE corromperia
            // el heap.  Solo invocamos el destructor + dejamos
            // que el GC libere el objeto cuando ningun root lo
            // referencie (stack scanning A.34.fix8).
            if (it->inner_dtor_vtable_index > 0) {
                // AOT (native_poo): el inner de un unique<T> tiene tipo
                // ESTATICO conocido (T == tipo dinamico salvo polimorfismo).
                // Si NO es polimorfico, despachar el dtor con un CALL DIRECTO a
                // `<Class>____dtor` (PURE_NATIVE) en vez de CALLVIRT (que el
                // selector AOT no soporta).  Asi unique<T> con dtor compila a
                // nativo.  Para inner polimorfico o el path VM/JIT, CALLVIRT.
                if (native_poo_ && !it->inner_dtor_virtual &&
                    !it->inner_dtor_func_name.empty()) {
                    ir::IrInstr cd{};
                    cd.op = ir::IrOp::CALL;
                    cd.type = ir::IrType::VOID;
                    cd.dst = ir::IR_NO_VALUE;
                    cd.operands = {v_ptr};
                    cd.func_name = it->inner_dtor_func_name; // <Class>____dtor
                    cd.source_line = it->source_line;
                    emit(current_block_, std::move(cd));
                } else {
                    ir::IrInstr cv{};
                    cv.op = ir::IrOp::CALLVIRT;
                    cv.type = ir::IrType::VOID;
                    cv.dst = ir::IR_NO_VALUE;
                    cv.operands = {v_ptr};
                    cv.imm = static_cast<uint64_t>(it->inner_dtor_vtable_index);
                    cv.source_line = it->source_line;
                    emit(current_block_, std::move(cv));
                }
            }
            if (it->inner_is_gc_class) {
                // El objeto inner es GC-managed: NO hacer RAW_FREE
                // del host_ptr (el GC se encarga del inner object).
                // El SLOT (raw-alloced de 8/16 bytes) tambien necesita
                // liberarse, pero usa slot_addr (opnds[0]) no v_ptr.
                // Sin embargo, el slot RAW_ALLOC vive solo si fue
                // unique_box (que sigue siendo Tier 0 sin slot RAW).
                // En Tier 1 el slot es ALLOCA stack (no requiere free).
                // Por simplicidad: skip el free completo en este caso.
                // El GC libera el inner; el ALLOCA stack se libera
                // al exit del frame automaticamente.
                break;
            }

            // AOT (native_poo): el selector HOST_LEAF NO soporta el op
            // SMARTPTR_FREE.  Bajamos el cleanup a ops nativas que el selector
            // ya conoce: guard de null (el slot se zerifica tras un move -> no
            // llamar al deleter sobre un null) + CALL al deleter (Vesta) /
            // CALLN (extern) / RAW_FREE (default "free", null-safe) / CALLIND
            // dinamico (SRET).  El path VM/JIT (no native) sigue usando el op
            // SMARTPTR_FREE mas abajo.
            if (native_poo_) {
                const uint32_t ln = it->source_line;
                // "free" es null-safe (RAW_FREE(0)=no-op) -> sin guard.
                if (it->literal_deleter == "free") {
                    ir::IrInstr fr{};
                    fr.op = ir::IrOp::RAW_FREE;
                    fr.type = ir::IrType::VOID;
                    fr.dst = ir::IR_NO_VALUE;
                    fr.operands = {v_ptr};
                    fr.source_line = ln;
                    emit(current_block_, std::move(fr));
                    break;
                }
                // Resto (deleter custom/extern/SRET): guard `if (ptr != 0)`.
                const ir::IrBlockId bb_do = fn_->new_block("sp_do");
                const ir::IrBlockId bb_skip = fn_->new_block("sp_skip");
                const ir::IrValueId v_z = emit_const(ir::IrType::I64, 0, ln);
                const ir::IrValueId v_cond = fn_->new_value(ir::IrType::BOOL);
                {
                    ir::IrInstr cm{};
                    cm.op = ir::IrOp::CMP_NE;
                    cm.type = ir::IrType::BOOL;
                    cm.dst = v_cond;
                    cm.operands = {v_ptr, v_z};
                    cm.source_line = ln;
                    emit(current_block_, std::move(cm));
                }
                {
                    ir::IrInstr br{};
                    br.op = ir::IrOp::BR_COND;
                    br.type = ir::IrType::VOID;
                    br.dst = ir::IR_NO_VALUE;
                    br.operands = {v_cond};
                    br.target_block = bb_do;
                    br.false_block = bb_skip;
                    br.source_line = ln;
                    emit(current_block_, std::move(br));
                    fn_->blocks[current_block_].succs.push_back(bb_do);
                    fn_->blocks[current_block_].succs.push_back(bb_skip);
                    fn_->blocks[bb_do].preds.push_back(current_block_);
                    fn_->blocks[bb_skip].preds.push_back(current_block_);
                }
                current_block_ = bb_do;
                if (it->literal_deleter.rfind("@extern:", 0) == 0) {
                    // deleter extern "<lib>:<fn>" -> CALLN (HOST_LEAF lo baja a
                    // CALL_SYM; el linker lo resuelve).
                    const std::string sym = it->literal_deleter.substr(8);
                    ir::IrInstr cn{};
                    cn.op = ir::IrOp::CALLN;
                    cn.type = ir::IrType::VOID;
                    cn.dst = ir::IR_NO_VALUE;
                    cn.func_name = sym;
                    cn.operands = {v_ptr};
                    cn.source_line = ln;
                    cn.is_call_site = true;
                    emit(current_block_, std::move(cn));
                } else if (it->literal_deleter.empty()) {
                    // SRET: deleter dinamico en slot+8.  Si !=0 -> CALLIND;
                    // si ==0 -> RAW_FREE.  Nested guard.
                    const ir::IrValueId v_eight =
                        emit_const(ir::IrType::I64, 8, ln);
                    const ir::IrValueId v_slot8 =
                        fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_slot8].is_host_ptr = true;
                    {
                        ir::IrInstr ad{};
                        ad.op = ir::IrOp::ADD;
                        ad.type = ir::IrType::PTR;
                        ad.dst = v_slot8;
                        ad.operands = {opnds[0], v_eight};
                        ad.source_line = ln;
                        emit(current_block_, std::move(ad));
                    }
                    const ir::IrValueId v_del = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_del].is_host_ptr = true;
                    {
                        ir::IrInstr ld{};
                        ld.op = ir::IrOp::LOAD;
                        ld.type = ir::IrType::I64;
                        ld.dst = v_del;
                        ld.operands = {v_slot8};
                        ld.source_line = ln;
                        emit(current_block_, std::move(ld));
                    }
                    const ir::IrBlockId bb_call = fn_->new_block("sp_call");
                    const ir::IrBlockId bb_free = fn_->new_block("sp_free");
                    const ir::IrValueId v_z2 =
                        emit_const(ir::IrType::I64, 0, ln);
                    const ir::IrValueId v_c2 = fn_->new_value(ir::IrType::BOOL);
                    {
                        ir::IrInstr cm{};
                        cm.op = ir::IrOp::CMP_NE;
                        cm.type = ir::IrType::BOOL;
                        cm.dst = v_c2;
                        cm.operands = {v_del, v_z2};
                        cm.source_line = ln;
                        emit(current_block_, std::move(cm));
                    }
                    {
                        ir::IrInstr br{};
                        br.op = ir::IrOp::BR_COND;
                        br.type = ir::IrType::VOID;
                        br.dst = ir::IR_NO_VALUE;
                        br.operands = {v_c2};
                        br.target_block = bb_call;
                        br.false_block = bb_free;
                        br.source_line = ln;
                        emit(current_block_, std::move(br));
                        fn_->blocks[current_block_].succs.push_back(bb_call);
                        fn_->blocks[current_block_].succs.push_back(bb_free);
                        fn_->blocks[bb_call].preds.push_back(current_block_);
                        fn_->blocks[bb_free].preds.push_back(current_block_);
                    }
                    // bb_call: CALLIND deleter(ptr) -> bb_skip.
                    current_block_ = bb_call;
                    {
                        ir::IrInstr ci{};
                        ci.op = ir::IrOp::CALLIND;
                        ci.type = ir::IrType::VOID;
                        ci.dst = ir::IR_NO_VALUE;
                        ci.func_ptr = v_del;
                        ci.operands = {v_ptr};
                        ci.source_line = ln;
                        ci.is_call_site = true;
                        emit(current_block_, std::move(ci));
                        ir::IrInstr br{};
                        br.op = ir::IrOp::BR;
                        br.type = ir::IrType::VOID;
                        br.target_block = bb_skip;
                        br.source_line = ln;
                        emit(current_block_, std::move(br));
                        fn_->blocks[bb_call].succs.push_back(bb_skip);
                        fn_->blocks[bb_skip].preds.push_back(bb_call);
                    }
                    // bb_free: RAW_FREE(ptr) -> bb_skip.
                    current_block_ = bb_free;
                    {
                        ir::IrInstr fr{};
                        fr.op = ir::IrOp::RAW_FREE;
                        fr.type = ir::IrType::VOID;
                        fr.dst = ir::IR_NO_VALUE;
                        fr.operands = {v_ptr};
                        fr.source_line = ln;
                        emit(current_block_, std::move(fr));
                        ir::IrInstr br{};
                        br.op = ir::IrOp::BR;
                        br.type = ir::IrType::VOID;
                        br.target_block = bb_skip;
                        br.source_line = ln;
                        emit(current_block_, std::move(br));
                        fn_->blocks[bb_free].succs.push_back(bb_skip);
                        fn_->blocks[bb_skip].preds.push_back(bb_free);
                    }
                    current_block_ = bb_skip;
                    break;
                } else {
                    // deleter Vesta (fn por nombre) -> CALL directo.
                    ir::IrInstr ca{};
                    ca.op = ir::IrOp::CALL;
                    ca.type = ir::IrType::VOID;
                    ca.dst = ir::IR_NO_VALUE;
                    ca.func_name = it->literal_deleter;
                    ca.operands = {v_ptr};
                    ca.source_line = ln;
                    ca.is_call_site = true;
                    emit(current_block_, std::move(ca));
                }
                // bb_do -> bb_skip (para extern/vesta; SRET ya retorno).
                {
                    ir::IrInstr br{};
                    br.op = ir::IrOp::BR;
                    br.type = ir::IrType::VOID;
                    br.target_block = bb_skip;
                    br.source_line = ln;
                    emit(current_block_, std::move(br));
                    fn_->blocks[bb_do].succs.push_back(bb_skip);
                    fn_->blocks[bb_skip].preds.push_back(bb_do);
                }
                current_block_ = bb_skip;
                break;
            }

            if (it->literal_deleter.empty()) {
                // SRET case: el smart pointer vino de una funcion
                // (factory).  No tenemos info compile-time del
                // deleter; lo leemos dinamicamente del slot+8.
                // Si deleter_addr == 0 -> RAW_FREE; si != 0 ->
                // callvmr al puntero (deleter Vesta).
                const ir::IrValueId v_eight =
                    emit_const(ir::IrType::I64, 8, it->source_line);
                const ir::IrValueId v_slot8 = fn_->new_value(ir::IrType::PTR);
                {
                    ir::IrInstr add{};
                    add.op = ir::IrOp::ADD;
                    add.type = ir::IrType::I64;
                    add.dst = v_slot8;
                    add.operands = {opnds[0], v_eight};
                    add.source_line = it->source_line;
                    emit(current_block_, std::move(add));
                }
                const ir::IrValueId v_del = fn_->new_value(ir::IrType::I64);
                {
                    ir::IrInstr ldd{};
                    ldd.op = ir::IrOp::LOAD;
                    ldd.type = ir::IrType::I64;
                    ldd.dst = v_del;
                    ldd.operands = {v_slot8};
                    ldd.source_line = it->source_line;
                    emit(current_block_, std::move(ldd));
                }
                const uint32_t lbl = ++cleanup_label_seq_;
                const std::string default_lbl =
                    "__sp_def_" + std::to_string(lbl);
                const std::string skip_lbl = "__sp_skip_" + std::to_string(lbl);
                const std::string done_lbl = "__sp_done_" + std::to_string(lbl);
                // cmpu ptr, 0; jmp.je done  (skip si moved)
                // cmpu deleter, 0; jmp.je default  (deleter=0 -> RAW_FREE)
                // mov r1, ptr; mov r15, 1; callvmr deleter; jmp done
                // default: mov r1, ptr; (RAW_FREE inline)
                // raw_asm-elim wave 2: SMARTPTR_FREE kind=0 (SRET_DISPATCH).
                // El emitter expande a la secuencia equivalente con
                // labels unicos via contador thread-local; mismo
                // bytecode emitido.  Eliminamos done_lbl/default_lbl
                // ya que el emitter los genera internamente.
                ir::IrInstr sf{};
                sf.op = ir::IrOp::SMARTPTR_FREE;
                sf.type = ir::IrType::VOID;
                sf.dst = ir::IR_NO_VALUE;
                sf.operands = {v_ptr, v_del};
                sf.imm = 0; /* SRET_DISPATCH */
                sf.source_line = it->source_line;
                sf.is_call_site = true;
                emit(current_block_, std::move(sf));
                (void)done_lbl;
                (void)default_lbl; /* labels no usadas (emitter las genera) */
            } else if (it->literal_deleter == "free") {
                // Deleter por defecto: RAW_FREE (null-safe).
                ir::IrInstr fr{};
                fr.op = ir::IrOp::RAW_FREE;
                fr.type = ir::IrType::VOID;
                fr.dst = ir::IR_NO_VALUE;
                fr.operands = {v_ptr};
                fr.source_line = it->source_line;
                emit(current_block_, std::move(fr));
            } else if (it->literal_deleter.rfind("@extern:", 0) == 0) {
                // raw_asm-elim wave 2: SMARTPTR_FREE kind=1 (EXTERN_CALLN).
                const std::string fn_label =
                    it->literal_deleter.substr(8); // skip "@extern:"
                ir::IrInstr sf{};
                sf.op = ir::IrOp::SMARTPTR_FREE;
                sf.type = ir::IrType::VOID;
                sf.dst = ir::IR_NO_VALUE;
                sf.operands = {v_ptr};
                sf.imm = 1;              /* EXTERN_CALLN */
                sf.func_name = fn_label; /* "<lib>:<fn>" */
                sf.source_line = it->source_line;
                sf.is_call_site = true;
                emit(current_block_, std::move(sf));
            } else {
                // raw_asm-elim wave 2: SMARTPTR_FREE kind=2 (VESTA_CALLVM).
                ir::IrInstr sf{};
                sf.op = ir::IrOp::SMARTPTR_FREE;
                sf.type = ir::IrType::VOID;
                sf.dst = ir::IR_NO_VALUE;
                sf.operands = {v_ptr};
                sf.imm = 2;                         /* VESTA_CALLVM */
                sf.func_name = it->literal_deleter; /* "<fn_label>" */
                sf.source_line = it->source_line;
                sf.is_call_site = true;
                emit(current_block_, std::move(sf));
            }
            break;
        }
        case CleanupAction::Kind::SHAREDPTR_REL: {
            // Sprint 6.C: cleanup de @c shared<T> via IR ops puros.
            //
            // Implementacion: LOAD ctrl; si ctrl != 0, LOAD rc; SUB 1; STORE
            // rc. No emitimos free explicito porque el GcHeap se encarga de
            // liberar bloques sin roots cuando se ejecuta major_gc.
            //
            // Antes: 7 lineas de RAW_ASM con jmp.je por label.
            // Ahora: 7 IR ops (LOAD + CMP + BR_COND + 2 bloques + LOAD + SUB +
            // STORE).
            if (opnds.empty()) break;
            const ir::IrValueId v_slot = opnds[0];
            // ctrl = LOAD i64 [v_slot]   (host_ptr al control block).
            const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ctrl].is_host_ptr = true;
            {
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = ir::IrType::I64;
                ld.dst = v_ctrl;
                ld.operands = {v_slot};
                ld.source_line = it->source_line;
                emit(current_block_, std::move(ld));
            }
            // cmp ctrl, 0  -- si moved/null, skip.
            const ir::IrValueId v_zero =
                emit_const(ir::IrType::I64, 0, it->source_line);
            const ir::IrValueId v_cmp = fn_->new_value(ir::IrType::BOOL);
            {
                ir::IrInstr cmp{};
                cmp.op = ir::IrOp::CMP_NE;
                cmp.type = ir::IrType::I64;
                cmp.dst = v_cmp;
                cmp.operands = {v_ctrl, v_zero};
                cmp.source_line = it->source_line;
                emit(current_block_, std::move(cmp));
            }
            // br.cond v_cmp, dec_bb, skip_bb.
            const ir::IrBlockId dec_bb = fn_->new_block("sh_dec");
            const ir::IrBlockId skip_bb = fn_->new_block("sh_skip");
            {
                ir::IrInstr br{};
                br.op = ir::IrOp::BR_COND;
                br.operands = {v_cmp};
                br.target_block = dec_bb;
                br.false_block = skip_bb;
                br.source_line = it->source_line;
                emit(current_block_, std::move(br));
                fn_->blocks[current_block_].succs.push_back(dec_bb);
                fn_->blocks[current_block_].succs.push_back(skip_bb);
                fn_->blocks[dec_bb].preds.push_back(current_block_);
                fn_->blocks[skip_bb].preds.push_back(current_block_);
            }
            // dec_bb: refcount-- (LOAD + SUB + STORE).
            current_block_ = dec_bb;
            const ir::IrValueId v_rc = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = ir::IrType::I64;
                ld.dst = v_rc;
                ld.operands = {v_ctrl};
                ld.source_line = it->source_line;
                emit(current_block_, std::move(ld));
            }
            const ir::IrValueId v_one =
                emit_const(ir::IrType::I64, 1, it->source_line);
            const ir::IrValueId v_rc_dec = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr sub{};
                sub.op = ir::IrOp::SUB;
                sub.type = ir::IrType::I64;
                sub.dst = v_rc_dec;
                sub.operands = {v_rc, v_one};
                sub.source_line = it->source_line;
                emit(current_block_, std::move(sub));
            }
            {
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {v_rc_dec, v_ctrl};
                st.source_line = it->source_line;
                emit(current_block_, std::move(st));
            }
            // H3 no-GC: si el refcount cayo a 0, liberar el bloque de control
            // (RAW_FREE).  Refcount puro determinista -> sin GC.  cmp rc==0.
            const ir::IrValueId v_zero2 =
                emit_const(ir::IrType::I64, 0, it->source_line);
            const ir::IrValueId v_is0 = fn_->new_value(ir::IrType::BOOL);
            {
                ir::IrInstr cmp{};
                cmp.op = ir::IrOp::CMP_EQ;
                cmp.type = ir::IrType::I64;
                cmp.dst = v_is0;
                cmp.operands = {v_rc_dec, v_zero2};
                cmp.source_line = it->source_line;
                emit(current_block_, std::move(cmp));
            }
            const ir::IrBlockId free_bb = fn_->new_block("sh_free");
            {
                ir::IrInstr br{};
                br.op = ir::IrOp::BR_COND;
                br.operands = {v_is0};
                br.target_block = free_bb;
                br.false_block = skip_bb;
                br.source_line = it->source_line;
                emit(current_block_, std::move(br));
                fn_->blocks[dec_bb].succs.push_back(free_bb);
                fn_->blocks[dec_bb].succs.push_back(skip_bb);
                fn_->blocks[free_bb].preds.push_back(dec_bb);
                fn_->blocks[skip_bb].preds.push_back(dec_bb);
            }
            // free_bb: RAW_FREE(v_ctrl) + br skip_bb.
            current_block_ = free_bb;
            {
                ir::IrInstr fr{};
                fr.op = ir::IrOp::RAW_FREE;
                fr.type = ir::IrType::VOID;
                fr.operands = {v_ctrl};
                fr.source_line = it->source_line;
                emit(current_block_, std::move(fr));
            }
            {
                ir::IrInstr br{};
                br.op = ir::IrOp::BR;
                br.target_block = skip_bb;
                br.source_line = it->source_line;
                emit(current_block_, std::move(br));
                fn_->blocks[free_bb].succs.push_back(skip_bb);
                fn_->blocks[skip_bb].preds.push_back(free_bb);
            }
            // current_block_ = skip_bb para que el siguiente cleanup
            // se siga emitiendo en orden lineal.
            current_block_ = skip_bb;
            break;
        }
        }
    }
}

void Lowering::emit_shared_refcount_inc(ir::IrValueId v_slot, uint32_t line) {
    // Ownership ruta B (H3 inc-on-copy): al COPIAR un shared<T> (`b = a`, campo
    // = a, paso por valor) incrementamos el refcount del bloque de control.
    // El slot guarda el host_ptr al ctrl block; refcount esta en [ctrl + 0].
    // Si ctrl == 0 (movido/null) es no-op.  Simetrico al SHAREDPTR_REL (dec).
    if (v_slot == ir::IR_NO_VALUE) return;
    const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_ctrl].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_ctrl;
        ld.operands = {v_slot};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, line);
    const ir::IrValueId v_cmp = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr cmp{};
        cmp.op = ir::IrOp::CMP_NE;
        cmp.type = ir::IrType::I64;
        cmp.dst = v_cmp;
        cmp.operands = {v_ctrl, v_zero};
        cmp.source_line = line;
        emit(current_block_, std::move(cmp));
    }
    const ir::IrBlockId inc_bb = fn_->new_block("sh_inc");
    const ir::IrBlockId skip_bb = fn_->new_block("sh_inc_skip");
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands = {v_cmp};
        br.target_block = inc_bb;
        br.false_block = skip_bb;
        br.source_line = line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(inc_bb);
        fn_->blocks[current_block_].succs.push_back(skip_bb);
        fn_->blocks[inc_bb].preds.push_back(current_block_);
        fn_->blocks[skip_bb].preds.push_back(current_block_);
    }
    current_block_ = inc_bb;
    const ir::IrValueId v_rc = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_rc;
        ld.operands = {v_ctrl};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    const ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, line);
    const ir::IrValueId v_rc_inc = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr add{};
        add.op = ir::IrOp::ADD;
        add.type = ir::IrType::I64;
        add.dst = v_rc_inc;
        add.operands = {v_rc, v_one};
        add.source_line = line;
        emit(current_block_, std::move(add));
    }
    {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {v_rc_inc, v_ctrl};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR;
        br.target_block = skip_bb;
        br.source_line = line;
        emit(current_block_, std::move(br));
        fn_->blocks[inc_bb].succs.push_back(skip_bb);
        fn_->blocks[skip_bb].preds.push_back(inc_bb);
    }
    current_block_ = skip_bb;
}

void Lowering::emit_shared_refcount_dec(ir::IrValueId v_slot, uint32_t line) {
    // Ownership ruta B (H3/H5 dec-on-drop): decrementa el refcount del bloque
    // de control de un shared<T> y, si cae a 0, lo libera (RAW_FREE).  El slot
    // guarda el host_ptr al ctrl; refcount en [ctrl+0].  No-op si ctrl==0
    // (movido/null).  Lo usan el cleanup SHAREDPTR_REL del scope local y el
    // destructor del contenedor para un campo shared (H5).
    if (v_slot == ir::IR_NO_VALUE) return;
    const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_ctrl].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_ctrl;
        ld.operands = {v_slot};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, line);
    const ir::IrValueId v_cmp = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr cmp{};
        cmp.op = ir::IrOp::CMP_NE;
        cmp.type = ir::IrType::I64;
        cmp.dst = v_cmp;
        cmp.operands = {v_ctrl, v_zero};
        cmp.source_line = line;
        emit(current_block_, std::move(cmp));
    }
    const ir::IrBlockId dec_bb = fn_->new_block("shf_dec");
    const ir::IrBlockId skip_bb = fn_->new_block("shf_skip");
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands = {v_cmp};
        br.target_block = dec_bb;
        br.false_block = skip_bb;
        br.source_line = line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(dec_bb);
        fn_->blocks[current_block_].succs.push_back(skip_bb);
        fn_->blocks[dec_bb].preds.push_back(current_block_);
        fn_->blocks[skip_bb].preds.push_back(current_block_);
    }
    current_block_ = dec_bb;
    const ir::IrValueId v_rc = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_rc;
        ld.operands = {v_ctrl};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    const ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, line);
    const ir::IrValueId v_rc_dec = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr sub{};
        sub.op = ir::IrOp::SUB;
        sub.type = ir::IrType::I64;
        sub.dst = v_rc_dec;
        sub.operands = {v_rc, v_one};
        sub.source_line = line;
        emit(current_block_, std::move(sub));
    }
    {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {v_rc_dec, v_ctrl};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
    const ir::IrValueId v_is0 = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr cmp{};
        cmp.op = ir::IrOp::CMP_EQ;
        cmp.type = ir::IrType::I64;
        cmp.dst = v_is0;
        cmp.operands = {v_rc_dec, v_zero};
        cmp.source_line = line;
        emit(current_block_, std::move(cmp));
    }
    const ir::IrBlockId free_bb = fn_->new_block("shf_free");
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands = {v_is0};
        br.target_block = free_bb;
        br.false_block = skip_bb;
        br.source_line = line;
        emit(current_block_, std::move(br));
        fn_->blocks[dec_bb].succs.push_back(free_bb);
        fn_->blocks[dec_bb].succs.push_back(skip_bb);
        fn_->blocks[free_bb].preds.push_back(dec_bb);
        fn_->blocks[skip_bb].preds.push_back(dec_bb);
    }
    current_block_ = free_bb;
    {
        ir::IrInstr fr{};
        fr.op = ir::IrOp::RAW_FREE;
        fr.type = ir::IrType::VOID;
        fr.operands = {v_ctrl};
        fr.source_line = line;
        emit(current_block_, std::move(fr));
    }
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR;
        br.target_block = skip_bb;
        br.source_line = line;
        emit(current_block_, std::move(br));
        fn_->blocks[free_bb].succs.push_back(skip_bb);
        fn_->blocks[skip_bb].preds.push_back(free_bb);
    }
    current_block_ = skip_bb;
}

// ---------------------------------------------------------------------
// synchronized (obj) { body }   (cierre completo con cleanup)
//
// Lowering con exception safety + return safety:
//   1. Bajar target -> ptr -> gchandle -> monenter (todo en 1 RAW_ASM).
//   2. Emitir tryenter catch-all con handler que hace monexit + rethrow.
//   3. Push CleanupAction(tryleave + monexit) al cleanup_stack_.
//   4. Bajar el body.  Si hace `return`, lower_return correra todos los
//      cleanups del stack (incluyendo el nuestro) antes del RET.
//   5. Pop CleanupAction.
//   6. Si el body NO termino: emitir tryleave + monexit (cleanup normal).
//   7. Bloque handler (alcanzable solo via excepcion del body): emitir
//      monexit + rethrow.
//
// Resultado: el monitor se libera SIEMPRE, sea por:
//   - flujo normal: paso 6.
//   - return temprano: emit_cleanups_all() en lower_return.
//   - throw: el handler del paso 7 lo libera y re-lanza.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// spawn { body } -- arranca proceso hijo en scheduler actual.
//
// Estrategia:
//   1. generate_spawn_helper compila el body como funcion sintetica
//      __spawn_<N> con return type VOID + body original + hlt al final
//      (NO ret: un proceso hijo no retorna a un caller que no existe).
//   2. lower_spawn_expr emite RAW_ASM con:
//        mov {dst_pc_holder}, @Absolute("code.__spawn_<N>")
//        spawn {dst_pc_holder}
//        mov {dst}, r0     ; r0 contiene el PID encoded del hijo
//   3. SSA value devuelto = PID encoded como i64.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// closures: generate_lambda_helper + lower_lambda_expr.
//
// Diseno:
//   - Cada @c LambdaExpr produce una @c IrFunction sintetica
//     @c __lambda_<N>(p0, p1, ...) cuyos params son los declarados
//     por la lambda.  Los captures se pasan via R14 (env_ptr) y se
//     leen en el prologue del helper desde @c [r14 + 8*i] a SSA
//     values con el nombre de la captura.  Asi el body trata las
//     capturas como si fueran locales, sin distinguirlas de los
//     params (el type checker ya las acumulo en e->captures).
//
//   - El call site emite:
//       1. ALLOCA @c 8*N bytes para el env block (si N > 0).
//       2. STORE de cada captura en @c [env + 8*i].
//       3. ALLOCA 16 bytes para el "function value":
//            `[+0 fn_addr][+8 env_addr]`.
//       4. RAW_ASM @c mov ..., @c \@Absolute("code.__lambda_<N>")
//          + STORE en @c [fv+0].
//       5. STORE @c env_addr en @c [fv+8] (o 0 si sin captures).
//
//   - Al llamar a la closure (vease @c lower_call cuando callee es
//     IdentExpr de tipo FUNCTION): se cargan @c fn_addr y @c env_addr
//     del slot, y se emite IrOp::CALLCLOSURE con func_ptr=fn_addr y
//     operands=[env_addr, args...].  El emisor IR coloca env en R14,
//     args en R1..R12 via parallel-move y emite @c callvmr.
// ---------------------------------------------------------------------


// ---------------------------------------------------------------------
// ADTs: lower_enum_constructor + lower_match_expr.
//
// Layout del slot del enum (mismo para todas las variantes):
//   [+0  i64 tag]
//   [+8  i64 payload[0]]
//   [+16 i64 payload[1]] ...
// Tamano total: 8 + 8 * max_payload_fields.  Cada payload se
// promociona a i64 para tener acceso uniforme por offset.  Cero
// alocaciones de heap; cero overhead GC; mismo modelo que
// Optional / Result.
// ---------------------------------------------------------------------

uint64_t Lowering::nested_sret_flat_size(const std::string &callee,
                                         bool *out_is_host) const {
    // Tamano del buffer SRET de BUFFER PLANO (value-type) que devuelve @p
    // callee, o 0 si no lo es.  El fix nested-SRET copia el retbuf del
    // productor a un slot fresco; la NATURALEZA de ese slot (via @p
    // out_is_host) debe coincidir con como el CALLEE lee su parametro:
    //   - ENUM/ADT: desde el modelo "agregados en memoria HOST" ([[proj_
    //     aggregates_host]], commit a0ffa68) el callee marca su param enum como
    //     is_host_ptr y lo lee via acceso HOST (`movh [t]`) -> fresh HOST
    //     (out_is_host=true).  Un slot VM daria segfault (el callee leeria host
    //     sobre una direccion VM).  ESTE sitio nested-SRET se le paso al fix
    //     original -> `emit(classify(x))` inline crasheaba (MOVH sobre pila
    //     VM).
    //   - Optional/Result: el callee los lee via acceso HOST (isPresent/unwrap/
    //     value/error con is_host_ptr) -> fresh HOST (out_is_host=true).  Un
    //     slot VM daria segfault (leeria host en una direccion VM).
    // function/smart-ptr se excluyen (ownership de env/ctrl: copiar el buffer
    // los duplicaria).
    if (out_is_host) *out_is_host = false;
    auto it_er = fn_ret_enum_name_.find(callee);
    if (it_er != fn_ret_enum_name_.end()) {
        const auto &elays = tc_.enum_layouts();
        auto it_e = elays.find(it_er->second);
        if (it_e != elays.end()) {
            if (out_is_host) *out_is_host = true; // enum agregado -> host
            return static_cast<uint64_t>(it_e->second.size_bytes);
        }
        return 0;
    }
    auto it_kind = fn_ret_kind_.find(callee);
    if (it_kind != fn_ret_kind_.end()) {
        if (it_kind->second == PrimitiveKind::OPTIONAL) {
            if (out_is_host) *out_is_host = true;
            return 16ULL;
        }
        if (it_kind->second == PrimitiveKind::RESULT) {
            if (out_is_host) *out_is_host = true;
            return 24ULL;
        }
    }
    return 0;
}

void Lowering::emit_enum_copy(ir::IrValueId dst_addr, ir::IrValueId src_addr,
                              bool src_is_host, uint64_t size_bytes,
                              uint32_t line) {
    if (dst_addr == ir::IR_NO_VALUE || src_addr == ir::IR_NO_VALUE) return;
    const bool dst_is_host = fn_->values[dst_addr].is_host_ptr;
    const uint64_t qwords = (size_bytes + 7) / 8;
    for (uint64_t qi = 0; qi < qwords; ++qi) {
        const uint64_t off = qi * 8;
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, static_cast<int64_t>(off), line);
        // src + off (hereda la naturaleza del origen para el LOAD).
        const ir::IrValueId v_src_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_src_at].is_host_ptr = src_is_host;
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_src_at;
            ad.operands = {src_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        const ir::IrValueId v_word = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_word;
            ld.operands = {v_src_at};
            ld.source_line = line;
            emit(current_block_, std::move(ld));
        }
        // dst + off (naturaleza del slot destino, tipicamente VM ALLOCA).
        const ir::IrValueId v_dst_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_dst_at].is_host_ptr = dst_is_host;
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_dst_at;
            ad.operands = {dst_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {v_word, v_dst_at};
            st.source_line = line;
            emit(current_block_, std::move(st));
        }
    }
}

ir::IrValueId Lowering::lower_enum_constructor(
    const std::string &enum_name, const std::string &variant_name,
    const std::vector<std::unique_ptr<ast::Expr>> &args, const SourceLoc &loc) {
    // Localizar el layout del enum y la variante.
    const auto &elays = tc_.enum_layouts();
    auto it = elays.find(enum_name);
    if (it == elays.end()) {
        // `typedef Color Tinta new;` -> el layout (variantes, tags, payloads)
        // es el del enum de debajo; el newtype solo anade la identidad, que ya
        // viaja en el Type.
        if (const std::string real = tc_.underlying_layout_name(enum_name);
            !real.empty())
            it = elays.find(real);
    }
    if (it == elays.end()) {
        error_at(loc, "lowering: enum desconocido '" + enum_name + "'");
        return ir::IR_NO_VALUE;
    }
    const EnumLayout &elay = it->second;
    const EnumVariantInfo *var = nullptr;
    for (const auto &v : elay.variants) {
        if (v.name == variant_name) {
            var = &v;
            break;
        }
    }
    if (!var) {
        error_at(loc, "lowering: variante desconocida '" + variant_name +
                          "' en enum '" + enum_name + "'");
        return ir::IR_NO_VALUE;
    }

    // Un enum CON VALOR no es un agregado: es su entero base, y la variante ES
    // ese numero.  Construirle un buffer con tag y campos lo convierte en una
    // direccion, y a partir de ahi comparar dos variantes compara direcciones
    // -- dos `Less` distintos dejan de ser iguales.
    //
    // La comprobacion va AQUI, por donde pasan todas las formas de nombrar una
    // variante, y no en cada llamante: con el enum importado alguno de ellos
    // no lo detectaba y caia al camino de agregado.
    if (elay.is_valued && elay.backing_type_name.empty()) {
        const ir::IrType t = ir_type_from_primitive(elay.backing);
        const ir::IrValueId c = fn_->new_value(t);
        fn_->values[c].is_const = true;
        fn_->values[c].const_val = static_cast<uint64_t>(var->int_value);
        ir::IrInstr ci{};
        ci.op = ir::IrOp::CONST;
        ci.type = t;
        ci.dst = c;
        ci.imm = static_cast<uint64_t>(var->int_value);
        ci.source_line = loc.line;
        emit(current_block_, std::move(ci));
        return c;
    }

    // marker: MAKE_VARIANT identifica la construccion completa de
    // un valor ADT.  Emitido ANTES de la secuencia ALLOCA + STOREs para
    // que el C2 JIT ( D.8) pueda reconocer el patron y aplicar
    // escape analysis (promocion del slot a regs si no escapa) +
    // case-splitting eficiente del match downstream.  No produce SSA
    // value; el emitter actual lo trata como no-op.
    //
    // Lower de los args ANTES del marker para que sus SSA values
    // esten disponibles como operandos.  Cada payload se promueve a
    // un slot de 8 bytes (i64).  Para floats (F32/F64) usamos BITCAST
    // (preserva los bits IEEE) en lugar de FTOI/F2I (que truncaria
    // el valor).  El bug se manifiesta como `Circle(5.0)` con payload
    // 0 porque FTOI(5.0) -> 5, pero luego al destructurar como f64
    // se interpreta 5 como bits IEEE de un denormal cerca de cero.
    std::vector<ir::IrValueId> payload_vals;
    payload_vals.reserve(args.size());
    for (size_t i = 0; i < args.size() && i < var->field_types.size(); ++i) {
        // Auto-promotion literal -> StringObject cuando el payload
        // tipo es STRING.  Sin esto, `Token.Word("hello")` almacena
        // el raw ptr del literal en lugar del GcHandle, y la
        // extraccion `case Word(s) => s` da un ptr invalido al
        // intentar usarlo como string.
        ir::IrValueId v;
        const ast::Expr *ae = args[i].get();
        if (var->field_types[i].kind == PrimitiveKind::STRING && ae &&
            ae->kind == ast::NodeKind::StringLitExpr) {
            auto *slit = const_cast<ast::StringLitExpr *>(
                static_cast<const ast::StringLitExpr *>(ae));
            v = lower_string_literal_to_string_object(slit);
        } else {
            v = lower_expr(args[i].get());
        }
        if (v == ir::IR_NO_VALUE) {
            v = emit_const(ir::IrType::I64, 0, loc.line);
        }
        ir::IrType vt = fn_->values[v].type;
        if (vt == ir::IrType::F64) {
            // bitcast f64 -> i64 (mismo ancho, preserva bits IEEE).
            ir::IrValueId v2 = fn_->new_value(ir::IrType::I64);
            ir::IrInstr bc{};
            bc.op = ir::IrOp::BITCAST;
            bc.type = ir::IrType::I64;
            bc.dst = v2;
            bc.operands = {v};
            bc.source_line = loc.line;
            emit(current_block_, std::move(bc));
            v = v2;
        } else if (vt == ir::IrType::F32) {
            // f32: primero ampliar a f64 (preserva el valor), luego
            // bitcast a i64 (preserva los bits IEEE).
            ir::IrValueId vw = fn_->new_value(ir::IrType::F64);
            {
                ir::IrInstr ext{};
                ext.op = ir::IrOp::F32TOF64;
                ext.type = ir::IrType::F64;
                ext.dst = vw;
                ext.operands = {v};
                ext.source_line = loc.line;
                emit(current_block_, std::move(ext));
            }
            ir::IrValueId v2 = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr bc{};
                bc.op = ir::IrOp::BITCAST;
                bc.type = ir::IrType::I64;
                bc.dst = v2;
                bc.operands = {vw};
                bc.source_line = loc.line;
                emit(current_block_, std::move(bc));
            }
            v = v2;
        } else if (vt != ir::IrType::I64 && vt != ir::IrType::PTR) {
            // Tipos enteros mas estrechos: promocion normal a i64
            // (sign/zero-extend segun signedness).
            v = cast_if_needed(v, vt, ir::IrType::I64, loc.line);
        }
        payload_vals.push_back(v);
    }
    {
        ir::IrInstr mv{};
        mv.op = ir::IrOp::MAKE_VARIANT;
        mv.type = ir::IrType::VOID;
        mv.dst = ir::IR_NO_VALUE;
        mv.operands = payload_vals;
        mv.func_name = enum_name + "." + variant_name;
        mv.imm = static_cast<uint64_t>(var->tag);
        mv.source_line = loc.line;
        emit(current_block_, std::move(mv));
    }

    // 1. ALLOCA slot del enum (size_bytes = 8 + 8*max_payload_fields).
    const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = addr;
        al.imm = static_cast<uint64_t>(elay.size_bytes);
        // Host, como todo agregado (ver lower_var_decl).
        al.host_alloca = true;
        al.source_line = loc.line;
        emit(current_block_, std::move(al));
        fn_->values[addr].is_host_ptr = true;
    }

    // 2. STORE i64 tag en offset 0 (= addr).
    {
        ir::IrValueId tag_v = emit_const(
            ir::IrType::I64, static_cast<uint64_t>(var->tag), loc.line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {tag_v, addr};
        st.source_line = loc.line;
        emit(current_block_, std::move(st));
    }

    // 3. STORE de cada payload arg en offset 8 + 8*i (promovido a i64).
    // Reusa los payload_vals ya lowereados arriba (para el marker
    // MAKE_VARIANT): evita doble-lowering de los args.
    for (size_t i = 0; i < payload_vals.size(); ++i) {
        ir::IrValueId v = payload_vals[i];
        if (v == ir::IR_NO_VALUE) continue;

        // Calcular addr_i = addr + (8 + 8*i).
        const uint64_t off = 8ULL + 8ULL * static_cast<uint64_t>(i);
        ir::IrValueId addr_i = fn_->new_value(ir::IrType::PTR);
        ir::IrValueId off_v = emit_const(ir::IrType::I64, off, loc.line);
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = addr_i;
        ad.operands = {addr, off_v};
        ad.source_line = loc.line;
        emit(current_block_, std::move(ad));
        // Propagar is_host_ptr del buffer al puntero buf+off (patron
        // is_host_ptr-en-add): el STORE del payload usa la naturaleza del
        // buffer.  No-op hoy (el buffer del constructor es VM stack) pero
        // unifica el patron con Some/Ok/value/error/unwrap.
        fn_->values[addr_i].is_host_ptr = fn_->values[addr].is_host_ptr;

        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {v, addr_i};
        st.source_line = loc.line;
        emit(current_block_, std::move(st));
    }

    // El SSA value de la expresion es la direccion del slot.
    return addr;
}


void Lowering::emit_match_arm_phis(
    const std::vector<std::unordered_map<std::string, ir::IrValueId>>
        &entry_scopes,
    const std::vector<
        std::vector<std::unordered_map<std::string, ir::IrValueId>>>
        &arm_scopes,
    const std::vector<ir::IrBlockId> &arm_preds,
    const std::vector<char> &arm_reaches, ir::IrBlockId merge_bb,
    uint32_t line) {
    // Base: partimos del scope de entry (variables no tocadas conservan su
    // valor).
    scopes_ = entry_scopes;
    const size_t depth = entry_scopes.size();
    for (size_t lvl = 0; lvl < depth; ++lvl) {
        for (auto &kv : entry_scopes[lvl]) {
            const std::string &name = kv.first;
            // Recolectar (val, pred) de cada arm que ALCANZA el merge.
            std::vector<std::pair<ir::IrValueId, ir::IrBlockId>> args;
            bool all_same = true;
            ir::IrValueId first = ir::IR_NO_VALUE;
            for (size_t i = 0; i < arm_scopes.size(); ++i) {
                if (!arm_reaches[i]) continue;
                if (lvl >= arm_scopes[i].size()) continue;
                auto it = arm_scopes[i][lvl].find(name);
                if (it == arm_scopes[i][lvl].end()) continue;
                args.push_back({it->second, arm_preds[i]});
                if (first == ir::IR_NO_VALUE)
                    first = it->second;
                else if (it->second != first)
                    all_same = false;
            }
            if (args.empty()) continue;
            if (all_same) {
                scopes_[lvl][name] = first; // coherente en todos los arms
                continue;
            }
            // PHI N-vias al inicio del merge (uno por arm que llega).
            const ir::IrType phi_ty = fn_->values[first].type;
            ir::IrValueId phi_v = fn_->new_value(phi_ty);
            ir::IrInstr phi{};
            phi.op = ir::IrOp::PHI;
            phi.type = phi_ty;
            phi.dst = phi_v;
            for (auto &a : args)
                phi.phi_args.push_back({a.first, a.second});
            phi.source_line = line;
            fn_->blocks[merge_bb].instrs.insert(
                fn_->blocks[merge_bb].instrs.begin(), std::move(phi));
            scopes_[lvl][name] = phi_v;
        }
    }
}



// ---------------------------------------------------------------------
// @Async sugar.  Transforma una FunctionDecl con flag
// is_async en DOS funciones IR:
//
//   1. Wrapper publico `<name>` con firma `i64 <name>()` que el caller
//      invoca.  Internamente:
//        a. future_alloc -> fut handle
//        b. spawn helper sintetica __async_<name>
//        c. msgsend(child_pid, fut handle)
//        d. return fut handle
//
//   2. Spawn helper `__async_<name>(void)` que ejecuta el body original
//      como hijo cooperativo:
//        a. msgrecv -> my_fut handle (set como async_fut_id_)
//        b. lower body original (cada return X intercepta a fulfill+hlt)
//        c. fallback al final: fulfill(my_fut, 0) + hlt
//
// El usuario escribe:
//   @Async i64 compute() { return 42; }
//   i32 main() { i64 r = await compute(); return r; }
// ---------------------------------------------------------------------


// ---------------------------------------------------------------------
//  AS: inline asm nativo -> IrOp::INLINE_ASM (marker host).
//
// Incremento 1 (aditivo): el cuerpo NASM viaja verbatim en func_name y
// los calificadores + efectos (memory/flags) en un bitfield en imm.  El
// marker no produce valor SSA ni consume operandos (las variables Vesta se
// enlazaran a registros via register() en el Incremento 2).
//
// Backends:
//   - port-C / JIT / AOT: materializan el asm en la CPU host.
//   - bytecode/interp: NO lo soporta.  El driver (compile_vx_source)
//     detecta INLINE_ASM cuando el target es bytecode y reporta un error
//     claro ANTES de emitir el .vel.
//
// Los clobbers de registros EXPLICITOS (vector) aun no se bajan al IR;
// los efectos memory/flags si (bits 4/5 de imm).  El Incremento 3
// (port-C) decidira el transporte de la lista de registros.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Expresiones.
// ---------------------------------------------------------------------

ir::IrValueId Lowering::lower_expr(ast::Expr *e) {
    if (!e) return ir::IR_NO_VALUE;
    /* Mientras se baja esta expresion, lo que se emita lleva SU columna.  Al
     * anidarse, la de dentro tapa a la de fuera y se restaura al salir, con lo
     * que cada instruccion acaba con la de la expresion mas ajustada que la
     * produjo.  Ahora es posible porque todo pasa por un solo sitio al
     * emitir. */
    struct ColumnaVigente {
        uint32_t *slot;
        uint32_t previa;
        uint32_t *slot_len;
        uint32_t previa_len;

        ~ColumnaVigente() {
            *slot = previa;
            *slot_len = previa_len;
        }
    } col_vigente{&pend_stmt_column_, pend_stmt_column_, &pend_stmt_len_,
                  pend_stmt_len_};
    if (e->loc.column > 0) {
        pend_stmt_column_ = e->loc.column;
        // La longitud va con su columna; si no se sabe, mejor ninguna que
        // la de la expresion de fuera, que recortaria otro trozo.
        pend_stmt_len_ = e->loc.length;
    }
    switch (e->kind) {
    case ast::NodeKind::IntLitExpr: {
        auto *ie = static_cast<ast::IntLitExpr *>(e);
        const ir::IrType t = ir_type_from_primitive(e->result_type.kind);
        return emit_const(t == ir::IrType::VOID ? ir::IrType::I64 : t,
                          ie->value, e->loc.line);
    }
    case ast::NodeKind::FloatLitExpr: {
        auto *fe = static_cast<ast::FloatLitExpr *>(e);
        ir::IrType t = ir_type_from_primitive(e->result_type.kind);
        if (t == ir::IrType::VOID) t = ir::IrType::F64;
        // Reinterpretamos los bits IEEE del literal como uint64 para alojarlos
        // en el campo imm de la instruccion CONST.  Para un literal cuyo tipo
        // es F32, hay que estrechar el double a float PRIMERO y tomar sus 32
        // bits: sin esto un `atomic<f32>.store(5.0)` guardaba los 32 bits bajos
        // del PATRON f64 (= 0).  Los literales F64 (el caso por defecto)
        // conservan los 64 bits del double.
        uint64_t bits;
        static_assert(sizeof(double) == sizeof(uint64_t),
                      "double debe ocupar 64 bits para reinterpret_cast");
        if (t == ir::IrType::F32) {
            const float f32v = static_cast<float>(fe->value);
            uint32_t b32;
            __builtin_memcpy(&b32, &f32v, sizeof(float));
            bits = b32; // bits IEEE-754 binary32 en la parte baja
        } else {
            __builtin_memcpy(&bits, &fe->value, sizeof(double));
        }
        return emit_const(t, bits, e->loc.line);
    }
    case ast::NodeKind::BoolLitExpr: {
        auto *be = static_cast<ast::BoolLitExpr *>(e);
        return emit_const(ir::IrType::BOOL, be->value ? 1 : 0, e->loc.line);
    }
    case ast::NodeKind::CharLitExpr: {
        auto *ce = static_cast<ast::CharLitExpr *>(e);
        return emit_const(ir::IrType::U8, ce->codepoint, e->loc.line);
    }
    case ast::NodeKind::StringLitExpr:
        return lower_string_lit(static_cast<ast::StringLitExpr *>(e));
    case ast::NodeKind::NullLitExpr:
        // Null se modela como el escalar 0 del tipo i64; el type
        // checker valida que solo se asigne a punteros / handles
        // (donde 0 es la representacion canonica de "ausente").
        return emit_const(ir::IrType::I64, 0, e->loc.line);
    case ast::NodeKind::IdentExpr:
        return lower_ident(static_cast<ast::IdentExpr *>(e));
    case ast::NodeKind::FieldAccessExpr:
        return lower_field_access(static_cast<ast::FieldAccessExpr *>(e));
    case ast::NodeKind::BinaryExpr:
        return lower_binary(static_cast<ast::BinaryExpr *>(e));
    case ast::NodeKind::UnaryExpr:
        return lower_unary(static_cast<ast::UnaryExpr *>(e));
    case ast::NodeKind::CallExpr:
        return lower_call(static_cast<ast::CallExpr *>(e));
    case ast::NodeKind::AssignExpr:
        return lower_assign(static_cast<ast::AssignExpr *>(e));
    case ast::NodeKind::TernaryExpr:
        return lower_ternary(static_cast<ast::TernaryExpr *>(e));
    case ast::NodeKind::TryExpr:
        return lower_try_expr(static_cast<ast::TryExpr *>(e));
    case ast::NodeKind::IndexExpr:
        return lower_index(static_cast<ast::IndexExpr *>(e));
    case ast::NodeKind::ThisExpr:
        return lower_this_expr(static_cast<ast::ThisExpr *>(e));
    case ast::NodeKind::NewExpr:
        return lower_new_expr(static_cast<ast::NewExpr *>(e));
    case ast::NodeKind::SuperCallExpr:
        return lower_super_call_expr(static_cast<ast::SuperCallExpr *>(e));
    case ast::NodeKind::SuperMethodCallExpr:
        return lower_super_method_call_expr(
            static_cast<ast::SuperMethodCallExpr *>(e));
    case ast::NodeKind::SpawnExpr:
        return lower_spawn_expr(static_cast<ast::SpawnExpr *>(e));
    case ast::NodeKind::RSpawnExpr:
        return lower_rspawn_expr(static_cast<ast::RSpawnExpr *>(e));
    case ast::NodeKind::LambdaExpr:
        return lower_lambda_expr(static_cast<ast::LambdaExpr *>(e));
    case ast::NodeKind::MatchExpr:
        return lower_match_expr(static_cast<ast::MatchExpr *>(e));
    case ast::NodeKind::CastExpr:
        return lower_cast_expr(static_cast<ast::CastExpr *>(e));
    default:
        unsupported(e->loc, "expresion no soportada por el lowering actual");
        return ir::IR_NO_VALUE;
    }
}


// ---------------------------------------------------------------------
// Helpers: tamano de tipo y aritmetica de punteros.
// ---------------------------------------------------------------------

size_t Lowering::optional_buf_bytes(const Type &t, size_t base) const {
    // Payload escalar: 8 bytes, como toda la vida.  Struct por valor: su
    // tamano real alineado a 8, para que quepa entero dentro del buffer.
    size_t payload = 8;
    if (t.pointee) {
        const Type &p = *t.pointee;
        if (p.kind == PrimitiveKind::STRUCT) {
            const size_t sz = size_of_type(p);
            if (sz > payload) payload = (sz + 7u) & ~static_cast<size_t>(7);
        }
    }
    return base + payload;
}

size_t Lowering::size_of_type(const Type &t) const {
    if (t.kind == PrimitiveKind::STRUCT) {
        const auto &layouts = tc_.struct_layouts();
        auto it = layouts.find(t.struct_name);
        if (it != layouts.end()) return it->second.size_bytes;
        // bug4: STRUCT puede ser un ENUM (mismo kind).  Buscar en
        // enum_layouts_ tambien.  size_bytes = 8 (tag) + 8*max_payload.
        const auto &elayouts = tc_.enum_layouts();
        auto ite = elayouts.find(t.struct_name);
        return (ite == elayouts.end()) ? 0 : ite->second.size_bytes;
    }
    if (t.kind == PrimitiveKind::PTR) return 8;
    if (t.kind == PrimitiveKind::ARRAY) {
        // T[N] ocupa N*sizeof(T) bytes; T[] (size==0) decae a puntero.
        // bug4: para T[] (dynamic) sin tamano fijo, sizeof = 8 (el
        // valor es un host_ptr al buffer).  Caller que pregunte
        // sizeof(slot) obtiene la pointer-size correcta.
        if (!t.pointee) return 0;
        if (t.array_size == 0) return 8; // host_ptr al buffer
        return static_cast<size_t>(t.array_size) * size_of_type(*t.pointee);
    }
    // CLASS: ref host_ptr al objeto GC.
    if (t.kind == PrimitiveKind::CLASS) return 8;
    // STRING: GcHandle u64.
    if (t.kind == PrimitiveKind::STRING) return 8;
    // cfn (puntero a funcion crudo, fn_is_raw): SOLO la direccion -> 8 bytes.
    // Un lambda fn(...) es un fat-pointer de 16 bytes {fn_addr, env}.
    if (t.kind == PrimitiveKind::FUNCTION && t.fn_is_raw) return 8;
    return primitive_size_bytes(t.kind);
}

bool Lowering::type_is_overlay(const Type &t) const {
    if (t.kind != PrimitiveKind::STRUCT || t.struct_name.empty()) return false;
    const auto &layouts = tc_.struct_layouts();
    auto it = layouts.find(t.struct_name);
    return it != layouts.end() && it->second.is_overlay;
}


ir::IrValueId
Lowering::lower_string_literal_to_string_object(ast::StringLitExpr *slit) {
    // Helper local: emite STRMAKE de un trozo literal y devuelve
    // el handle StringObject resultante.
    auto make_part_handle = [&](const std::string &part_text,
                                int line) -> ir::IrValueId {
        std::vector<uint8_t> pbytes(part_text.begin(), part_text.end());
        const uint64_t p_idx = out_mod_->intern_static_data(std::move(pbytes));
        const uint64_t p_len = (uint64_t)part_text.size();
        ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr is{};
            is.op = ir::IrOp::STR_LIT_ADDR;
            is.type = ir::IrType::PTR;
            is.dst = v_addr;
            is.imm = p_idx;
            is.source_line = line;
            emit(current_block_, std::move(is));
        }
        ir::IrValueId v_len = emit_const(ir::IrType::I64, p_len, line);
        ir::IrValueId v_handle =
            emit_string_literal_repr(v_addr, v_len, -1, line);
        return v_handle;
    };

    // Fast path: string literal SIN interpolacion -> 1 sola STRMAKE.
    if (!slit->is_interpolated()) {
        return make_part_handle(slit->value, slit->loc.line);
    }

    // Path interpolado: construimos el StringObject final como
    // cadena de STRCATs sobre los parts literales y los exprs
    // interpolados.  Layout: parts[0] + exprs[0] + parts[1] + ...
    // + parts[N] (siempre N+1 parts para N exprs).
    //
    // Cada `${expr}` se baja a un StringObject handle.  Strings
    // pasan tal cual; tipos primitivos (int/uint/bool/char/ptr/gc)
    // se pasan por un helper nativo que escribe su representacion
    // ASCII en un buffer VM y luego construimos el StringObject
    // via STRMAKE desde ese buffer.
    const int line = slit->loc.line;

    // Helper: emite la secuencia ALLOCA + CALLN(stringify_to_vmbuf)
    // + STRMAKE para un valor primitivo.  El `native_fn` es el nombre
    // de la funcion en `stdlib/native/io/vesta_io` que toma
    // (proc_ptr, vm_addr, value) y devuelve la longitud escrita.
    // El buffer VM es ALLOCA de 32 bytes (suficiente para todos los
    // tipos: i64=20+signo, hex=18, "false"=5, char UTF-8=4).
    /* Esta secuencia ya existe como @c stringify_primitive_via_native, palabra
     * por palabra.  Era una COPIA, y una copia no es solo mas lineas: es un
     * segundo criterio.  Al declarar lo que hace la nativa se vio enseguida --
     * se declaro en una de las dos y la mitad de las llamadas siguieron
     * saliendo como opacas. */
    auto stringify_primitive = [&](ir::IrValueId v_val, const char *native_fn,
                                   int ln) -> ir::IrValueId {
        return stringify_primitive_via_native(v_val, native_fn, ln);
    };

    // BUG-3 fix: honrar el KIND del format spec `${expr:fmt}` al CONSTRUIR
    // string (return / var-decl), no solo en `print`.  Extrae el keyword de
    // kind (char/hex/bin/oct/dec/ptr/bool/gc) del formato ignorando la parte
    // de alineacion (`>N`/`<N`).  Devuelve "" si no hay kind explicito.
    auto fmt_kind_of = [](const std::string &fmt) -> std::string {
        size_t i = 0;
        while (i < fmt.size()) {
            while (i < fmt.size() && (fmt[i] == ' ' || fmt[i] == '\t'))
                ++i;
            if (i >= fmt.size()) break;
            if (fmt[i] == '<' || fmt[i] == '>') {
                // Segmento de alineacion: `<`/`>` + digitos + fill opcional.
                ++i;
                while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
                    ++i;
                if (i < fmt.size() && fmt[i] != ':') ++i; // fill char
            } else {
                size_t start = i;
                while (i < fmt.size() && fmt[i] != ':' && fmt[i] != ' ' &&
                       fmt[i] != '\t')
                    ++i;
                std::string kw = fmt.substr(start, i - start);
                if (kw == "char" || kw == "hex" || kw == "bin" || kw == "oct" ||
                    kw == "dec" || kw == "ptr" || kw == "bool" || kw == "gc")
                    return kw;
            }
            while (i < fmt.size() && (fmt[i] == ' ' || fmt[i] == '\t'))
                ++i;
            if (i < fmt.size() && fmt[i] == ':') ++i;
        }
        return std::string();
    };

    auto coerce_to_string_handle =
        [&](ast::Expr *ex, const std::string &fmt) -> ir::IrValueId {
        if (!ex) return ir::IR_NO_VALUE;
        if (ex->kind == ast::NodeKind::StringLitExpr) {
            auto *sl = static_cast<ast::StringLitExpr *>(ex);
            return lower_string_literal_to_string_object(sl);
        }
        ir::IrValueId v = lower_expr(ex);
        if (v == ir::IR_NO_VALUE) return v;
        const PrimitiveKind ek = ex->result_type.kind;
        const int ln = ex->loc.line;
        // Strings: pasan directamente.
        if (ek == PrimitiveKind::STRING) return v;
        // BUG-3: si el format spec fuerza un KIND concreto, enrutar al helper
        // nativo correspondiente en lugar del dispatch por tipo.  `char`
        // interpreta el valor entero como codepoint -> UTF-8; `hex`/`ptr`/
        // `bool` usan su helper; `bin`/`oct`/`gc`/`dec` no tienen helper de
        // construccion dedicado -> caen al dispatch por tipo (default).
        if (!fmt.empty()) {
            const std::string k = fmt_kind_of(fmt);
            if (k == "char" &&
                (ek == PrimitiveKind::I8 || ek == PrimitiveKind::I16 ||
                 ek == PrimitiveKind::I32 || ek == PrimitiveKind::I64 ||
                 ek == PrimitiveKind::U8 || ek == PrimitiveKind::U16 ||
                 ek == PrimitiveKind::U32 || ek == PrimitiveKind::U64 ||
                 ek == PrimitiveKind::CHAR)) {
                return stringify_primitive(v, "vio_char_to_vmbuf", ln);
            }
            if (k == "hex" &&
                (ek == PrimitiveKind::I8 || ek == PrimitiveKind::I16 ||
                 ek == PrimitiveKind::I32 || ek == PrimitiveKind::I64 ||
                 ek == PrimitiveKind::U8 || ek == PrimitiveKind::U16 ||
                 ek == PrimitiveKind::U32 || ek == PrimitiveKind::U64)) {
                return stringify_primitive(v, "vio_hex_to_vmbuf", ln);
            }
            if (k == "ptr" &&
                (ek == PrimitiveKind::PTR || ek == PrimitiveKind::ARRAY ||
                 ek == PrimitiveKind::I64 || ek == PrimitiveKind::U64)) {
                return stringify_primitive(v, "vio_ptr_to_vmbuf", ln);
            }
            if (k == "bool") {
                return stringify_primitive(v, "vio_bool_to_vmbuf", ln);
            }
        }
        // Stringify por tipo primitivo.  Cada categoria mapea a un
        // helper nativo en vesta_io.
        switch (ek) {
        case PrimitiveKind::I8:
        case PrimitiveKind::I16:
        case PrimitiveKind::I32:
        case PrimitiveKind::I64:
            return stringify_primitive(v, "vio_int_to_vmbuf", ln);
        case PrimitiveKind::U8:
        case PrimitiveKind::U16:
        case PrimitiveKind::U32:
        case PrimitiveKind::U64:
            return stringify_primitive(v, "vio_uint_to_vmbuf", ln);
        case PrimitiveKind::BOOL:
            return stringify_primitive(v, "vio_bool_to_vmbuf", ln);
        case PrimitiveKind::CHAR:
            return stringify_primitive(v, "vio_char_to_vmbuf", ln);
        case PrimitiveKind::PTR:
        case PrimitiveKind::ARRAY:
            return stringify_primitive(v, "vio_ptr_to_vmbuf", ln);
        case PrimitiveKind::F64: {
            // BugFix R7: BITCAST f64->i64 (preserva bits IEEE) y
            // delega a vio_float_to_vmbuf.
            ir::IrValueId v_bits = fn_->new_value(ir::IrType::I64);
            ir::IrInstr bc{};
            bc.op = ir::IrOp::BITCAST;
            bc.type = ir::IrType::I64;
            bc.dst = v_bits;
            bc.operands = {v};
            bc.source_line = ln;
            emit(current_block_, std::move(bc));
            return stringify_primitive(v_bits, "vio_float_to_vmbuf", ln);
        }
        case PrimitiveKind::F32: {
            // BugFix R7: f32 -> f64 (re-encode) -> BITCAST i64.
            ir::IrValueId v_f64 = fn_->new_value(ir::IrType::F64);
            ir::IrInstr ext{};
            ext.op = ir::IrOp::F32TOF64;
            ext.type = ir::IrType::F64;
            ext.dst = v_f64;
            ext.operands = {v};
            ext.source_line = ln;
            emit(current_block_, std::move(ext));
            ir::IrValueId v_bits = fn_->new_value(ir::IrType::I64);
            ir::IrInstr bc{};
            bc.op = ir::IrOp::BITCAST;
            bc.type = ir::IrType::I64;
            bc.dst = v_bits;
            bc.operands = {v_f64};
            bc.source_line = ln;
            emit(current_block_, std::move(bc));
            return stringify_primitive(v_bits, "vio_float_to_vmbuf", ln);
        }
        case PrimitiveKind::CLASS: {
            // BugFix R7: convert host_ptr -> GcHandle via gchandle
            // RAW_ASM y delegar a vio_gchandle_to_vmbuf.
            ir::IrValueId v_handle = emit_gc_handle_for_ptr(v, ln);
            return stringify_primitive(v_handle, "vio_gchandle_to_vmbuf", ln);
        }
        default: break;
        }
        error_at(ex->loc, "interpolacion `${expr}` en contexto string: tipo "
                          "no soportado todavia (struct/enum/optional/result). "
                          "Construye el mensaje con `print` o usa los builtins "
                          "de stringify explicito por ahora.");
        return ir::IR_NO_VALUE;
    };

    auto make_strcat = [&](ir::IrValueId a, ir::IrValueId b,
                           int ln) -> ir::IrValueId {
        return emit_strcat(a, b, static_cast<uint32_t>(ln));
    };

    const size_t ne = slit->interp_exprs.size();
    const size_t np = slit->interp_parts.size();
    ir::IrValueId acc = ir::IR_NO_VALUE;

    // Parte literal inicial (parts[0]).  La emitimos siempre (incluso
    // si es vacia) cuando ne > 0 porque necesitamos un acumulador
    // para los STRCAT subsiguientes; si es vacia, el primer STRCAT
    // se evita anclando el acc al primer expr handle.
    if (np > 0 && !slit->interp_parts[0].empty()) {
        acc = make_part_handle(slit->interp_parts[0], line);
    }
    for (size_t i = 0; i < ne; ++i) {
        const std::string fmt_i = (i < slit->interp_formats.size())
                                      ? slit->interp_formats[i]
                                      : std::string();
        ir::IrValueId expr_h =
            coerce_to_string_handle(slit->interp_exprs[i].get(), fmt_i);
        if (expr_h == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        if (acc == ir::IR_NO_VALUE) {
            acc = expr_h;
        } else {
            acc = make_strcat(acc, expr_h, line);
        }
        if (i + 1 < np && !slit->interp_parts[i + 1].empty()) {
            ir::IrValueId p_h =
                make_part_handle(slit->interp_parts[i + 1], line);
            acc = make_strcat(acc, p_h, line);
        }
    }
    if (acc == ir::IR_NO_VALUE) {
        // Edge case: todas las partes vacias y sin exprs.  Devolvemos
        // un StringObject vacio para mantener el contrato (handle
        // valido siempre).
        acc = make_part_handle(std::string(), line);
    }
    return acc;
}

ir::IrValueId Lowering::emit_topfn_value(const std::string &fn_name, int line) {
    // 1. ALLOCA 16 bytes para el slot del function value.
    ir::IrValueId fv_addr = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = fv_addr;
        al.imm = 16;
        al.source_line = line;
        emit(current_block_, std::move(al));
    }
    // 2. fn_addr via LABEL_ADDR IR op (Sprint 3).
    ir::IrValueId fn_addr = emit_label_addr(fn_name, line);
    // 3. env_addr = 0 (sin captures; el callee no debe leer r14).
    ir::IrValueId env_addr = emit_const(ir::IrType::I64, 0, line);
    // 4. STORE fn_addr en [fv_addr+0].
    {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {fn_addr, fv_addr};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
    // 5. STORE env_addr en [fv_addr+8].
    {
        ir::IrValueId fv_plus_8 = fn_->new_value(ir::IrType::PTR);
        ir::IrValueId off8 = emit_const(ir::IrType::I64, 8, line);
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = fv_plus_8;
        ad.operands = {fv_addr, off8};
        ad.source_line = line;
        emit(current_block_, std::move(ad));

        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {env_addr, fv_plus_8};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
    return fv_addr;
}

// ---------------------------------------------------------------------
// Field access: p.x  (lectura) y  p.x = v  (escritura).
//
// Modelo: las variables tipo struct se representan en scope como un
// IrValueId de tipo PTR que apunta a la zona de memoria reservada
// por ALLOCA (ver lower_var_decl, caso STRUCT).  Para acceder a un
// campo:
//   1. Bajar la base -> ptr al inicio del struct.
//   2. Sumar el offset del campo (consultado al StructLayout del
//      type checker) con un IR ADD.
//   3. Emitir LOAD (lectura) o STORE (escritura) sobre ese puntero.
//
// Si offset == 0 (primer campo del struct) la suma se omite y se
// reusa directamente el ptr base.  Esta optimizacion local evita
// ruido en el .vel para el caso comun de "campo cero".
// ---------------------------------------------------------------------

ir::IrValueId Lowering::lower_overlay_root(ast::Expr *e) {
    // Camina hasta la vista raiz: la base mas profunda que NO es un acceso a
    // campo/elemento overlay (p.ej. `pe` en `pe.Imports[i].name`).
    ast::Expr *root = e;
    for (;;) {
        if (root->kind == ast::NodeKind::FieldAccessExpr) {
            root = static_cast<ast::FieldAccessExpr *>(root)->base.get();
            continue;
        }
        if (root->kind == ast::NodeKind::IndexExpr) {
            root = static_cast<ast::IndexExpr *>(root)->base.get();
            continue;
        }
        break;
    }
    return lower_expr(root);
}

std::string Lowering::generate_overlay_resolver(const StructLayout &lay,
                                                const StructFieldInfo &fi,
                                                bool is_element) {
    const std::string fn_name =
        (is_element ? "__ovl_element_" : "__ovl_resolve_") + lay.name + "_" +
        fi.name;
    if (generated_overlay_resolvers_.count(fn_name)) return fn_name; // dedup
    // Insertar YA el nombre: un resolver @element puede RECURSAR
    // (`self.Name[index-1]`); sin esto la generacion compile-time no
    // terminaria. La recursion se vuelve un CALL runtime al mismo resolver.
    generated_overlay_resolvers_.insert(fn_name);
    ast::BlockStmt *body_block =
        is_element ? fi.element_block : fi.offset_block;

    // Salvar contexto del padre (mismo protocolo que generate_lambda_helper).
    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;
    std::vector<std::unordered_map<std::string, ir::IrValueId>> saved_scopes =
        std::move(scopes_);
    std::unordered_set<std::string> saved_addr_taken =
        std::move(address_taken_locals_);
    std::vector<CleanupAction> saved_cleanups = std::move(cleanup_stack_);
    const bool saved_sret_active = sret_active_;
    const ir::IrValueId saved_sret_retbuf = sret_retbuf_;
    const uint64_t saved_sret_buf_size = sret_buf_size_;
    const bool saved_returns_fn = current_fn_returns_function_;
    sret_active_ = false;
    sret_retbuf_ = ir::IR_NO_VALUE;
    sret_buf_size_ = 0;
    current_fn_returns_function_ = false;

    ir::IrFunction child_fn;
    child_fn.name = fn_name;
    child_fn.ret_type = ir::IrType::PTR; // devuelve una DIRECCION (host)

    // Param `self` = puntero base de la vista (host).
    ir::IrValueId self_pv = child_fn.new_value(ir::IrType::PTR, "%self");
    child_fn.values[self_pv].is_param = true;
    child_fn.values[self_pv].is_host_ptr = true;
    child_fn.params.push_back(self_pv);
    // @element: 2o param `index` (i64) = el elemento a resolver.  Orden de
    // params: self, [index], [root].
    ir::IrValueId index_pv = ir::IR_NO_VALUE;
    if (is_element) {
        index_pv = child_fn.new_value(ir::IrType::I64, "%index");
        child_fn.values[index_pv].is_param = true;
        child_fn.params.push_back(index_pv);
    }
    // F4: si el resolver usa parent<T>(), un param `root` = puntero de la vista
    // RAIZ (el call site lo enhebra caminando la cadena de accesos).
    ir::IrValueId root_pv = ir::IR_NO_VALUE;
    if (fi.resolver_uses_parent) {
        root_pv = child_fn.new_value(ir::IrType::PTR, "%root");
        child_fn.values[root_pv].is_param = true;
        child_fn.values[root_pv].is_host_ptr = true;
        child_fn.params.push_back(root_pv);
    }

    const ir::IrBlockId entry = child_fn.new_block("entry");
    fn_ = &child_fn;
    current_block_ = entry;
    block_terminated_ = false;
    scopes_.clear();
    push_scope();
    address_taken_locals_.clear();
    host_bearing_locals_.clear();
    cleanup_stack_.clear();

    // `base` = self; cada campo hermano de offset CONSTANTE se lee de
    // [self + off] (host) y se liga por nombre -> el body los usa como locales.
    // F4: `this`/`self` = la vista completa (el propio puntero base), para que
    // el resolver navegue arrays hermanos declarativamente
    // (`this.Sections[i].campo`) via la maquinaria overlay -- sin aritmetica de
    // punteros ni helpers.
    bind("base", self_pv);
    bind("this", self_pv);
    // F4: `parent<T>()` en el body baja a este valor (el puntero raiz).
    if (root_pv != ir::IR_NO_VALUE) bind("__ovl_root", root_pv);
    // @element: `index` en scope.
    if (index_pv != ir::IR_NO_VALUE) bind("index", index_pv);
    for (const auto &sib : lay.fields) {
        // Saltar dinamicos y ARRAYS: los arrays no son un escalar cargable; se
        // navegan por `this.<array>[i]` (no como nombre desnudo).
        if (sib.offset_expr || sib.offset_block || sib.array_count ||
            sib.array_stride || sib.element_block)
            continue; // solo escalares de offset constante
        ir::IrValueId saddr = self_pv;
        if (sib.offset != 0) {
            ir::IrValueId so =
                emit_const(ir::IrType::I64, (uint64_t)sib.offset, 0);
            saddr = child_fn.new_value(ir::IrType::PTR);
            child_fn.values[saddr].is_host_ptr = true;
            ir::IrInstr a{};
            a.op = ir::IrOp::ADD;
            a.type = ir::IrType::PTR;
            a.dst = saddr;
            a.operands = {self_pv, so};
            child_fn.append(entry, std::move(a));
        }
        const ir::IrType st = ir_type_from_primitive(sib.type.kind);
        ir::IrValueId sv = child_fn.new_value(st);
        ir::IrInstr l{};
        l.op = ir::IrOp::LOAD;
        l.type = st;
        l.dst = sv;
        l.operands = {saddr};
        child_fn.append(entry, std::move(l));
        bind(sib.name, sv);
    }

    // Lower del body: if/else, multiples return, etc. -> RET (la direccion).
    lower_block(body_block);
    if (!block_terminated_) {
        // Defensa: si el body no termina en return, devolvemos base
        // (identidad).
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::PTR;
        rt.operands = {self_pv};
        emit(current_block_, std::move(rt));
        block_terminated_ = true;
    }

    pop_scope();
    pending_spawn_helpers_.push_back(std::move(child_fn));
    generated_overlay_resolvers_.insert(fn_name);

    // Restaurar contexto del padre.
    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    scopes_ = std::move(saved_scopes);
    address_taken_locals_ = std::move(saved_addr_taken);
    cleanup_stack_ = std::move(saved_cleanups);
    sret_active_ = saved_sret_active;
    sret_retbuf_ = saved_sret_retbuf;
    sret_buf_size_ = saved_sret_buf_size;
    current_fn_returns_function_ = saved_returns_fn;
    return fn_name;
}

std::string Lowering::generate_overlay_extent(const StructLayout &lay) {
    const std::string fn_name = "__ovl_extent_" + lay.name;
    if (generated_overlay_resolvers_.count(fn_name)) return fn_name; // dedup
    generated_overlay_resolvers_.insert(fn_name);

    // Salvar contexto (mismo protocolo que generate_overlay_resolver).
    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;
    std::vector<std::unordered_map<std::string, ir::IrValueId>> saved_scopes =
        std::move(scopes_);
    std::unordered_set<std::string> saved_addr_taken =
        std::move(address_taken_locals_);
    std::vector<CleanupAction> saved_cleanups = std::move(cleanup_stack_);
    const bool saved_sret_active = sret_active_;
    const ir::IrValueId saved_sret_retbuf = sret_retbuf_;
    const uint64_t saved_sret_buf_size = sret_buf_size_;
    const bool saved_returns_fn = current_fn_returns_function_;
    sret_active_ = false;
    sret_retbuf_ = ir::IR_NO_VALUE;
    sret_buf_size_ = 0;
    current_fn_returns_function_ = false;

    ir::IrFunction child_fn;
    child_fn.name = fn_name;
    child_fn.ret_type = ir::IrType::U64; // el span en bytes
    ir::IrValueId self_pv = child_fn.new_value(ir::IrType::PTR, "%self");
    child_fn.values[self_pv].is_param = true;
    child_fn.values[self_pv].is_host_ptr = true;
    child_fn.params.push_back(self_pv);

    const ir::IrBlockId entry = child_fn.new_block("entry");
    fn_ = &child_fn;
    current_block_ = entry;
    block_terminated_ = false;
    scopes_.clear();
    push_scope();
    address_taken_locals_.clear();
    host_bearing_locals_.clear();
    cleanup_stack_.clear();

    bind("base", self_pv);
    bind("this", self_pv);
    // Ligar hermanos escalares de offset constante (para
    // offset_expr/count/stride).
    for (const auto &sib : lay.fields) {
        if (sib.offset_expr || sib.offset_block || sib.array_count ||
            sib.array_stride || sib.element_block)
            continue;
        ir::IrValueId saddr = self_pv;
        if (sib.offset != 0) {
            ir::IrValueId so =
                emit_const(ir::IrType::I64, (uint64_t)sib.offset, 0);
            saddr = child_fn.new_value(ir::IrType::PTR);
            child_fn.values[saddr].is_host_ptr = true;
            ir::IrInstr a{};
            a.op = ir::IrOp::ADD;
            a.type = ir::IrType::PTR;
            a.dst = saddr;
            a.operands = {self_pv, so};
            child_fn.append(entry, std::move(a));
        }
        const ir::IrType st = ir_type_from_primitive(sib.type.kind);
        ir::IrValueId sv = child_fn.new_value(st);
        ir::IrInstr l{};
        l.op = ir::IrOp::LOAD;
        l.type = st;
        l.dst = sv;
        l.operands = {saddr};
        child_fn.append(entry, std::move(l));
        bind(sib.name, sv);
    }

    // Helpers de emision (u64).
    auto bin = [&](ir::IrOp op, ir::IrValueId a, ir::IrValueId b,
                   ir::IrType t) -> ir::IrValueId {
        ir::IrValueId d = fn_->new_value(t);
        ir::IrInstr in{};
        in.op = op;
        in.type = t;
        in.dst = d;
        in.operands = {a, b};
        emit(current_block_, std::move(in));
        return d;
    };
    // max sin ramas: max(a,b) = b ^ ((a^b) & -(a>b)).
    auto emit_max = [&](ir::IrValueId a, ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId gt = bin(ir::IrOp::CMP_UGT, a, b, ir::IrType::BOOL);
        ir::IrValueId mask = fn_->new_value(ir::IrType::U64);
        {
            ir::IrInstr n{};
            n.op = ir::IrOp::NEG;
            n.type = ir::IrType::U64;
            n.dst = mask;
            n.operands = {gt};
            emit(current_block_, std::move(n));
        }
        ir::IrValueId axb = bin(ir::IrOp::XOR, a, b, ir::IrType::U64);
        ir::IrValueId tmp = bin(ir::IrOp::AND, axb, mask, ir::IrType::U64);
        return bin(ir::IrOp::XOR, b, tmp, ir::IrType::U64);
    };

    ir::IrValueId maxv = emit_const(ir::IrType::U64, 0, 0);
    for (const auto &fi : lay.fields) {
        // Saltar lo que extent no cubre (documentado).
        if (fi.resolver_uses_parent) continue; // necesita root
        if (fi.is_array && (!fi.array_count || fi.element_block))
            continue; // sin count / @element
        ir::IrValueId end = ir::IR_NO_VALUE;
        const uint64_t fsz = fi.size;
        // base_off del campo/array (relativo a self).
        ir::IrValueId base_off;
        if (fi.offset_block) {
            const std::string rname = generate_overlay_resolver(lay, fi);
            ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            fn_->values[addr].is_host_ptr = true;
            ir::IrInstr in{};
            in.op = ir::IrOp::CALL;
            in.func_name = rname;
            in.type = ir::IrType::PTR;
            in.dst = addr;
            in.operands = {self_pv};
            in.is_call_site = true;
            emit(current_block_, std::move(in));
            base_off = bin(ir::IrOp::SUB, addr, self_pv, ir::IrType::U64);
        } else if (fi.offset_expr) {
            base_off = lower_expr(fi.offset_expr);
            if (base_off == ir::IR_NO_VALUE) continue;
        } else {
            base_off = emit_const(ir::IrType::U64, (uint64_t)fi.offset, 0);
        }
        if (fi.is_array) {
            ir::IrValueId cnt = lower_expr(fi.array_count);
            ir::IrValueId strd = lower_expr(fi.array_stride);
            if (cnt == ir::IR_NO_VALUE || strd == ir::IR_NO_VALUE) continue;
            ir::IrValueId span = bin(ir::IrOp::MUL, cnt, strd, ir::IrType::U64);
            end = bin(ir::IrOp::ADD, base_off, span, ir::IrType::U64);
        } else {
            ir::IrValueId sz = emit_const(ir::IrType::U64, fsz, 0);
            end = bin(ir::IrOp::ADD, base_off, sz, ir::IrType::U64);
        }
        maxv = emit_max(maxv, end);
    }
    {
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::U64;
        rt.operands = {maxv};
        emit(current_block_, std::move(rt));
        block_terminated_ = true;
    }

    pop_scope();
    pending_spawn_helpers_.push_back(std::move(child_fn));

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    scopes_ = std::move(saved_scopes);
    address_taken_locals_ = std::move(saved_addr_taken);
    cleanup_stack_ = std::move(saved_cleanups);
    sret_active_ = saved_sret_active;
    sret_retbuf_ = saved_sret_retbuf;
    sret_buf_size_ = saved_sret_buf_size;
    current_fn_returns_function_ = saved_returns_fn;
    return fn_name;
}

ir::IrValueId Lowering::lower_field_addr(ast::FieldAccessExpr *e) {
    // `lib.G` sobre un global de otro modulo: no hay base que bajar (`lib` es
    // un namespace, no un valor), la direccion ES la del slot compartido.
    // Punto unico: por aqui pasan la lectura, la escritura y el `&`.
    uint64_t ns_slot = 0;
    if (imported_global_slot_of(e, ns_slot)) {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        // El storage vive en memoria host (seccion `gdata`), como el de
        // cualquier global.
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr is{};
        is.op = ir::IrOp::STR_LIT_ADDR;
        is.type = ir::IrType::PTR;
        is.dst = v;
        is.imm = ns_slot;
        is.source_line = e->loc.line;
        emit(current_block_, std::move(is));
        return v;
    }
    const ir::IrValueId base = lower_expr(e->base.get());
    if (base == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

    const Type bt = e->base->result_type;
    if (bt.kind != PrimitiveKind::STRUCT) {
        error_at(e->loc, "lowering: '.' sobre tipo no-struct");
        return ir::IR_NO_VALUE;
    }
    const auto &layouts = tc_.struct_layouts();
    auto it = layouts.find(bt.struct_name);
    if (it == layouts.end()) {
        error_at(e->loc, "lowering: layout no disponible para struct '" +
                             bt.struct_name + "'");
        return ir::IR_NO_VALUE;
    }
    const StructLayout &lay = it->second;
    uint32_t offset = 0;
    const StructFieldInfo *fifound = nullptr;
    for (const auto &f : lay.fields) {
        if (f.name == e->field_name) {
            offset = f.offset;
            fifound = &f;
            break;
        }
    }
    // Campo `comptime` (property_kind=97): su slot vive apilado tras los campos
    // runtime (offset asignado en el layout, dentro de @c comptime_size_bytes).
    // Solo se accede desde codigo comptime (ctor/metodo comptime, ejecutado en
    // la ComptimeVM cuyo buffer `this` se dimensiona a @c comptime_size_bytes).
    // No hay overlay/bitfield/offset dinamico en campos comptime: address plana
    // base + offset.
    if (!fifound) {
        for (const auto &f : lay.comptime_fields) {
            if (f.name == e->field_name) {
                offset = f.offset;
                fifound = &f;
                break;
            }
        }
        if (fifound) {
            if (offset == 0) return base;
            const ir::IrValueId off_c =
                emit_const(ir::IrType::I64, offset, e->loc.line);
            const ir::IrValueId ca = fn_->new_value(ir::IrType::PTR);
            fn_->values[ca].is_host_ptr = fn_->values[base].is_host_ptr;
            ir::IrInstr ci{};
            ci.op = ir::IrOp::ADD;
            ci.type = ir::IrType::PTR;
            ci.dst = ca;
            ci.operands = {base, off_c};
            ci.source_line = e->loc.line;
            emit(current_block_, std::move(ci));
            return ca;
        }
    }
    if (!fifound) {
        error_at(e->loc, "lowering: campo '" + e->field_name +
                             "' no encontrado en struct '" + bt.struct_name +
                             "'");
        return ir::IR_NO_VALUE;
    }
    // Overlay F3: resolver de BLOQUE `@offset { ...; return <direccion>; }`.
    // Se sintetiza como funcion `__ovl_resolve_<S>_<f>(self)` (control de flujo
    // completo: if/else, multiples return; sin ALLOCA-en-bucle) y se llama con
    // el puntero base.  El resultado ES la direccion (host) donde
    // leer/escribir.
    if (fifound->offset_block) {
        const std::string rname = generate_overlay_resolver(lay, *fifound);
        ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        fn_->values[addr].is_host_ptr = fn_->values[base].is_host_ptr;
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALL;
        ins.func_name = rname;
        ins.type = ir::IrType::PTR;
        ins.dst = addr;
        ins.operands = {base};
        // F4: enhebrar `root` si el resolver usa parent<T>() (la vista raiz de
        // la cadena de accesos: `pe` en `pe.Imports[i].name`).
        if (fifound->resolver_uses_parent) {
            const ir::IrValueId root_v = lower_overlay_root(e->base.get());
            if (root_v != ir::IR_NO_VALUE) ins.operands.push_back(root_v);
        }
        ins.is_call_site = true;
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        return addr;
    }

    // Overlay F2: offset DINAMICO `@offset(hermano + N)`.  Evaluamos la
    // expresion en tiempo de acceso; los nombres desnudos de campos hermanos
    // se enlazan a `LOAD [base + hermano.offset]` (host) en un scope temporal
    // y `lower_expr` los resuelve por @c lookup.  El DAG del type checker
    // garantiza que no hay ciclos.  fld_addr = base + offset_dinamico.
    if (fifound->offset_expr) {
        push_scope();
        for (const auto &sib : lay.fields) {
            if (sib.offset_expr) continue; // solo hermanos de offset constante
            ir::IrValueId saddr = base;
            if (sib.offset != 0) {
                ir::IrValueId so = emit_const(
                    ir::IrType::I64, (uint64_t)sib.offset, e->loc.line);
                saddr = fn_->new_value(ir::IrType::PTR);
                fn_->values[saddr].is_host_ptr = fn_->values[base].is_host_ptr;
                ir::IrInstr a{};
                a.op = ir::IrOp::ADD;
                a.type = ir::IrType::PTR;
                a.dst = saddr;
                a.operands = {base, so};
                a.source_line = e->loc.line;
                emit(current_block_, std::move(a));
            }
            const ir::IrType st = ir_type_from_primitive(sib.type.kind);
            ir::IrValueId sv = fn_->new_value(st);
            ir::IrInstr l{};
            l.op = ir::IrOp::LOAD;
            l.type = st;
            l.dst = sv;
            l.operands = {saddr};
            l.source_line = e->loc.line;
            emit(current_block_, std::move(l));
            bind(sib.name, sv);
        }
        const ir::IrValueId off_val = lower_expr(fifound->offset_expr);
        pop_scope();
        if (off_val == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrValueId fld_addr = fn_->new_value(ir::IrType::PTR);
        fn_->values[fld_addr].is_host_ptr = fn_->values[base].is_host_ptr;
        ir::IrInstr ins{};
        ins.op = ir::IrOp::ADD;
        ins.type = ir::IrType::PTR;
        ins.dst = fld_addr;
        ins.operands = {base, off_val};
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        return fld_addr;
    }

    if (offset == 0) return base;

    // ptr_field = ptr_base + offset.  Tratamos los punteros como i64
    // a efectos aritmeticos (la VM no distingue tipos de puntero a
    // este nivel; la aritmetica ya escalada queda en el caller).
    const ir::IrValueId off_val =
        emit_const(ir::IrType::I64, offset, e->loc.line);
    const ir::IrValueId fld_addr = fn_->new_value(ir::IrType::PTR);
    // B1 fix: heredar is_host_ptr del base.  Sin esto, el LOAD/STORE
    // posterior sobre fld_addr emite `mov` (memoria VM) cuando el base
    // es host_ptr -> lee/escribe garbage.  Caso observado:
    // `(*ptr_of(unique_struct)).y` con offset=4 leia 0 (memoria VM
    // aleatoria) en lugar del valor real del campo.
    fn_->values[fld_addr].is_host_ptr = fn_->values[base].is_host_ptr;
    ir::IrInstr ins{};
    ins.op = ir::IrOp::ADD;
    ins.type = ir::IrType::PTR;
    ins.dst = fld_addr;
    ins.operands = {base, off_val};
    ins.source_line = e->loc.line;
    emit(current_block_, std::move(ins));
    return fld_addr;
}

ir::IrValueId Lowering::emit_overlay_endian_swap(ast::Expr *base_expr,
                                                 const StructLayout &lay,
                                                 const StructFieldInfo &fi,
                                                 ir::IrValueId value,
                                                 uint32_t line) {
    if (!fi.endian_expr) return value;
    // 1. Evaluar la expr de endianness con los hermanos ligados desde la base.
    const ir::IrValueId ov_base = lower_expr(base_expr);
    if (ov_base == ir::IR_NO_VALUE) return value;
    push_scope();
    bind("base", ov_base);
    bind("this", ov_base);
    for (const auto &sib : lay.fields) {
        if (sib.offset_expr || sib.offset_block || sib.array_count ||
            sib.array_stride || sib.element_block)
            continue; // solo escalares de offset constante
        ir::IrValueId saddr = ov_base;
        if (sib.offset != 0) {
            ir::IrValueId so =
                emit_const(ir::IrType::I64, (uint64_t)sib.offset, line);
            saddr = fn_->new_value(ir::IrType::PTR);
            fn_->values[saddr].is_host_ptr = fn_->values[ov_base].is_host_ptr;
            ir::IrInstr a{};
            a.op = ir::IrOp::ADD;
            a.type = ir::IrType::PTR;
            a.dst = saddr;
            a.operands = {ov_base, so};
            a.source_line = line;
            emit(current_block_, std::move(a));
        }
        const ir::IrType st = ir_type_from_primitive(sib.type.kind);
        ir::IrValueId sv = fn_->new_value(st);
        ir::IrInstr l{};
        l.op = ir::IrOp::LOAD;
        l.type = st;
        l.dst = sv;
        l.operands = {saddr};
        l.source_line = line;
        emit(current_block_, std::move(l));
        bind(sib.name, sv);
    }
    const ir::IrValueId big = lower_expr(fi.endian_expr);
    pop_scope();
    if (big == ir::IR_NO_VALUE) return value;

    auto bin = [&](ir::IrOp op, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId d = fn_->new_value(ir::IrType::U64);
        ir::IrInstr in{};
        in.op = op;
        in.type = ir::IrType::U64;
        in.dst = d;
        in.operands = {a, b};
        in.source_line = line;
        emit(current_block_, std::move(in));
        return d;
    };
    // 2. sw = bswap64(value) >> (8-w)*8  (BYTESWAP swapea los 8 bytes).
    ir::IrValueId sw64 = fn_->new_value(ir::IrType::U64);
    {
        ir::IrInstr b{};
        b.op = ir::IrOp::BYTESWAP;
        b.type = ir::IrType::U64;
        b.dst = sw64;
        b.operands = {value};
        b.source_line = line;
        emit(current_block_, std::move(b));
    }
    ir::IrValueId sw = sw64;
    if (fi.size < 8) {
        ir::IrValueId shamt =
            emit_const(ir::IrType::U64, (uint64_t)(8 - fi.size) * 8, line);
        sw = bin(ir::IrOp::SHR, sw64, shamt);
    }
    // 3. select sin ramas: big ? sw : value = value ^ ((value ^ sw) &
    // -(big!=0)).
    ir::IrValueId zero = emit_const(ir::IrType::U64, 0, line);
    ir::IrValueId nz = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr c{};
        c.op = ir::IrOp::CMP_NE;
        c.type = ir::IrType::BOOL;
        c.dst = nz;
        c.operands = {big, zero};
        c.source_line = line;
        emit(current_block_, std::move(c));
    }
    ir::IrValueId mask = fn_->new_value(ir::IrType::U64);
    {
        ir::IrInstr n{};
        n.op = ir::IrOp::NEG;
        n.type = ir::IrType::U64;
        n.dst = mask;
        n.operands = {nz};
        n.source_line = line;
        emit(current_block_, std::move(n));
    }
    ir::IrValueId vxs = bin(ir::IrOp::XOR, value, sw);
    ir::IrValueId tmp = bin(ir::IrOp::AND, vxs, mask);
    return bin(ir::IrOp::XOR, value, tmp);
}




// ---------------------------------------------------------------------
// Lowering de asignaciones.
//
// En el modelo SSA-construction de Braun, una asignacion `x = expr` no
// produce ninguna instruccion explicita: simplemente actualiza el mapa
// "nombre -> IrValueId actual" en el scope donde @c x esta definida.
// El siguiente uso de @c x leera ese nuevo IrValueId via lookup().
//
// Las asignaciones compuestas (+=, -=, *=, ...) se traducen a un
// binop IR seguido del mismo update; reusan emit_binop_ir() para no
// duplicar la logica de seleccion de opcode aritmetico/bitwise.
// ---------------------------------------------------------------------

// Helper: emite un IrInstr binario y devuelve el IrValueId del resultado.
// Se usa tanto en lower_binary() como en compound assignments.
ir::IrValueId Lowering::emit_binop_ir(ast::BinOp op, ir::IrValueId lhs_val,
                                      ir::IrValueId rhs_val,
                                      PrimitiveKind common,
                                      const SourceLoc &loc) {
    const ir::IrType common_ir = ir_type_from_primitive(common);
    const bool is_float = is_floating(common);
    const bool is_unsign = is_integral(common) && !is_signed_integral(common);

    ir::IrOp ir_op = ir::IrOp::ADD;
    ir::IrType res_ir = common_ir;
    switch (op) {
    case ast::BinOp::Add:
        ir_op = is_float ? ir::IrOp::FADD : ir::IrOp::ADD;
        break;
    case ast::BinOp::Sub:
        ir_op = is_float ? ir::IrOp::FSUB : ir::IrOp::SUB;
        break;
    case ast::BinOp::Mul:
        ir_op = is_float ? ir::IrOp::FMUL : ir::IrOp::MUL;
        break;
    case ast::BinOp::Div:
        ir_op = is_float ? ir::IrOp::FDIV : ir::IrOp::DIV;
        break;
    case ast::BinOp::Mod: ir_op = ir::IrOp::MOD; break;
    case ast::BinOp::BitAnd: ir_op = ir::IrOp::AND; break;
    case ast::BinOp::BitOr: ir_op = ir::IrOp::OR; break;
    case ast::BinOp::BitXor: ir_op = ir::IrOp::XOR; break;
    case ast::BinOp::Shl: ir_op = ir::IrOp::SHL; break;
    case ast::BinOp::Shr:
        ir_op = is_unsign ? ir::IrOp::SHR : ir::IrOp::SAR;
        break;
    // Las comparaciones / logicos no suelen aparecer en compound
    // assignment (no existen ==, &&, etc.), pero las dejamos por
    // completitud; result_ir se cambia a BOOL.
    case ast::BinOp::Eq:
        ir_op = is_float ? ir::IrOp::FCMP_EQ : ir::IrOp::CMP_EQ;
        res_ir = ir::IrType::BOOL;
        break;
    case ast::BinOp::Neq:
        ir_op = is_float ? ir::IrOp::FCMP_NE : ir::IrOp::CMP_NE;
        res_ir = ir::IrType::BOOL;
        break;
    case ast::BinOp::Lt:
        ir_op = is_float ? ir::IrOp::FCMP_LT
                         : (is_unsign ? ir::IrOp::CMP_ULT : ir::IrOp::CMP_LT);
        res_ir = ir::IrType::BOOL;
        break;
    case ast::BinOp::Le:
        ir_op = is_float ? ir::IrOp::FCMP_LE
                         : (is_unsign ? ir::IrOp::CMP_ULE : ir::IrOp::CMP_LE);
        res_ir = ir::IrType::BOOL;
        break;
    case ast::BinOp::Gt:
        ir_op = is_float ? ir::IrOp::FCMP_GT
                         : (is_unsign ? ir::IrOp::CMP_UGT : ir::IrOp::CMP_GT);
        res_ir = ir::IrType::BOOL;
        break;
    case ast::BinOp::Ge:
        ir_op = is_float ? ir::IrOp::FCMP_GE
                         : (is_unsign ? ir::IrOp::CMP_UGE : ir::IrOp::CMP_GE);
        res_ir = ir::IrType::BOOL;
        break;
    case ast::BinOp::LogicalAnd:
        ir_op = ir::IrOp::AND;
        res_ir = ir::IrType::BOOL;
        break;
    case ast::BinOp::LogicalOr:
        ir_op = ir::IrOp::OR;
        res_ir = ir::IrType::BOOL;
        break;
    }

    const ir::IrValueId dst = fn_->new_value(res_ir);
    ir::IrInstr ins{};
    ins.op = ir_op;
    ins.type = res_ir;
    ins.dst = dst;
    ins.operands = {lhs_val, rhs_val};
    ins.source_line = loc.line;
    emit(current_block_, std::move(ins));
    return dst;
}

// Mapea AssignOp compuesto al BinOp aritmetico/bitwise correspondiente.
// Se usa por todas las rutas de lower_assign que necesitan implementar
// x op= v (struct field, class field, p[i], *p y la ya existente para
// identifier).  Devuelve BinOp::Add para Assign (no deberia llamarse
// con ese caso; el caller filtra antes).
static ast::BinOp compound_assign_op_to_binop(ast::AssignOp op) {
    switch (op) {
    case ast::AssignOp::AddAssign: return ast::BinOp::Add;
    case ast::AssignOp::SubAssign: return ast::BinOp::Sub;
    case ast::AssignOp::MulAssign: return ast::BinOp::Mul;
    case ast::AssignOp::DivAssign: return ast::BinOp::Div;
    case ast::AssignOp::ModAssign: return ast::BinOp::Mod;
    case ast::AssignOp::BitAndAssign: return ast::BinOp::BitAnd;
    case ast::AssignOp::BitOrAssign: return ast::BinOp::BitOr;
    case ast::AssignOp::BitXorAssign: return ast::BinOp::BitXor;
    case ast::AssignOp::ShlAssign: return ast::BinOp::Shl;
    case ast::AssignOp::ShrAssign: return ast::BinOp::Shr;
    case ast::AssignOp::Assign: return ast::BinOp::Add;
    }
    return ast::BinOp::Add;
}

/**
 * @brief A.39 - emite el cuerpo de un `comptime for` desenrollado.
 *
 * Evalua lo/hi en compile-time (ya validados por type checker) y
 * por cada valor del index push scope con i=valor, lower body, pop.
 * Cero loop runtime: N copias del body emitidas en secuencia, con
 * el index sustituido como CONST en cada copia via
 * @c lowering_comptime_scopes_ (consultado por @c lower_ident).
 */
void Lowering::lower_comptime_for(ast::ComptimeForStmt *s) {
    if (!s || !s->lo_expr || !s->hi_expr || !s->body) return;
    const ComptimeEvalResult lo = comptime_eval_expr(tc_, s->lo_expr.get());
    const ComptimeEvalResult hi = comptime_eval_expr(tc_, s->hi_expr.get());
    if (!lo.ok || !hi.ok || lo.is_str || hi.is_str) {
        error_at(s->loc, "comptime for: rango no evaluable (lo/hi deben ser "
                         "enteros comptime)");
        return;
    }
    /* Limite defensivo para evitar explosion de codigo. */
    const int64_t lo_v = lo.value;
    int64_t hi_v = hi.value;
    if (s->inclusive) hi_v += 1;
    if (hi_v - lo_v > 4096) {
        error_at(s->loc, "comptime for: rango excede 4096 iteraciones; usar un "
                         "loop runtime en su lugar");
        return;
    }
    /* A.39: el bind del index lo hacemos en DOS lugares:
     *   1. `lowering_comptime_scopes_` para que @c lower_ident lo
     *      inline como CONST en el codigo runtime emitido.
     *   2. `tc.comptime_const_locals_` para que @c comptime_eval_expr
     *      pueda resolverlo cuando aparezca como arg de un comptime fn
     *      o builtin comptime.  Sin esto, `fact(k)` desde el body
     *      del for fallaria con "no comptime-evaluable" porque k
     *      no estaria en tc's stack. */
    auto &mut_tc = const_cast<TypeChecker &>(tc_);
    for (int64_t i = lo_v; i < hi_v; ++i) {
        /* Push lowering scope. */
        std::unordered_map<std::string, ComptimeLocalEntry> scope;
        ComptimeLocalEntry ent;
        ent.value = i;
        ent.ir_t = ir::IrType::I64;
        scope[s->var_name] = ent;
        lowering_comptime_scopes_.push_back(std::move(scope));
        /* Push tc scope. */
        mut_tc.push_comptime_scope();
        TypeChecker::ComptimeConst c;
        c.type = Type{PrimitiveKind::I64};
        c.value = i;
        mut_tc.register_comptime_local(s->var_name, std::move(c));
        /* Lower body. */
        lower_stmt(s->body.get());
        /* Pop. */
        mut_tc.pop_comptime_scope();
        lowering_comptime_scopes_.pop_back();
    }
}

/**
 * @brief A.38 - lowering del operador ternario `cond ? then : else`.
 *
 * Estructura CFG identica a lower_if con PHI en el merge:
 *   current -> br_cond cond, then_bb, else_bb
 *   then_bb -> lower(then_expr) -> br merge_bb
 *   else_bb -> lower(else_expr) -> br merge_bb
 *   merge   -> %r = phi [then_val, then_end] [else_val, else_end]
 *
 * El tipo resultado se toma del then_expr (ya validado por el type
 * checker que tt y et son asignables entre si).  Si difieren, se
 * aplica @c cast_if_needed al else para igualar.  Si then es un side-
 * effecting expr y la cond es comptime-evaluable, una optimizacion
 * futura podria dead-branch-eliminate; por ahora siempre emite el if.
 */

/**
 * @brief P2: lowering del operador `?` postfix para Result<V,E>.
 *
 * Desugar:
 * @code
 *   let v = expr?;
 * @endcode
 * a:
 * @code
 *   let __tmp = expr;       // SSA value = PTR al slot Result (24 bytes)
 *   if (tag(__tmp) == 0) {  // Err branch
 *     // copy __tmp (24 bytes) al sret_retbuf del caller
 *     // RET void
 *   }
 *   // Ok branch: extraer V de [__tmp + 8]
 * @endcode
 *
 * Layout del slot Result<V,E> (24 bytes):
 *   +0  i32 tag (0=Err, 1=Ok)
 *   +8  V      (Ok payload)
 *   +16 E      (Err payload)
 */
ir::IrValueId Lowering::lower_try_expr(ast::TryExpr *e) {
    if (!e || !e->operand) {
        error_at(e ? e->loc : SourceLoc{}, "lowering: TryExpr sin operand");
        return ir::IR_NO_VALUE;
    }
    const uint32_t src_line = e->loc.line;

    // 1. Bajar el operand -> SSA PTR al slot Result.
    const ir::IrValueId v_buf = lower_expr(e->operand.get());
    if (v_buf == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

    // 2. LOAD i32 del tag en offset 0.
    const ir::IrValueId tag_v = fn_->new_value(ir::IrType::I32);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I32;
        ld.dst = tag_v;
        ld.operands = {v_buf};
        ld.source_line = src_line;
        emit(current_block_, std::move(ld));
    }

    // 3. Comparacion tag == 0 (=Err).
    const ir::IrValueId zero_v = emit_const(ir::IrType::I32, 0, src_line);
    const ir::IrValueId cond_v = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr cm{};
        cm.op = ir::IrOp::CMP_EQ;
        cm.type = ir::IrType::BOOL;
        cm.dst = cond_v;
        cm.operands = {tag_v, zero_v};
        cm.source_line = src_line;
        emit(current_block_, std::move(cm));
    }

    // 4. Crear bloques: err_bb (early-return), ok_bb (extract value).
    const ir::IrBlockId err_bb =
        fn_->new_block("try_err_" + std::to_string(ternary_counter_));
    const ir::IrBlockId ok_bb =
        fn_->new_block("try_ok_" + std::to_string(ternary_counter_));
    ++ternary_counter_;

    // br_cond: si tag==0 -> err_bb, else -> ok_bb.
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands.push_back(cond_v);
        br.target_block = err_bb;
        br.false_block = ok_bb;
        br.source_line = src_line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(err_bb);
        fn_->blocks[current_block_].succs.push_back(ok_bb);
        fn_->blocks[err_bb].preds.push_back(current_block_);
        fn_->blocks[ok_bb].preds.push_back(current_block_);
    }

    // 5. err_bb: copia v_buf (24 bytes) al sret_retbuf + RET.
    // Mismo patron que lower_return cuando sret_active_ es true.
    current_block_ = err_bb;
    block_terminated_ = false;
    if (sret_active_ && sret_retbuf_ != ir::IR_NO_VALUE) {
        const uint64_t qwords = sret_buf_size_ / 8;
        for (uint64_t qi = 0; qi < qwords; ++qi) {
            const uint64_t off = qi * 8;
            const ir::IrValueId v_off =
                emit_const(ir::IrType::I64, off, src_line);
            const ir::IrValueId v_src_at = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr add{};
                add.op = ir::IrOp::ADD;
                add.type = ir::IrType::I64;
                add.dst = v_src_at;
                add.operands = {v_buf, v_off};
                add.source_line = src_line;
                emit(current_block_, std::move(add));
            }
            // BugFix 163 (2026-06-05): propagar is_host_ptr de v_buf al LOAD
            // side (igual que el STORE side abajo).  Sin esto el LOAD del
            // Err a copiar usaba `mov` (VM) en vez de `movh` (host) y leia
            // basura -> error(r) != el valor real (path de error de `?`).
            fn_->values[v_src_at].is_host_ptr = fn_->values[v_buf].is_host_ptr;
            const ir::IrValueId v_tmp = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = ir::IrType::I64;
                ld.dst = v_tmp;
                ld.operands = {v_src_at};
                ld.source_line = src_line;
                emit(current_block_, std::move(ld));
            }
            const ir::IrValueId v_off2 =
                emit_const(ir::IrType::I64, off, src_line);
            const ir::IrValueId v_dst_at = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr add{};
                add.op = ir::IrOp::ADD;
                add.type = ir::IrType::I64;
                add.dst = v_dst_at;
                add.operands = {sret_retbuf_, v_off2};
                add.source_line = src_line;
                emit(current_block_, std::move(add));
            }
            // BugFix sret-cross-mem (2026-06-04): propagar is_host_ptr.
            fn_->values[v_dst_at].is_host_ptr =
                fn_->values[sret_retbuf_].is_host_ptr;
            {
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {v_tmp, v_dst_at};
                st.source_line = src_line;
                emit(current_block_, std::move(st));
            }
        }
    }
    // Emit cleanups (synchronized, etc.) y RET.
    emit_cleanups_all();
    {
        ir::IrInstr ret{};
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::VOID;
        ret.source_line = src_line;
        emit(current_block_, std::move(ret));
        block_terminated_ = true;
    }

    // 6. ok_bb: extraer V de v_buf+8.  El tipo V se obtiene del result_type
    // que el type checker ya validamos (pointee del Result).
    current_block_ = ok_bb;
    block_terminated_ = false;
    const Type result_t = e->result_type;
    const ir::IrType payload_t = (result_t.kind != PrimitiveKind::VOID &&
                                  result_t.kind != PrimitiveKind::COUNT)
                                     ? ir_type_from_primitive(result_t.kind)
                                     : ir::IrType::I64;
    const ir::IrValueId v_off8 = emit_const(ir::IrType::I64, 8, src_line);
    const ir::IrValueId v_at8 = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr add{};
        add.op = ir::IrOp::ADD;
        add.type = ir::IrType::I64;
        add.dst = v_at8;
        add.operands = {v_buf, v_off8};
        add.source_line = src_line;
        emit(current_block_, std::move(add));
    }
    // BugFix 163 (2026-06-05): propagar is_host_ptr de v_buf a v_at8.  El
    // buffer del Result temporal del operando es un host_ptr; sin esta
    // marca, el LOAD de V emitia `mov` (VM mem) en vez de `movh` (host) y
    // leia 0/basura.  La rama err ya lo propagaba (de ahi que err funcione
    // y ok no).  Aplica al value extraction de la rama ok.
    fn_->values[v_at8].is_host_ptr = fn_->values[v_buf].is_host_ptr;
    const ir::IrValueId v_dst = fn_->new_value(payload_t);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = payload_t;
        ld.dst = v_dst;
        ld.operands = {v_at8};
        ld.source_line = src_line;
        emit(current_block_, std::move(ld));
    }
    return v_dst;
}

ir::IrValueId Lowering::lower_overloaded_step(ast::UnaryExpr *e, bool is_inc,
                                              bool is_pre) {
    // `c++` sobre un tipo que sobrecarga la suma ES `c += 1`.  Se fabrica ese
    // compound y se le pide al type checker que lo resuelva igual que a uno
    // escrito a mano (`__iadd__` in-place, o desazucarado a `c = c + 1`).  El
    // AssignExpr nace AQUI, con el checker ya pasado, asi que hay que pedirselo
    // explicitamente -- pero la regla vive en un solo sitio.
    const Type &tt = e->operand->result_type;
    const int ln = e->loc.line;
    // Postfijo: el valor de la expresion es el ANTERIOR -> leerlo antes.  En
    // los tres lvalues el "valor" de un agregado es su direccion, asi que esto
    // devuelve la direccion del objeto, no una copia; para `c++` como
    // sentencia (el uso normal) da igual, y en expresion es lo mismo que hace
    // el resto del lowering con los agregados.
    ir::IrValueId prev = ir::IR_NO_VALUE;
    if (!is_pre) prev = lower_expr(e->operand.get());

    auto one = std::make_unique<ast::IntLitExpr>();
    one->value = 1;
    // El `1` es un ENTERO aunque `c` no lo sea: el dunder recibe un i64.  Si le
    // copiaramos el tipo de `c` (un STRUCT), el dispatch no casaria.
    one->result_type = Type{PrimitiveKind::I64};
    one->loc = e->loc;

    ast::AssignExpr asn;
    asn.op = is_inc ? ast::AssignOp::AddAssign : ast::AssignOp::SubAssign;
    asn.loc = e->loc;
    asn.result_type = tt;
    asn.target = std::move(e->operand);
    asn.value = std::move(one);
    Type res;
    const bool ok = tc_.prepare_overloaded_compound_assign(
        &asn, tt, Type{PrimitiveKind::I64}, res);
    ir::IrValueId new_val = ir::IR_NO_VALUE;
    if (ok) new_val = lower_assign(&asn);
    e->operand = std::move(asn.target); // devolver el operando al AST
    if (!ok) {
        error_at(e->loc, "lowering: '" + std::string(is_inc ? "++" : "--") +
                             "' sobre un tipo que no sobrecarga la suma");
        return ir::IR_NO_VALUE;
    }
    (void)ln;
    return is_pre ? new_val : prev;
}

ir::IrValueId Lowering::lower_assign(ast::AssignExpr *e) {
    // admitimos como lvalue: IdentExpr (variable simple) o
    // FieldAccessExpr (p.x = v).  Otros lvalues (deref de puntero,
    // indexado de array)
    if (!e->target) {
        error_at(e->loc, "lowering: target de '=' nulo");
        return ir::IR_NO_VALUE;
    }
    // static field de struct como LHS (`Struct.campo = v`): su storage es la
    // global sintetica `<Struct>__<campo>`.  Reescribimos el target al
    // IdentExpr de esa global y dejamos que el flujo normal de
    // asignacion-a-global lo maneje (incluye compound assign).
    if (e->target->kind == ast::NodeKind::FieldAccessExpr) {
        auto *fa = static_cast<ast::FieldAccessExpr *>(e->target.get());
        if (fa->property_kind == 8 && fa->base &&
            fa->base->kind == ast::NodeKind::IdentExpr) {
            auto *bid = static_cast<ast::IdentExpr *>(fa->base.get());
            auto gid = std::make_unique<ast::IdentExpr>();
            gid->loc = fa->loc;
            gid->name = bid->name + "__" + fa->field_name;
            gid->result_type = fa->result_type;
            e->target = std::move(gid);
        }
    }
    // Asignacion sobrecargada: el type checker dejo en @c overload_method el
    // dunder (`__assign__` para `=`, `__iadd__` para `+=`, ...).  Se desugara a
    // UNA llamada `target.__op__(value)` -- ni copia memberwise ni
    // load-op-store.  Esa es toda la diferencia: para `atomic<T>` la operacion
    // tiene que ser indivisible.
    // Mismo patron que el binario sobrecargado (ver lower_binary): se roban los
    // hijos para el call sintetico y se devuelven despues.
    if (!e->overload_method.empty() && e->target && e->value) {
        const bool recv_is_struct =
            (e->target->result_type.kind == PrimitiveKind::STRUCT);
        ast::CallExpr synth;
        synth.loc = e->loc;
        auto fa = std::make_unique<ast::FieldAccessExpr>();
        fa->loc = e->loc;
        fa->field_name = e->overload_method;
        fa->base = std::move(e->target);
        synth.callee = std::move(fa);
        synth.args.push_back(std::move(e->value));
        const ir::IrValueId v_call = recv_is_struct
                                         ? lower_struct_method_call(&synth)
                                         : lower_class_method_call(&synth);
        auto *fa_back = static_cast<ast::FieldAccessExpr *>(synth.callee.get());
        e->target = std::move(fa_back->base);
        e->value = std::move(synth.args[0]);
        return v_call;
    }
    // Si el valor es un lambda-literal que se almacena en un campo / slot /
    // deref, su env ESCAPA del scope actual (el objeto contenedor puede
    // sobrevivir al frame) -> debe alocarse en heap (GC).  Activamos el flag
    // mientras se baja el valor; un guard RAII lo restaura en cualquier
    // return de esta funcion.  lower_lambda_expr lo consulta.
    struct EscapeFlagGuard {
        bool &flag;
        bool prev;

        EscapeFlagGuard(bool &f, bool v) : flag(f), prev(f) { flag = v; }

        ~EscapeFlagGuard() { flag = prev; }
    };
    // El modelo de env owned-by-holder (RAW_ALLOC liberado por el destructor)
    // requiere que el contenedor tenga un punto de destruccion determinista.
    // En v1 solo lo aplicamos a campos de CLASE (su destructor aumentado
    // libera el env; ver emit_free_closure_env_field).  Para holders struct
    // (value-type, sin destructor de campos) el env se queda en STACK -- es
    // correcto y zero-cost para el caso no-escapante (el comun); un struct
    // con closure que se copia fuera de scope comparte el env de stack (misma
    // limitacion que cualquier struct con puntero crudo).  Ver
    // doc/VMdoc/Vesta/ClosuresEnCampos.md.
    // El RHS es "una lambda" tanto si es un LambdaExpr directo como si es un
    // metodo ligado `&obj.metodo` (UnaryExpr AddrOf con desugared_bound_method,
    // que el lowering baja como un lambda que captura el receptor).  Sin
    // detectar el segundo caso, current_lambda_store_escapes_ no se activa ->
    // el lambda del bound-method usa slot STACK (ALLOCA) en vez de heap owned
    // -> el reassign-free/dtor harian `free` de una direccion de stack ->
    // crash.
    bool _val_is_lambda =
        e->value && e->value->kind == ast::NodeKind::LambdaExpr;
    if (!_val_is_lambda && e->value &&
        e->value->kind == ast::NodeKind::UnaryExpr) {
        auto *uv = static_cast<ast::UnaryExpr *>(e->value.get());
        if (uv->desugared_bound_method) _val_is_lambda = true;
    }
    bool _tgt_is_class_field = false;
    bool _tgt_is_escaping_struct_field = false;
    if (e->target->kind == ast::NodeKind::FieldAccessExpr) {
        auto *fa = static_cast<ast::FieldAccessExpr *>(e->target.get());
        if (fa->base && fa->base->result_type.kind == PrimitiveKind::CLASS) {
            _tgt_is_class_field = true;
        }
        // Ownership escape-sensitive: un lambda almacenado en un campo de un
        // STRUCT local que ESCAPA (return/store -> escaping_locals_) necesita
        // env en HEAP (como el caso clase), porque el struct se mueve por valor
        // fuera del scope productor y el env de stack colgaria.  El consumidor
        // (init-from-call) lo libera (CLOSURE_ENV_FREE).  Si el struct NO
        // escapa, el env se queda en stack (cero coste).
        else if (fa->base &&
                 fa->base->result_type.kind == PrimitiveKind::STRUCT &&
                 fa->base->kind == ast::NodeKind::IdentExpr) {
            auto *bid = static_cast<ast::IdentExpr *>(fa->base.get());
            if (escaping_locals_.find(bid->name) != escaping_locals_.end())
                _tgt_is_escaping_struct_field = true;
        }
    }
    EscapeFlagGuard _esc_guard(
        current_lambda_store_escapes_,
        _val_is_lambda &&
            (_tgt_is_class_field || _tgt_is_escaping_struct_field));
    // Ownership: si el target es un campo unique<T> y el RHS construye un
    // unique (unique_box/unique_with), el slot Tier 1 debe ir a HEAP para
    // sobrevivir al scope productor (el campo lo posee; el dtor del contenedor
    // lo libera).  unique_slot_buf consume el flag al alocar el slot.
    bool _tgt_is_unique_field = false;
    if (e->target->kind == ast::NodeKind::FieldAccessExpr) {
        auto *fa = static_cast<ast::FieldAccessExpr *>(e->target.get());
        if (fa->result_type.kind == PrimitiveKind::UNIQUE_PTR && fa->base &&
            (fa->base->result_type.kind == PrimitiveKind::CLASS ||
             fa->base->result_type.kind == PrimitiveKind::STRUCT))
            _tgt_is_unique_field = true;
    }
    bool _val_is_unique_ctor = false;
    if (e->value && e->value->kind == ast::NodeKind::CallExpr) {
        auto *cv = static_cast<ast::CallExpr *>(e->value.get());
        if (cv->callee && cv->callee->kind == ast::NodeKind::IdentExpr) {
            const std::string &n =
                static_cast<ast::IdentExpr *>(cv->callee.get())->name;
            // bug3: `move(local)` que aterriza en un CAMPO unique tambien debe
            // materializar el slot movido en HEAP (no un ALLOCA de stack): el
            // campo lo posee y el dtor del contenedor hace RAW_FREE del slot.
            // Sin esto, el move dejaba el slot en la pila y el dtor liberaba
            // una direccion de stack -> SIGSEGV en VM/JIT.
            _val_is_unique_ctor =
                (n == "unique_box" || n == "unique_with" || n == "move");
        }
    }
    EscapeFlagGuard _uniq_guard(unique_slot_to_heap_,
                                _tgt_is_unique_field && _val_is_unique_ctor);
    // Caso FieldAccessExpr: dos rutas distintas por tipo de receptor.
    if (e->target->kind == ast::NodeKind::FieldAccessExpr) {
        auto *fa = static_cast<ast::FieldAccessExpr *>(e->target.get());
        // CLASS o static field (limitacion G cerrada, property_kind=3):
        // ruta SETFIELD con offset (lower_class_field_store), que
        // detecta property_kind=3 y emite findclass + setstatic.
        if ((fa->base && fa->base->result_type.kind == PrimitiveKind::CLASS) ||
            fa->property_kind == 3) {
            // fix.lazy-string - si el field es de tipo STRING y el
            // rhs es un string literal no interpolado, promovemos el
            // literal a StringObject (STRMAKE) ANTES del store.  Sin
            // esto, escribiriamos el host_ptr al literal en static_data
            // dentro del slot del field, que luego se interpretaria como
            // GcHandle invalido y crashearia al primer acceso.  La
            // promocion ya se hace para var-decl (`string s = "lit"`)
            // pero faltaba esta ruta para `this.field = "lit"` y
            // `obj.field = "lit"`.
            ir::IrValueId rhs = ir::IR_NO_VALUE;
            bool promoted = false;
            if (e->value && e->value->kind == ast::NodeKind::StringLitExpr &&
                fa->base &&
                fa->base->result_type.kind == PrimitiveKind::CLASS) {
                auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
                // Promovemos tanto literales puros como interpolados:
                // el helper detecta el caso y emite STRMAKE simple
                // (puro) o cadena STRMAKE+STRCAT (interpolado).
                auto it_cls =
                    tc_.class_layouts().find(fa->base->result_type.struct_name);
                if (it_cls != tc_.class_layouts().end()) {
                    for (const auto &f : it_cls->second.fields) {
                        if (f.name == fa->field_name &&
                            f.type.kind == PrimitiveKind::STRING) {
                            rhs = lower_string_literal_to_string_object(slit);
                            promoted = true;
                            break;
                        }
                    }
                }
            }
            if (!promoted) {
                rhs = lower_expr(e->value.get());
            }
            if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            if (e->op != ast::AssignOp::Assign) {
                // Compound: leer valor actual via getter o GETFIELD,
                // aplicar el op, escribir via setter o SETFIELD.  Reusa
                // lower_class_field_load (maneja getters de propiedades
                // y GETFIELD por offset) para cero duplicacion logica.
                ir::IrValueId cur = lower_class_field_load(fa);
                if (cur == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                const ast::BinOp bop = compound_assign_op_to_binop(e->op);
                rhs =
                    emit_binop_ir(bop, cur, rhs, fa->result_type.kind, e->loc);
            }
            return lower_class_field_store(fa, rhs, e->loc);
        }
        // STRUCT: ruta original via lower_field_addr + STORE.
        const ir::IrValueId addr = lower_field_addr(fa);
        if (addr == ir::IR_NO_VALUE) {
            (void)lower_expr(e->value.get());
            return ir::IR_NO_VALUE;
        }
        // Bug fix 2026-05-23 (Audit 45): auto-promotion del string literal
        // a StringObject cuando el field STRUCT es de tipo string.  Misma
        // motivacion que CLASS arriba: sin esto el host_ptr al literal
        // se guarda como GcHandle invalido en el slot.
        ir::IrValueId rhs;
        if (fa->result_type.kind == PrimitiveKind::STRING && e->value &&
            e->value->kind == ast::NodeKind::StringLitExpr) {
            auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
            rhs = lower_string_literal_to_string_object(slit);
        } else {
            rhs = lower_expr(e->value.get());
        }
        if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        const ir::IrType ft = ir_type_from_primitive(fa->result_type.kind);
        // Compound assign: leer el valor actual del campo (con
        // extraccion de bit field si aplica), aplicar el operador,
        // y luego seguir con la ruta de store normal (que tambien
        // maneja bit field RMW).
        if (e->op != ast::AssignOp::Assign) {
            ir::IrValueId cur = lower_field_access(fa);
            if (cur == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            const ast::BinOp bop = compound_assign_op_to_binop(e->op);
            rhs = emit_binop_ir(bop, cur, rhs, fa->result_type.kind, e->loc);
        }
        rhs = cast_if_needed(rhs, fn_->values[rhs].type, ft,
                             e->value ? e->value->loc : e->loc);

        // F5: campo `@endian(expr)` -> swap CONDICIONAL del valor a escribir
        // (simetrico con el read): el store LE deja los bytes en el orden que
        // dicta la expr.  Comptime se pliega; runtime = select sin ramas.
        {
            const Type bt_w = fa->base ? fa->base->result_type : Type{};
            if (bt_w.kind == PrimitiveKind::STRUCT) {
                auto it_w = tc_.struct_layouts().find(bt_w.struct_name);
                if (it_w != tc_.struct_layouts().end()) {
                    for (const auto &f : it_w->second.fields) {
                        if (f.name == fa->field_name && f.endian_expr &&
                            f.bit_width == 0 &&
                            (f.size == 2 || f.size == 4 || f.size == 8)) {
                            rhs = emit_overlay_endian_swap(fa->base.get(),
                                                           it_w->second, f, rhs,
                                                           e->loc.line);
                            break;
                        }
                    }
                }
            }
        }

        // WRITE de bit field: read-modify-write.  Si el campo es bit
        // field, leemos el storage word completo, le limpiamos los
        // bits del rango con AND inverse_mask, le metemos el valor
        // con OR ((rhs & mask) << bit_offset), y hacemos STORE de
        // vuelta.  Para campo normal (no-bitfield): STORE directo
        // del rhs sin lectura previa.
        const Type bt = fa->base ? fa->base->result_type : Type{};
        if (bt.kind == PrimitiveKind::STRUCT) {
            const auto &layouts = tc_.struct_layouts();
            auto it_l = layouts.find(bt.struct_name);
            if (it_l != layouts.end()) {
                for (const auto &f : it_l->second.fields) {
                    if (f.name == fa->field_name && f.bit_width > 0) {
                        // 1. LOAD storage word completo.
                        ir::IrValueId v_old = fn_->new_value(ft);
                        {
                            ir::IrInstr ld{};
                            ld.op = ir::IrOp::LOAD;
                            ld.type = ft;
                            ld.dst = v_old;
                            ld.operands = {addr};
                            ld.source_line = e->loc.line;
                            emit(current_block_, std::move(ld));
                        }
                        // 2. mask = (1 << bit_width) - 1 (en el tipo
                        //    del storage; truncar a tamano del LOAD).
                        const uint64_t mask =
                            (f.bit_width == 64)
                                ? UINT64_MAX
                                : ((uint64_t(1) << f.bit_width) - 1);
                        const uint64_t inv_mask = ~(mask << f.bit_offset);
                        // 3. cleared = old & inv_mask
                        ir::IrValueId v_inv =
                            emit_const(ft, inv_mask, e->loc.line);
                        ir::IrValueId v_clr = fn_->new_value(ft);
                        {
                            ir::IrInstr an{};
                            an.op = ir::IrOp::AND;
                            an.type = ft;
                            an.dst = v_clr;
                            an.operands = {v_old, v_inv};
                            an.source_line = e->loc.line;
                            emit(current_block_, std::move(an));
                        }
                        // 4. trimmed = rhs & mask  (clamp a rango).
                        ir::IrValueId v_msk = emit_const(ft, mask, e->loc.line);
                        ir::IrValueId v_tr = fn_->new_value(ft);
                        {
                            ir::IrInstr an{};
                            an.op = ir::IrOp::AND;
                            an.type = ft;
                            an.dst = v_tr;
                            an.operands = {rhs, v_msk};
                            an.source_line = e->loc.line;
                            emit(current_block_, std::move(an));
                        }
                        // 5. shifted = trimmed << bit_offset
                        ir::IrValueId v_sh = v_tr;
                        if (f.bit_offset > 0) {
                            ir::IrValueId v_amt = emit_const(
                                ft, (uint64_t)f.bit_offset, e->loc.line);
                            v_sh = fn_->new_value(ft);
                            ir::IrInstr sh{};
                            sh.op = ir::IrOp::SHL;
                            sh.type = ft;
                            sh.dst = v_sh;
                            sh.operands = {v_tr, v_amt};
                            sh.source_line = e->loc.line;
                            emit(current_block_, std::move(sh));
                        }
                        // 6. new = cleared | shifted
                        ir::IrValueId v_new = fn_->new_value(ft);
                        {
                            ir::IrInstr or_{};
                            or_.op = ir::IrOp::OR;
                            or_.type = ft;
                            or_.dst = v_new;
                            or_.operands = {v_clr, v_sh};
                            or_.source_line = e->loc.line;
                            emit(current_block_, std::move(or_));
                        }
                        // 7. STORE new -> addr
                        ir::IrInstr st{};
                        st.op = ir::IrOp::STORE;
                        st.type = ft;
                        st.dst = ir::IR_NO_VALUE;
                        st.operands = {v_new, addr};
                        st.source_line = e->loc.line;
                        emit(current_block_, std::move(st));
                        return rhs;
                    }
                }
            }
        }
        // Campo de tipo STRUCT (value-type): @c rhs es la DIRECCION del struct
        // origen -> copia memberwise (qword-by-qword) sus bytes al campo, NO un
        // STORE escalar (que guardaria la direccion origen).  Si el struct
        // declara `__clone__` (copy-hook), tras la copia aplica el efecto sobre
        // la copia del campo (p.ej. ++refcount).  Mismo modelo que el path
        // CLASS (lower_class_field_store).
        //
        // EXCEPCION: si el campo es de tipo `@overlay struct`, el slot guarda
        // el HANDLE de la vista (8 bytes), no un agregado embebido -> STORE
        // directo del puntero (cae al camino generico de abajo).  Sin esto la
        // copia memberwise volcaba el PAYLOAD apuntado por el handle en el
        // campo.
        if (fa->result_type.kind == PrimitiveKind::STRUCT &&
            !type_is_overlay(fa->result_type)) {
            uint64_t sz = 8;
            auto it_sl = tc_.struct_layouts().find(fa->result_type.struct_name);
            if (it_sl != tc_.struct_layouts().end())
                sz = static_cast<uint64_t>(it_sl->second.size_bytes);
            const bool dst_host = fn_->values[addr].is_host_ptr;
            const bool src_host = fn_->values[rhs].is_host_ptr;
            const uint64_t qwords = (sz + 7) / 8;
            for (uint64_t qi = 0; qi < qwords; ++qi) {
                const ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, static_cast<int64_t>(qi * 8), e->loc.line);
                const ir::IrValueId s_at = fn_->new_value(ir::IrType::PTR);
                fn_->values[s_at].is_host_ptr = src_host;
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = s_at;
                    ad.operands = {rhs, v_off};
                    ad.source_line = e->loc.line;
                    emit(current_block_, std::move(ad));
                }
                const ir::IrValueId w = fn_->new_value(ir::IrType::I64);
                {
                    ir::IrInstr ld{};
                    ld.op = ir::IrOp::LOAD;
                    ld.type = ir::IrType::I64;
                    ld.dst = w;
                    ld.operands = {s_at};
                    ld.source_line = e->loc.line;
                    emit(current_block_, std::move(ld));
                }
                const ir::IrValueId d_at = fn_->new_value(ir::IrType::PTR);
                fn_->values[d_at].is_host_ptr = dst_host;
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = d_at;
                    ad.operands = {addr, v_off};
                    ad.source_line = e->loc.line;
                    emit(current_block_, std::move(ad));
                }
                {
                    ir::IrInstr st{};
                    st.op = ir::IrOp::STORE;
                    st.type = ir::IrType::I64;
                    st.operands = {w, d_at};
                    st.source_line = e->loc.line;
                    emit(current_block_, std::move(st));
                }
            }
            if (it_sl != tc_.struct_layouts().end() &&
                it_sl->second.has_copy_hook) {
                emit_struct_method_on_host_field(
                    addr, fa->result_type.struct_name,
                    fa->result_type.struct_name + "__" + "__clone__",
                    e->loc.line);
            }
            return rhs;
        }
        // Campo shared<T> (H5) en contenedor struct: igual que en clase --
        // dec del shared anterior del campo (free-when-0), LOAD ctrl desde el
        // slot origen (rhs), STORE ctrl al campo, inc del refcount.
        if (fa->result_type.kind == PrimitiveKind::SHARED_PTR) {
            emit_shared_refcount_dec(addr, e->loc.line);
            const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ctrl].is_host_ptr = true;
            {
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = ir::IrType::I64;
                ld.dst = v_ctrl;
                ld.operands = {rhs};
                ld.source_line = e->loc.line;
                emit(current_block_, std::move(ld));
            }
            {
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {v_ctrl, addr};
                st.source_line = e->loc.line;
                emit(current_block_, std::move(st));
            }
            emit_shared_refcount_inc(addr, e->loc.line);
            return rhs;
        }
        // Campo normal: STORE directo.
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ft;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {rhs, addr}; // STORE: operands[0]=val, operands[1]=ptr
        st.source_line = e->loc.line;
        emit(current_block_, std::move(st));
        return rhs;
    }
    // Caso IndexExpr: 'p[i] = v' equivale a *(p + i*sizeof(*p)) = v.
    // Reusamos lower_index_addr para calcular el puntero del elemento.
    if (e->target->kind == ast::NodeKind::IndexExpr) {
        auto *ix = static_cast<ast::IndexExpr *>(e->target.get());
        // Operator overloading (escritura): el type checker marco
        // @c ix->index_set_method cuando @c base es CLASS o STRUCT con
        // @c __index_set__(index, value).  Construimos un CallExpr
        // sintetico @c `base.__index_set__(index, value)` y delegamos en
        // la maquinaria de metodos (CALLVIRT para CLASS, CALL para
        // STRUCT).  Robamos los hijos del AST y los restauramos despues.
        if (!ix->index_set_method.empty() && ix->base && ix->index &&
            e->value && e->op == ast::AssignOp::Assign) {
            const bool recv_is_struct =
                (ix->base->result_type.kind == PrimitiveKind::STRUCT);
            ast::CallExpr synth;
            synth.loc = e->loc;
            auto fa = std::make_unique<ast::FieldAccessExpr>();
            fa->loc = e->loc;
            fa->field_name = ix->index_set_method;
            fa->base = std::move(ix->base); // receptor (CLASS o STRUCT)
            synth.callee = std::move(fa);
            synth.args.push_back(std::move(ix->index)); // arg 0: indice
            synth.args.push_back(std::move(e->value));  // arg 1: valor
            ir::IrValueId v_call = recv_is_struct
                                       ? lower_struct_method_call(&synth)
                                       : lower_class_method_call(&synth);
            // Restaurar los hijos a sus nodos originales.
            auto *fa_back =
                static_cast<ast::FieldAccessExpr *>(synth.callee.get());
            ix->base = std::move(fa_back->base);
            ix->index = std::move(synth.args[0]);
            e->value = std::move(synth.args[1]);
            return v_call;
        }
        // String Inc (native_poo_): `s[i] = c` muta el byte i del
        // value-string in-place.  Calculamos data_ptr via el accesor
        // flag-aware (SSO vs HEAP, ya cubre ambos layouts) + STORE u8
        // del char (truncado a byte) en [data + i].  Sin bounds-check
        // (C-style, coherente con `s[i]` lectura).  No altera len.
        if (ix->base && ix->base->result_type.kind == PrimitiveKind::STRING &&
            !ix->is_range) {
            if (!native_poo_) {
                error_at(e->loc,
                         "escritura indexada de string (s[i]=c) solo "
                         "soportada en compilacion nativa (AOT Embed/Bare) "
                         "por ahora");
                return ir::IR_NO_VALUE;
            }
            if (!ix->index) {
                error_at(e->loc, "escritura indexada de string sin indice");
                return ir::IR_NO_VALUE;
            }
            const ir::IrValueId v_src = lower_expr(ix->base.get());
            if (v_src == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            ir::IrValueId v_idx = lower_expr(ix->index.get());
            if (v_idx == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            v_idx = cast_if_needed(v_idx, fn_->values[v_idx].type,
                                   ir::IrType::I64, e->loc.line);
            ir::IrValueId v_val = lower_expr(e->value.get());
            if (v_val == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            // Compound `s[i] += c`: leer el byte actual, aplicar el op.
            if (e->op != ast::AssignOp::Assign) {
                const ir::IrValueId v_old =
                    build_native_string_index_char(v_src, v_idx, e->loc.line);
                if (v_old == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                const ast::BinOp bop = compound_assign_op_to_binop(e->op);
                v_val =
                    emit_binop_ir(bop, v_old, v_val, PrimitiveKind::U8, e->loc);
            }
            // data_ptr (flag-aware) + i -> direccion del byte.
            ir::IrValueId v_ptr = emit_native_str_data_ptr(v_src, e->loc.line);
            ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_addr].is_host_ptr = true;
            {
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_addr;
                ad.operands = {v_ptr, v_idx};
                ad.source_line = e->loc.line;
                emit(current_block_, std::move(ad));
            }
            // STORE u8: el char rhs se guarda truncado a 1 byte.
            v_val = cast_if_needed(v_val, fn_->values[v_val].type,
                                   ir::IrType::U8, e->loc.line);
            {
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::U8;
                st.dst = ir::IR_NO_VALUE;
                st.operands = {v_val, v_addr};
                st.source_line = e->loc.line;
                emit(current_block_, std::move(st));
            }
            return v_val;
        }
        const ir::IrValueId addr = lower_index_addr(ix);
        if (addr == ir::IR_NO_VALUE) {
            (void)lower_expr(e->value.get());
            return ir::IR_NO_VALUE;
        }
        // Caso struct-value en slot de array: `arr[i] = struct_expr`
        // necesita memcpy de sizeof(Struct) bytes desde el RHS PTR al
        // slot (igual que `*ptr = struct_value`).  Sin esto, solo se
        // copia el primer qword.
        // Array de HANDLES overlay: el RHS ya ES el puntero de la vista (8
        // bytes), no la direccion de un payload que copiar.  Excluirlo del
        // memcpy: cae al STORE generico de abajo (pt = PTR) que guarda el
        // puntero tal cual.  Sin esto, `hs[i] = Foo(p)` emitia LOAD [p] +
        // STORE -> guardaba el CONTENIDO apuntado en vez del puntero.
        // `v.arr[i] = ...` (@c is_overlay_array, elemento INLINE en la vista)
        // conserva la copia de bytes: ahi no hay puntero que guardar.
        if ((ix->result_type.kind == PrimitiveKind::STRUCT ||
             ix->result_type.kind == PrimitiveKind::ARRAY) &&
            (ix->is_overlay_array || !type_is_overlay(ix->result_type)) &&
            e->op == ast::AssignOp::Assign) {
            uint64_t struct_size = 0;
            if (ix->result_type.kind == PrimitiveKind::STRUCT) {
                const auto &layouts = tc_.struct_layouts();
                auto it = layouts.find(ix->result_type.struct_name);
                if (it != layouts.end()) {
                    struct_size = static_cast<uint64_t>(it->second.size_bytes);
                }
                if (struct_size == 0) {
                    const auto &elays = tc_.enum_layouts();
                    auto ite = elays.find(ix->result_type.struct_name);
                    if (ite != elays.end()) {
                        struct_size =
                            static_cast<uint64_t>(ite->second.size_bytes);
                    }
                }
            }
            if (struct_size > 0 && (struct_size % 8) == 0) {
                const ir::IrValueId src = lower_expr(e->value.get());
                if (src == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                const bool src_host = fn_->values[src].is_host_ptr;
                const bool dst_host = fn_->values[addr].is_host_ptr;
                const uint64_t qwords = struct_size / 8;
                for (uint64_t q = 0; q < qwords; ++q) {
                    ir::IrValueId off_src = src;
                    ir::IrValueId off_dst = addr;
                    if (q > 0) {
                        const uint64_t byte_off = q * 8;
                        ir::IrValueId v_off =
                            emit_const(ir::IrType::I64, byte_off, e->loc.line);
                        {
                            ir::IrValueId v_new =
                                fn_->new_value(ir::IrType::PTR);
                            if (src_host) fn_->values[v_new].is_host_ptr = true;
                            ir::IrInstr ad{};
                            ad.op = ir::IrOp::ADD;
                            ad.type = ir::IrType::I64;
                            ad.dst = v_new;
                            ad.operands = {src, v_off};
                            ad.source_line = e->loc.line;
                            emit(current_block_, std::move(ad));
                            off_src = v_new;
                        }
                        {
                            ir::IrValueId v_new =
                                fn_->new_value(ir::IrType::PTR);
                            if (dst_host) fn_->values[v_new].is_host_ptr = true;
                            ir::IrInstr ad{};
                            ad.op = ir::IrOp::ADD;
                            ad.type = ir::IrType::I64;
                            ad.dst = v_new;
                            ad.operands = {addr, v_off};
                            ad.source_line = e->loc.line;
                            emit(current_block_, std::move(ad));
                            off_dst = v_new;
                        }
                    }
                    ir::IrValueId v_qw = fn_->new_value(ir::IrType::I64);
                    {
                        ir::IrInstr ld{};
                        ld.op = ir::IrOp::LOAD;
                        ld.type = ir::IrType::I64;
                        ld.dst = v_qw;
                        ld.operands = {off_src};
                        ld.source_line = e->loc.line;
                        emit(current_block_, std::move(ld));
                    }
                    {
                        ir::IrInstr st{};
                        st.op = ir::IrOp::STORE;
                        st.type = ir::IrType::I64;
                        st.dst = ir::IR_NO_VALUE;
                        st.operands = {v_qw, off_dst};
                        st.source_line = e->loc.line;
                        emit(current_block_, std::move(st));
                    }
                }
                return addr;
            }
        }
        // Bug fix 2026-05-23 (Audit 44): auto-promotion de string literal
        // a StringObject cuando el slot del array es de tipo string.
        // Sin esto, `arr[i] = "lit"` almacenaba la direccion raw del
        // literal y `str_length(arr[i])` daba 0 / garbage.
        ir::IrValueId rhs;
        if (ix->result_type.kind == PrimitiveKind::STRING && e->value &&
            e->value->kind == ast::NodeKind::StringLitExpr) {
            auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
            rhs = lower_string_literal_to_string_object(slit);
        } else {
            rhs = lower_expr(e->value.get());
        }
        if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrType pt = ir_type_from_primitive(ix->result_type.kind);
        // Compound assign: LOAD elemento, op, STORE de vuelta a la
        // misma direccion (calculada una sola vez).  Cubre +=, -= y
        // todos los compound enteros/float sobre arrays e indexados.
        if (e->op != ast::AssignOp::Assign) {
            ir::IrValueId v_old = fn_->new_value(pt);
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = pt;
            ld.dst = v_old;
            ld.operands = {addr};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
            const ast::BinOp bop = compound_assign_op_to_binop(e->op);
            rhs = emit_binop_ir(bop, v_old, rhs, ix->result_type.kind, e->loc);
        }
        rhs = cast_if_needed(rhs, fn_->values[rhs].type, pt,
                             e->value ? e->value->loc : e->loc);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = pt;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {rhs, addr};
        st.source_line = e->loc.line;
        emit(current_block_, std::move(st));
        return rhs;
    }
    // Caso UnaryExpr(Deref, p): '*p = v' escribe a traves del puntero.
    if (e->target->kind == ast::NodeKind::UnaryExpr) {
        auto *un = static_cast<ast::UnaryExpr *>(e->target.get());
        if (un->op == ast::UnOp::Deref) {
            // Bajar el puntero (operando del Deref) y el valor.
            const ir::IrValueId addr = lower_expr(un->operand.get());
            if (addr == ir::IR_NO_VALUE) {
                (void)lower_expr(e->value.get());
                return ir::IR_NO_VALUE;
            }
            // Caso struct-value assign `*ptr = struct_expr`: el lowering
            // generico emite un solo STORE de 8 bytes (ptr value), lo
            // que SOLO copia el primer qword del struct.  Para structs
            // reales necesitamos memcpy del payload completo.
            // El tipo que manda es el de lo que se ASIGNA.  Mirar solo el del
            // deref se queda corto en una instancia monomorfizada: alli el
            // parametro llega como puntero generico y el deref no dice STRUCT,
            // asi que `(*out) = valor` guardaba LA DIRECCION del struct en vez
            // de sus bytes -- una generica con `T` struct devolvia punteros
            // como si fueran valores.  Si cualquiera de los dos lados es un
            // agregado, se copia.
            const Type &deref_t = un->result_type;
            const Type &value_t = e->value->result_type;
            const bool deref_agg = (deref_t.kind == PrimitiveKind::STRUCT ||
                                    deref_t.kind == PrimitiveKind::ARRAY);
            const bool value_agg = (value_t.kind == PrimitiveKind::STRUCT ||
                                    value_t.kind == PrimitiveKind::ARRAY);
            if ((deref_agg || value_agg) && e->op == ast::AssignOp::Assign) {
                // Calcular sizeof.  STRUCT: lookup en struct_layouts_;
                // ARRAY: type.array_size * sizeof(elt) si conocido.
                uint64_t struct_size = 0;
                const Type &agg_t = deref_agg ? deref_t : value_t;
                if (agg_t.kind == PrimitiveKind::STRUCT) {
                    const auto &layouts = tc_.struct_layouts();
                    auto it = layouts.find(agg_t.struct_name);
                    if (it != layouts.end()) {
                        struct_size =
                            static_cast<uint64_t>(it->second.size_bytes);
                    }
                    // Tambien enum (encoded como STRUCT con struct_name).
                    if (struct_size == 0) {
                        const auto &elays = tc_.enum_layouts();
                        auto ite = elays.find(agg_t.struct_name);
                        if (ite != elays.end()) {
                            struct_size =
                                static_cast<uint64_t>(ite->second.size_bytes);
                        }
                    }
                }
                if (struct_size > 0 && (struct_size % 8) == 0) {
                    // Bajar RHS para obtener el PTR fuente.
                    const ir::IrValueId src = lower_expr(e->value.get());
                    if (src == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                    // Copia qword a qword.  Para size_bytes no multiplo
                    // de 8 usariamos byte-loops; los structs Vesta tienen
                    // padding a 8-bytes por field-alignment, asi que
                    // size_bytes siempre es multiplo de 8 para Vesta
                    // structs.  Defensa por bytes <8: fall-through.
                    // Propagamos is_host_ptr de src/addr a los LOAD/STORE
                    // para emitir movh cuando corresponda.
                    const bool src_host = fn_->values[src].is_host_ptr;
                    const bool dst_host = fn_->values[addr].is_host_ptr;
                    const uint64_t qwords = struct_size / 8;
                    for (uint64_t q = 0; q < qwords; ++q) {
                        // src + q*8
                        ir::IrValueId off_src = src;
                        ir::IrValueId off_dst = addr;
                        if (q > 0) {
                            const uint64_t byte_off = q * 8;
                            ir::IrValueId v_off = emit_const(
                                ir::IrType::I64, byte_off, e->loc.line);
                            // src + off
                            {
                                ir::IrValueId v_new =
                                    fn_->new_value(ir::IrType::PTR);
                                if (src_host)
                                    fn_->values[v_new].is_host_ptr = true;
                                ir::IrInstr ad{};
                                ad.op = ir::IrOp::ADD;
                                ad.type = ir::IrType::I64;
                                ad.dst = v_new;
                                ad.operands = {src, v_off};
                                ad.source_line = e->loc.line;
                                emit(current_block_, std::move(ad));
                                off_src = v_new;
                            }
                            {
                                ir::IrValueId v_new =
                                    fn_->new_value(ir::IrType::PTR);
                                if (dst_host)
                                    fn_->values[v_new].is_host_ptr = true;
                                ir::IrInstr ad{};
                                ad.op = ir::IrOp::ADD;
                                ad.type = ir::IrType::I64;
                                ad.dst = v_new;
                                ad.operands = {addr, v_off};
                                ad.source_line = e->loc.line;
                                emit(current_block_, std::move(ad));
                                off_dst = v_new;
                            }
                        }
                        // LOAD i64 del src + q*8
                        ir::IrValueId v_qw = fn_->new_value(ir::IrType::I64);
                        {
                            ir::IrInstr ld{};
                            ld.op = ir::IrOp::LOAD;
                            ld.type = ir::IrType::I64;
                            ld.dst = v_qw;
                            ld.operands = {off_src};
                            ld.source_line = e->loc.line;
                            emit(current_block_, std::move(ld));
                        }
                        // STORE al dst + q*8
                        {
                            ir::IrInstr st{};
                            st.op = ir::IrOp::STORE;
                            st.type = ir::IrType::I64;
                            st.dst = ir::IR_NO_VALUE;
                            st.operands = {v_qw, off_dst};
                            st.source_line = e->loc.line;
                            emit(current_block_, std::move(st));
                        }
                    }
                    return addr;
                }
                // Si no se pudo calcular el size, cae al path generico
                // (que solo copia 8 bytes -- bug documentado).
            }
            // Bug fix 2026-05-23 (Audit 45): auto-promotion para `*p = "lit"`
            // cuando p es string* (deref produce STRING).
            ir::IrValueId rhs;
            if (un->result_type.kind == PrimitiveKind::STRING && e->value &&
                e->value->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
                rhs = lower_string_literal_to_string_object(slit);
            } else {
                rhs = lower_expr(e->value.get());
            }
            if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

            const ir::IrType pt = ir_type_from_primitive(un->result_type.kind);
            // Compound assign sobre '*p': LOAD valor actual, op, STORE.
            if (e->op != ast::AssignOp::Assign) {
                ir::IrValueId v_old = fn_->new_value(pt);
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = pt;
                ld.dst = v_old;
                ld.operands = {addr};
                ld.source_line = e->loc.line;
                emit(current_block_, std::move(ld));
                const ast::BinOp bop = compound_assign_op_to_binop(e->op);
                rhs = emit_binop_ir(bop, v_old, rhs, un->result_type.kind,
                                    e->loc);
            }
            rhs = cast_if_needed(rhs, fn_->values[rhs].type, pt, e->loc.line);
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = pt;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {rhs, addr};
            st.source_line = e->loc.line;
            emit(current_block_, std::move(st));
            return rhs;
        }
    }
    if (e->target->kind != ast::NodeKind::IdentExpr) {
        error_at(e->loc, "lowering: el lado izquierdo de '=' debe ser un "
                         "identificador o un acceso a campo");
        (void)lower_expr(e->value.get());
        return ir::IR_NO_VALUE;
    }
    auto *id = static_cast<ast::IdentExpr *>(e->target.get());

    // Vesta Embed Inc 2: `s += t` / `s += 'c'` / `s += "lit"` en native_poo_.
    // El target `s` es un value-string (PTR al slot {ptr,len,cap}); el
    // append muta el slot in-place (grow del buffer si hace falta) sin
    // crear slot nuevo.  Soportamos RHS string (otra var/concat/literal) y
    // char.  El path Full (sin native_poo_) NO entra aqui: `string += x`
    // sobre StringObject cae al manejo generico de abajo (que para STRING
    // no es comun; el frontend Full usa STRCAT).
    if (native_poo_ && id->result_type.kind == PrimitiveKind::STRING &&
        e->op == ast::AssignOp::AddAssign && e->value) {
        const ir::IrValueId v_slot = lookup(id->name);
        if (v_slot == ir::IR_NO_VALUE) {
            error_at(e->loc, "lowering: nombre no resuelto en '+=': '" +
                                 id->name + "'");
            return ir::IR_NO_VALUE;
        }
        const uint32_t ln = static_cast<uint32_t>(e->loc.line);
        // Caso RHS char: append de 1 byte.
        if (e->value->result_type.kind == PrimitiveKind::CHAR) {
            const ir::IrValueId v_ch = lower_expr(e->value.get());
            if (v_ch == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            // Buffer scratch de 1 byte con el char.
            ir::IrValueId v_scr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA;
                al.type = ir::IrType::I8;
                al.dst = v_scr;
                al.imm = 1;
                al.host_alloca = native_poo_;
                al.source_line = ln;
                emit(current_block_, std::move(al));
            }
            if (native_poo_) fn_->values[v_scr].is_host_ptr = true;
            {
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::U8;
                st.dst = ir::IR_NO_VALUE;
                st.operands = {v_ch, v_scr};
                st.source_line = ln;
                emit(current_block_, std::move(st));
            }
            build_native_string_append_inplace(
                v_slot, v_scr, emit_const(ir::IrType::I64, 1, ln), ln);
            return v_slot;
        }
        // Caso RHS string (var, concat, literal): cargar ptr/len de la
        // fuente y appendear.  Para un literal lo materializamos via el
        // helper de literal native (slot temporal con buffer en heap,
        // liberado tras leer sus bytes -- pero como ALLOCA del slot y el
        // buffer del literal NO se registran STRING_FREE, hay que
        // liberarlo aqui para no leakear).  Para una expr string owned
        // (concat) liberamos su buffer tras copiarlo.
        ir::IrValueId v_src = ir::IR_NO_VALUE;
        bool free_src_buf = false; // liberar el buffer fuente tras copiar
        if (e->value->kind == ast::NodeKind::StringLitExpr &&
            !static_cast<ast::StringLitExpr *>(e->value.get())
                 ->is_interpolated()) {
            auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
            v_src = build_native_string_from_literal(slit, ln);
            free_src_buf = true; // buffer temporal owned -> liberar
        } else {
            v_src = lower_expr(e->value.get());
            // Un concat `a + b` produce un slot owned con buffer fresco;
            // tras copiar sus bytes hay que liberarlo (no se registro
            // STRING_FREE porque no es un var-decl).  Una var simple
            // (IdentExpr) NO se libera (su buffer lo posee la var).
            if (e->value->kind != ast::NodeKind::IdentExpr) free_src_buf = true;
        }
        if (v_src == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        // Inc 5 (SSO): (ptr, len) de la fuente via accesores flag-aware.
        // Calcularlos ANTES del append (que muta current_block_ con su
        // branch SSO/HEAP).
        ir::IrValueId v_sptr = emit_native_str_data_ptr(v_src, ln);
        ir::IrValueId v_slen = emit_native_str_len(v_src, ln);
        build_native_string_append_inplace(v_slot, v_sptr, v_slen, ln);
        if (free_src_buf) {
            // Liberar el buffer fuente temporal SOLO si estaba en HEAP
            // (SSO no tiene buffer que liberar).  El append ya copio los
            // bytes a un buffer/inline propio del destino.
            emit_native_str_free_if_heap(v_src, ln);
        }
        return v_slot;
    }

    // Bajar el lado derecho.
    // Bug fix 2026-05-23 (Audit 48): auto-promotion del string literal a
    // StringObject cuando el target es una var local de tipo string.  Sin
    // esto, `s = "lit"` (post var-decl) almacenaba el host_ptr al literal
    // como GcHandle invalido.
    ir::IrValueId rhs;
    if (id->result_type.kind == PrimitiveKind::STRING && e->value &&
        e->value->kind == ast::NodeKind::StringLitExpr) {
        auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
        rhs = lower_string_literal_to_string_object(slit);
    } else {
        rhs = lower_expr(e->value.get());
    }
    if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

    /* Value-type ENUM: `t = <enum>` COPIA los bytes del enum al slot ESTABLE
     * de `t` (no repunta el puntero).  Igual modelo que un struct; sin esto,
     * una asignacion (condicional o no) repuntaria `t` al slot del constructor
     * (naturaleza VM/host distinta al slot del var-decl) y un PHI mezclaria
     * punteros de naturaleza mixta -> el `match t` posterior leeria el tag con
     * el load de la naturaleza equivocada (movh sobre direccion VM) -> SIGSEGV.
     */
    if (e->op == ast::AssignOp::Assign &&
        id->result_type.kind == PrimitiveKind::STRUCT) {
        const auto &elays_e = tc_.enum_layouts();
        auto ite_e = elays_e.find(id->result_type.struct_name);
        if (ite_e != elays_e.end()) {
            const ir::IrValueId slot = lookup(id->name);
            if (slot != ir::IR_NO_VALUE) {
                emit_enum_copy(slot, rhs, fn_->values[rhs].is_host_ptr,
                               ite_e->second.size_bytes, e->loc.line);
                return slot;
            }
        }
    }

    /* L2.2: target es global runtime con storage en static_data.
     * Emit STORE al slot.  Soporta `=` directo y compound assigns
     * via load-modify-store. */
    {
        auto rit = runtime_global_slots_.find(id->name);
        if (rit != runtime_global_slots_.end()) {
            const uint64_t slot_idx = rit->second;
            const int ln = e->loc.line;
            ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr is{};
                is.op = ir::IrOp::STR_LIT_ADDR;
                is.type = ir::IrType::PTR;
                is.dst = v_addr;
                is.imm = slot_idx;
                is.source_line = ln;
                emit(current_block_, std::move(is));
                // El slot vive en memoria host (seccion `gdata`) -> el
                // load-modify-store de abajo es acceso host directo.
                fn_->values[v_addr].is_host_ptr = true;
            }
            // Tipo declarado del global.  El compound assign tiene que operar
            // con EL del global, no con i64: sobre un `f64 g`, un `g += x` con
            // aritmetica entera sumaria los BITS IEEE (basura: 1.5+1.5+1.5 daba
            // -0.75).  `g = g + x` no sufria porque va por el camino normal,
            // con el tipo real.  Para STRING el valor es el GcHandle -> i64.
            PrimitiveKind gprim = PrimitiveKind::I64;
            for (auto &decl : mod_.decls) {
                if (!decl || decl->kind != ast::NodeKind::GlobalVarDecl)
                    continue;
                auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
                if (gv->name != id->name) continue;
                if (gv->type &&
                    gv->type->kind == ast::NodeKind::PrimitiveTypeNode)
                    gprim =
                        static_cast<ast::PrimitiveTypeNode *>(gv->type.get())
                            ->prim;
                break;
            }
            const bool gfloat =
                (gprim == PrimitiveKind::F32 || gprim == PrimitiveKind::F64);
            const ir::IrType gty =
                gfloat ? ir_type_from_primitive(gprim) : ir::IrType::I64;
            // El valor a guardar tiene que llegar en el ancho del global: los
            // literales float se parsean como double, asi que `f32 g = 1.25`
            // trae un f64 y guardarlo como F32 sin convertir escribe basura.
            if (gfloat && e->value) {
                const ir::IrType from =
                    (e->value->result_type.kind == PrimitiveKind::F32)
                        ? ir::IrType::F32
                        : ir::IrType::F64;
                rhs = cast_if_needed(rhs, from, gty, e->loc);
            }

            // Compound assign: load cur + combine.
            if (e->op != ast::AssignOp::Assign) {
                ir::IrValueId v_cur = fn_->new_value(gty);
                {
                    ir::IrInstr ld{};
                    ld.op = ir::IrOp::LOAD;
                    ld.type = gty;
                    ld.dst = v_cur;
                    ld.operands = {v_addr};
                    ld.source_line = ln;
                    emit(current_block_, std::move(ld));
                }
                const ast::BinOp bop = compound_assign_op_to_binop(e->op);
                rhs =
                    emit_binop_ir(bop, v_cur, rhs,
                                  gfloat ? gprim : PrimitiveKind::I64, e->loc);
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = gty;
            st.operands = {rhs, v_addr};
            st.source_line = ln;
            emit(current_block_, std::move(st));
            return rhs;
        }
    }

    /*  MC.17.2: si estamos dentro de un @Macro Y el target es
     * un comptime global int, emit STORE al slot @c static_data
     * correspondiente.  Soporta `=` directo y compound `+=`/`-=`
     * (el caller computa cur op rhs en `rhs` antes de llegar aqui). */
    if (current_fn_is_macro_) {
        auto cit = tc_.comptime_const_values().find(id->name);
        if (cit != tc_.comptime_const_values().end() && !cit->second.is_str) {
            const uint64_t slot_idx =
                get_or_create_comptime_global_slot(id->name);
            if (slot_idx != UINT64_MAX) {
                const int ln = e->loc.line;
                /* Si compound assign, leer valor actual y combinar
                 * con rhs ANTES del store.  Esto es paralelo al
                 * camino general que sigue mas abajo, pero como
                 * salimos antes de llegar a ese punto, lo
                 * replicamos aqui inline para compound. */
                if (e->op != ast::AssignOp::Assign) {
                    /* Compound assign sobre global: load cur from
                     * slot + combine + store back. */
                    ir::IrValueId v_addr_load = fn_->new_value(ir::IrType::PTR);
                    {
                        ir::IrInstr is{};
                        is.op = ir::IrOp::STR_LIT_ADDR;
                        is.type = ir::IrType::PTR;
                        is.dst = v_addr_load;
                        is.imm = slot_idx;
                        is.source_line = ln;
                        emit(current_block_, std::move(is));
                    }
                    ir::IrValueId v_cur = fn_->new_value(ir::IrType::I64);
                    {
                        ir::IrInstr ld{};
                        ld.op = ir::IrOp::LOAD;
                        ld.type = ir::IrType::I64;
                        ld.dst = v_cur;
                        ld.operands = {v_addr_load};
                        ld.source_line = ln;
                        emit(current_block_, std::move(ld));
                    }
                    /* Combine via emit_binop equivalent.  Mapeamos
                     * AssignOp -> BinOp y emitimos.  Para simplicidad
                     * solo cubrimos los compound mas comunes; otros
                     * caen al camino general (que falla porque
                     * write_local no encontrara el name). */
                    ast::BinOp bop = ast::BinOp::Add;
                    bool supported = true;
                    switch (e->op) {
                    case ast::AssignOp::AddAssign: bop = ast::BinOp::Add; break;
                    case ast::AssignOp::SubAssign: bop = ast::BinOp::Sub; break;
                    case ast::AssignOp::MulAssign: bop = ast::BinOp::Mul; break;
                    case ast::AssignOp::DivAssign: bop = ast::BinOp::Div; break;
                    case ast::AssignOp::ModAssign: bop = ast::BinOp::Mod; break;
                    case ast::AssignOp::BitAndAssign:
                        bop = ast::BinOp::BitAnd;
                        break;
                    case ast::AssignOp::BitOrAssign:
                        bop = ast::BinOp::BitOr;
                        break;
                    case ast::AssignOp::BitXorAssign:
                        bop = ast::BinOp::BitXor;
                        break;
                    case ast::AssignOp::ShlAssign: bop = ast::BinOp::Shl; break;
                    case ast::AssignOp::ShrAssign: bop = ast::BinOp::Shr; break;
                    default: supported = false; break;
                    }
                    if (supported) {
                        /* Use emit_binop_ir (mismo helper que el
                         * camino normal de compound assign).  Common
                         * = I64 (los globals son int de 64-bit). */
                        rhs = emit_binop_ir(bop, v_cur, rhs, PrimitiveKind::I64,
                                            e->loc);
                    }
                }
                /* STORE rhs al slot. */
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                {
                    ir::IrInstr is{};
                    is.op = ir::IrOp::STR_LIT_ADDR;
                    is.type = ir::IrType::PTR;
                    is.dst = v_addr;
                    is.imm = slot_idx;
                    is.source_line = ln;
                    emit(current_block_, std::move(is));
                }
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {rhs, v_addr};
                st.source_line = ln;
                emit(current_block_, std::move(st));
                return rhs;
            }
        }
    }

    // Tipo destino: el del simbolo en el scope (o el result_type del
    // target que el type checker dejo).
    const ir::IrType dst_ir =
        ir_type_from_primitive(e->target->result_type.kind);

    // Para asignaciones compuestas (+=, -=, etc.) cargamos el valor
    // actual y combinamos.  El operador ASCII '=' simplemente se
    // ignora aqui y va directo al write_local con rhs.
    if (e->op != ast::AssignOp::Assign) {
        // Lectura previa respeta promocion address-taken.
        const ir::IrValueId cur = read_local(id->name, dst_ir, e->loc.line);
        if (cur == ir::IR_NO_VALUE) {
            error_at(e->loc,
                     "lowering: nombre no resuelto: '" + id->name + "'");
            return ir::IR_NO_VALUE;
        }
        /* P1: `string += X` en el path Full/VM (no native_poo_).  El path arith
         * generico de abajo (promote_arith + emit_binop) NO hace STRCAT sobre
         * StringObject -> daba string vacio.  Emitimos STRCAT como `s = s + X`.
         * (Antes funcionaba solo porque la comptime fn se AST-evaluaba; con el
         * rewrite corre en la VM y necesita el lowering correcto.) */
        if (!native_poo_ &&
            e->target->result_type.kind == PrimitiveKind::STRING &&
            e->op == ast::AssignOp::AddAssign && e->value) {
            /* El lado derecho YA esta bajado arriba, con la misma coercion que
             * hace falta aqui: un literal se promueve a StringObject (STRMAKE)
             * en vez de quedarse como STR_LIT_ADDR crudo, que es lo que STRCAT
             * espera, y vale igual para el literal de una pieza y para el
             * interpolado (la cadena de trozos la arma el mismo constructor).
             *
             * Volver a bajarlo aqui -- que es lo que se hacia -- emitia la
             * expresion DOS veces y tiraba la primera.  Con un literal sale
             * gratis, pero con `s += "${x}"` la conversion es una llamada
             * nativa que escribe en un buffer de pila: dos reservas y dos
             * llamadas por interpolacion, una de ellas muerta y que ningun
             * DCE puede quitar porque la llamada tiene efectos. */
            const ir::IrValueId v_cat =
                emit_strcat(cur, rhs, static_cast<uint32_t>(e->loc.line));
            write_local(id->name, v_cat, dst_ir, e->loc.line);
            return v_cat;
        }
        // Promocion al tipo comun entre cur y rhs (igual que en
        // lower_binary).  En la mayoria de casos ambos tienen el
        // tipo de la variable; el cast es trivial.
        const PrimitiveKind ltk = e->target->result_type.kind;
        const PrimitiveKind rtk = e->value->result_type.kind;
        const PrimitiveKind common =
            (ltk == PrimitiveKind::BOOL && rtk == PrimitiveKind::BOOL)
                ? PrimitiveKind::BOOL
                : promote_arith(ltk, rtk);
        const ir::IrType common_ir = ir_type_from_primitive(common);

        ir::IrValueId l = cast_if_needed(cur, ir_type_from_primitive(ltk),
                                         common_ir, e->loc.line);
        ir::IrValueId r = cast_if_needed(rhs, ir_type_from_primitive(rtk),
                                         common_ir, e->loc.line);

        // Mapear AssignOp a su BinOp equivalente.
        ast::BinOp bop = ast::BinOp::Add;
        switch (e->op) {
        case ast::AssignOp::AddAssign: bop = ast::BinOp::Add; break;
        case ast::AssignOp::SubAssign: bop = ast::BinOp::Sub; break;
        case ast::AssignOp::MulAssign: bop = ast::BinOp::Mul; break;
        case ast::AssignOp::DivAssign: bop = ast::BinOp::Div; break;
        case ast::AssignOp::ModAssign: bop = ast::BinOp::Mod; break;
        case ast::AssignOp::BitAndAssign: bop = ast::BinOp::BitAnd; break;
        case ast::AssignOp::BitOrAssign: bop = ast::BinOp::BitOr; break;
        case ast::AssignOp::BitXorAssign: bop = ast::BinOp::BitXor; break;
        case ast::AssignOp::ShlAssign: bop = ast::BinOp::Shl; break;
        case ast::AssignOp::ShrAssign: bop = ast::BinOp::Shr; break;
        case ast::AssignOp::Assign: break; // ya filtrado arriba
        }
        rhs = emit_binop_ir(bop, l, r, common, e->loc);
    }

    // Self-assign via metodo: `x = x.metodo(...)` (el receptor del metodo ES el
    // target).  El metodo SRET escribe su retbuf y luego se rebindearia x a ese
    // retbuf; pero si esto esta en un LOOP, el ALLOCA del retbuf se hoista al
    // prologo (un solo buffer) y en la 2a+ iteracion `this` (=x, ya rebindeado
    // al retbuf) y el retbuf ALIASAN -> el metodo lee y escribe el mismo buffer
    // = corrupcion (el JIT lo sufre; el interp re-aloca por iteracion y lo
    // enmascara). Tratarlo como el caso address-taken: COPIAR el retbuf al
    // buffer ESTABLE de x (sin rebind) -> `this` y el retbuf quedan SIEMPRE
    // distintos.
    bool is_self_method_assign = false;
    if (e->op == ast::AssignOp::Assign && e->value &&
        e->value->kind == ast::NodeKind::CallExpr) {
        auto *cv = static_cast<ast::CallExpr *>(e->value.get());
        if (cv->callee && cv->callee->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(cv->callee.get());
            if (fa->base && fa->base->kind == ast::NodeKind::IdentExpr &&
                static_cast<ast::IdentExpr *>(fa->base.get())->name == id->name)
                is_self_method_assign = true;
        }
    }
    // @Virtual/struct: si el target es un STRUCT ADDRESS-TAKEN (o un
    // self-assign via metodo, ver arriba), @c rhs es el PTR a un buffer origen
    // (el retbuf de un metodo SRET, u otro struct).  Hay que COPIAR sus bytes
    // al buffer del target, NO rebindear el slot al ptr origen: con `&x` tomado
    // el buffer del target es fijo, y write_local guardaria el PUNTERO en el
    // slot en vez del contenido.
    if (rhs != ir::IR_NO_VALUE && e->op == ast::AssignOp::Assign &&
        e->target->result_type.kind == PrimitiveKind::STRUCT &&
        (address_taken_locals_.count(id->name) || is_self_method_assign) &&
        !type_is_overlay(e->target->result_type)) {
        const std::string &sn = e->target->result_type.struct_name;
        auto it_sl = tc_.struct_layouts().find(sn);
        if (it_sl != tc_.struct_layouts().end()) {
            // Para un struct address-taken el ALLOCA ES el buffer; lookup() da
            // su direccion (read_local haria un LOAD, devolviendo el
            // contenido).
            const ir::IrValueId dst_addr = lookup(id->name);
            if (dst_addr != ir::IR_NO_VALUE && dst_addr != rhs) {
                const uint64_t sz =
                    static_cast<uint64_t>(it_sl->second.size_bytes);
                const bool dst_host = fn_->values[dst_addr].is_host_ptr;
                const bool src_host = fn_->values[rhs].is_host_ptr;
                const uint64_t qwords = (sz + 7) / 8;
                for (uint64_t qi = 0; qi < qwords; ++qi) {
                    const ir::IrValueId v_off =
                        emit_const(ir::IrType::I64,
                                   static_cast<int64_t>(qi * 8), e->loc.line);
                    const ir::IrValueId s_at = fn_->new_value(ir::IrType::PTR);
                    fn_->values[s_at].is_host_ptr = src_host;
                    {
                        ir::IrInstr ad{};
                        ad.op = ir::IrOp::ADD;
                        ad.type = ir::IrType::I64;
                        ad.dst = s_at;
                        ad.operands = {rhs, v_off};
                        ad.source_line = e->loc.line;
                        emit(current_block_, std::move(ad));
                    }
                    const ir::IrValueId w = fn_->new_value(ir::IrType::I64);
                    {
                        ir::IrInstr ld{};
                        ld.op = ir::IrOp::LOAD;
                        ld.type = ir::IrType::I64;
                        ld.dst = w;
                        ld.operands = {s_at};
                        ld.source_line = e->loc.line;
                        emit(current_block_, std::move(ld));
                    }
                    const ir::IrValueId d_at = fn_->new_value(ir::IrType::PTR);
                    fn_->values[d_at].is_host_ptr = dst_host;
                    {
                        ir::IrInstr ad{};
                        ad.op = ir::IrOp::ADD;
                        ad.type = ir::IrType::I64;
                        ad.dst = d_at;
                        ad.operands = {dst_addr, v_off};
                        ad.source_line = e->loc.line;
                        emit(current_block_, std::move(ad));
                    }
                    {
                        ir::IrInstr st{};
                        st.op = ir::IrOp::STORE;
                        st.type = ir::IrType::I64;
                        st.operands = {w, d_at};
                        st.source_line = e->loc.line;
                        emit(current_block_, std::move(st));
                    }
                }
                return dst_addr;
            }
        }
    }

    // Cast final al tipo declarado de la variable y actualizar el scope.
    const ir::IrType rhs_ir =
        (rhs != ir::IR_NO_VALUE) ? fn_->values[rhs].type : dst_ir;
    rhs = cast_if_needed(rhs, rhs_ir, dst_ir, e->loc.line);
    write_local(id->name, rhs, dst_ir, e->loc.line);
    return rhs;
}

// ---------------------------------------------------------------------
// Lowering de literales de string y builtins FFI.
// ---------------------------------------------------------------------

ir::IrValueId Lowering::lower_string_lit(ast::StringLitExpr *e) {
    if (!out_mod_) {
        error_at(e->loc, "lowering: out_mod_ nulo al bajar StringLitExpr");
        return ir::IR_NO_VALUE;
    }
    // Vesta Embed Inc 2: en native_poo_ un literal INTERPOLADO se baja a un
    // value-string {ptr,len,cap} construido inline (sin StringObject GC ni
    // STRMAKE/STRCAT).  Devuelve el PTR al slot owned; el caller registra
    // su STRING_FREE (var-decl caso (c) ya lo hace).  El path Full/JIT/
    // interp (sin native_poo_) NO entra aqui: cae al STR_LIT_ADDR / a la
    // promocion StringObject de los callers superiores.
    if (native_poo_ && e->is_interpolated()) {
        return build_native_string_interp(e);
    }
    // Convertir el contenido resuelto a vector<uint8_t> y registrarlo
    // (deduplicado) en static_data.  Los duplicados retornan el mismo
    // indice, ahorrando bytes en el .vel emitido.
    //
    // NUL terminator: el STR_LIT_ADDR se usa como `char*` (C string) -- p.ej.
    // pasado a una funcion que itera hasta el 0.  Sin el nul, dos literales
    // contiguos en .rodata se leen como uno solo (un `dputs("A")` seguia
    // hasta el siguiente literal).  En PE/exe "funcionaba" por relleno de
    // alineacion casual; en el .bin empaquetado no.  El +1 byte es inocuo
    // para los consumidores que usan longitud explicita (STRMAKE lee
    // value.size() bytes e ignora el nul).
    std::vector<uint8_t> bytes(e->value.begin(), e->value.end());
    bytes.push_back(0);
    const uint64_t idx = out_mod_->intern_static_data(std::move(bytes));

    // Emitir IrOp::STR_LIT_ADDR -> el emisor genera "mov rDst,
    // @Absolute(\"code.s_<idx>\")".
    const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::STR_LIT_ADDR;
    ins.type = ir::IrType::PTR;
    ins.dst = dst;
    ins.imm = idx;
    ins.source_line = e->loc.line;
    emit(current_block_, std::move(ins));
    return dst;
}

// Forward decls de los helpers definidos mas abajo en el TU.  Necesarias
// porque try_lower_builtin_call los invoca en la implementacion de
// forName/getClass

/* try_lower_builtin_call vive ahora en lowering/builtins.cpp: eran 7425 lineas * en este fichero, un diecisiete por ciento del total, y tienen tema propio. */

// ---------------------------------------------------------------------
// POO: clases en Vesta. la integracion completa
// (registro en module init, NEWOBJ + CALLVIRT, GETFIELD/SETFIELD)
// se implementa por fases.  Cada metodo nuevo emite un error claro
// hasta que su implementacion concreta este lista.
// ---------------------------------------------------------------------



// -----------------------------------------------------------------
// NS.6-ext: baja los metodos de extension / impl como funciones libres
// <clave_layout>__<metodo>.  Reusa la emision de lower_struct_methods via un
// StructDecl temporal cuyo `name` es la clave del layout destino (mangled si
// es importado).  El cuerpo ya lo tipo el checker (con current_struct_/
// current_class_ correcto), asi que la emision es agnostica del kind salvo el
// binding de `this` (parametrizado por ext_this_is_class_).
// -----------------------------------------------------------------
void Lowering::lower_extension_methods(ir::IrModule &out) {
    for (auto &decl : mod_.decls) {
        if (!decl) continue;
        const bool is_ext = decl->kind == ast::NodeKind::ExtensionDecl;
        const bool is_impl = decl->kind == ast::NodeKind::ImplDecl;
        if (!is_ext && !is_impl) continue;
        std::string target_src;
        std::vector<std::unique_ptr<ast::ClassMethodDecl>> *methods = nullptr;
        if (is_ext) {
            auto *e = static_cast<ast::ExtensionDecl *>(decl.get());
            target_src = e->target_type;
            methods = &e->methods;
        } else {
            auto *im = static_cast<ast::ImplDecl *>(decl.get());
            target_src = im->target_type;
            methods = &im->methods;
        }
        // Resolver la clave del layout destino (misma logica que el checker).
        std::string key;
        bool is_class = false;
        auto set_key = [&](const std::string &k) -> bool {
            if (tc_.struct_layouts().count(k)) {
                key = k;
                is_class = false;
                return true;
            }
            if (tc_.class_layouts().count(k)) {
                key = k;
                is_class = true;
                return true;
            }
            return false;
        };
        if (!set_key(target_src)) {
            std::string mangled = target_src;
            for (size_t p = mangled.find('.'); p != std::string::npos;
                 p = mangled.find('.'))
                mangled.replace(p, 1, "__");
            if (mangled == target_src || !set_key(mangled)) {
                const Type rt = tc_.resolve_type_string(target_src);
                if (rt.kind == PrimitiveKind::STRUCT ||
                    rt.kind == PrimitiveKind::CLASS)
                    set_key(rt.struct_name);
            }
        }
        if (key.empty()) continue;
        // StructDecl temporal: name = clave, methods = los de la extension
        // (movidos temporalmente y devueltos al terminar).
        ast::StructDecl tmp;
        tmp.name = key;
        tmp.methods = std::move(*methods);
        const bool saved = ext_this_is_class_;
        ext_this_is_class_ = is_class;
        lower_struct_methods(&tmp, out);
        ext_this_is_class_ = saved;
        *methods = std::move(tmp.methods); // devolver para no invalidar el AST
    }
}

// -----------------------------------------------------------------
// Helpers de generacion de codigo .vel para POO dinamica.
// -----------------------------------------------------------------

/**
 * @brief Registra el nombre como bytes UTF-8 en static_data y devuelve
 *        el indice para construir @c @Absolute("code.s_<idx>").
 */
uint64_t intern_class_name(ir::IrModule &mod, const std::string &name) {
    std::vector<uint8_t> bytes(name.begin(), name.end());
    return mod.intern_static_data(std::move(bytes));
}

/**
 * @brief Interna un literal de cadena en los datos estaticos CON su nul.
 *
 * Aparte de @c intern_class_name a proposito: aquel guarda los bytes tal cual
 * porque sus clientes (nombres de clase, mensajes de panic) llevan siempre la
 * longitud al lado.  Un `string` que apunta aqui, en cambio, tiene que poder
 * dar un `cstr()` valido, y eso exige el nul en memoria -- no se puede anadir
 * despues sobre datos de solo lectura.
 *
 * El nul NO cuenta para la longitud: `.bytes()` sigue dando los bytes del
 * literal.  Solo esta para que la cadena valga en la frontera con C.
 *
 * @param mod Modulo donde viven los datos estaticos.
 * @param lit Bytes del literal (UTF-8, sin nul).
 * @return Indice del blob internado.
 */
static uint64_t intern_string_literal_nul(ir::IrModule &mod,
                                          const std::string &lit) {
    std::vector<uint8_t> bytes(lit.begin(), lit.end());
    bytes.push_back(0);
    return mod.intern_static_data(std::move(bytes));
}

/**
 * @brief fix11 - reserva un slot de 8 bytes en static_data para
 * cachear el `ClassInfo*` de una clase.  El slot inicia en 0 y se
 * llena en `__module_init` despues del `defclass`; cada llamada
 * subsiguiente a `__new_<Class>` lee del slot directamente sin
 * pasar por `findclass` (ahorra ~8 instrucciones VM por alloc +
 * elimina el string lookup en el runtime ClassRegistry).
 *
 * Los bytes contienen: 8 ceros (el slot del ClassInfo*) + bytes
 * unicos del nombre de la clase + sentinel 0xFF para evitar
 * deduplicacion con otras clases o con el nombre del simbolo
 * (que es el patron de `intern_class_name`).  El runtime accede
 * SOLO a los primeros 8 bytes del slot.
 */
uint64_t intern_class_cache_slot(ir::IrModule &mod,
                                        const std::string &name) {
    std::vector<uint8_t> bytes(8, 0); // 8 zeros (cache)
    bytes.push_back(0xFF); // sentinel: distingue de intern_class_name
    bytes.insert(bytes.end(), name.begin(), name.end()); // nombre para unicidad
    return mod.intern_static_data(std::move(bytes));
}

/**
 * @brief Emite la liberacion RAII del closure almacenado en un campo: libera
 *        el env (RAW_ALLOC, si tiene capturas) y el slot de 16 bytes (RAW_ALLOC
 *        owned por el campo).  Modelo sin GC; ver
 * doc/VMdoc/Vesta/ClosuresEnCampos.md.
 *
 * El campo guarda un PTR a un slot heap de 16 bytes {fn_addr@+0, env_ptr@+8}.
 * Secuencia:
 *   slot = [this + field_offset];       if (slot == 0) skip;   // campo sin
 * closure env  = [slot + 8];                  if (env  != 0) RAW_FREE(env);  //
 * captura RAW_FREE(slot);                                              //
 * siempre
 */
void Lowering::emit_free_closure_env_field(ir::IrValueId this_vid,
                                           uint32_t field_offset,
                                           uint32_t line) {
    const ir::IrBlockId skip_bb = fn_->new_block("free_clo_skip");
    const ir::IrValueId zero = emit_const(ir::IrType::I64, 0, line);

    // slot = LOAD [this + field_offset]  (host_ptr al slot RAW_ALLOC).
    const ir::IrValueId slot_addr =
        emit_field_addr(fn_, current_block_, this_vid, field_offset, line);
    const ir::IrValueId slot = fn_->new_value(ir::IrType::I64);
    fn_->values[slot].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = slot;
        ld.operands = {slot_addr};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    // if (slot == 0) -> skip  (campo nunca asignado / closure null).
    const ir::IrBlockId slot_ok = fn_->new_block("free_clo_slot_ok");
    {
        const ir::IrValueId is_null = fn_->new_value(ir::IrType::BOOL);
        ir::IrInstr cmp{};
        cmp.op = ir::IrOp::CMP_EQ;
        cmp.type = ir::IrType::BOOL;
        cmp.dst = is_null;
        cmp.operands = {slot, zero};
        cmp.source_line = line;
        emit(current_block_, std::move(cmp));
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands = {is_null};
        br.target_block = skip_bb; // null -> skip
        br.false_block = slot_ok;
        br.source_line = line;
        // CFG explicita (succs/preds): SIN esto el analisis de vivacidad NO
        // ve los edges del diamante del free hacia skip_bb -> las constantes
        // vivas que cruzan el free (p.ej. el offset +8 del call posterior)
        // se consideran muertas y el regalloc reusa su registro como scratch
        // del env-load -> direccion basura en el call -> segfault.
        fn_->blocks[current_block_].succs.push_back(skip_bb);
        fn_->blocks[current_block_].succs.push_back(slot_ok);
        fn_->blocks[skip_bb].preds.push_back(current_block_);
        fn_->blocks[slot_ok].preds.push_back(current_block_);
        emit(current_block_, std::move(br));
        current_block_ = slot_ok;
    }
    // env = LOAD [slot + 8]
    const ir::IrValueId env_addr = fn_->new_value(ir::IrType::PTR);
    fn_->values[env_addr].is_host_ptr = true;
    {
        const ir::IrValueId eight = emit_const(ir::IrType::I64, 8, line);
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = env_addr;
        ad.operands = {slot, eight};
        ad.source_line = line;
        emit(current_block_, std::move(ad));
    }
    const ir::IrValueId env = fn_->new_value(ir::IrType::I64);
    fn_->values[env].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = env;
        ld.operands = {env_addr};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    // Bloque que SIEMPRE libera el slot (heap owned), tras (quiza) liberar env.
    const ir::IrBlockId free_slot_bb = fn_->new_block("free_clo_slot");
    // if (env == 0) -> free_slot; else RAW_FREE(env) -> free_slot
    {
        const ir::IrValueId is_null = fn_->new_value(ir::IrType::BOOL);
        ir::IrInstr cmp{};
        cmp.op = ir::IrOp::CMP_EQ;
        cmp.type = ir::IrType::BOOL;
        cmp.dst = is_null;
        cmp.operands = {env, zero};
        cmp.source_line = line;
        emit(current_block_, std::move(cmp));
        const ir::IrBlockId free_env_bb = fn_->new_block("free_clo_env");
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands = {is_null};
        br.target_block = free_slot_bb; // env null -> solo libera el slot
        br.false_block = free_env_bb;
        br.source_line = line;
        fn_->blocks[current_block_].succs.push_back(free_slot_bb);
        fn_->blocks[current_block_].succs.push_back(free_env_bb);
        fn_->blocks[free_slot_bb].preds.push_back(current_block_);
        fn_->blocks[free_env_bb].preds.push_back(current_block_);
        emit(current_block_, std::move(br));
        current_block_ = free_env_bb;
    }
    {
        ir::IrInstr rf{};
        rf.op = ir::IrOp::RAW_FREE;
        rf.type = ir::IrType::VOID;
        rf.dst = ir::IR_NO_VALUE;
        rf.operands = {env};
        rf.source_line = line;
        emit(current_block_, std::move(rf));
        ir::IrInstr brj{};
        brj.op = ir::IrOp::BR;
        brj.target_block = free_slot_bb;
        brj.source_line = line;
        fn_->blocks[current_block_].succs.push_back(free_slot_bb);
        fn_->blocks[free_slot_bb].preds.push_back(current_block_);
        emit(current_block_, std::move(brj));
    }
    // free_slot_bb: RAW_FREE(slot); br skip.
    current_block_ = free_slot_bb;
    {
        ir::IrInstr rf{};
        rf.op = ir::IrOp::RAW_FREE;
        rf.type = ir::IrType::VOID;
        rf.dst = ir::IR_NO_VALUE;
        rf.operands = {slot};
        rf.source_line = line;
        emit(current_block_, std::move(rf));
        ir::IrInstr brj{};
        brj.op = ir::IrOp::BR;
        brj.target_block = skip_bb;
        brj.source_line = line;
        fn_->blocks[current_block_].succs.push_back(skip_bb);
        fn_->blocks[skip_bb].preds.push_back(current_block_);
        emit(current_block_, std::move(brj));
    }
    current_block_ = skip_bb;
    block_terminated_ = false;
}

void Lowering::emit_free_unique_field(ir::IrValueId this_vid,
                                      uint32_t field_offset, uint32_t line) {
    // El CAMPO vive en la memoria del CONTENEDOR: VM (struct en VM stack) o
    // host (clase, o cualquiera en native_poo/AOT).  La carga del campo hereda
    // la host-ness de @c this_vid (NO emit_field_addr, que la fuerza a host).
    // El slot que el campo guarda es SIEMPRE un host_ptr (heap RAW_ALLOC).
    const bool container_host = fn_->values[this_vid].is_host_ptr;
    ir::IrValueId slot_addr = this_vid;
    if (field_offset != 0) {
        const ir::IrValueId off = emit_const(
            ir::IrType::I64, static_cast<int64_t>(field_offset), line);
        slot_addr = fn_->new_value(ir::IrType::PTR);
        fn_->values[slot_addr].is_host_ptr = container_host;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = slot_addr;
        ad.operands = {this_vid, off};
        ad.source_line = line;
        emit(current_block_, std::move(ad));
    }
    // slot = LOAD [this + field_offset]  (mov/movh segun el contenedor).
    const ir::IrValueId slot = fn_->new_value(ir::IrType::I64);
    fn_->values[slot].is_host_ptr = true; // el slot es heap host
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = slot;
        ld.operands = {slot_addr};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    emit_free_unique_slot(slot, line);
}

void Lowering::emit_memberwise_copy(ir::IrValueId dst_addr,
                                    ir::IrValueId src_addr, uint64_t size_bytes,
                                    uint32_t line) {
    const bool dst_host = fn_->values[dst_addr].is_host_ptr;
    const bool src_host = fn_->values[src_addr].is_host_ptr;
    const uint64_t qwords = (size_bytes + 7) / 8;
    for (uint64_t qi = 0; qi < qwords; ++qi) {
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, static_cast<int64_t>(qi * 8), line);
        const ir::IrValueId s_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[s_at].is_host_ptr = src_host;
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = s_at;
            ad.operands = {src_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        const ir::IrValueId w = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = w;
            ld.operands = {s_at};
            ld.source_line = line;
            emit(current_block_, std::move(ld));
        }
        const ir::IrValueId d_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[d_at].is_host_ptr = dst_host;
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = d_at;
            ad.operands = {dst_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {w, d_at};
            st.source_line = line;
            emit(current_block_, std::move(st));
        }
    }
}

void Lowering::emit_struct_method_on_host_field(ir::IrValueId field_addr,
                                                const std::string &struct_name,
                                                const std::string &method_label,
                                                uint32_t line) {
    // Historico: cuando un metodo de struct se compilaba con this=VM en
    // interp/JIT, un campo en el payload HOST de una clase habia que COPIARLO a
    // un temp VM para poder llamarlo.  Ese rodeo ya no hace falta -- `this` es
    // host en los tres modos (ver lower_struct_methods) -- y ademas era DANINO
    // para los metodos que MUTAN: el dtor o el `__clone__` operaban sobre la
    // COPIA, asi que el refcount real no se tocaba (el free nunca llegaba, o
    // llegaba de mas).  El CALL va directo sobre el campo.
    const bool need_temp = false;
    if (!need_temp) {
        ir::IrInstr cd{};
        cd.op = ir::IrOp::CALL;
        cd.type = ir::IrType::VOID;
        cd.dst = ir::IR_NO_VALUE;
        cd.operands = {field_addr};
        cd.func_name = method_label;
        cd.source_line = line;
        emit(current_block_, std::move(cd));
        return;
    }
    // interp/JIT: copiar el campo (host) a un temporal VM-stack y llamar el
    // metodo sobre el temporal (this VM == lo que el metodo asume).
    uint64_t sz = 8;
    auto it_sl = tc_.struct_layouts().find(struct_name);
    if (it_sl != tc_.struct_layouts().end())
        sz = static_cast<uint64_t>(it_sl->second.size_bytes);
    if (sz == 0) sz = 8;
    // ALLOCA temp VM (is_host_ptr = false).
    const ir::IrValueId tmp = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.imm = sz;
        al.dst = tmp;
        al.source_line = line;
        emit(current_block_, std::move(al));
    }
    // memcpy field_addr (host) -> tmp (VM): qword por qword.
    const uint64_t qwords = (sz + 7) / 8;
    for (uint64_t qi = 0; qi < qwords; ++qi) {
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, static_cast<int64_t>(qi * 8), line);
        const ir::IrValueId src_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[src_at].is_host_ptr = true; // campo en payload host
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = src_at;
            ad.operands = {field_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        const ir::IrValueId word = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = word;
            ld.operands = {src_at};
            ld.source_line = line;
            emit(current_block_, std::move(ld));
        }
        const ir::IrValueId dst_at = fn_->new_value(ir::IrType::PTR);
        // tmp es VM (is_host_ptr = false por defecto).
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = dst_at;
            ad.operands = {tmp, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {word, dst_at};
            st.source_line = line;
            emit(current_block_, std::move(st));
        }
    }
    // CALL method_label(tmp).
    {
        ir::IrInstr cd{};
        cd.op = ir::IrOp::CALL;
        cd.type = ir::IrType::VOID;
        cd.dst = ir::IR_NO_VALUE;
        cd.operands = {tmp};
        cd.func_name = method_label;
        cd.source_line = line;
        emit(current_block_, std::move(cd));
    }
}

ir::IrValueId Lowering::emit_struct_arg_copy_clone(
    ir::IrValueId v_src, const std::string &struct_name, uint32_t line) {
    // Ownership ruta B (H1 paso por valor): pasar un struct CON copy-hook por
    // valor a una funcion (`f(a)`) hace una COPIA -- la callee recibe su propia
    // instancia.  Alocamos una copia (ALLOCA, misma memory class que un struct
    // local: VM en interp/JIT, host en AOT/native_poo), memcpy del origen, y
    // `copia.__clone__()` para que el tipo gestionado ajuste su recurso (p.ej.
    // ++refcount).  Devuelve la direccion de la copia para pasarla al CALL.  El
    // caller emite el `~dtor` de la copia tras el CALL (la callee no posee el
    // param, igual que cualquier struct por valor en Vesta).
    uint64_t sz = 8;
    auto it_sl = tc_.struct_layouts().find(struct_name);
    if (it_sl != tc_.struct_layouts().end())
        sz = static_cast<uint64_t>(it_sl->second.size_bytes);
    if (sz == 0) sz = 8;
    // ALLOCA copia: host-ness identica a un struct local, o sea HOST en los
    // tres modos (ver lower_var_decl).  Cuando esta copia se quedaba en la pila
    // VM en interp/JIT, el callee -- que lee sus params agregados con `movh` --
    // la leia como basura, y su `__clone__` / `~dtor` operaban sobre esa
    // basura.
    const ir::IrValueId copy = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.imm = sz;
        al.dst = copy;
        al.host_alloca = true;
        al.source_line = line;
        emit(current_block_, std::move(al));
    }
    fn_->values[copy].is_host_ptr = true;
    // memcpy v_src -> copy (respetando host-ness de origen y destino).
    const bool src_is_host = fn_->values[v_src].is_host_ptr;
    const bool dst_is_host = fn_->values[copy].is_host_ptr;
    const uint64_t qwords = (sz + 7) / 8;
    for (uint64_t qi = 0; qi < qwords; ++qi) {
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, static_cast<int64_t>(qi * 8), line);
        const ir::IrValueId src_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[src_at].is_host_ptr = src_is_host;
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = src_at;
            ad.operands = {v_src, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        const ir::IrValueId word = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = word;
            ld.operands = {src_at};
            ld.source_line = line;
            emit(current_block_, std::move(ld));
        }
        const ir::IrValueId dst_at = fn_->new_value(ir::IrType::PTR);
        fn_->values[dst_at].is_host_ptr = dst_is_host;
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = dst_at;
            ad.operands = {copy, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {word, dst_at};
            st.source_line = line;
            emit(current_block_, std::move(st));
        }
    }
    // copia.__clone__()  (this = copy, misma memory class -> sin mismatch).
    emit_struct_method_on_host_field(copy, struct_name,
                                     struct_name + "__" + "__clone__", line);
    return copy;
}

void Lowering::emit_free_unique_slot(ir::IrValueId slot, uint32_t line) {
    const ir::IrBlockId skip_bb = fn_->new_block("free_uniq_skip");
    const ir::IrValueId zero = emit_const(ir::IrType::I64, 0, line);
    // if (slot == 0) -> skip  (slot nulo / unique movido).
    const ir::IrBlockId slot_ok = fn_->new_block("free_uniq_slot_ok");
    {
        const ir::IrValueId is_null = fn_->new_value(ir::IrType::BOOL);
        ir::IrInstr cmp{};
        cmp.op = ir::IrOp::CMP_EQ;
        cmp.type = ir::IrType::BOOL;
        cmp.dst = is_null;
        cmp.operands = {slot, zero};
        cmp.source_line = line;
        emit(current_block_, std::move(cmp));
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands = {is_null};
        br.target_block = skip_bb;
        br.false_block = slot_ok;
        br.source_line = line;
        fn_->blocks[current_block_].succs.push_back(skip_bb);
        fn_->blocks[current_block_].succs.push_back(slot_ok);
        fn_->blocks[skip_bb].preds.push_back(current_block_);
        fn_->blocks[slot_ok].preds.push_back(current_block_);
        emit(current_block_, std::move(br));
        current_block_ = slot_ok;
    }
    // ptr = LOAD [slot + 0]  (el valor/host_ptr a liberar).
    const ir::IrValueId ptr = fn_->new_value(ir::IrType::I64);
    fn_->values[ptr].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = ptr;
        ld.operands = {slot};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    // deleter = LOAD [slot + 8].
    const ir::IrValueId del_addr = fn_->new_value(ir::IrType::PTR);
    fn_->values[del_addr].is_host_ptr = true;
    {
        const ir::IrValueId eight = emit_const(ir::IrType::I64, 8, line);
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = del_addr;
        ad.operands = {slot, eight};
        ad.source_line = line;
        emit(current_block_, std::move(ad));
    }
    const ir::IrValueId deleter = fn_->new_value(ir::IrType::I64);
    fn_->values[deleter].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = deleter;
        ld.operands = {del_addr};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    // if (deleter != 0) -> CALLIND deleter(ptr); else RAW_FREE(ptr).
    const ir::IrBlockId call_bb = fn_->new_block("free_uniq_call");
    const ir::IrBlockId free_bb = fn_->new_block("free_uniq_raw");
    {
        const ir::IrValueId has_del = fn_->new_value(ir::IrType::BOOL);
        ir::IrInstr cmp{};
        cmp.op = ir::IrOp::CMP_NE;
        cmp.type = ir::IrType::BOOL;
        cmp.dst = has_del;
        cmp.operands = {deleter, zero};
        cmp.source_line = line;
        emit(current_block_, std::move(cmp));
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands = {has_del};
        br.target_block = call_bb;
        br.false_block = free_bb;
        br.source_line = line;
        fn_->blocks[current_block_].succs.push_back(call_bb);
        fn_->blocks[current_block_].succs.push_back(free_bb);
        fn_->blocks[call_bb].preds.push_back(current_block_);
        fn_->blocks[free_bb].preds.push_back(current_block_);
        emit(current_block_, std::move(br));
    }
    // Bloque que SIEMPRE libera el slot heap (16B), tras liberar el inner.
    const ir::IrBlockId free_slot_bb = fn_->new_block("free_uniq_slot");
    // call_bb: CALLIND deleter(ptr) -> free_slot.
    current_block_ = call_bb;
    {
        ir::IrInstr ci{};
        ci.op = ir::IrOp::CALLIND;
        ci.type = ir::IrType::VOID;
        ci.dst = ir::IR_NO_VALUE;
        ci.func_ptr = deleter;
        ci.operands = {ptr};
        ci.source_line = line;
        ci.is_call_site = true;
        emit(current_block_, std::move(ci));
        ir::IrInstr brj{};
        brj.op = ir::IrOp::BR;
        brj.target_block = free_slot_bb;
        brj.source_line = line;
        fn_->blocks[current_block_].succs.push_back(free_slot_bb);
        fn_->blocks[free_slot_bb].preds.push_back(current_block_);
        emit(current_block_, std::move(brj));
    }
    // free_bb: RAW_FREE(ptr) -> free_slot  (deleter por defecto; null-safe).
    current_block_ = free_bb;
    {
        ir::IrInstr rf{};
        rf.op = ir::IrOp::RAW_FREE;
        rf.type = ir::IrType::VOID;
        rf.dst = ir::IR_NO_VALUE;
        rf.operands = {ptr};
        rf.source_line = line;
        emit(current_block_, std::move(rf));
        ir::IrInstr brj{};
        brj.op = ir::IrOp::BR;
        brj.target_block = free_slot_bb;
        brj.source_line = line;
        fn_->blocks[current_block_].succs.push_back(free_slot_bb);
        fn_->blocks[free_slot_bb].preds.push_back(current_block_);
        emit(current_block_, std::move(brj));
    }
    // free_slot_bb: RAW_FREE(slot heap) -> skip.
    current_block_ = free_slot_bb;
    {
        ir::IrInstr rf{};
        rf.op = ir::IrOp::RAW_FREE;
        rf.type = ir::IrType::VOID;
        rf.dst = ir::IR_NO_VALUE;
        rf.operands = {slot};
        rf.source_line = line;
        emit(current_block_, std::move(rf));
        ir::IrInstr brj{};
        brj.op = ir::IrOp::BR;
        brj.target_block = skip_bb;
        brj.source_line = line;
        fn_->blocks[current_block_].succs.push_back(skip_bb);
        fn_->blocks[skip_bb].preds.push_back(current_block_);
        emit(current_block_, std::move(brj));
    }
    current_block_ = skip_bb;
    block_terminated_ = false;
}

std::string Lowering::func_ref_label(const std::string &name,
                                     const std::string &mangled) {
    // Si el nombre es un extern, su direccion cruda no es invocable por
    // callvmr (es codigo nativo, no bytecode VM).  Generamos un thunk Vesta
    // `__cfnthunk_<fn>` que reenvia al CALLN; el cfn apunta a ese thunk.
    if (extern_lib_by_fn_name_.count(name)) {
        extern_cfn_thunks_.insert(name);
        return "__cfnthunk_" + name;
    }
    return mangled.empty() ? name : mangled;
}

void Lowering::generate_extern_cfn_thunks(ir::IrModule &out) {
    // Por cada extern cuya direccion se tomo como cfn, sintetizamos un
    // thunk Vesta:  __cfnthunk_<fn>(params...) { return <lib>:<fn>(params...);
    // } El cuerpo es un unico CALLN (reenvio nativo) + RET.  Asi el cfn se
    // invoca por CALLIND -> entra al thunk -> el thunk hace el CALLN, que
    // cada backend resuelve normalmente (LoadLibrary/GetProcAddress, IAT/GOT).
    for (const auto &fn_name : extern_cfn_thunks_) {
        const FunctionSig *sig = tc_.function_sig_by_name(fn_name);
        if (!sig || sig->extern_lib.empty()) continue;

        ir::IrFunction fn;
        fn.name = "__cfnthunk_" + fn_name;
        const ir::IrType ret_ir =
            (sig->return_type.kind == PrimitiveKind::VOID ||
             sig->return_type.kind == PrimitiveKind::COUNT)
                ? ir::IrType::VOID
                : ir_type_from_primitive(sig->return_type.kind);
        fn.ret_type = ret_ir;

        // Params: uno por parametro del extern, en orden.
        std::vector<ir::IrValueId> param_vids;
        param_vids.reserve(sig->param_types.size());
        for (size_t i = 0; i < sig->param_types.size(); ++i) {
            const ir::IrType pt =
                ir_type_from_primitive(sig->param_types[i].kind);
            const ir::IrValueId vid =
                fn.new_value(pt, "%a" + std::to_string(i));
            fn.values[vid].is_param = true;
            // PTR/ARRAY nativos (no VirtualPtr) son host_ptr.
            const PrimitiveKind pk = sig->param_types[i].kind;
            if ((pk == PrimitiveKind::PTR || pk == PrimitiveKind::ARRAY) &&
                !sig->param_types[i].is_virtual) {
                fn.values[vid].is_host_ptr = true;
            }
            fn.params.push_back(vid);
            param_vids.push_back(vid);
        }

        const ir::IrBlockId entry = fn.new_block("entry");
        // CALLN @Method("<lib>:<fn>") con los params como args.
        out.register_native_import(sig->extern_lib, fn_name);
        const ir::IrValueId dst = (ret_ir == ir::IrType::VOID)
                                      ? ir::IR_NO_VALUE
                                      : fn.new_value(ret_ir);
        {
            ir::IrInstr ins{};
            ins.op = ir::IrOp::CALLN;
            ins.type = ret_ir;
            ins.dst = dst;
            ins.func_name = sig->extern_lib + ":" + fn_name;
            ins.operands = param_vids;
            ins.source_line = 0;
            fn.append(entry, std::move(ins));
        }
        {
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = ret_ir;
            if (dst != ir::IR_NO_VALUE) ret.operands = {dst};
            ret.source_line = 0;
            fn.append(entry, std::move(ret));
        }
        out.add_function(std::move(fn));
    }
}

void Lowering::generate_free_uniq_helper(ir::IrModule &out) {
    if (!needs_free_uniq_helper_) return;
    // void __vx_free_uniq(i64 slot) { <emit_free_unique_slot(slot)>; ret; }
    // El cuerpo reusa emit_free_unique_slot (null-guard + deleter dispatch +
    // RAW_FREE del slot).  Como es una funcion normal, su diamante interno no
    // colisiona con el tailcall del dtor en el call site del reassign-free.
    ir::IrFunction fn;
    fn.name = "__vx_free_uniq";
    fn.ret_type = ir::IrType::VOID;
    const ir::IrValueId slot = fn.new_value(ir::IrType::I64, "%slot");
    fn.values[slot].is_param = true;
    fn.values[slot].is_host_ptr = true; // el slot es heap host
    fn.params.push_back(slot);
    const ir::IrBlockId entry = fn.new_block("entry");

    // Activar el contexto del lowering para reusar emit_free_unique_slot.
    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_term = block_terminated_;
    fn_ = &fn;
    current_block_ = entry;
    block_terminated_ = false;
    emit_free_unique_slot(slot, 0);
    // RET void al final (emit_free_unique_slot deja current_block_ en su skip).
    {
        ir::IrInstr ret{};
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::VOID;
        ret.source_line = 0;
        fn.append(current_block_, std::move(ret));
    }
    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_term;
    out.add_function(std::move(fn));
}


namespace {

/**
 * @brief Instrucciones por tanda al partir `__module_init`.
 *
 * Ajustable con `VESTA_MODULE_INIT_CHUNK` para poder comparar; se lee una vez
 * porque consultar el entorno recorre su bloque entero.
 */
size_t module_init_chunk_budget() {
    static const size_t n = [] {
        const long v = util::flag_int(util::FlagId::ModuleInitChunk, 0);
        if (v > 0) return static_cast<size_t>(v);
        return static_cast<size_t>(2000);
    }();
    return n;
}

/// Por debajo de esto no se parte: una llamada por tanda no se paga por
/// ahorrar tan poco.
constexpr size_t kModuleInitSplitMin = 4000;

} // namespace

/* Deja de ser interna a este fichero: la llama generate_module_init_function,
 * que vive con el resto de la POO en lowering/oop.cpp. */
/**
 * @brief Parte @p init en tandas y la deja llamandolas en orden.
 *
 * POR QUE.  `__module_init` es, con diferencia, la funcion mas grande que
 * genera el compilador: en un modulo de 750 funciones se lleva 24.020 de las
 * 69.370 instrucciones del IR -- el 34,6% -- mientras que cada funcion de
 * usuario ronda las 83.  Eso hace dos danos a la vez.  Reparte mal: el bucle
 * de pases se reparte POR FUNCION, asi que un hilo se queda con esta y los
 * demas esperan (medido: el 79% del tiempo dentro del reparto era espera).  Y
 * cuesta de mas: el coste de compilar crece MAS que lineal con el tamano
 * (medido, ~n^1,28 entre 750 y 3.000 funciones), asi que partirla no solo
 * equilibra -- quita trabajo.
 *
 * POR QUE SE PUEDE.  Los bloques de `__module_init` son una CADENA lineal, uno
 * por clase o advice, y NO se pasan valores entre si: lo que una clase necesita
 * de otra viaja por estado GLOBAL -- el hueco estatico donde queda su
 * `ClassInfo*` y el registro que `findclass` consulta por nombre.  Comprobado
 * sobre el IR emitido: de 331 valores, el UNICO que cruza de bloque es el
 * buffer de parametros (un `ALLOCA`), y ese se rematerializa fresco por tanda.
 *
 * Es conservador a proposito.  Cualquier forma que no encaje con eso -- un
 * PHI, una rama, un valor que cruza y no es un `ALLOCA` -- deja la funcion como
 * estaba: partir mal aqui no da un programa mas lento, da uno que registra mal
 * sus clases.
 *
 * @param init `__module_init` recien generada; queda reescrita a llamadas.
 * @param out  Modulo donde se anaden las tandas.
 * @return true si se partio; false si se dejo intacta.
 */
bool split_module_init_into_chunks(ir::IrFunction &init, ir::IrModule &out) {
    const size_t nblocks = init.blocks.size();
    if (nblocks < 2) return false;

    size_t total = 0;
    for (const auto &b : init.blocks) total += b.instrs.size();
    if (total < kModuleInitSplitMin) return false;

    // --- 1. La cadena tiene que ser lineal y sin PHIs ---------------------
    for (size_t i = 0; i < nblocks; ++i) {
        const ir::IrBlock &b = init.blocks[i];
        if (b.id != static_cast<ir::IrBlockId>(i)) return false;
        if (b.instrs.empty()) return false;
        for (size_t k = 0; k < b.instrs.size(); ++k) {
            const ir::IrOp op = b.instrs[k].op;
            if (op == ir::IrOp::PHI) return false;
            // Un terminador a mitad de bloque significa que esto no es la
            // cadena que creemos estar mirando.
            if (k + 1 != b.instrs.size() &&
                (op == ir::IrOp::BR || op == ir::IrOp::BR_COND ||
                 op == ir::IrOp::RET))
                return false;
        }
        const ir::IrInstr &last = b.instrs.back();
        if (i + 1 < nblocks) {
            if (last.op != ir::IrOp::BR) return false;
            if (last.target_block != static_cast<ir::IrBlockId>(i + 1))
                return false;
        } else if (last.op != ir::IrOp::RET) {
            return false;
        }
    }

    // --- 2. Que valores cruzan de bloque ----------------------------------
    const size_t nvals = init.values.size();
    std::vector<size_t> def_block(nvals, SIZE_MAX);
    std::vector<const ir::IrInstr *> def_instr(nvals, nullptr);
    for (size_t i = 0; i < nblocks; ++i)
        for (const ir::IrInstr &in : init.blocks[i].instrs)
            if (in.dst != ir::IR_NO_VALUE &&
                static_cast<size_t>(in.dst) < nvals) {
                def_block[in.dst] = i;
                def_instr[in.dst] = &in;
            }

    std::vector<ir::IrValueId> shared; // los que cruzan (en la practica, 1)
    bool splittable = true;
    auto note_use = [&](ir::IrValueId v, size_t blk) {
        if (v == ir::IR_NO_VALUE) return;
        if (static_cast<size_t>(v) >= nvals || def_block[v] == SIZE_MAX) {
            splittable = false; // viene de fuera de la cadena: no se toca
            return;
        }
        if (def_block[v] != blk &&
            std::find(shared.begin(), shared.end(), v) == shared.end())
            shared.push_back(v);
    };
    for (size_t i = 0; i < nblocks && splittable; ++i)
        for (const ir::IrInstr &in : init.blocks[i].instrs) {
            for (ir::IrValueId v : in.operands) note_use(v, i);
            note_use(in.func_ptr, i);
        }
    if (!splittable) return false;

    // Rematerializar solo se sabe hacer con un ALLOCA.
    for (ir::IrValueId v : shared)
        if (!def_instr[v] || def_instr[v]->op != ir::IrOp::ALLOCA) return false;

    // --- 3. Cortes por presupuesto de instrucciones -----------------------
    const size_t budget = module_init_chunk_budget();
    std::vector<std::pair<size_t, size_t>> chunks; // [inicio, fin)
    size_t start = 0, acc = 0;
    for (size_t i = 0; i < nblocks; ++i) {
        acc += init.blocks[i].instrs.size();
        if (acc >= budget && i + 1 != nblocks) {
            chunks.emplace_back(start, i + 1);
            start = i + 1;
            acc = 0;
        }
    }
    chunks.emplace_back(start, nblocks);
    if (chunks.size() < 2) return false;

    // --- 4. Una funcion por tanda -----------------------------------------
    const std::string base = init.name;
    std::vector<std::string> names;
    names.reserve(chunks.size());

    for (size_t c = 0; c < chunks.size(); ++c) {
        const size_t first = chunks[c].first, limit = chunks[c].second;
        ir::IrFunction f;
        f.name = base + "_part" + std::to_string(c);
        f.ret_type = ir::IrType::VOID;

        std::vector<ir::IrBlockId> newid(nblocks, 0);
        for (size_t i = first; i < limit; ++i)
            newid[i] = f.new_block(init.blocks[i].name);

        // El buffer de parametros: uno FRESCO por tanda.
        std::vector<ir::IrValueId> vmap(nvals, ir::IR_NO_VALUE);
        for (ir::IrValueId v : shared) {
            ir::IrInstr a = *def_instr[v];
            const ir::IrValueId nv = f.new_value(init.values[v].type);
            f.values[nv].is_host_ptr = init.values[v].is_host_ptr;
            a.dst = nv;
            vmap[v] = nv;
            f.append(newid[first], std::move(a));
        }

        for (size_t i = first; i < limit; ++i)
            for (const ir::IrInstr &in : init.blocks[i].instrs) {
                // El ALLOCA compartido ya se emitio fresco arriba.
                if (in.op == ir::IrOp::ALLOCA && in.dst != ir::IR_NO_VALUE &&
                    std::find(shared.begin(), shared.end(), in.dst) !=
                        shared.end())
                    continue;
                ir::IrInstr copy = in;
                for (ir::IrValueId &v : copy.operands)
                    if (v != ir::IR_NO_VALUE) v = vmap[v];
                if (copy.func_ptr != ir::IR_NO_VALUE)
                    copy.func_ptr = vmap[copy.func_ptr];
                if (copy.op == ir::IrOp::BR) {
                    if (i + 1 < limit) {
                        copy.target_block = newid[i + 1];
                    } else {
                        // Fin de tanda: se vuelve, no se salta a la siguiente.
                        const int ln = copy.source_line;
                        copy = ir::IrInstr{};
                        copy.op = ir::IrOp::RET;
                        copy.type = ir::IrType::VOID;
                        copy.source_line = ln;
                    }
                }
                if (copy.dst != ir::IR_NO_VALUE) {
                    const ir::IrValueId old = copy.dst;
                    const ir::IrValueId nv = f.new_value(init.values[old].type);
                    f.values[nv].is_host_ptr = init.values[old].is_host_ptr;
                    vmap[old] = nv;
                    copy.dst = nv;
                }
                f.append(newid[i], std::move(copy));
            }

        names.push_back(f.name);
        out.add_function(std::move(f));
    }

    // --- 5. `__module_init` pasa a ser la lista de llamadas ---------------
    init.blocks.clear();
    init.values.clear();
    const ir::IrBlockId entry = init.new_block("entry");
    for (const std::string &n : names) {
        ir::IrInstr call{};
        call.op = ir::IrOp::CALL;
        call.type = ir::IrType::VOID;
        call.dst = ir::IR_NO_VALUE;
        call.func_name = n;
        call.source_line = 0;
        init.append(entry, std::move(call));
    }
    ir::IrInstr ret{};
    ret.op = ir::IrOp::RET;
    ret.type = ir::IrType::VOID;
    ret.source_line = 0;
    init.append(entry, std::move(ret));
    return true;
}



std::string Lowering::build_module_init_asm(ir::IrModule & /*out_module*/) {
    // No se usa: la generacion de __module_init se hace via
    // generate_module_init_function (IrFunction completa, no cadena).
    return std::string();
}


ir::IrValueId Lowering::lower_this_expr(ast::ThisExpr *e) {
    const ir::IrValueId v = lookup("this");
    if (v == ir::IR_NO_VALUE) {
        error_at(e->loc, "lowering: 'this' fuera de contexto de metodo");
    }
    return v;
}

/**
 * @brief Helper: construye un IrValueId que apunta a @c base+offset,
 *        marcado como host_ptr (para que LOAD/STORE emitan @c movh).
 *        Si offset es 0, devuelve el base directamente.
 */
ir::IrValueId emit_field_addr(ir::IrFunction *fn, ir::IrBlockId block,
                                     ir::IrValueId base, uint32_t offset,
                                     uint32_t line) {
    if (offset == 0) {
        // El frontend marca el resultado como host_ptr para que LOAD/
        // STORE usen movh.  Si la base ya tiene is_host_ptr=true, la
        // propagacion es trivial; si no, lo forzamos aqui (siempre lo
        // sera para nuestros punteros de objeto Vesta).
        fn->values[base].is_host_ptr = true;
        return base;
    }
    // Crear constante con el offset y sumar.
    ir::IrInstr c{};
    const ir::IrValueId off_val = fn->new_value(ir::IrType::I64);
    fn->values[off_val].is_const = true;
    fn->values[off_val].const_val = offset;
    c.op = ir::IrOp::CONST;
    c.type = ir::IrType::I64;
    c.dst = off_val;
    c.imm = offset;
    c.source_line = line;
    fn->append(block, std::move(c));

    const ir::IrValueId addr = fn->new_value(ir::IrType::PTR);
    // Marcar host_ptr: las operaciones LOAD/STORE consultan este flag
    // para emitir mov (VM) o movh (host).  Las direcciones derivadas
    // de un host_ptr siguen siendo host_ptr.
    fn->values[addr].is_host_ptr = true;
    ir::IrInstr add{};
    add.op = ir::IrOp::ADD;
    add.type = ir::IrType::PTR;
    add.dst = addr;
    add.operands = {base, off_val};
    add.source_line = line;
    fn->append(block, std::move(add));
    return addr;
}

ir::IrValueId Lowering::lower_class_field_load(ast::FieldAccessExpr *e) {
    // Limitacion G (cerrada): @c property_kind == 3 marca acceso a
    // static field via @c ClassName.field.  El base es IdentExpr cuyo
    // nombre es la clase; lo resolvemos via findclass inline + getstatic
    // con offset compile-time.  No leemos el tipo del base con
    // @c check_expr (fallaria por "nombre no declarado") sino que
    // tomamos el ClassLayout directamente del nombre.
    if (e->property_kind == 3) {
        if (!e->base || e->base->kind != ast::NodeKind::IdentExpr) {
            error_at(e->loc, "lowering: static field con base no-ClassName");
            return ir::IR_NO_VALUE;
        }
        auto *base_id = static_cast<ast::IdentExpr *>(e->base.get());
        auto it_cls = tc_.class_layouts().find(base_id->name);
        if (it_cls == tc_.class_layouts().end()) {
            error_at(e->loc,
                     "lowering: clase desconocida '" + base_id->name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &lay_s = it_cls->second;
        uint32_t s_off = 0;
        Type s_typ = Type{PrimitiveKind::COUNT};
        bool s_ok = false;
        for (const auto &f : lay_s.static_fields) {
            if (f.name == e->field_name) {
                s_off = f.offset;
                s_typ = f.type;
                s_ok = true;
                break;
            }
        }
        if (!s_ok) {
            error_at(e->loc, "lowering: static field '" + e->field_name +
                                 "' no encontrado en la clase '" +
                                 base_id->name + "'");
            return ir::IR_NO_VALUE;
        }
        // AOT (native_poo_): un campo estatico es almacenamiento por-clase,
        // sin ClassRegistry.  Lo mapeamos a un GLOBAL plano (slot static_data
        // unico por <Clase>_<campo>) -> STR_LIT_ADDR + LOAD, igual que un
        // global runtime.  Evita findclass+getstatic (runtime, no bare).
        if (native_poo_) {
            const ir::IrType ir_t = ir_type_from_primitive(s_typ.kind);
            const uint64_t slot = get_or_create_runtime_global_slot(
                "__static_" + base_id->name + "_" + e->field_name, 8);
            ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_addr].is_host_ptr = true;
            {
                ir::IrInstr is{};
                is.op = ir::IrOp::STR_LIT_ADDR;
                is.type = ir::IrType::PTR;
                is.dst = v_addr;
                is.imm = slot;
                is.source_line = e->loc.line;
                emit(current_block_, std::move(is));
            }
            ir::IrValueId v_val = fn_->new_value(
                ir_t == ir::IrType::VOID ? ir::IrType::I64 : ir_t);
            {
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = (ir_t == ir::IrType::VOID) ? ir::IrType::I64 : ir_t;
                ld.dst = v_val;
                ld.operands = {v_addr};
                ld.source_line = e->loc.line;
                emit(current_block_, std::move(ld));
            }
            return v_val;
        }
        // 1) Sprint 5: findclass via IR ops (ALLOCA + STORE + FINDCLASS).
        const uint64_t cname_idx = intern_class_name(*out_mod_, base_id->name);
        const uint32_t cname_len = static_cast<uint32_t>(base_id->name.size());
        const ir::IrValueId v_cls =
            emit_findclass_by_name(cname_idx, cname_len, e->loc.line);
        // 2) getstatic {dst}, {src0}, offset_imm  -> v_val.
        // El opcode lee SIEMPRE 8 bytes (i64).  Para tipos < i64 la
        // semantica de sign/zero-extension coincide porque setstatic
        // almacena los bits high del reg fuente que el productor
        // sign-extendio (LOAD/CONST genericos hacen shl+sar para signed).
        const ir::IrType ir_t = ir_type_from_primitive(s_typ.kind);
        // Emite GETSTATIC IR op; el bytecode lee i64 que truncamos por tipo
        // mas abajo si el SSA val se usa como ancho menor (semantica heredada).
        ir::IrValueId v_val =
            emit_getstatic(v_cls, static_cast<uint64_t>(s_off), e->loc.line);
        // Cast al tipo logico del field si difiere de I64.
        if (ir_t != ir::IrType::I64) {
            v_val = cast_if_needed(v_val, ir::IrType::I64, ir_t, e->loc.line,
                                   /*is_explicit=*/true);
        }
        // Si el tipo del field es PTR host (no VirtualPtr), propagar
        // is_host_ptr al SSA value (mismo tratamiento que field de
        // instancia, ver final de esta funcion).
        // VirtualPtr (s_typ.is_virtual == true) NO recibe is_host_ptr.
        if (s_typ.kind == PrimitiveKind::PTR && !s_typ.is_virtual) {
            fn_->values[v_val].is_host_ptr = true;
        }
        return v_val;
    }

    const Type bt = e->base->result_type;
    if (bt.kind != PrimitiveKind::CLASS) {
        error_at(e->loc,
                 "lowering: '.' sobre tipo no-clase en lower_class_field_load");
        return ir::IR_NO_VALUE;
    }
    auto it = tc_.class_layouts().find(bt.struct_name);
    if (it == tc_.class_layouts().end()) {
        error_at(e->loc,
                 "lowering: clase desconocida '" + bt.struct_name + "'");
        return ir::IR_NO_VALUE;
    }
    const ClassLayout &lay = it->second;
    // si el type checker marco el acceso como propiedad, emitir
    // CALLVIRT al getter `get_<field_name>` en vez de getfield.
    if (e->property_kind == 1) {
        const std::string getter_name = std::string("get_") + e->field_name;
        const ClassMethodInfo *mtd = nullptr;
        for (const auto &m : lay.methods) {
            if (!m.is_constructor && m.name == getter_name) {
                mtd = &m;
                break;
            }
        }
        if (!mtd) {
            error_at(e->loc, "lowering: getter de propiedad '" + e->field_name +
                                 "' no encontrado en la clase '" +
                                 bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ir::IrValueId obj = lower_expr(e->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrType ret_ir = ir_type_from_primitive(mtd->return_type.kind);
        const ir::IrValueId dst = (ret_ir == ir::IrType::VOID)
                                      ? ir::IR_NO_VALUE
                                      : fn_->new_value(ret_ir);
        // Sprint edge-bugs (2026-06-03): si el metodo retorna CLASS, el
        // dst es un host_ptr a un objeto GC.  Marcarlo asi para que el
        // regalloc lo trate como GC-managed (save_live_regs lo convierte
        // a GcHandle antes de cualquier CALL siguiente).  Sin esto un
        // patron `p2 = p1.factory(); p3 = p2.factory();` rompe en interp:
        // el host_ptr de p2 queda stale tras el GC dentro del segundo
        // factory.  Mismo bug con `*->is_host_ptr` no marcado para
        // tipos PTR (e.g. `int* get_buf()`).
        if (dst != ir::IR_NO_VALUE) {
            const PrimitiveKind rk = mtd->return_type.kind;
            if (rk == PrimitiveKind::CLASS) {
                fn_->values[dst].is_host_ptr = true;
                fn_->values[dst].is_gc_object = true;
            } else if ((rk == PrimitiveKind::PTR ||
                        rk == PrimitiveKind::ARRAY) &&
                       !mtd->return_type.is_virtual) {
                fn_->values[dst].is_host_ptr = true;
            }
        }
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLVIRT;
        ins.type = ret_ir;
        ins.dst = dst;
        ins.operands.push_back(obj);
        ins.imm = static_cast<uint64_t>(mtd->vtable_index);
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        return dst;
    }
    uint32_t off = 0;
    bool ok = false;
    Type ftyp = Type{PrimitiveKind::COUNT};
    for (const auto &f : lay.fields) {
        if (f.name == e->field_name) {
            off = f.offset;
            ftyp = f.type;
            ok = true;
            break;
        }
    }
    if (!ok) {
        error_at(e->loc, "lowering: campo '" + e->field_name +
                             "' no encontrado en la clase '" + bt.struct_name +
                             "'");
        return ir::IR_NO_VALUE;
    }
    const ir::IrValueId obj = lower_expr(e->base.get());
    if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
    // Bajar a addr = obj + off (host_ptr) + LOAD estandar.  El emisor
    // IR consulta is_host_ptr y emite movh (memoria host).  Esto evita
    // el patron cur0/gcderef que colisionaba con el regalloc.
    const ir::IrValueId addr =
        emit_field_addr(fn_, current_block_, obj, off, e->loc.line);
    // Campo STRUCT: es INLINE en el payload de la clase (no un puntero).  Su
    // "valor" como agregado ES su DIRECCION (igual que un struct local: el SSA
    // value de un struct es su buffer).  NO hacer LOAD (eso leeria los primeros
    // bytes tratando el campo como puntero -> `obj.s.x` accederia a [[obj+off]]
    // en vez de [obj+off]; en AOT el campo sin inicializar es 0 -> store/deref
    // a NULL -> SIGSEGV).  Devolver la direccion del campo inline.  (Los arrays
    // T[] dinamicos SI son punteros host; los sized inline tienen su propio
    // manejo mas abajo, por eso solo interceptamos STRUCT aqui.)
    if (ftyp.kind == PrimitiveKind::STRUCT) {
        return addr;
    }
    // Campo shared<T> (H5): el campo ES el slot que guarda el host_ptr al ctrl.
    // Su "valor" como shared es la DIRECCION del campo (igual que un struct
    // inline): use_count/ptr_of cargan el ctrl desde [field_addr].  NO hacer
    // LOAD aqui (eso devolveria el ctrl ptr y ptr_of lo trataria como slot).
    if (ftyp.kind == PrimitiveKind::SHARED_PTR) {
        return addr;
    }
    const ir::IrType ir_t = ir_type_from_primitive(ftyp.kind);
    const ir::IrValueId dst = fn_->new_value(ir_t);
    ir::IrInstr ld{};
    ld.op = ir::IrOp::LOAD;
    ld.type = ir_t;
    ld.dst = dst;
    ld.operands = {addr};
    ld.source_line = e->loc.line;
    emit(current_block_, std::move(ld));
    // fix: si el TIPO del campo es PTR (host pointer obtenido
    // via malloc o similar), propagar @c is_host_ptr=true al SSA value
    // resultante para que indexaciones / derefs posteriores emitan
    // @c movh y NO @c mov (que iria a VM memory y leeria garbage).
    // Sin esto, `box.p[i]` con `box.p: i32*` cargaba con `mov` en lugar
    // de `movh`, leyendo memoria virtual VM en vez del buffer host
    // de malloc -> garbage o segfault.
    // EXCEPCION: VirtualPtr<T> (ftyp.is_virtual == true) es una direccion
    // VM aunque el tipo base sea PTR.  El valor cargado es una VA del
    // espacio VM, NO un puntero host.  Marcar is_host_ptr=true sobre un
    // VirtualPtr causaria que `*field` emitiera movh en vez de mov,
    // interpretando la VA como direccion host -> segfault.
    if (ftyp.kind == PrimitiveKind::PTR && !ftyp.is_virtual) {
        fn_->values[dst].is_host_ptr = true;
    }
    // Dynamic arrays `T[]` (size==0) stored as fields also hold host_ptrs
    // (from `new T[N]` via RAW_ALLOC).  Sin esto, `box.data[i] = ...`
    // emitia `mov` (VM mem) en vez de `movh` (host mem) tras LOAD del
    // field -> escribia/leia en vm_mem en una direccion que es realmente
    // host -> valores corrompidos.  Bug bug4-extension.
    // Para arrays dinamicos (`T[]`, array_size==0) el field guarda
    // un host_ptr (de `new T[N]` que usa RAW_ALLOC).  El default de
    // @c Type::make_array es is_virtual=true pero ese flag aplica a
    // arrays SIZED en stack; los dinamicos son siempre host.
    if (ftyp.kind == PrimitiveKind::ARRAY && ftyp.array_size == 0) {
        fn_->values[dst].is_host_ptr = true;
    }
    // Campo de tipo FUNCTION (lambda fn(...), NO cfn): en una CLASE el
    // campo guarda un PTR al slot heap de 16 bytes {fn_addr, env}
    // alocado con RAW_ALLOC (host) -- modelo de closures-en-campos
    // owned (RAII).  Marcar is_host_ptr=true para que, al llamar, las
    // cargas de fn_addr=[slot] y env=[slot+8] emitan movh (memoria
    // host) y NO mov (que iria a vm_mem y leeria basura -> callvmr a
    // una direccion invalida -> cuelgue/crash en VM/JIT).  En AOT todo
    // el espacio es host, por eso solo divergia en VM/JIT.
    // El cfn (fn_is_raw) es la direccion cruda de 8 bytes (CALLIND
    // directo, sin deref de slot), no necesita host-ness aqui.
    if (ftyp.kind == PrimitiveKind::FUNCTION && !ftyp.fn_is_raw) {
        fn_->values[dst].is_host_ptr = true;
    }
    // Campo unique<T> (ownership): el campo guarda la direccion del slot Tier 1
    // (16B) alocado en HEAP (RAW_ALLOC) cuando el unique va a un campo.  Marcar
    // el valor cargado como host_ptr para que ptr_of/read/use_count emitan movh
    // al deref-ear el slot (slot+0 ptr, slot+8 deleter); sin esto leerian
    // vm_mem en una direccion host -> 0/garbage.
    if (ftyp.kind == PrimitiveKind::UNIQUE_PTR) {
        fn_->values[dst].is_host_ptr = true;
    }
    // fix - field de tipo CLASS guarda un GcHandle (estable a
    // evacuacion del GC).  Tras LOADear el handle, hacemos @c gcderef
    // para obtener el host_ptr actual del objeto (refrescado tras
    // cualquier movimiento del GC).  Sin esta refresh, el ptr leido
    // del campo seria stale si el objeto migro a OldGen entre el store
    // y este load -> segfault al hacer @c callvirt o leer fields.
    if (ftyp.kind == PrimitiveKind::CLASS) {
        // raw_asm-elim 2026-05-28: gcderef + xchg -> IrOp::GC_DEREF_HOST.
        ir::IrValueId v_host = fn_->new_value(ir::IrType::I64);
        fn_->values[v_host].is_host_ptr = true;
        fn_->values[v_host].is_gc_object = true;
        ir::IrInstr deref{};
        deref.op = ir::IrOp::GC_DEREF_HOST;
        deref.type = ir::IrType::PTR;
        deref.dst = v_host;
        deref.operands = {dst};
        deref.source_line = e->loc.line;
        emit(current_block_, std::move(deref));
        return v_host;
    }
    return dst;
}

ir::IrValueId Lowering::lower_class_field_store(ast::FieldAccessExpr *target,
                                                ir::IrValueId rhs,
                                                const SourceLoc &loc) {
    if (!target || rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
    // Limitacion G (cerrada): @c property_kind == 3 marca asignacion a
    // static field via @c ClassName.field = v.  Mismo patron que
    // lower_class_field_load: findclass inline + setstatic con offset
    // compile-time.  El base es IdentExpr (nombre de clase), no
    // referenciable como SSA value; resolvemos directo del layout.
    if (target->property_kind == 3) {
        if (!target->base || target->base->kind != ast::NodeKind::IdentExpr) {
            error_at(loc, "lowering: static field store con base no-ClassName");
            return ir::IR_NO_VALUE;
        }
        auto *base_id = static_cast<ast::IdentExpr *>(target->base.get());
        auto it_cls = tc_.class_layouts().find(base_id->name);
        if (it_cls == tc_.class_layouts().end()) {
            error_at(loc,
                     "lowering: clase desconocida '" + base_id->name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &lay_s = it_cls->second;
        uint32_t s_off = 0;
        Type s_typ = Type{PrimitiveKind::COUNT};
        bool s_ok = false;
        for (const auto &f : lay_s.static_fields) {
            if (f.name == target->field_name) {
                s_off = f.offset;
                s_typ = f.type;
                s_ok = true;
                break;
            }
        }
        if (!s_ok) {
            error_at(loc, "lowering: static field '" + target->field_name +
                              "' no encontrado en la clase '" + base_id->name +
                              "'");
            return ir::IR_NO_VALUE;
        }
        // Coerce rhs al tipo del field si difieren.
        const ir::IrType field_ir = ir_type_from_primitive(s_typ.kind);
        const ir::IrValueId rhs_cast =
            cast_if_needed(rhs, fn_->values[rhs].type, field_ir, loc.line);
        // AOT (native_poo_): campo estatico = global plano -> STR_LIT_ADDR +
        // STORE (mismo slot que la lectura: <Clase>_<campo>).
        if (native_poo_) {
            const uint64_t slot = get_or_create_runtime_global_slot(
                "__static_" + base_id->name + "_" + target->field_name, 8);
            ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_addr].is_host_ptr = true;
            {
                ir::IrInstr is{};
                is.op = ir::IrOp::STR_LIT_ADDR;
                is.type = ir::IrType::PTR;
                is.dst = v_addr;
                is.imm = slot;
                is.source_line = loc.line;
                emit(current_block_, std::move(is));
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = field_ir;
            st.operands = {rhs_cast, v_addr};
            st.source_line = loc.line;
            emit(current_block_, std::move(st));
            return rhs_cast;
        }
        // 1) Sprint 5: findclass via IR ops.
        const uint64_t cname_idx = intern_class_name(*out_mod_, base_id->name);
        const uint32_t cname_len = static_cast<uint32_t>(base_id->name.size());
        const ir::IrValueId v_cls =
            emit_findclass_by_name(cname_idx, cname_len, loc.line);
        // 2) setstatic.  Coerce rhs_cast a I64 si fuera necesario.
        ir::IrValueId v_val_i64 = rhs_cast;
        if (fn_->values[rhs_cast].type != ir::IrType::I64) {
            v_val_i64 = cast_if_needed(rhs_cast, fn_->values[rhs_cast].type,
                                       ir::IrType::I64, loc.line,
                                       /*is_explicit=*/true);
        }
        emit_setstatic(v_cls, v_val_i64, static_cast<uint64_t>(s_off),
                       loc.line);
        return rhs_cast;
    }

    const Type bt = target->base->result_type;
    if (bt.kind != PrimitiveKind::CLASS) {
        error_at(
            loc,
            "lowering: '.' sobre tipo no-clase en lower_class_field_store");
        return ir::IR_NO_VALUE;
    }
    auto it = tc_.class_layouts().find(bt.struct_name);
    if (it == tc_.class_layouts().end()) {
        error_at(loc, "lowering: clase desconocida '" + bt.struct_name + "'");
        return ir::IR_NO_VALUE;
    }
    const ClassLayout &lay = it->second;
    // si el type checker marco el target como setter de
    // propiedad, emitir CALLVIRT al setter `set_<field_name>` en vez
    // de setfield.  El rhs se pasa como argumento del setter.
    if (target->property_kind == 2) {
        const std::string setter_name =
            std::string("set_") + target->field_name;
        const ClassMethodInfo *mtd = nullptr;
        for (const auto &m : lay.methods) {
            if (!m.is_constructor && m.name == setter_name) {
                mtd = &m;
                break;
            }
        }
        if (!mtd) {
            error_at(loc, "lowering: setter de propiedad '" +
                              target->field_name +
                              "' no encontrado en la clase '" + bt.struct_name +
                              "'");
            return ir::IR_NO_VALUE;
        }
        const ir::IrValueId obj = lower_expr(target->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrType param_ir =
            mtd->param_types.empty()
                ? ir::IrType::I64
                : ir_type_from_primitive(mtd->param_types.front().kind);
        const ir::IrValueId rhs_cast =
            cast_if_needed(rhs, fn_->values[rhs].type, param_ir, loc.line);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLVIRT;
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.operands.push_back(obj);
        ins.operands.push_back(rhs_cast);
        ins.imm = static_cast<uint64_t>(mtd->vtable_index);
        ins.source_line = loc.line;
        emit(current_block_, std::move(ins));
        return rhs_cast;
    }
    uint32_t off = 0;
    bool ok = false;
    Type ftyp = Type{PrimitiveKind::COUNT};
    for (const auto &f : lay.fields) {
        if (f.name == target->field_name) {
            off = f.offset;
            ftyp = f.type;
            ok = true;
            break;
        }
    }
    if (!ok) {
        error_at(loc, "lowering: campo '" + target->field_name +
                          "' no encontrado en la clase '" + bt.struct_name +
                          "'");
        return ir::IR_NO_VALUE;
    }
    const ir::IrValueId obj = lower_expr(target->base.get());
    if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
    // Reasignar un campo LAMBDA: liberar el slot+env (RAW_ALLOC owned) anterior
    // ANTES de guardar el nuevo, como reasignar un unique<T>.  Sin esto el
    // slot/env viejos se fugarian.  Null-guard interno (campo == 0 -> no libera
    // nada).  El nuevo slot+env (rhs) ya estan alocados y son distintos de los
    // viejos -> sin use-after-free.  Modelo sin GC -- ver
    // doc/VMdoc/Vesta/ClosuresEnCampos.md.
    if (ftyp.kind == PrimitiveKind::FUNCTION && !ftyp.fn_is_raw) {
        emit_free_closure_env_field(obj, off, loc.line);
    }
    const ir::IrValueId addr =
        emit_field_addr(fn_, current_block_, obj, off, loc.line);
    // Reasignar un campo unique<T>: capturamos el slot ANTERIOR (el campo aun
    // lo guarda) ANTES de sobreescribirlo; tras el store del nuevo lo liberamos
    // via CALL al helper __vx_free_uniq (NO inline, para no pegar el diamante
    // del free al tailcall del dtor en este call site -> evita el bucle).  El
    // nuevo slot ya se aloco -> distinto del viejo, sin double-free.
    ir::IrValueId uniq_old_slot = ir::IR_NO_VALUE;
    if (ftyp.kind == PrimitiveKind::UNIQUE_PTR) {
        uniq_old_slot = fn_->new_value(ir::IrType::I64);
        fn_->values[uniq_old_slot].is_host_ptr = true;
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = uniq_old_slot;
        ld.operands = {addr};
        ld.source_line = loc.line;
        emit(current_block_, std::move(ld));
    }
    // Campo STRUCT value-type (Fase 2b/3): el campo es un struct inline; @c rhs
    // es la DIRECCION del struct origen.  Copiamos memberwise (qword-by-qword)
    // sus bytes al campo -- NO un STORE escalar (que pisaria el primer qword
    // con la direccion origen).  move-on-store: el local origen ya esta en
    // @c escaping_locals_ (scan_escaping_locals marca el value de un store a
    // campo) -> su dtor de scope-exit se suprime; solo el dtor augmentado del
    // contenedor libera el recurso (un unico free).  Identico interp/JIT/AOT.
    if (ftyp.kind == PrimitiveKind::STRUCT) {
        uint64_t sz = 8;
        auto it_sl = tc_.struct_layouts().find(ftyp.struct_name);
        if (it_sl != tc_.struct_layouts().end())
            sz = static_cast<uint64_t>(it_sl->second.size_bytes);
        const bool dst_host = fn_->values[addr].is_host_ptr;
        const bool src_host = fn_->values[rhs].is_host_ptr;
        const uint64_t qwords = (sz + 7) / 8;
        for (uint64_t qi = 0; qi < qwords; ++qi) {
            const ir::IrValueId v_off = emit_const(
                ir::IrType::I64, static_cast<int64_t>(qi * 8), loc.line);
            const ir::IrValueId v_src_at = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_src_at].is_host_ptr = src_host;
            {
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_src_at;
                ad.operands = {rhs, v_off};
                ad.source_line = loc.line;
                emit(current_block_, std::move(ad));
            }
            const ir::IrValueId v_word = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD;
                ld.type = ir::IrType::I64;
                ld.dst = v_word;
                ld.operands = {v_src_at};
                ld.source_line = loc.line;
                emit(current_block_, std::move(ld));
            }
            const ir::IrValueId v_dst_at = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_dst_at].is_host_ptr = dst_host;
            {
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD;
                ad.type = ir::IrType::I64;
                ad.dst = v_dst_at;
                ad.operands = {addr, v_off};
                ad.source_line = loc.line;
                emit(current_block_, std::move(ad));
            }
            {
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::I64;
                st.operands = {v_word, v_dst_at};
                st.source_line = loc.line;
                emit(current_block_, std::move(st));
            }
        }
        // Copy-hook (ruta B): si el campo struct declara `__clone__`, este
        // store es una COPIA -> tras el memcpy, `campo.__clone__()` aplica el
        // efecto (p.ej. ++refcount) sobre la copia del campo.  El campo vive en
        // el payload HOST de la clase, asi que usamos el helper (copia a temp
        // VM en interp/JIT; el __clone__ opera sobre el pointee, sin
        // copy-back).  El origen NO se mueve (scan_escaping_locals lo excluye
        // para copy-hook).
        if (it_sl != tc_.struct_layouts().end() &&
            it_sl->second.has_copy_hook) {
            emit_struct_method_on_host_field(
                addr, ftyp.struct_name, ftyp.struct_name + "__" + "__clone__",
                loc.line);
        }
        return rhs;
    }
    // Campo shared<T> (H5): el campo guarda el host_ptr al bloque de control
    // (NO el slot stack del origen, que colgaria).  El store es una COPIA:
    //   1. dec del shared ANTERIOR del campo (free-when-0; no-op si era 0).
    //   2. LOAD ctrl desde [rhs] (rhs = slot del shared origen).
    //   3. STORE ctrl al campo.
    //   4. inc del refcount (el campo es un dueno mas).
    // El origen conserva su propia referencia (no se mueve;
    // scan_escaping_locals lo excluye).  El dtor del contenedor decrementa el
    // campo (dec-on-dtor).
    if (ftyp.kind == PrimitiveKind::SHARED_PTR) {
        // 1. dec del valor anterior del campo (reasignacion sin fuga).
        emit_shared_refcount_dec(addr, loc.line);
        // 2. LOAD ctrl desde el slot del origen.
        const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_ctrl].is_host_ptr = true;
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_ctrl;
            ld.operands = {rhs};
            ld.source_line = loc.line;
            emit(current_block_, std::move(ld));
        }
        // 3. STORE ctrl al campo.
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.operands = {v_ctrl, addr};
            st.source_line = loc.line;
            emit(current_block_, std::move(st));
        }
        // 4. inc del refcount (el campo es un dueno mas).
        emit_shared_refcount_inc(addr, loc.line);
        return rhs;
    }
    const ir::IrType ir_t = ir_type_from_primitive(ftyp.kind);
    const ir::IrValueId rhs_cast =
        cast_if_needed(rhs, fn_->values[rhs].type, ir_t, loc.line);
    // Si el campo es CLASS, almacenamos el GcHandle (estable a evacuacion
    // del GC) en vez del host_ptr crudo.  Sin esto, una alocacion entre
    // el store y el siguiente load podria mover el objeto y dejar el ptr
    // guardado apuntando a memoria liberada/reusada -> segfault al
    // hacer @c callvirt sobre `this.field`.
    ir::IrValueId v_to_store = rhs_cast;
    if (ftyp.kind == PrimitiveKind::CLASS) {
        v_to_store = emit_gc_handle_for_ptr(rhs_cast, loc.line);
    }
    ir::IrInstr st{};
    st.op = ir::IrOp::STORE;
    st.type = ir_t;
    st.dst = ir::IR_NO_VALUE;
    st.operands = {v_to_store, addr};
    st.source_line = loc.line;
    emit(current_block_, std::move(st));
    // Write-barrier generacional old->young.  Al guardar una referencia GC
    // (campo CLASS) en el campo de un objeto que puede ser OLD, registrar el
    // CONTENEDOR en el remembered_set del GC para que el minor_gc encuentre el
    // young alcanzable SOLO via este campo old->young.  Sin el barrier, el
    // nursery preciso perderia ese young (UAF con el GC movible).  Se emite
    // TRAS el STORE del puntero young.  Solo en interp/JIT (comparten el GcHeap
    // con nursery real): en AOT (native_poo_) es NO-OP -- el nursery queda
    // vacio (alloc_pinned -> OldGen) y el major escanea preciso via field-maps
    // -> no se emite.  El contenedor `obj` es un host_ptr; GC_HANDLE_FOR_PTR lo
    // mapea a su GcHandle.  GCWB_IR baja a `gcwb` (interp) o a
    // vrt_gc_write_barrier (JIT); write_barrier() filtra por generacion (skip
    // si el contenedor es YOUNG) -> el remembered_set solo acumula old->young
    // reales.
    if (ftyp.kind == PrimitiveKind::CLASS && !native_poo_) {
        const ir::IrValueId v_cont_handle =
            emit_gc_handle_for_ptr(obj, loc.line);
        ir::IrInstr wb{};
        wb.op = ir::IrOp::GCWB_IR;
        wb.type = ir::IrType::VOID;
        wb.dst = ir::IR_NO_VALUE;
        wb.operands = {v_cont_handle};
        wb.source_line = loc.line;
        emit(current_block_, std::move(wb));
    }
    // Reassign-free del campo unique<T> via CALL al helper (1 instr, sin
    // diamante en el call site).  El helper hace null-guard internamente -> el
    // primer store (campo == 0) es un no-op.
    if (uniq_old_slot != ir::IR_NO_VALUE) {
        needs_free_uniq_helper_ = true;
        ir::IrInstr ci{};
        ci.op = ir::IrOp::CALL;
        ci.type = ir::IrType::VOID;
        ci.dst = ir::IR_NO_VALUE;
        ci.func_name = "__vx_free_uniq";
        ci.operands = {uniq_old_slot};
        ci.source_line = loc.line;
        ci.is_call_site = true;
        emit(current_block_, std::move(ci));
    }
    return rhs_cast;
}


ir::IrValueId Lowering::lower_struct_method_call(ast::CallExpr *e) {
    // s.method(args) sobre un struct value-type.  Bajamos a un CALL
    // directo a <Struct>__<metodo>(struct_addr, [retbuf], args...).
    // El SSA value del struct (fa->base) ES la direccion del buffer:
    // un ALLOCA en VM-stack para `S s;`, o un host_ptr si el struct
    // vive en host memory (malloc / ptr_of).  No tocamos is_host_ptr:
    // el callee recibe el ptr tal cual (el metodo se compila con
    // 'this' en memoria VM por defecto; structs en VM-stack son el
    // caso comun y dominante).
    auto *fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());
    Type bt = fa->base->result_type;
    // @Virtual: `ptr.metodo()` sobre un `Struct*` -> el struct es el pointee, y
    // el `this` es el VALOR del puntero (la direccion del objeto), no la de un
    // ALLOCA.  lower_expr(fa->base) ya da ese valor.
    if (bt.kind == PrimitiveKind::PTR && bt.pointee &&
        bt.pointee->kind == PrimitiveKind::STRUCT)
        bt = *bt.pointee;
    auto it = tc_.struct_layouts().find(bt.struct_name);
    if (it == tc_.struct_layouts().end()) {
        error_at(e->loc,
                 "lowering: struct desconocido '" + bt.struct_name + "'");
        return ir::IR_NO_VALUE;
    }
    const StructLayout &lay = it->second;
    const ClassMethodInfo *mtd = nullptr;
    for (const auto &m : lay.methods) {
        if (m.name == fa->field_name) {
            mtd = &m;
            break;
        }
    }
    if (!mtd) {
        error_at(e->loc, "lowering: metodo '" + fa->field_name +
                             "' no encontrado en struct '" + bt.struct_name +
                             "'");
        return ir::IR_NO_VALUE;
    }

    // Direccion del struct (= this).
    const ir::IrValueId this_addr = lower_expr(fa->base.get());
    if (this_addr == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

    // Bajar argumentos (con auto-promocion de string literales).
    std::vector<ir::IrValueId> arg_vals;
    arg_vals.reserve(e->args.size());
    for (size_t ai = 0; ai < e->args.size(); ++ai) {
        auto &a = e->args[ai];
        if (!a) return ir::IR_NO_VALUE;
        const bool param_is_string =
            ai < mtd->param_types.size() &&
            mtd->param_types[ai].kind == PrimitiveKind::STRING;
        if (param_is_string && a->kind == ast::NodeKind::StringLitExpr) {
            auto *slit = static_cast<ast::StringLitExpr *>(a.get());
            const ir::IrValueId av =
                lower_string_literal_to_string_object(slit);
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
            continue;
        }
        const ir::IrValueId av = lower_expr(a.get());
        if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        arg_vals.push_back(av);
    }

    // SRET: si el metodo devuelve Optional/Result, el caller aloca el
    // retbuf (host_alloca para que callee/caller usen movh) y lo pasa
    // como segundo "arg" (tras this).  El dst del CALL es VOID; el
    // valor visible es el retbuf.
    // native_poo_: un metodo que devuelve `string` value-type usa SRET
    // (simetrico con el callee en lower_struct_methods).  El caller aloca el
    // retbuf de 24 bytes en host-stack y lo pasa tras 'this'.
    // STRUCT por valor: mismo motivo que Optional/Result -- el buffer del
    // struct vive en el frame del callee y muere al RET.  Un `@overlay struct`
    // no: su valor ES un puntero de 8 bytes, va por registro.
    const StructLayout *ret_slay = nullptr;
    if (mtd->return_type.kind == PrimitiveKind::STRUCT &&
        !mtd->return_type.struct_name.empty()) {
        const auto &elays = tc_.enum_layouts();
        if (elays.find(mtd->return_type.struct_name) == elays.end()) {
            auto it_rs =
                tc_.struct_layouts().find(mtd->return_type.struct_name);
            if (it_rs != tc_.struct_layouts().end() &&
                !it_rs->second.is_overlay)
                ret_slay = &it_rs->second;
        }
    }
    const bool method_sret =
        (mtd->return_type.kind == PrimitiveKind::OPTIONAL ||
         mtd->return_type.kind == PrimitiveKind::RESULT ||
         (native_poo_ && mtd->return_type.kind == PrimitiveKind::STRING) ||
         ret_slay != nullptr);
    ir::IrValueId v_retbuf = ir::IR_NO_VALUE;
    if (method_sret) {
        const uint64_t buf_bytes =
            ret_slay != nullptr
                ? ((static_cast<uint64_t>(ret_slay->size_bytes) + 7ULL) & ~7ULL)
            : (mtd->return_type.kind == PrimitiveKind::OPTIONAL)
                ? (uint64_t)optional_buf_bytes(mtd->return_type)
                : 24ULL;
        v_retbuf = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.imm = buf_bytes;
        al.dst = v_retbuf;
        al.host_alloca = true;
        al.source_line = e->loc.line;
        emit(current_block_, std::move(al));
        fn_->values[v_retbuf].is_host_ptr = true;
    }

    const ir::IrType ret_ir_decl =
        ir_type_from_primitive(mtd->return_type.kind);
    const ir::IrType ret_ir = method_sret ? ir::IrType::VOID : ret_ir_decl;
    const ir::IrValueId dst =
        (ret_ir == ir::IrType::VOID) ? ir::IR_NO_VALUE : fn_->new_value(ret_ir);
    if (dst != ir::IR_NO_VALUE) {
        const PrimitiveKind rk = mtd->return_type.kind;
        if (rk == PrimitiveKind::CLASS) {
            fn_->values[dst].is_host_ptr = true;
            fn_->values[dst].is_gc_object = true;
        } else if ((rk == PrimitiveKind::PTR || rk == PrimitiveKind::ARRAY) &&
                   !mtd->return_type.is_virtual) {
            fn_->values[dst].is_host_ptr = true;
        }
    }

    // Operandos del CALL: this_addr, [retbuf], args...
    std::vector<ir::IrValueId> operands;
    operands.reserve(arg_vals.size() + 2);
    operands.push_back(this_addr);
    if (method_sret) operands.push_back(v_retbuf);
    for (auto av : arg_vals)
        operands.push_back(av);

    if (mtd->is_virtual) {
        // @Virtual: dispatch DINAMICO por vtable.  El vptr (offset 0 del
        // objeto) apunta a la vtable del tipo REAL; el slot da la
        // implementacion.  Es correcto tanto por Base* (tipo dinamico) como por
        // valor concreto (el vptr se fija a la vtable del concreto en la
        // construccion).  La devirtualizacion a CALL directo cuando el tipo es
        // estatico y concreto es una optimizacion posterior.
        const uint32_t slot = mtd->vtable_index;
        // %vptr = LOAD [this_addr + 0]  (this es host -> movh recupera el vptr,
        // que es una direccion VM de la vtable en la seccion de codigo).
        const ir::IrValueId v_vptr = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_vptr;
            ld.operands = {this_addr};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        // %fnaddr = %vptr + slot*8  (%vptr es VM -> load de la entrada es mov
        // VM)
        ir::IrValueId v_fnaddr = v_vptr;
        if (slot != 0) {
            const ir::IrValueId v_off =
                emit_const(ir::IrType::I64, (uint64_t)slot * 8u, e->loc.line);
            v_fnaddr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_fnaddr;
            ad.operands = {v_vptr, v_off};
            ad.source_line = e->loc.line;
            emit(current_block_, std::move(ad));
        }
        // %fn = LOAD [%fnaddr]  (cfn: direccion del metodo; slot host -> movh)
        const ir::IrValueId v_fn = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_fn;
            ld.operands = {v_fnaddr};
            ld.source_line = e->loc.line;
            emit(current_block_, std::move(ld));
        }
        // CALLIND %fn(this_addr, [retbuf], args...)
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALLIND;
        ins.type = ret_ir;
        ins.dst = dst;
        ins.func_ptr = v_fn;
        ins.operands = std::move(operands);
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        return method_sret ? v_retbuf : dst;
    }

    ir::IrInstr ins{};
    ins.op = ir::IrOp::CALL;
    ins.type = ret_ir;
    ins.dst = dst;
    // Metodo IMPORTADO cross-module: usar el simbolo real del .velb origen
    // (link_name, p.ej. "std__wideint__u128____div__"); reconstruir
    // "<struct_local>__<metodo>" llevaria el mangling del consumidor y el
    // linker no lo resolveria.  Metodos del propio modulo: link_name vacio ->
    // el label clasico.
    ins.func_name = mtd->link_name.empty()
                        ? (bt.struct_name + "__" + fa->field_name)
                        : mtd->link_name;
    ins.operands = std::move(operands);
    ins.source_line = e->loc.line;
    emit(current_block_, std::move(ins));

    return method_sret ? v_retbuf : dst;
}

// ---------------------------------------------------------------------
// @Virtual: vtable estatica + init del vptr (modelo AOT, structs value-type).
// ---------------------------------------------------------------------

uint64_t Lowering::get_or_emit_struct_vtable(const StructLayout &lay) {
    auto cit = struct_vtable_didx_.find(lay.name);
    if (cit != struct_vtable_didx_.end()) return cit->second;

    // Numero de slots = max(vtable_index)+1 sobre los metodos virtuales.
    uint32_t nslots = 0;
    for (const auto &mi : lay.methods)
        if (mi.is_virtual && mi.vtable_index + 1u > nslots)
            nslots = mi.vtable_index + 1u;
    // Blob de nslots*8 bytes a cero; cada slot recibe una reloc ABS64 al
    // simbolo del metodo (<owner>__<metodo>) que lo ocupa.  El owner es la
    // clase que DEFINE el metodo tras el aplanado (defining_class = este
    // struct, porque el flatten reescribe los heredados con el nombre del
    // derivado -> el override gana su slot con el simbolo del derivado).
    std::vector<uint8_t> vt(static_cast<size_t>(nslots) * 8u, 0);
    const uint64_t idx = out_mod_->static_data.push_back(std::move(vt));
    auto &vm = out_mod_->static_data.meta_at(idx);
    vm.section_name = ".data.rel.ro"; // RELRO como las vtables de C++
    vm.flags |=
        ir::IrModule::SD_FLAG_FORCE_EMIT | ir::IrModule::SD_FLAG_NON_DEDUP;
    for (const auto &mi : lay.methods) {
        if (!mi.is_virtual) continue;
        const std::string owner =
            mi.defining_class.empty() ? lay.name : mi.defining_class;
        ir::IrModule::StaticDataMeta::SymRef sr;
        sr.offset = mi.vtable_index * 8u;
        sr.sym = owner + "__" + mi.name; // reloc datos->codigo
        sr.width = 8;
        sr.is_rel = 0;
        vm.sym_refs.push_back(std::move(sr));
    }
    struct_vtable_didx_[lay.name] = idx;
    return idx;
}

void Lowering::emit_struct_vptr_init(ir::IrValueId struct_addr,
                                     const StructLayout &lay, uint32_t line) {
    if (!lay.is_polymorphic) return;
    const uint64_t vt_idx = get_or_emit_struct_vtable(lay);
    // %vt = &vtable (STR_LIT_ADDR del blob).  La vtable vive en la seccion de
    // CODIGO (direccion VM en interp/JIT; .rodata en AOT), como un string
    // literal -> NO is_host_ptr.  El struct SI es host (host_alloca): el STORE
    // del vptr a [struct_addr+0] usa movh porque struct_addr es host, pero el
    // VALOR guardado (la direccion de la vtable) es VM.  Al leer el vptr
    // (load [struct_addr] = movh) se recupera esa direccion VM, y el load de la
    // entrada (load [vptr] = mov VM) lee la vtable correctamente.
    const ir::IrValueId v_vt = fn_->new_value(ir::IrType::PTR);
    ir::IrInstr sa{};
    sa.op = ir::IrOp::STR_LIT_ADDR;
    sa.type = ir::IrType::PTR;
    sa.dst = v_vt;
    sa.imm = vt_idx;
    sa.source_line = line;
    emit(current_block_, std::move(sa));
    // STORE %vt -> [struct_addr + 0]  (el vptr).
    ir::IrInstr st{};
    st.op = ir::IrOp::STORE;
    st.type = ir::IrType::I64;
    st.dst = ir::IR_NO_VALUE;
    st.operands = {v_vt, struct_addr};
    st.source_line = line;
    emit(current_block_, std::move(st));
}

// ---------------------------------------------------------------------
// Helpers de constantes y casts.
// ---------------------------------------------------------------------

bool Lowering::materialize_comptime_bytes(const std::vector<uint8_t> &bytes,
                                          const StructLayout &layout,
                                          ir::IrValueId v_dst,
                                          uint32_t source_line) {
    if (v_dst == ir::IR_NO_VALUE || bytes.empty()) return false;

    // El valor ES el bloque de memoria que dejo la ejecucion, asi que se copia
    // entero, por palabras.  No se recorren los campos a proposito: mirar la
    // estructura obliga a resolver uniones (varias vistas de los mismos
    // bytes), anidamiento y relleno, y nada de eso cambia lo que hay que
    // copiar.  Un `u256` son cuatro palabras seguidas, se llame como se llame
    // cada trozo por dentro.
    //
    // Lo que si descalifica al tipo es que contenga una direccion: un puntero
    // calculado al compilar apunta a memoria del compilador, que no existe
    // cuando el programa corre.  Eso no se puede trasladar y se dice, en vez
    // de dejar una direccion invalida en el binario.
    for (const auto &f : layout.fields)
        if (f.type.kind == PrimitiveKind::PTR) return false;

    const size_t n = bytes.size();
    for (size_t off = 0; off < n; off += 8) {
        const size_t chunk = (n - off >= 8) ? 8 : (n - off);
        uint64_t raw = 0;
        std::memcpy(&raw, bytes.data() + off, chunk);

        const ir::IrType wt =
            (chunk == 8)
                ? ir::IrType::I64
                : (chunk >= 4
                       ? ir::IrType::I32
                       : (chunk >= 2 ? ir::IrType::I16 : ir::IrType::I8));
        const ir::IrValueId v_val = emit_const(wt, raw, source_line);
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, off, source_line);
        const ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr add{};
        add.op = ir::IrOp::ADD;
        add.type = ir::IrType::PTR;
        add.dst = v_addr;
        add.operands = {v_dst, v_off};
        add.source_line = source_line;
        emit(current_block_, std::move(add));
        fn_->values[v_addr].is_host_ptr = fn_->values[v_dst].is_host_ptr;

        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = wt;
        st.operands = {v_val, v_addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    }
    return true;
}

ir::IrValueId Lowering::emit_const(ir::IrType t, uint64_t imm,
                                   uint32_t source_line) {
    const ir::IrValueId dst = fn_->new_value(t);
    fn_->values[dst].is_const = true;
    fn_->values[dst].const_val = imm;
    ir::IrInstr c{};
    c.op = ir::IrOp::CONST;
    c.type = t;
    c.dst = dst;
    c.imm = imm;
    c.source_line = source_line;
    emit(current_block_, std::move(c));
    return dst;
}

// -----------------------------------------------------------------------
// Helpers para IR ops nativos (anteriormente RAW_ASM).  Migracion 2026-05-23.
// -----------------------------------------------------------------------

ir::IrValueId Lowering::emit_strmake(ir::IrValueId v_buf, ir::IrValueId v_len,
                                     uint32_t source_line) {
    // STRMAKE retorna el GcHandle uint32 zero-extended a i64.  El
    // handle es indice estable en la HandleTable (no se mueve con GC),
    // asi que NO se marca is_gc_object (esa flag indica "host_ptr a
    // payload" que SI se mueve y necesita gcderef en reloads).
    const ir::IrValueId v_str = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::STRMAKE;
    ins.type = ir::IrType::I64;
    ins.dst = v_str;
    ins.operands = {v_buf, v_len};
    ins.is_call_site = true;
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
    return v_str;
}

ir::IrValueId Lowering::emit_string_literal_repr(ir::IrValueId v_addr,
                                                 ir::IrValueId v_len,
                                                 int64_t known_len,
                                                 uint32_t source_line) {
    // native_poo (AOT): value-string nativo (PURE_NATIVE, SSO) en vez de
    // STRMAKE (RUNTIME_DEPENDENT).  Resto de tiers: GcHandle via STRMAKE.
    if (native_poo_)
        return build_native_string_from_buffer(v_addr, v_len, source_line,
                                               known_len);
    return emit_strmake(v_addr, v_len, source_line);
}

// ---------------------------------------------------------------------
// C-3: ruteo del operador `+`/`==` del string built-in a una funcion
// libre marcada con @StringConcat / @StringEq.  Materializa ambos
// operandos al repr `string` adecuado (StringObject handle i64 en Full,
// PTR a value-string en native_poo_) y emite una CALL a la funcion del
// usuario.  La funcion se compila como cualquier otra; solo cambia el
// SITIO de llamada (mecanismo, no politica -- como @AllocatorOverride).
// ---------------------------------------------------------------------
ir::IrValueId
Lowering::emit_string_override_call(const std::string &fn_name, ast::Expr *lhs,
                                    ast::Expr *rhs, ir::IrType ret_ir,
                                    bool negate, uint32_t source_line) {
    // Materializa un operando como `string` para pasarlo por valor a la
    // funcion del usuario.  En native un literal se construye en un slot
    // value-string TEMPORAL (la callee copia los bytes -> se libera tras
    // la CALL); las variables/expresiones devuelven su slot via lower_expr
    // y NO se liberan aqui (su RAII manda).  En Full el literal se
    // promueve a StringObject via STRMAKE; el GC libera el intermedio.
    auto materialize = [&](ast::Expr *ex, bool &is_temp) -> ir::IrValueId {
        is_temp = false;
        if (native_poo_) {
            if (ex && ex->kind == ast::NodeKind::StringLitExpr &&
                !static_cast<ast::StringLitExpr *>(ex)->is_interpolated()) {
                is_temp = true;
                return build_native_string_from_literal(
                    static_cast<ast::StringLitExpr *>(ex), source_line);
            }
            // Concat anidado / cast (string)<char>: slot owned sin RAII ->
            // temporal a liberar tras copiar bytes (igual que el concat).
            if (ex && ex->kind == ast::NodeKind::BinaryExpr &&
                static_cast<ast::BinaryExpr *>(ex)->op == ast::BinOp::Add &&
                ex->result_type.kind == PrimitiveKind::STRING) {
                is_temp = true;
                return lower_expr(ex);
            }
            if (ex && ex->kind == ast::NodeKind::CastExpr &&
                ex->result_type.kind == PrimitiveKind::STRING) {
                is_temp = true;
                return lower_expr(ex);
            }
            return lower_expr(ex);
        }
        // Full: literal -> StringObject handle; var/expr -> handle directo.
        if (ex && ex->kind == ast::NodeKind::StringLitExpr) {
            return lower_string_literal_to_string_object(
                static_cast<ast::StringLitExpr *>(ex));
        }
        return lower_expr(ex);
    };

    bool a_temp = false, b_temp = false;
    ir::IrValueId v_a = materialize(lhs, a_temp);
    ir::IrValueId v_b = materialize(rhs, b_temp);
    if (v_a == ir::IR_NO_VALUE || v_b == ir::IR_NO_VALUE)
        return ir::IR_NO_VALUE;

    // Resolver el label real de la funcion (mismo manejo cross-module que
    // lower_call: si fue importada con mangling, usar el mangled label).
    std::string callee_name = fn_name;
    {
        const FunctionSig *fs = tc_.function_sig_by_name(fn_name);
        if (fs && !fs->mangled_label.empty()) callee_name = fs->mangled_label;
    }

    // Vesta Embed (native_poo_): si el override (@StringConcat) retorna un
    // `string` value-type, su firma IR real es void + retbuf hidden (SRET
    // de 24 bytes).  El call site debe alocar el retbuf, pasarlo PRIMERO,
    // y devolver el retbuf como "valor" del override.  Sin esto, el CALL
    // emite firma i64 (registro) contra una callee void+retbuf -> segfault.
    const bool override_is_str_sret =
        native_poo_ &&
        (fn_returns_str_value_.find(fn_name) != fn_returns_str_value_.end());
    if (override_is_str_sret) {
        const ir::IrValueId v_retbuf = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_retbuf].is_host_ptr = true;
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_retbuf;
        al.imm = 24; // value-string {ptr,len,cap}
        al.host_alloca = true;
        al.source_line = source_line;
        emit(current_block_, std::move(al));
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALL;
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.func_name = std::move(callee_name);
        ins.operands = {v_retbuf, v_a, v_b};
        ins.source_line = source_line;
        emit(current_block_, std::move(ins));
        // Liberar operandos temporales (bytes ya copiados por la callee).
        // Inc 5 (SSO): solo libera si estaba en HEAP.
        if (a_temp) emit_native_str_free_if_heap(v_a, source_line);
        if (b_temp) emit_native_str_free_if_heap(v_b, source_line);
        // El "valor" del override es el retbuf (PTR al value-string).
        return v_retbuf;
    }

    const ir::IrValueId v_ret = fn_->new_value(ret_ir);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::CALL;
    ins.type = ret_ir;
    ins.dst = v_ret;
    ins.func_name = std::move(callee_name);
    ins.operands = {v_a, v_b};
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));

    // Liberar los operandos LITERAL/expr-temporales en native (sus bytes
    // ya estan copiados por la callee).  Inc 5 (SSO): solo libera si
    // estaba en HEAP.  Las variables NO se liberan aqui.
    if (native_poo_) {
        if (a_temp) emit_native_str_free_if_heap(v_a, source_line);
        if (b_temp) emit_native_str_free_if_heap(v_b, source_line);
    }

    // Para `!=` sobre @StringEq: negar el bool resultante.
    if (negate && ret_ir == ir::IrType::BOOL) {
        const ir::IrValueId v_zero =
            emit_const(ir::IrType::BOOL, 0, source_line);
        const ir::IrValueId v_neg = fn_->new_value(ir::IrType::BOOL);
        ir::IrInstr cmp{};
        cmp.op = ir::IrOp::CMP_EQ; // (resultado == false) => negacion
        cmp.type = ir::IrType::BOOL;
        cmp.dst = v_neg;
        cmp.operands = {v_ret, v_zero};
        cmp.source_line = source_line;
        emit(current_block_, std::move(cmp));
        return v_neg;
    }
    return v_ret;
}

ir::IrValueId Lowering::emit_strcat(ir::IrValueId v_a, ir::IrValueId v_b,
                                    uint32_t source_line) {
    const ir::IrValueId v_str = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::STRCAT;
    ins.type = ir::IrType::I64;
    ins.dst = v_str;
    ins.operands = {v_a, v_b};
    ins.is_call_site = true;
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
    return v_str;
}

ir::IrValueId Lowering::emit_strraw(ir::IrValueId v_str, uint32_t source_line) {
    // STRRAW devuelve host_ptr al buffer data[] del StringObject.
    // Es PTR-typed con is_host_ptr=true para que LOAD/STORE posteriores
    // emitan movh (memoria host) en vez de mov (memoria VM).
    const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_ptr].is_host_ptr = true;
    ir::IrInstr ins{};
    ins.op = ir::IrOp::STRRAW;
    ins.type = ir::IrType::PTR;
    ins.dst = v_ptr;
    ins.operands = {v_str};
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
    return v_ptr;
}

ir::IrValueId Lowering::emit_strconv(ir::IrValueId v_str, uint64_t enc_imm,
                                     uint32_t source_line) {
    // AOT (native_poo): el value-string es canonicamente UTF-8 -> un `string`
    // ES una secuencia de code-points (no de bytes con un tag de encoding).
    // str_convert preserva los code-points: deep-copy del value-string (los
    // mismos bytes UTF-8).  str_length(resultado) = cplen (code-points) ->
    // correcto.  El encoding concreto solo importa en la frontera FFI, donde se
    // usa str_wstr (UTF-16) / str_raw (bytes) sobre el resultado.  El enc_imm
    // es advisory en este modelo.
    if (native_poo_) {
        (void)enc_imm;
        const ir::IrValueId v_ptr =
            emit_native_str_data_ptr(v_str, source_line);
        const ir::IrValueId v_blen = emit_native_str_len(v_str, source_line);
        return build_native_string_from_buffer(v_ptr, v_blen, source_line);
    }
    // VM/JIT: STRCONV retorna GcHandle del nuevo StringObject re-encoded.
    const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::STRCONV;
    ins.type = ir::IrType::I64;
    ins.dst = v_dst;
    ins.operands = {v_str};
    ins.imm = enc_imm;
    ins.is_call_site = true;
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
    return v_dst;
}

ir::IrValueId Lowering::emit_strgetbytes(ir::IrValueId v_str,
                                         uint32_t source_line) {
    const ir::IrValueId v_n = fn_->new_value(ir::IrType::U64);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::STRGETBYTES;
    ins.type = ir::IrType::U64;
    ins.dst = v_n;
    ins.operands = {v_str};
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
    return v_n;
}

// -----------------------------------------------------------------------
// Vesta Embed Inc 0: string value-type (solo native_poo_).
//
// Repr: struct de 24 bytes en stack (ALLOCA) { u8* ptr; u64 len; u64 cap }.
// HEAP-ALWAYS (sin SSO todavia): incluso "hi" aloca un buffer.  El valor
// SSA de una variable `string` es el PTR al slot de 24 bytes, igual que un
// struct value-type (ver lower_ident: STRUCT/ARRAY devuelven lookup()).
// -----------------------------------------------------------------------

void Lowering::store_slot_fields_prestado(ir::IrValueId v_slot,
                                          ir::IrValueId v_buf, uint64_t len,
                                          uint32_t source_line) {
    auto store_at = [&](uint64_t off, ir::IrValueId v_val, ir::IrType ty) {
        ir::IrValueId v_addr = v_slot;
        if (off > 0) {
            ir::IrValueId v_off = emit_const(ir::IrType::I64, off, source_line);
            v_addr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_addr].is_host_ptr = fn_->values[v_slot].is_host_ptr;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_addr;
            ad.operands = {v_slot, v_off};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
        }
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ty;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };
    store_at(0, v_buf, ir::IrType::I64);
    store_at(8, emit_const(ir::IrType::I64, len, source_line), ir::IrType::I64);
    // Capacidad 0: no hay sitio libre detras, cualquier escritura tiene que
    // copiar antes.  byte[23] = 0xC0 -> bit 7 (los datos estan detras del
    // puntero) + bit 6 (prestado).
    store_at(16, emit_const(ir::IrType::I64, 0, source_line), ir::IrType::I64);
    store_at(23, emit_const(ir::IrType::U8, 0xC0, source_line), ir::IrType::U8);
}

ir::IrValueId
Lowering::build_native_string_from_literal(ast::StringLitExpr *slit,
                                           uint32_t source_line) {
    // El literal ya tiene su contenido resuelto (UTF-8 raw, sin nul) en
    // @c slit->value.  len = numero de bytes; cap = len + 1 (el nul).
    const std::string &lit = slit->value;
    const uint64_t len = static_cast<uint64_t>(lit.size());
    const uint64_t cap = len + 1; // +1 para el nul terminador.

    // 1. Slot de 24 bytes en stack (ALLOCA host).  En native_poo_/AOT el
    //    value-string vive en HOST stack: su ptr debe ser un host_ptr
    //    coherente en TODO el programa (se pasa a funciones que lo leen
    //    via `movh`, se copia al retbuf SRET host).  Un ALLOCA VM-stack
    //    (`[rbx+regs]`) daria un VM-addr que al cruzar a un callee
    //    host_ptr se leeria como host -> segfault.  host_alloca=true +
    //    is_host_ptr=true mantienen la coherencia.
    const ir::IrValueId v_slot = fn_->new_value(ir::IrType::PTR);
    if (native_poo_) fn_->values[v_slot].is_host_ptr = true;
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_slot;
        al.imm = 24;
        al.host_alloca = native_poo_;
        al.source_line = source_line;
        emit(current_block_, std::move(al));
    }
    // String Inc 5 (SSO): zero-init el slot (bytes no usados definidos).
    emit_zero_native_str_slot(v_slot, source_line);

    // Helpers comunes (STORE empaquetado al buf en una direccion off).
    auto buf_store_at = [&](ir::IrValueId v_base, uint64_t off, uint64_t val,
                            ir::IrType ty) {
        ir::IrValueId v_dst = v_base;
        if (off != 0) {
            ir::IrValueId v_off = emit_const(ir::IrType::I64, off, source_line);
            v_dst = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_dst].is_host_ptr = true;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_dst;
            ad.operands = {v_base, v_off};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
        }
        ir::IrValueId v_val = emit_const(ty, val, source_line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ty;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_dst};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };
    auto pack = [&](const std::vector<uint8_t> &data, uint64_t pos,
                    int n) -> uint64_t {
        uint64_t v = 0;
        for (int k = 0; k < n; ++k)
            v |= static_cast<uint64_t>(data[pos + k]) << (8 * k);
        return v;
    };
    // Helper: escribir `data` (incluye el nul final) byte-a-byte agrupado
    // en qwords/dword/word/byte a partir de @p v_base + base_off.
    auto write_packed = [&](ir::IrValueId v_base, uint64_t base_off,
                            const std::vector<uint8_t> &data) {
        uint64_t pos = 0;
        const uint64_t total_w = data.size();
        // En un target de 32 bits un inmediato de 64 no existe: agrupar de
        // ocho en ocho producia una constante que no cabe en un registro, el
        // encoder la truncaba y el literal salia recortado.  Alli se agrupa de
        // cuatro en cuatro.
        const uint64_t paso = (asm_target_bits_ == 32) ? 4u : 8u;
        for (; pos + paso <= total_w; pos += paso)
            buf_store_at(v_base, base_off + pos,
                         pack(data, pos, static_cast<int>(paso)),
                         paso == 8 ? ir::IrType::I64 : ir::IrType::I32);
        if (pos + 4 <= total_w) {
            buf_store_at(v_base, base_off + pos, pack(data, pos, 4),
                         ir::IrType::I32);
            pos += 4;
        }
        if (pos + 2 <= total_w) {
            buf_store_at(v_base, base_off + pos, pack(data, pos, 2),
                         ir::IrType::I16);
            pos += 2;
        }
        if (pos + 1 <= total_w) {
            buf_store_at(v_base, base_off + pos, pack(data, pos, 1),
                         ir::IrType::U8);
            pos += 1;
        }
    };

    // String Inc 5 (SSO): si el literal cabe inline (<= 22 bytes), lo
    // construimos en SSO -> CERO malloc.  Escribimos data + nul INLINE en
    // bytes[0..len] y el len en byte[23] (flag SSO=0 en el bit alto).
    if (len <= 22) {
        std::vector<uint8_t> data(lit.begin(), lit.end());
        data.push_back(0); // nul; data.size() == len+1, cabe en [0..22].
        write_packed(v_slot, 0, data);
        // qword2 = (len << 56): byte[23]=len (SSO), bytes 16..22=0.
        emit_str_meta_sso(v_slot, emit_const(ir::IrType::I64, len, source_line),
                          source_line);
        return v_slot;
    }

    // --- Literal largo (> 22 bytes): VISTA sobre .rodata ---
    //
    // El contenido ya esta en el binario, en datos de solo lectura; copiarlo a
    // memoria pedida al asignador solo para poder leerlo era pagar dos veces
    // por lo mismo -- y arrastraba el asignador entero a cualquier programa
    // que mencionara una cadena, cosa que un modulo freestanding como vx_io no
    // puede permitirse.  Ahora el slot APUNTA al literal y se marca prestado:
    // nadie lo libera y nadie escribe encima (quien vaya a escribir lo copia
    // antes, ver build_native_string_append_inplace).
    //
    // El literal se interna con su nul para que `cstr()` valga tal cual.
    (void)cap;
    const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_buf].is_host_ptr = true;
    {
        ir::IrInstr sa{};
        sa.op = ir::IrOp::STR_LIT_ADDR;
        sa.type = ir::IrType::PTR;
        sa.dst = v_buf;
        sa.imm = intern_string_literal_nul(*out_mod_, lit);
        sa.source_line = source_line;
        emit(current_block_, std::move(sa));
    }
    store_slot_fields_prestado(v_slot, v_buf, len, source_line);
    return v_slot;

    // 3 + 4. Escribir el contenido del literal + nul final al buffer.
    //    El contenido es CONOCIDO en compile-time -> emitimos STOREs
    //    desempaquetados de constantes (sin .rodata, sin loop, sin
    //    LOAD): el nul terminador es el byte (len) del buffer, asi que
    //    escribimos `cap = len+1` bytes (literal + nul) agrupados en
    //    qwords (8B), luego dword (4B), word (2B) y byte (1B) de cola.
    //    Cada STORE es de un CONST empaquetado en little-endian.  ~8x
    //    menos STOREs que byte-a-byte para literales largos; cero
    //    branches (totalmente desenrollado).  Todas las ops son
    //    PURE_NATIVE (CONST + ADD + STORE).
    {
        // Bytes a escribir = el literal seguido del nul terminador.
        std::vector<uint8_t> data(lit.begin(), lit.end());
        data.push_back(0); // nul final; data.size() == cap.

        // Helper: dst = v_buf + off (host_ptr).  off==0 -> v_buf directo.
        auto buf_at = [&](uint64_t off) -> ir::IrValueId {
            if (off == 0) return v_buf;
            ir::IrValueId v_off = emit_const(ir::IrType::I64, off, source_line);
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_dst].is_host_ptr = true;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_dst;
            ad.operands = {v_buf, v_off};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
            return v_dst;
        };
        // Helper: STORE de un valor empaquetado de `w` bytes (1/2/4/8) en
        // buf[off].  val ya viene empaquetado little-endian.
        auto store_chunk = [&](uint64_t off, uint64_t val, ir::IrType ty) {
            ir::IrValueId v_dst = buf_at(off);
            ir::IrValueId v_val = emit_const(ty, val, source_line);
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ty;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_val, v_dst};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        };
        // Empaquetar `n` bytes de data[pos..] en un entero little-endian.
        auto pack = [&](uint64_t pos, int n) -> uint64_t {
            uint64_t v = 0;
            for (int k = 0; k < n; ++k)
                v |= static_cast<uint64_t>(data[pos + k]) << (8 * k);
            return v;
        };

        uint64_t pos = 0;
        const uint64_t total_w = data.size(); // = cap
        // Qwords (8B) -- dwords si el target es de 32 bits, donde un inmediato
        // de 64 no cabe en un registro (ver la nota del otro empaquetador).
        const uint64_t paso = (asm_target_bits_ == 32) ? 4u : 8u;
        for (; pos + paso <= total_w; pos += paso)
            store_chunk(pos, pack(pos, static_cast<int>(paso)),
                        paso == 8 ? ir::IrType::I64 : ir::IrType::I32);
        // Dword (4B).
        if (pos + 4 <= total_w) {
            store_chunk(pos, pack(pos, 4), ir::IrType::I32);
            pos += 4;
        }
        // Word (2B).
        if (pos + 2 <= total_w) {
            store_chunk(pos, pack(pos, 2), ir::IrType::I16);
            pos += 2;
        }
        // Byte (1B).
        if (pos + 1 <= total_w) {
            store_chunk(pos, pack(pos, 1), ir::IrType::U8);
            pos += 1;
        }
    }

    // 5. Escribir los 3 campos del slot: ptr@0, len@8, cap@16.
    auto store_field = [&](uint64_t off, ir::IrValueId v_val) {
        ir::IrValueId v_addr = v_slot;
        if (off > 0) {
            ir::IrValueId v_off = emit_const(ir::IrType::I64, off, source_line);
            v_addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_addr;
            ad.operands = {v_slot, v_off};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
        }
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };
    store_field(0, v_buf);
    store_field(8, emit_const(ir::IrType::I64, len, source_line));
    // qword2 = cap (bytes 16..22) | flag HEAP (byte[23]=0x80), un solo i64.
    emit_str_meta_heap(v_slot, emit_const(ir::IrType::I64, cap, source_line),
                       source_line);

    return v_slot;
}

ir::IrValueId Lowering::build_native_string_from_char(ir::IrValueId v_char,
                                                      uint32_t source_line) {
    // Vesta Embed: cast (string)<char> -> value-string de UN caracter.
    // String Inc 5 (SSO): un solo char (len=1 <= 22) SIEMPRE es SSO ->
    // CERO malloc.  La data inline en bytes[0..1]: byte[0]=char, byte[1]=
    // nul, byte[23]=1 (flag SSO=0).

    // 1. Slot de 24 bytes en stack.
    const ir::IrValueId v_slot = fn_->new_value(ir::IrType::PTR);
    if (native_poo_) fn_->values[v_slot].is_host_ptr = true;
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_slot;
        al.imm = 24;
        al.host_alloca = native_poo_;
        al.source_line = source_line;
        emit(current_block_, std::move(al));
    }
    emit_zero_native_str_slot(v_slot, source_line);

    auto ptr_add = [&](uint64_t off) -> ir::IrValueId {
        if (off == 0) return v_slot;
        ir::IrValueId v_off = emit_const(ir::IrType::I64, off, source_line);
        ir::IrValueId v_dst = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_dst].is_host_ptr = fn_->values[v_slot].is_host_ptr;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_dst;
        ad.operands = {v_slot, v_off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v_dst;
    };
    auto store_u8 = [&](ir::IrValueId v_addr, ir::IrValueId v_val) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::U8;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };

    // byte[0] = char.
    store_u8(v_slot, v_char);
    // byte[1] = nul.
    store_u8(ptr_add(1), emit_const(ir::IrType::U8, 0, source_line));
    // qword2 = (1 << 56): byte[23]=1 (SSO len 1), bytes 16..22=0.
    emit_str_meta_sso(v_slot, emit_const(ir::IrType::I64, 1, source_line),
                      source_line);

    return v_slot;
}

ir::IrValueId Lowering::build_native_string_concat(ir::IrValueId v_a,
                                                   ir::IrValueId v_b,
                                                   uint32_t source_line) {
    // Vesta Embed Inc 1: a + b -> nuevo string owned.  String Inc 5 (SSO):
    // si el total cabe inline (<= 22) construye SSO (cero malloc); si no,
    // HEAP.  v_a / v_b son PTR a slots value-string (no se consumen);
    // leemos su (ptr, len) via los accesores flag-aware.  Branch real
    // porque el malloc debe ser condicional.

    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_addr].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr;
        ad.operands = {base, off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v_addr;
    };
    auto emit_memcpy = [&](ir::IrValueId dst, ir::IrValueId src,
                           ir::IrValueId len) {
        // CPU dispatch (Inc 2): en native (AOT) el memcpy va por la tabla de
        // punteros (variante elegida por cpuid al arranque).  En interp/JIT/
        // Full sigue siendo MEMCPY inline (rep movsb), sin cambio.
        if (native_poo_) {
            emit_memcpy_dispatched(dst, src, len, source_line);
            return;
        }
        ir::IrInstr mc{};
        mc.op = ir::IrOp::MEMCPY;
        mc.type = ir::IrType::I8;
        mc.dst = ir::IR_NO_VALUE;
        mc.operands = {dst, src, len};
        mc.source_line = source_line;
        emit(current_block_, std::move(mc));
    };
    auto store_at = [&](ir::IrValueId addr, ir::IrValueId val, ir::IrType ty) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ty;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };

    // 1. (ptr, len) de ambos operandos via accesores flag-aware.
    ir::IrValueId v_a_ptr = emit_native_str_data_ptr(v_a, source_line);
    ir::IrValueId v_a_len = emit_native_str_len(v_a, source_line);
    ir::IrValueId v_b_ptr = emit_native_str_data_ptr(v_b, source_line);
    ir::IrValueId v_b_len = emit_native_str_len(v_b, source_line);

    // 2. total = la + lb.
    ir::IrValueId v_total = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_total;
        ad.operands = {v_a_len, v_b_len};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }

    // 3. Slot de 24 bytes del resultado.
    const ir::IrValueId v_slot = fn_->new_value(ir::IrType::PTR);
    if (native_poo_) fn_->values[v_slot].is_host_ptr = true;
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_slot;
        al.imm = 24;
        al.host_alloca = native_poo_;
        al.source_line = source_line;
        emit(current_block_, std::move(al));
    }
    emit_zero_native_str_slot(v_slot, source_line);

    // 4. cond = (total > 22) -> HEAP.
    ir::IrValueId v_22 = emit_const(ir::IrType::I64, 22, source_line);
    ir::IrValueId v_cond = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr c{};
        c.op = ir::IrOp::CMP_GT;
        c.type = ir::IrType::I64;
        c.dst = v_cond;
        c.operands = {v_total, v_22};
        c.source_line = source_line;
        emit(current_block_, std::move(c));
    }
    const ir::IrBlockId heap_bb = fn_->new_block("concat_heap");
    const ir::IrBlockId sso_bb = fn_->new_block("concat_sso");
    const ir::IrBlockId merge_bb = fn_->new_block("concat_merge");
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands.push_back(v_cond);
        br.target_block = heap_bb;
        br.false_block = sso_bb;
        br.source_line = source_line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(heap_bb);
        fn_->blocks[current_block_].succs.push_back(sso_bb);
        fn_->blocks[heap_bb].preds.push_back(current_block_);
        fn_->blocks[sso_bb].preds.push_back(current_block_);
    }

    // --- HEAP: total > 22 ---
    current_block_ = heap_bb;
    {
        ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, source_line);
        ir::IrValueId v_cap = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_cap;
            ad.operands = {v_total, v_one};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
        }
        ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_buf].is_host_ptr = true;
        {
            ir::IrInstr ra{};
            ra.op = ir::IrOp::RAW_ALLOC;
            ra.type = ir::IrType::PTR;
            ra.dst = v_buf;
            ra.operands = {v_cap};
            ra.source_line = source_line;
            emit(current_block_, std::move(ra));
        }
        emit_memcpy(v_buf, v_a_ptr, v_a_len);
        emit_memcpy(ptr_add(v_buf, v_a_len), v_b_ptr, v_b_len);
        store_at(ptr_add(v_buf, v_total),
                 emit_const(ir::IrType::U8, 0, source_line), ir::IrType::U8);
        store_at(v_slot, v_buf, ir::IrType::I64);
        store_at(ptr_add(v_slot, emit_const(ir::IrType::I64, 8, source_line)),
                 v_total, ir::IrType::I64);
        // qword2 = cap | flag HEAP (un solo i64).
        emit_str_meta_heap(v_slot, v_cap, source_line);
        ir::IrInstr br{};
        br.op = ir::IrOp::BR;
        br.target_block = merge_bb;
        br.source_line = source_line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(merge_bb);
        fn_->blocks[merge_bb].preds.push_back(current_block_);
    }

    // --- SSO: total <= 22 ---
    current_block_ = sso_bb;
    {
        emit_memcpy(v_slot, v_a_ptr, v_a_len);
        emit_memcpy(ptr_add(v_slot, v_a_len), v_b_ptr, v_b_len);
        store_at(ptr_add(v_slot, v_total),
                 emit_const(ir::IrType::U8, 0, source_line), ir::IrType::U8);
        // qword2 = (total << 56): byte[23]=total (SSO).
        emit_str_meta_sso(v_slot, v_total, source_line);
        ir::IrInstr br{};
        br.op = ir::IrOp::BR;
        br.target_block = merge_bb;
        br.source_line = source_line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(merge_bb);
        fn_->blocks[merge_bb].preds.push_back(current_block_);
    }

    current_block_ = merge_bb;
    return v_slot;
}

ir::IrValueId Lowering::build_native_string_slice(ir::IrValueId v_src,
                                                  ir::IrValueId v_lo,
                                                  ir::IrValueId v_hi,
                                                  bool inclusive,
                                                  uint32_t source_line) {
    // String Inc 3: `s[a..b]` (exclusivo) o `s[a..=b]` (inclusivo) ->
    // NUEVO string owned con la copia de los bytes [a, b) (o [a, b]).
    // Repr value-string {ptr@0,len@8,cap@16} en stack + buffer fresco en
    // heap.  La copia usa MEMCPY (rep movsb).  Todas las ops son
    // PURE_NATIVE/LIBC (RAW_ALLOC=malloc, MEMCPY=rep movsb).  v1 asume
    // indices validos (a <= b <= src.len); indices negativos / OOB no
    // soportados (mismo contrato que el resto del AOT bare).
    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_addr].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr;
        ad.operands = {base, off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v_addr;
    };
    auto emit_sub = [&](ir::IrValueId a, ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr s{};
        s.op = ir::IrOp::SUB;
        s.type = ir::IrType::I64;
        s.dst = v;
        s.operands = {a, b};
        s.source_line = source_line;
        emit(current_block_, std::move(s));
        return v;
    };
    auto emit_add = [&](ir::IrValueId a, ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v;
        ad.operands = {a, b};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v;
    };

    // 1. Cargar el data_ptr del slot fuente via accesor flag-aware (SSO
    //    o HEAP).  Los limites a/b ya estan en regs.
    ir::IrValueId v_src_ptr = emit_native_str_data_ptr(v_src, source_line);

    // 2. len = b - a  (o  b - a + 1  si es `..=` inclusivo).
    ir::IrValueId v_len = emit_sub(v_hi, v_lo);
    if (inclusive) {
        ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, source_line);
        v_len = emit_add(v_len, v_one);
    }

    // 3. Slot de 24 bytes del resultado.
    const ir::IrValueId v_slot = fn_->new_value(ir::IrType::PTR);
    if (native_poo_) fn_->values[v_slot].is_host_ptr = true;
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_slot;
        al.imm = 24;
        al.host_alloca = native_poo_;
        al.source_line = source_line;
        emit(current_block_, std::move(al));
    }
    emit_zero_native_str_slot(v_slot, source_line);

    // 4. src_off = src.data + a.  String Inc 5 (SSO): finalize decide
    //    SSO vs HEAP runtime (cero malloc si el slice cabe inline).
    ir::IrValueId v_src_off = ptr_add(v_src_ptr, v_lo);
    build_native_string_finalize(v_slot, v_src_off, v_len, source_line);

    return v_slot;
}

ir::IrValueId Lowering::build_native_string_index_char(ir::IrValueId v_src,
                                                       ir::IrValueId v_idx,
                                                       uint32_t source_line) {
    // String Inc 3: `s[i]` -> el byte en la posicion i del value-string.
    // String Inc 5 (SSO): data_ptr via accesor flag-aware + LOAD u8 de
    // [data+i].  El resultado es un U8 zero-extended (0-255) que el type
    // checker marca como char.  v1 asume i valido (0 <= i < src.len).
    ir::IrValueId v_ptr = emit_native_str_data_ptr(v_src, source_line);
    // addr = ptr + i (host_ptr).
    ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_addr].is_host_ptr = true;
    {
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr;
        ad.operands = {v_ptr, v_idx};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }
    // LOAD u8: el codegen zero-extiende el byte a un registro completo.
    ir::IrValueId v_byte = fn_->new_value(ir::IrType::U8);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::U8;
        ld.dst = v_byte;
        ld.operands = {v_addr};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
    }
    return v_byte;
}

void Lowering::build_native_string_finalize(ir::IrValueId v_slot,
                                            ir::IrValueId v_src_ptr,
                                            ir::IrValueId v_len,
                                            uint32_t source_line,
                                            int64_t known_len) {
    // String Inc 5 (SSO): rellena el slot value-string ya alocado (24
    // bytes) decidiendo SSO vs HEAP segun la longitud.  Si len <= 22 -> SSO
    // (data inline, cero malloc); si len > 22 -> HEAP (RAW_ALLOC + MEMCPY +
    // ptr@0/len@8/cap@16 + flag).  Cada rama finaliza COMPLETAMENTE el slot
    // -> no necesita PHI.  Si @p known_len >= 0 (Tier B str_make) la decision
    // es COMPILE-TIME -> se emite solo el cuerpo aplicable, SIN rama runtime.
    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v;
        ad.operands = {base, off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v;
    };
    auto emit_memcpy = [&](ir::IrValueId dst, ir::IrValueId src,
                           ir::IrValueId len) {
        // CPU dispatch (Inc 2): native -> tabla de punteros; interp/JIT/Full
        // -> MEMCPY inline (rep movsb).
        if (native_poo_) {
            emit_memcpy_dispatched(dst, src, len, source_line);
            return;
        }
        ir::IrInstr mc{};
        mc.op = ir::IrOp::MEMCPY;
        mc.type = ir::IrType::I8;
        mc.dst = ir::IR_NO_VALUE;
        mc.operands = {dst, src, len};
        mc.source_line = source_line;
        emit(current_block_, std::move(mc));
    };
    auto store_at = [&](ir::IrValueId addr, ir::IrValueId val, ir::IrType ty) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ty;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };

    // Cuerpos SSO/HEAP como lambdas (emiten en current_block_, SIN el BR final
    // -> el caller decide si hay rama o no).  Reusados por la rama runtime y
    // por el fast-path const (Tier B).
    auto fill_heap = [&]() {
        // cap = len + 1.
        ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, source_line);
        ir::IrValueId v_cap = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_cap;
            ad.operands = {v_len, v_one};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
        }
        // buf = RAW_ALLOC(cap).
        ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_buf].is_host_ptr = true;
        {
            ir::IrInstr ra{};
            ra.op = ir::IrOp::RAW_ALLOC;
            ra.type = ir::IrType::PTR;
            ra.dst = v_buf;
            ra.operands = {v_cap};
            ra.source_line = source_line;
            emit(current_block_, std::move(ra));
        }
        // MEMCPY buf <- src (len bytes).
        emit_memcpy(v_buf, v_src_ptr, v_len);
        // nul en buf[len].
        store_at(ptr_add(v_buf, v_len),
                 emit_const(ir::IrType::U8, 0, source_line), ir::IrType::U8);
        // Campos: ptr@0 = buf, len@8 = len, qword2 = cap | flag HEAP.
        store_at(v_slot, v_buf, ir::IrType::I64);
        store_at(ptr_add(v_slot, emit_const(ir::IrType::I64, 8, source_line)),
                 v_len, ir::IrType::I64);
        emit_str_meta_heap(v_slot, v_cap, source_line);
    };
    auto fill_sso = [&]() {
        // MEMCPY slot <- src (len bytes; data inline en bytes[0..len)).
        // v_slot es PTR al inicio del struct; lo usamos como dst directo.
        emit_memcpy(v_slot, v_src_ptr, v_len);
        // nul en slot[len].
        store_at(ptr_add(v_slot, v_len),
                 emit_const(ir::IrType::U8, 0, source_line), ir::IrType::U8);
        // qword2 = (len << 56): byte[23]=len (SSO).
        emit_str_meta_sso(v_slot, v_len, source_line);
    };

    // Tier B (str_make con len constante): decision compile-time, SIN rama.
    if (known_len >= 0) {
        if (known_len <= 22)
            fill_sso();
        else
            fill_heap();
        return;
    }

    // Tier C (len runtime): rama CMP_GT(len, 22) -> heap_bb / sso_bb -> merge.
    ir::IrValueId v_22 = emit_const(ir::IrType::I64, 22, source_line);
    ir::IrValueId v_cond = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr c{};
        c.op = ir::IrOp::CMP_GT;
        c.type = ir::IrType::I64;
        c.dst = v_cond;
        c.operands = {v_len, v_22};
        c.source_line = source_line;
        emit(current_block_, std::move(c));
    }

    const ir::IrBlockId heap_bb = fn_->new_block("strfin_heap");
    const ir::IrBlockId sso_bb = fn_->new_block("strfin_sso");
    const ir::IrBlockId merge_bb = fn_->new_block("strfin_merge");
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands.push_back(v_cond);
        br.target_block = heap_bb;
        br.false_block = sso_bb;
        br.source_line = source_line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(heap_bb);
        fn_->blocks[current_block_].succs.push_back(sso_bb);
        fn_->blocks[heap_bb].preds.push_back(current_block_);
        fn_->blocks[sso_bb].preds.push_back(current_block_);
    }
    auto close_to_merge = [&]() {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR;
        br.target_block = merge_bb;
        br.source_line = source_line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(merge_bb);
        fn_->blocks[merge_bb].preds.push_back(current_block_);
    };
    current_block_ = heap_bb;
    fill_heap();
    close_to_merge();
    current_block_ = sso_bb;
    fill_sso();
    close_to_merge();
    current_block_ = merge_bb;
}

ir::IrValueId Lowering::build_native_string_from_buffer(ir::IrValueId v_ptr,
                                                        ir::IrValueId v_len,
                                                        uint32_t source_line,
                                                        int64_t known_len) {
    // str_make: COPIA known/runtime len bytes de v_ptr a un value-string
    // PROPIO (sin GC, RAII).  Slot 24B + finalize (SSO/HEAP; copia dispatched).
    const ir::IrValueId v_slot = fn_->new_value(ir::IrType::PTR);
    if (native_poo_) fn_->values[v_slot].is_host_ptr = true;
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_slot;
        al.imm = 24;
        al.host_alloca = native_poo_;
        al.source_line = source_line;
        emit(current_block_, std::move(al));
    }
    emit_zero_native_str_slot(v_slot, source_line);
    build_native_string_finalize(v_slot, v_ptr, v_len, source_line, known_len);
    return v_slot;
}

void Lowering::build_native_string_append_inplace(ir::IrValueId v_dst_slot,
                                                  ir::IrValueId v_app_ptr,
                                                  ir::IrValueId v_app_len,
                                                  uint32_t source_line) {
    // Vesta Embed Inc 2: `s += t` (y append de interpolacion).  Muta el
    // value-string apuntado por @p v_dst_slot in place.  String Inc 5
    // (SSO): branch en new_len > 22.  Como un HEAP nunca decrece
    // (new_len >= old_len), HEAP solo transiciona a HEAP; SSO puede
    // crecer SSO->SSO (cero malloc, data inline) o SSO->HEAP (alocar +
    // copiar la data inline al heap).  El free del buffer viejo solo se
    // hace si el slot estaba en HEAP (la data SSO es inline, no se libera).
    // Todas las ops son PURE_NATIVE/LIBC.
    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_addr].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr;
        ad.operands = {base, off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v_addr;
    };
    auto emit_memcpy = [&](ir::IrValueId dst, ir::IrValueId src,
                           ir::IrValueId len) {
        // CPU dispatch (Inc 2): native -> tabla de punteros; interp/JIT/Full
        // -> MEMCPY inline (rep movsb).
        if (native_poo_) {
            emit_memcpy_dispatched(dst, src, len, source_line);
            return;
        }
        ir::IrInstr mc{};
        mc.op = ir::IrOp::MEMCPY;
        mc.type = ir::IrType::I8;
        mc.dst = ir::IR_NO_VALUE;
        mc.operands = {dst, src, len};
        mc.source_line = source_line;
        emit(current_block_, std::move(mc));
    };
    auto store_at = [&](ir::IrValueId addr, ir::IrValueId val, ir::IrType ty) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ty;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };

    // 1. Estado actual del slot via accesores flag-aware.  v_old_data es
    //    el host_ptr a los bytes (SSO -> &slot; HEAP -> ptr@0).
    ir::IrValueId v_old_data =
        emit_native_str_data_ptr(v_dst_slot, source_line);
    ir::IrValueId v_old_len = emit_native_str_len(v_dst_slot, source_line);
    // 2. new_len = old_len + app_len ; new_cap = new_len + 1.
    ir::IrValueId v_new_len = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_new_len;
        ad.operands = {v_old_len, v_app_len};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }
    ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, source_line);
    ir::IrValueId v_new_cap = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_new_cap;
        ad.operands = {v_new_len, v_one};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }

    // 3. Branch new_len > 22.  HEAP nunca decrece (new_len >= old_len) ->
    //    HEAP solo transiciona a HEAP; SSO crece SSO->SSO (cero malloc,
    //    data inline) o SSO->HEAP.  El free del buffer viejo se hace en la
    //    rama HEAP (solo si era HEAP).
    ir::IrValueId v_22 = emit_const(ir::IrType::I64, 22, source_line);
    ir::IrValueId v_cond = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr c{};
        c.op = ir::IrOp::CMP_GT;
        c.type = ir::IrType::I64;
        c.dst = v_cond;
        c.operands = {v_new_len, v_22};
        c.source_line = source_line;
        emit(current_block_, std::move(c));
    }
    const ir::IrBlockId heap_bb = fn_->new_block("append_heap");
    const ir::IrBlockId sso_bb = fn_->new_block("append_sso");
    const ir::IrBlockId merge_bb = fn_->new_block("append_merge");
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands.push_back(v_cond);
        br.target_block = heap_bb;
        br.false_block = sso_bb;
        br.source_line = source_line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(heap_bb);
        fn_->blocks[current_block_].succs.push_back(sso_bb);
        fn_->blocks[heap_bb].preds.push_back(current_block_);
        fn_->blocks[sso_bb].preds.push_back(current_block_);
    }

    // --- HEAP: new_len > 22 (SSO->HEAP o HEAP->HEAP) ---
    current_block_ = heap_bb;
    {
        ir::IrValueId v_new_buf = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_new_buf].is_host_ptr = true;
        {
            ir::IrInstr ra{};
            ra.op = ir::IrOp::RAW_ALLOC;
            ra.type = ir::IrType::PTR;
            ra.dst = v_new_buf;
            ra.operands = {v_new_cap};
            ra.source_line = source_line;
            emit(current_block_, std::move(ra));
        }
        // Copiar lo viejo (old_data: inline o heap) + lo nuevo ANTES de
        // liberar y de tocar los campos.
        emit_memcpy(v_new_buf, v_old_data, v_old_len);
        emit_memcpy(ptr_add(v_new_buf, v_old_len), v_app_ptr, v_app_len);
        store_at(ptr_add(v_new_buf, v_new_len),
                 emit_const(ir::IrType::U8, 0, source_line), ir::IrType::U8);
        // Liberar el buffer viejo SOLO si era HEAP (lee el flag/ptr0 del
        // slot AQUI, antes de los stores).  new_buf es malloc fresco que
        // nunca aliasa el viejo -> sin doble-free.
        emit_native_str_free_if_heap(v_dst_slot, source_line);
        store_at(v_dst_slot, v_new_buf, ir::IrType::I64);
        store_at(
            ptr_add(v_dst_slot, emit_const(ir::IrType::I64, 8, source_line)),
            v_new_len, ir::IrType::I64);
        // qword2 = cap | flag HEAP (un solo i64).
        emit_str_meta_heap(v_dst_slot, v_new_cap, source_line);
        ir::IrInstr br{};
        br.op = ir::IrOp::BR;
        br.target_block = merge_bb;
        br.source_line = source_line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(merge_bb);
        fn_->blocks[merge_bb].preds.push_back(current_block_);
    }

    // --- SSO: new_len <= 22 (siempre SSO->SSO; la data vieja ya esta
    //     inline en slot[0..old_len), solo appendeamos lo nuevo) ---
    current_block_ = sso_bb;
    {
        // app -> slot[old_len..old_len+app_len).  old_data == &slot, asi
        // que la data vieja ya esta en su sitio; solo copiamos lo nuevo.
        emit_memcpy(ptr_add(v_dst_slot, v_old_len), v_app_ptr, v_app_len);
        store_at(ptr_add(v_dst_slot, v_new_len),
                 emit_const(ir::IrType::U8, 0, source_line), ir::IrType::U8);
        // qword2 = (new_len << 56): byte[23]=new_len (SSO).
        emit_str_meta_sso(v_dst_slot, v_new_len, source_line);
        ir::IrInstr br{};
        br.op = ir::IrOp::BR;
        br.target_block = merge_bb;
        br.source_line = source_line;
        emit(current_block_, std::move(br));
        fn_->blocks[current_block_].succs.push_back(merge_bb);
        fn_->blocks[merge_bb].preds.push_back(current_block_);
    }

    current_block_ = merge_bb;
}

ir::IrValueId Lowering::emit_native_itoa_to_buf(ir::IrValueId v_buf,
                                                ir::IrValueId v_val,
                                                bool is_signed,
                                                uint32_t source_line) {
    // Vesta Embed Inc 2: itoa decimal INLINE (sin helper nativo, AOT bare no
    // tiene plugin).  Escribe la representacion ASCII de v_val (I64) en
    // v_buf (host, >= 24 bytes garantizados por el caller) y devuelve la
    // longitud escrita (sin nul).
    //
    // Algoritmo:
    //   1. Si is_signed y val < 0: emitir '-', negar val (abs).  Trabajamos
    //      con un magnitude unsigned a partir de aqui.
    //   2. Caso val==0: escribir '0', len=1.
    //   3. Loop: extraer digitos por mod 10 (val % 10 + '0') a un buffer
    //      temporal en orden inverso, val /= 10, hasta val==0.
    //   4. Loop de inversion: copiar los digitos del temporal al v_buf en
    //      el orden correcto.
    // Para evitar PHIs complejos, usamos slots ALLOCA (mem2reg los promueve
    // en O2) para val, write index, y el buffer temporal de digitos.
    // Todas las ops PURE_NATIVE (ALLOCA/LOAD/STORE/DIV/MOD/ADD/SUB/CMP/BR).

    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v;
        ad.operands = {base, off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v;
    };
    auto new_slot = [&](uint64_t bytes) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        // En native_poo_/AOT los ALLOCA viven en HOST stack (host_alloca):
        // sin esto el slot daria un VM-addr que el codegen native trata
        // como host -> LOAD/STORE leerian basura.  is_host_ptr mantiene
        // la coherencia de los LOAD/STORE posteriores.
        if (native_poo_) fn_->values[v].is_host_ptr = true;
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v;
        al.imm = bytes;
        al.host_alloca = native_poo_;
        al.source_line = source_line;
        emit(current_block_, std::move(al));
        return v;
    };
    auto load_i64 = [&](ir::IrValueId addr) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v;
        ld.operands = {addr};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
        return v;
    };
    auto store_i64 = [&](ir::IrValueId addr, ir::IrValueId val) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };
    auto store_byte = [&](ir::IrValueId addr, ir::IrValueId val) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::U8;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };
    auto bin = [&](ir::IrOp op, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr in{};
        in.op = op;
        in.type = ir::IrType::I64;
        in.dst = v;
        in.operands = {a, b};
        in.source_line = source_line;
        emit(current_block_, std::move(in));
        return v;
    };
    auto new_block = [&]() -> ir::IrBlockId { return fn_->new_block(); };
    auto br = [&](ir::IrBlockId target) {
        ir::IrInstr b{};
        b.op = ir::IrOp::BR;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.target_block = target;
        b.source_line = source_line;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(target);
        fn_->blocks[target].preds.push_back(current_block_);
    };
    auto br_cond = [&](ir::IrValueId cond, ir::IrBlockId t_true,
                       ir::IrBlockId t_false) {
        ir::IrInstr b{};
        b.op = ir::IrOp::BR_COND;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.operands = {cond};
        b.target_block = t_true;
        b.false_block = t_false;
        b.source_line = source_line;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(t_true);
        fn_->blocks[current_block_].succs.push_back(t_false);
        fn_->blocks[t_true].preds.push_back(current_block_);
        fn_->blocks[t_false].preds.push_back(current_block_);
    };
    auto v_one_helper = [&](uint32_t) -> ir::IrValueId {
        return emit_const(ir::IrType::I64, 1, source_line);
    };
    auto cmp = [&](ir::IrOp op, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr in{};
        in.op = op;
        in.type = ir::IrType::I64;
        in.dst = v;
        in.operands = {a, b};
        in.source_line = source_line;
        emit(current_block_, std::move(in));
        return v;
    };

    // Slots: mag (magnitud unsigned), tmp_buf (digitos inversos, 24B),
    //        di (indice de escritura en tmp), out_len (longitud final),
    //        out_pos (indice de escritura en v_buf).
    ir::IrValueId s_mag = new_slot(8);
    ir::IrValueId s_tmp = new_slot(24);
    ir::IrValueId s_di = new_slot(8);
    ir::IrValueId s_pos = new_slot(8);

    ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, source_line);
    ir::IrValueId v_ten = emit_const(ir::IrType::I64, 10, source_line);
    ir::IrValueId v_z48 = emit_const(ir::IrType::I64, 48, source_line); // '0'

    store_i64(s_pos, v_zero); // out_pos = 0
    store_i64(s_di, v_zero);  // di = 0

    // --- Manejo del signo ---
    // Bloques: bb_neg (val<0), bb_setmag (mag = val o -val), join.
    if (is_signed) {
        ir::IrValueId is_neg = cmp(ir::IrOp::CMP_LT, v_val, v_zero); // signed <
        uint32_t bb_neg = new_block();
        uint32_t bb_pos = new_block();
        uint32_t bb_after_sign = new_block();
        br_cond(is_neg, bb_neg, bb_pos);
        // bb_neg: escribir '-' en v_buf[0], pos=1, mag = 0 - val.
        current_block_ = bb_neg;
        {
            ir::IrValueId v_minus =
                emit_const(ir::IrType::I64, 45, source_line);
            store_byte(v_buf, v_minus);
            store_i64(s_pos, v_one_helper(source_line));
            ir::IrValueId v_negmag = bin(ir::IrOp::SUB, v_zero, v_val);
            store_i64(s_mag, v_negmag);
        }
        br(bb_after_sign);
        // bb_pos: mag = val.
        current_block_ = bb_pos;
        store_i64(s_mag, v_val);
        br(bb_after_sign);
        current_block_ = bb_after_sign;
    } else {
        store_i64(s_mag, v_val);
    }

    // --- Caso especial mag == 0 ---
    // Bloques: bb_zero ('0', di=1), bb_digits (loop de extraccion), bb_inv.
    ir::IrValueId v_mag0 = load_i64(s_mag);
    ir::IrValueId is_zero = cmp(ir::IrOp::CMP_EQ, v_mag0, v_zero);
    uint32_t bb_zero = new_block();
    uint32_t bb_loop_hdr = new_block();
    uint32_t bb_after_digits = new_block();
    br_cond(is_zero, bb_zero, bb_loop_hdr);

    // bb_zero: tmp[0]='0', di=1.
    current_block_ = bb_zero;
    {
        store_byte(s_tmp, v_z48);
        store_i64(s_di, v_one_helper(source_line));
    }
    br(bb_after_digits);

    // bb_loop_hdr: while (mag != 0) { tmp[di++] = mag%10+'0'; mag/=10; }
    current_block_ = bb_loop_hdr;
    {
        ir::IrValueId v_mag = load_i64(s_mag);
        ir::IrValueId cont = cmp(ir::IrOp::CMP_NE, v_mag, v_zero);
        uint32_t bb_body = new_block();
        br_cond(cont, bb_body, bb_after_digits);
        // bb_body.
        current_block_ = bb_body;
        {
            ir::IrValueId v_m = load_i64(s_mag);
            // DIV/MOD: el signo lo determina el tipo IR del resultado.
            // Para signed ya trabajamos con magnitud positiva (cabe en
            // i64 salvo INT64_MIN); usamos I64.  Para unsigned usamos U64
            // para cubrir todo el rango u64 (valores con bit 63 alto).
            const ir::IrType dm_ty =
                is_signed ? ir::IrType::I64 : ir::IrType::U64;
            ir::IrValueId v_rem = fn_->new_value(dm_ty);
            {
                ir::IrInstr in{};
                in.op = ir::IrOp::MOD;
                in.type = dm_ty;
                in.dst = v_rem;
                in.operands = {v_m, v_ten};
                in.source_line = source_line;
                emit(current_block_, std::move(in));
            }
            ir::IrValueId v_digit = bin(ir::IrOp::ADD, v_rem, v_z48);
            ir::IrValueId v_di = load_i64(s_di);
            ir::IrValueId v_tmp_at = ptr_add(s_tmp, v_di);
            store_byte(v_tmp_at, v_digit);
            ir::IrValueId v_di1 =
                bin(ir::IrOp::ADD, v_di, v_one_helper(source_line));
            store_i64(s_di, v_di1);
            ir::IrValueId v_div = fn_->new_value(dm_ty);
            {
                ir::IrInstr in{};
                in.op = ir::IrOp::DIV;
                in.type = dm_ty;
                in.dst = v_div;
                in.operands = {v_m, v_ten};
                in.source_line = source_line;
                emit(current_block_, std::move(in));
            }
            store_i64(s_mag, v_div);
        }
        br(bb_loop_hdr);
    }

    // bb_after_digits: invertir tmp[0..di) -> v_buf[pos..pos+di).
    current_block_ = bb_after_digits;
    {
        // out_pos ya tiene 0 (positivo) o 1 (signo) escrito; los digitos
        // en tmp estan en orden inverso (menos significativo primero).
        // Copiar tmp[di-1], tmp[di-2], ..., tmp[0] a v_buf[pos], pos+1, ...
        // Usamos un indice src que decrece desde di-1 hasta 0.
        ir::IrValueId v_di_final = load_i64(s_di);
        // src = di - 1.
        ir::IrValueId s_src = new_slot(8);
        ir::IrValueId v_src0 =
            bin(ir::IrOp::SUB, v_di_final, v_one_helper(source_line));
        store_i64(s_src, v_src0);
        uint32_t bb_inv_hdr = new_block();
        br(bb_inv_hdr);
        // bb_inv_hdr: while (src >= 0) { v_buf[pos++] = tmp[src]; src--; }
        current_block_ = bb_inv_hdr;
        {
            ir::IrValueId v_src = load_i64(s_src);
            ir::IrValueId cont = cmp(ir::IrOp::CMP_GE, v_src, v_zero); // signed
            uint32_t bb_inv_body = new_block();
            uint32_t bb_done = new_block();
            br_cond(cont, bb_inv_body, bb_done);
            // body.
            current_block_ = bb_inv_body;
            {
                ir::IrValueId v_src_b = load_i64(s_src);
                ir::IrValueId v_tmp_at = ptr_add(s_tmp, v_src_b);
                // LOAD u8 del digito.
                ir::IrValueId v_d = fn_->new_value(ir::IrType::I64);
                {
                    ir::IrInstr ld{};
                    ld.op = ir::IrOp::LOAD;
                    ld.type = ir::IrType::U8;
                    ld.dst = v_d;
                    ld.operands = {v_tmp_at};
                    ld.source_line = source_line;
                    emit(current_block_, std::move(ld));
                }
                ir::IrValueId v_pos = load_i64(s_pos);
                ir::IrValueId v_dst_at = ptr_add(v_buf, v_pos);
                store_byte(v_dst_at, v_d);
                ir::IrValueId v_pos1 =
                    bin(ir::IrOp::ADD, v_pos, v_one_helper(source_line));
                store_i64(s_pos, v_pos1);
                ir::IrValueId v_src1 =
                    bin(ir::IrOp::SUB, v_src_b, v_one_helper(source_line));
                store_i64(s_src, v_src1);
            }
            br(bb_inv_hdr);
            current_block_ = bb_done;
        }
    }

    // len final = out_pos.
    return load_i64(s_pos);
}

std::string Lowering::ensure_itoa_helper(bool is_signed) {
    // Vesta Embed Inc 2: emite (una vez por modulo + signedness) el helper
    // itoa como funcion IR independiente.  Firma:
    //   i64 __vx_itoa_{s|u}(u8* buf, i64 val)
    // El cuerpo reutiliza emit_native_itoa_to_buf, que construye los loops
    // de extraccion/inversion sobre fn_/current_block_.  Al vivir en una
    // funcion APARTE con varios bloques:
    //   (a) el const-fold del optimizer NO foldea el itoa mid-expression
    //       (el bug de length erronea con argumento constante);
    //   (b) el inliner NO lo re-inlinea (is_inlineable exige 1 bloque).
    const int idx = is_signed ? 1 : 0;
    const std::string name = is_signed ? "__vx_itoa_s" : "__vx_itoa_u";
    if (itoa_helper_emitted_[idx]) return name;
    itoa_helper_emitted_[idx] = true;

    // Guardar el contexto del lowering en curso.
    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    // Construir el helper.  Params: buf (host_ptr), val (i64).
    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::I64;
    const ir::IrValueId p_buf = hf.new_value(ir::IrType::PTR, "%buf");
    hf.values[p_buf].is_param = true;
    hf.values[p_buf].is_host_ptr = true;
    hf.params.push_back(p_buf);
    const ir::IrValueId p_val = hf.new_value(ir::IrType::I64, "%val");
    hf.values[p_val].is_param = true;
    hf.params.push_back(p_val);
    const ir::IrBlockId e = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;

    // El itoa escribe en buf y devuelve la longitud.
    ir::IrValueId v_len =
        emit_native_itoa_to_buf(p_buf, p_val, is_signed, /*source_line=*/0);

    // ret len.
    {
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::I64;
        rt.dst = ir::IR_NO_VALUE;
        rt.operands = {v_len};
        rt.source_line = 0;
        emit(current_block_, std::move(rt));
    }

    // Restaurar el contexto del padre antes de mover el helper al modulo.
    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));
    return name;
}

std::string Lowering::ensure_btoa_helper() {
    // Vesta Embed Inc 2: helper bool->string nativo (una vez por modulo).
    //   i64 __vx_btoa(u8* buf, i64 b)
    //     if (b != 0) { buf <- "true";  ret 4; }
    //     else        { buf <- "false"; ret 5; }
    // Vive en una funcion APARTE con branch -> el optimizer no foldea el
    // append condicional mid-expression con argumento constante.
    const std::string name = "__vx_btoa";
    if (btoa_helper_emitted_) return name;
    btoa_helper_emitted_ = true;

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::I64;
    const ir::IrValueId p_buf = hf.new_value(ir::IrType::PTR, "%buf");
    hf.values[p_buf].is_param = true;
    hf.values[p_buf].is_host_ptr = true;
    hf.params.push_back(p_buf);
    const ir::IrValueId p_b = hf.new_value(ir::IrType::I64, "%b");
    hf.values[p_b].is_param = true;
    hf.params.push_back(p_b);
    const ir::IrBlockId e = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;

    // Helper: STORE empaquetado de los bytes de `s` en buf[off..].
    auto write_bytes = [&](const std::string &s) {
        std::vector<uint8_t> data(s.begin(), s.end());
        auto ptr_add = [&](ir::IrValueId base, uint64_t off) -> ir::IrValueId {
            if (off == 0) return base;
            ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
            fn_->values[v].is_host_ptr = true;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v;
            ad.operands = {base, emit_const(ir::IrType::I64, off, 0)};
            ad.source_line = 0;
            emit(current_block_, std::move(ad));
            return v;
        };
        auto store_chunk = [&](uint64_t off, uint64_t val, ir::IrType ty) {
            ir::IrValueId v_dst = ptr_add(p_buf, off);
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ty;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {emit_const(ty, val, 0), v_dst};
            st.source_line = 0;
            emit(current_block_, std::move(st));
        };
        auto pack = [&](uint64_t pos, int n) -> uint64_t {
            uint64_t v = 0;
            for (int k = 0; k < n; ++k)
                v |= static_cast<uint64_t>(data[pos + k]) << (8 * k);
            return v;
        };
        const uint64_t plen = data.size();
        uint64_t pos = 0;
        for (; pos + 4 <= plen; pos += 4)
            store_chunk(pos, pack(pos, 4), ir::IrType::I32);
        if (pos + 2 <= plen) {
            store_chunk(pos, pack(pos, 2), ir::IrType::I16);
            pos += 2;
        }
        if (pos + 1 <= plen) {
            store_chunk(pos, pack(pos, 1), ir::IrType::U8);
            pos += 1;
        }
    };
    auto ret_len = [&](uint64_t len) {
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::I64;
        rt.dst = ir::IR_NO_VALUE;
        rt.operands = {emit_const(ir::IrType::I64, len, 0)};
        rt.source_line = 0;
        emit(current_block_, std::move(rt));
    };

    // if (b != 0) -> bb_true ; else -> bb_false.
    ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, 0);
    ir::IrValueId v_cond = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr in{};
        in.op = ir::IrOp::CMP_NE;
        in.type = ir::IrType::I64;
        in.dst = v_cond;
        in.operands = {p_b, v_zero};
        in.source_line = 0;
        emit(current_block_, std::move(in));
    }
    ir::IrBlockId bb_true = fn_->new_block("btoa_true");
    ir::IrBlockId bb_false = fn_->new_block("btoa_false");
    {
        ir::IrInstr b{};
        b.op = ir::IrOp::BR_COND;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.operands = {v_cond};
        b.target_block = bb_true;
        b.false_block = bb_false;
        b.source_line = 0;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(bb_true);
        fn_->blocks[current_block_].succs.push_back(bb_false);
        fn_->blocks[bb_true].preds.push_back(current_block_);
        fn_->blocks[bb_false].preds.push_back(current_block_);
    }
    current_block_ = bb_true;
    write_bytes("true");
    ret_len(4);
    current_block_ = bb_false;
    write_bytes("false");
    ret_len(5);

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));
    return name;
}

std::string Lowering::ensure_ctoa_helper() {
    // BUG-3: helper codepoint -> UTF-8 nativo (una vez por modulo).
    //   i64 __vx_ctoa(u8* buf, i64 cp)
    //     cp < 0x80    -> 1 byte;  cp < 0x800   -> 2 bytes;
    //     cp < 0x10000 -> 3 bytes; else         -> 4 bytes.
    // Paridad byte-exacta con vio_char_to_vmbuf (interp/JIT).  Vive en una
    // funcion APARTE con branches -> evita const-fold mid-expression.
    const std::string name = "__vx_ctoa";
    if (ctoa_helper_emitted_) return name;
    ctoa_helper_emitted_ = true;

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::I64;
    const ir::IrValueId p_buf = hf.new_value(ir::IrType::PTR, "%buf");
    hf.values[p_buf].is_param = true;
    hf.values[p_buf].is_host_ptr = true;
    hf.params.push_back(p_buf);
    const ir::IrValueId p_cp = hf.new_value(ir::IrType::I64, "%cp");
    hf.values[p_cp].is_param = true;
    hf.params.push_back(p_cp);
    const ir::IrBlockId e = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;

    // Helpers locales de emision de instrucciones aritmeticas/bit.
    auto emit_bin = [&](ir::IrOp op, ir::IrValueId a,
                        ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr in{};
        in.op = op;
        in.type = ir::IrType::I64;
        in.dst = v;
        in.operands = {a, b};
        in.source_line = 0;
        emit(current_block_, std::move(in));
        return v;
    };
    auto shr = [&](ir::IrValueId a, uint64_t k) -> ir::IrValueId {
        return emit_bin(ir::IrOp::SHR, a, emit_const(ir::IrType::I64, k, 0));
    };
    auto andc = [&](ir::IrValueId a, uint64_t k) -> ir::IrValueId {
        return emit_bin(ir::IrOp::AND, a, emit_const(ir::IrType::I64, k, 0));
    };
    auto orc = [&](ir::IrValueId a, uint64_t k) -> ir::IrValueId {
        return emit_bin(ir::IrOp::OR, a, emit_const(ir::IrType::I64, k, 0));
    };
    auto store_u8_at = [&](uint64_t off, ir::IrValueId v_val) {
        ir::IrValueId v_dst = p_buf;
        if (off != 0) {
            v_dst = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_dst].is_host_ptr = true;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_dst;
            ad.operands = {p_buf, emit_const(ir::IrType::I64, off, 0)};
            ad.source_line = 0;
            emit(current_block_, std::move(ad));
        }
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::U8;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_dst};
        st.source_line = 0;
        emit(current_block_, std::move(st));
    };
    auto ret_len = [&](uint64_t len) {
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::I64;
        rt.dst = ir::IR_NO_VALUE;
        rt.operands = {emit_const(ir::IrType::I64, len, 0)};
        rt.source_line = 0;
        emit(current_block_, std::move(rt));
    };
    // cond = (cp u< limit) -> branch a bb_then, si no a bb_else.
    auto branch_ult = [&](uint64_t limit, ir::IrBlockId bb_then,
                          ir::IrBlockId bb_else) {
        ir::IrValueId v_cond = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr in{};
            in.op = ir::IrOp::CMP_ULT;
            in.type = ir::IrType::I64;
            in.dst = v_cond;
            in.operands = {p_cp, emit_const(ir::IrType::I64, limit, 0)};
            in.source_line = 0;
            emit(current_block_, std::move(in));
        }
        ir::IrInstr b{};
        b.op = ir::IrOp::BR_COND;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.operands = {v_cond};
        b.target_block = bb_then;
        b.false_block = bb_else;
        b.source_line = 0;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(bb_then);
        fn_->blocks[current_block_].succs.push_back(bb_else);
        fn_->blocks[bb_then].preds.push_back(current_block_);
        fn_->blocks[bb_else].preds.push_back(current_block_);
    };

    ir::IrBlockId bb1 = fn_->new_block("ctoa_1");
    ir::IrBlockId bb_ge1 = fn_->new_block("ctoa_ge1");
    ir::IrBlockId bb2 = fn_->new_block("ctoa_2");
    ir::IrBlockId bb_ge2 = fn_->new_block("ctoa_ge2");
    ir::IrBlockId bb3 = fn_->new_block("ctoa_3");
    ir::IrBlockId bb4 = fn_->new_block("ctoa_4");

    // if (cp < 0x80) -> 1 byte, else -> ge1.
    branch_ult(0x80, bb1, bb_ge1);
    // 1 byte: buf[0]=cp; ret 1.
    current_block_ = bb1;
    store_u8_at(0, p_cp);
    ret_len(1);
    // ge1: if (cp < 0x800) -> 2 bytes, else -> ge2.
    current_block_ = bb_ge1;
    branch_ult(0x800, bb2, bb_ge2);
    // 2 bytes: buf[0]=0xC0|(cp>>6); buf[1]=0x80|(cp&0x3F); ret 2.
    current_block_ = bb2;
    store_u8_at(0, orc(shr(p_cp, 6), 0xC0));
    store_u8_at(1, orc(andc(p_cp, 0x3F), 0x80));
    ret_len(2);
    // ge2: if (cp < 0x10000) -> 3 bytes, else -> 4 bytes.
    current_block_ = bb_ge2;
    branch_ult(0x10000, bb3, bb4);
    // 3 bytes.
    current_block_ = bb3;
    store_u8_at(0, orc(shr(p_cp, 12), 0xE0));
    store_u8_at(1, orc(andc(shr(p_cp, 6), 0x3F), 0x80));
    store_u8_at(2, orc(andc(p_cp, 0x3F), 0x80));
    ret_len(3);
    // 4 bytes.
    current_block_ = bb4;
    store_u8_at(0, orc(shr(p_cp, 18), 0xF0));
    store_u8_at(1, orc(andc(shr(p_cp, 12), 0x3F), 0x80));
    store_u8_at(2, orc(andc(shr(p_cp, 6), 0x3F), 0x80));
    store_u8_at(3, orc(andc(p_cp, 0x3F), 0x80));
    ret_len(4);

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));
    return name;
}

// ---------------------------------------------------------------------
// CPU dispatch (cimiento): global __vx_cpu_features + helper __vx_cpu_init.
//
// Detecta las features de la CPU via `cpuid` UNA VEZ al arranque (el wiring
// prepone `call __vx_cpu_init` al entry de main, solo native_poo_) y guarda
// un bitmask en el slot static_data del global.  El builtin cpu_features()
// lo lee (STR_LIT_ADDR + LOAD).  Sienta la base del despacho de helpers por
// CPU + del override por el usuario.
//
// Detalle critico: en AOT HOST_LEAF rbx esta RESERVADO (frame).  `cpuid`
// pisa eax/ebx/ecx/edx; ebx lleva las features de leaf 7 (AVX2/BMI/AVX512).
// El selector vreg (vreg_select.cpp) ya SALVA/RESTAURA rbx alrededor del
// bloque INLINE_ASM cuando rbx aparece en sus clobbers (asm_effects declara
// que cpuid clobbea rax/rbx/rcx/rdx).  Por eso TODA la deteccion + el
// empaquetado de bits viven DENTRO de un solo bloque asm: leemos ebx ahi
// (entre el push y el pop de rbx) y solo SACAMOS el bitmask final en rax.
// Asi nunca exponemos ebx al IR (no podriamos: rbx no es asignable).
//
// rsi/rdi se usan como scratch porque cpuid NO los pisa (sobreviven entre
// las dos llamadas a cpuid): rsi acumula el bitmask, rdi extrae cada bit.
// Numeros en HEX explicito: el bloque se ensambla con Keystone (que toma los
// enteros BARE como hex) sin pasar por asm_normalize_numbers (eso solo corre
// en lower_asm, no en helpers sinteticos).
// ---------------------------------------------------------------------
uint64_t Lowering::ensure_cpu_features_global() {
    // Idempotente: si ya esta emitido, devolver el slot existente.
    if (cpu_features_slot_ != UINT64_MAX) return cpu_features_slot_;

    // 1. Slot static_data de 8 bytes zero-init para el global.  Va a `.data`
    //    (WRITABLE): __vx_cpu_init le hace STORE en runtime.  El default de
    //    STR_LIT_ADDR es `.rodata` (read-only) -> un STORE ahi fallaria.
    //    NON_DEDUP para que el merge cross-module no lo colapse con otro
    //    all-zero; FORCE_EMIT para garantizar su presencia aunque el optimizer
    //    toque los relocs.
    std::vector<uint8_t> zero(8, 0);
    const uint64_t slot =
        static_cast<uint64_t>(out_mod_->static_data.push_back(std::move(zero)));
    {
        auto &m = out_mod_->static_data.meta_at(slot);
        m.section_name = ".data";
        m.flags |=
            ir::IrModule::SD_FLAG_NON_DEDUP | ir::IrModule::SD_FLAG_FORCE_EMIT;
        // Global de programa: unificar el slot cross-module en el merge.
        m.shared_key = "__vx_cpu_features";
    }
    cpu_features_slot_ = slot;

    if (cpu_init_emitted_) return slot;
    cpu_init_emitted_ = true;

    // 2. Helper __vx_cpu_init(): un bloque asm que detecta features y un
    //    STORE del bitmask al slot.  Construido como IrFunction aparte.
    const std::string name = "__vx_cpu_init";

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::VOID;
    const ir::IrBlockId e = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;
    const uint32_t ln = 0;

    // --- binding register("rax") u64 feat;  (output only) ---
    // ALLOCA estable + AsmRegBinding -> el selector lo precolorea a rax.
    const ir::IrValueId rax_slot = fn_->new_value(ir::IrType::PTR);
    fn_->values[rax_slot].is_host_ptr = true;
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = rax_slot;
        al.imm = 8;
        al.host_alloca = true;
        al.source_line = ln;
        emit(current_block_, std::move(al));
    }
    // En modo protegido los registros son de 32 bits y `rax` no existe: el
    // binding, y todo el cuerpo de abajo, se nombran segun el ANCHO DEL TARGET.
    // Antes se emitia siempre en 64 bits, asi que en x86-32 el ensamblado
    // fallaba ("xor rsi, rsi") y con el se caia la funcion ENTERA que hubiera
    // disparado la deteccion -- normalmente `main`.
    const bool bits32 = (asm_target_bits_ == 32);
    {
        ir::AsmRegBinding b{rax_slot, bits32 ? "eax" : "rax", ir::IrType::U64,
                            false, "__cpu_feat"};
        b.reg_class = b.reg; // registro concreto.
        fn_->asm_reg_bindings.push_back(std::move(b));
    }

    // --- bloque INLINE_ASM: deteccion + empaquetado completo, bitmask en rax
    // --- bit0=SSE2(L1.EDX.26) bit1=SSE4.2(L1.ECX.20) bit2=POPCNT(L1.ECX.23)
    // bit3=AVX(L1.ECX.28) bit4=AVX2(L7.EBX.5) bit5=BMI1(L7.EBX.3)
    // bit6=BMI2(L7.EBX.8) bit7=AVX512F(L7.EBX.16) bit8=ERMS(L7.EBX.9).
    const std::string asm_body = "xor rsi, rsi\n" // acumulador = 0
                                                  // ----- leaf 1 -----
                                 "mov rax, 0x1\n"
                                 "xor rcx, rcx\n"
                                 "cpuid\n" // -> ecx, edx
                                 // SSE2 = EDX bit26 -> acc bit0
                                 "mov rdi, rdx\n"
                                 "shr rdi, 0x1a\n"
                                 "and rdi, 0x1\n"
                                 "or rsi, rdi\n"
                                 // SSE4.2 = ECX bit20 -> acc bit1
                                 "mov rdi, rcx\n"
                                 "shr rdi, 0x14\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x1\n"
                                 "or rsi, rdi\n"
                                 // POPCNT = ECX bit23 -> acc bit2
                                 "mov rdi, rcx\n"
                                 "shr rdi, 0x17\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x2\n"
                                 "or rsi, rdi\n"
                                 // AVX = ECX bit28 -> acc bit3
                                 "mov rdi, rcx\n"
                                 "shr rdi, 0x1c\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x3\n"
                                 "or rsi, rdi\n"
                                 // ----- leaf 7 subleaf 0 -----
                                 "mov rax, 0x7\n"
                                 "xor rcx, rcx\n"
                                 "cpuid\n" // -> ebx, ecx, edx
                                 // AVX2 = EBX bit5 -> acc bit4
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x5\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x4\n"
                                 "or rsi, rdi\n"
                                 // BMI1 = EBX bit3 -> acc bit5
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x3\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x5\n"
                                 "or rsi, rdi\n"
                                 // BMI2 = EBX bit8 -> acc bit6
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x8\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x6\n"
                                 "or rsi, rdi\n"
                                 // AVX512F = EBX bit16 -> acc bit7
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x10\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x7\n"
                                 "or rsi, rdi\n"
                                 // ERMS = EBX bit9 -> acc bit8
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x9\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x8\n"
                                 "or rsi, rdi\n"
                                 // resultado -> rax (binding de salida)
                                 "mov rax, rsi\n";

    // El cuerpo se escribe una sola vez, en 64 bits, y se reescribe a los
    // nombres de 32 cuando toca: `cpuid` y todo lo que hace aqui (mascaras de
    // 9 bits, desplazamientos) existe igual en modo protegido, lo unico que no
    // existe alli son los registros anchos.
    std::string asm_body_t = asm_body;
    if (bits32) {
        static const char *const kRegs[][2] = {{"rax", "eax"}, {"rbx", "ebx"},
                                               {"rcx", "ecx"}, {"rdx", "edx"},
                                               {"rsi", "esi"}, {"rdi", "edi"}};
        for (const auto &par : kRegs) {
            size_t pos = 0;
            while ((pos = asm_body_t.find(par[0], pos)) != std::string::npos) {
                asm_body_t.replace(pos, 3, par[1]);
                pos += 3;
            }
        }
    }
    {
        ir::IrInstr ia{};
        ia.op = ir::IrOp::INLINE_ASM;
        ia.type = ir::IrType::VOID;
        ia.dst = ir::IR_NO_VALUE;
        ia.source_line = ln;
        ia.func_name = asm_body_t;
        ia.preserve = true; // volatile: nunca eliminar/reordenar.

        // Listar el slot del binding como operando (lo mantiene vivo + lo
        // clasifica como output via el LOAD posterior).
        ia.operands.push_back(rax_slot);

        // Clobbers explicitos: cpuid pisa rax/rbx/rcx/rdx; ademas usamos
        // rsi/rdi como scratch.  El selector excluye los GP usables de los
        // vregs no-binding vivos (no hay ninguno aqui) y SALVA/RESTAURA rbx
        // (reservado) alrededor del bloque.  flags: memory=0 (no toca mem),
        // flags=1 (cpuid/and/shr afectan flags).
        std::vector<std::string> clob = {"rbx", "rcx", "rdx", "rsi", "rdi"};
        uint64_t q = 0;
        q |= 1ull << 0; // volatile
        q |= 1ull << 5; // clobbers flags
        const uint64_t asm_id = (uint64_t)fn_->asm_clobber_lists.size();
        fn_->asm_clobber_lists.push_back(std::move(clob));
        q |= (asm_id & 0xFFFFFFull) << 8;
        ia.imm = q;
        emit(current_block_, std::move(ia));
    }

    // --- LOAD del binding (lee rax) -> bitmask u64 ---
    const ir::IrValueId v_feat = fn_->new_value(ir::IrType::U64);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::U64;
        ld.dst = v_feat;
        ld.operands = {rax_slot};
        ld.source_line = ln;
        emit(current_block_, std::move(ld));
    }

    // --- STORE del bitmask al slot global __vx_cpu_features ---
    const ir::IrValueId v_gaddr = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr is{};
        is.op = ir::IrOp::STR_LIT_ADDR;
        is.type = ir::IrType::PTR;
        is.dst = v_gaddr;
        is.imm = slot;
        is.source_line = ln;
        emit(current_block_, std::move(is));
    }
    {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_feat, v_gaddr};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    }

    // --- RET void ---
    {
        ir::IrInstr ret{};
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::VOID;
        ret.dst = ir::IR_NO_VALUE;
        ret.source_line = ln;
        emit(current_block_, std::move(ret));
    }
    block_terminated_ = true;

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));

    // Registrar el import nativo del runner de inline asm (igual que lower_asm)
    // para el path bytecode/interp; en native_poo_ no se usa, pero es
    // idempotente y mantiene la coherencia.
    out_mod_->register_native_import("vrt", "inline_asm_exec");
    return slot;
}

// ---------------------------------------------------------------------
// CPU dispatch (Inc 2): memcpy multi-versionado por tabla de punteros.
//
// Tres piezas:
//   1. Global __vx_memcpy_fp (u64 en ".data"): puntero a la variante elegida.
//   2. Variantes:
//        - __vx_memcpy_base(dst, src, n): rep movsb (segura, cualquier n).
//        - __vx_memcpy_avx2(dst, src, n): 32B con vmovdqu ymm + cola
//          byte-a-byte (sin leer/escribir fuera de [0,n)).
//   3. __vx_memcpy_init(): lee __vx_cpu_features, si el bit AVX2 (bit 4)
//      esta activo setea fp = &__vx_memcpy_avx2, si no &__vx_memcpy_base.
//
// El wiring (run()) prepone `call __vx_cpu_init` + `call __vx_memcpy_init`
// al entry de main (en ese orden).  Los memcpy del concat/slice/+= bajan a
// `call [__vx_memcpy_fp]` (CALLIND) en lugar de rep movsb inline.
//
// La direccion de cada variante se obtiene via LABEL_ADDR (en AOT baja a una
// reloc "fnsym:<name>" que el driver resuelve contra el offset de la funcion).
// Todo es PURE_NATIVE (CALL/CALLIND/LABEL_ADDR/INLINE_ASM/MEMCPY/LOAD/STORE).
// ---------------------------------------------------------------------
uint64_t Lowering::ensure_memcpy_dispatch() {
    cpu_dispatch_used_ = true;
    if (memcpy_helpers_emitted_) return memcpy_fp_slot_;
    memcpy_helpers_emitted_ = true;

    // 1. Global __vx_memcpy_fp (8 bytes zero-init) en ".data" (writable: el
    //    init le hace STORE en runtime).  NON_DEDUP + FORCE_EMIT como el slot
    //    de features.
    {
        std::vector<uint8_t> zero(8, 0);
        const uint64_t slot = static_cast<uint64_t>(
            out_mod_->static_data.push_back(std::move(zero)));
        auto &m = out_mod_->static_data.meta_at(slot);
        m.section_name = ".data";
        m.flags |=
            ir::IrModule::SD_FLAG_NON_DEDUP | ir::IrModule::SD_FLAG_FORCE_EMIT;
        // Global de programa: unificar el slot cross-module en el merge.
        m.shared_key = "__vx_memcpy_fp";
        memcpy_fp_slot_ = slot;
    }
    const uint64_t fp_slot = memcpy_fp_slot_;

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;
    const uint32_t ln = 0;

    // --- Helper para construir una variante memcpy(dst, src, n) -------------
    // body_emitter recibe los SSA de los 3 params + el bloque entry activo y
    // emite el cuerpo (terminando con RET void).
    auto build_variant =
        [&](const std::string &name,
            const std::function<void(ir::IrValueId, ir::IrValueId,
                                     ir::IrValueId)> &body_emitter) {
            ir::IrFunction hf;
            hf.name = name;
            hf.ret_type = ir::IrType::VOID;
            const ir::IrValueId p_dst = hf.new_value(ir::IrType::PTR, "%dst");
            hf.values[p_dst].is_param = true;
            hf.values[p_dst].is_host_ptr = true;
            hf.params.push_back(p_dst);
            const ir::IrValueId p_src = hf.new_value(ir::IrType::PTR, "%src");
            hf.values[p_src].is_param = true;
            hf.values[p_src].is_host_ptr = true;
            hf.params.push_back(p_src);
            const ir::IrValueId p_n = hf.new_value(ir::IrType::I64, "%n");
            hf.values[p_n].is_param = true;
            hf.params.push_back(p_n);
            const ir::IrBlockId e = hf.new_block("entry");

            fn_ = &hf;
            current_block_ = e;
            block_terminated_ = false;

            body_emitter(p_dst, p_src, p_n);

            block_terminated_ = true;
            out_mod_->add_function(std::move(hf));
        };

    // Cuerpo "base": MEMCPY (rep movsb) + RET void.  Cubre cualquier n,
    // incluido 0 (rep movsb con rcx=0 no copia nada).
    auto emit_base_body = [&](ir::IrValueId dst, ir::IrValueId src,
                              ir::IrValueId n) {
        ir::IrInstr mc{};
        mc.op = ir::IrOp::MEMCPY;
        mc.type = ir::IrType::I8;
        mc.dst = ir::IR_NO_VALUE;
        mc.operands = {dst, src, n};
        mc.source_line = ln;
        emit(current_block_, std::move(mc));
        ir::IrInstr ret{};
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::VOID;
        ret.dst = ir::IR_NO_VALUE;
        ret.source_line = ln;
        emit(current_block_, std::move(ret));
    };

    build_variant("__vx_memcpy_base", emit_base_body);

    // --- Variante AVX2: vmovdqu ymm de a 32 bytes + cola byte-a-byte ---------
    // Helper INLINE_ASM auto-contenido: los 3 params (dst/src/n) llegan en los
    // arg_regs del ABI; los fijamos a rdi/rsi/rdx via AsmRegBinding (el
    // selector precolorea y el regalloc inserta el move desde el arg_reg).  El
    // bloque asm copia bloques de 32 B con vmovdqu ymm0 mientras queden >= 32
    // bytes, luego la cola (< 32) byte-a-byte.  CRITICO valgrind: nunca
    // lee/escribe fuera de [0, n) -- el chunk de 32 solo corre con n >= 32; el
    // resto byte a byte. vzeroupper al final (penalizacion AVX<->SSE).  Labels
    // intra-bloque las resuelve Keystone.  El asm clobbea rax + ymm0 + flags +
    // memoria; rdi/rsi/ rdx son operandos (bindings), no clobbers.
    {
        ir::IrFunction hf;
        hf.name = "__vx_memcpy_avx2";
        hf.ret_type = ir::IrType::VOID;
        const ir::IrValueId p_dst = hf.new_value(ir::IrType::PTR, "%dst");
        hf.values[p_dst].is_param = true;
        hf.values[p_dst].is_host_ptr = true;
        hf.params.push_back(p_dst);
        const ir::IrValueId p_src = hf.new_value(ir::IrType::PTR, "%src");
        hf.values[p_src].is_param = true;
        hf.values[p_src].is_host_ptr = true;
        hf.params.push_back(p_src);
        const ir::IrValueId p_n = hf.new_value(ir::IrType::I64, "%n");
        hf.values[p_n].is_param = true;
        hf.params.push_back(p_n);
        const ir::IrBlockId e = hf.new_block("entry");

        fn_ = &hf;
        current_block_ = e;
        block_terminated_ = false;

        // 3 ALLOCA estables + AsmRegBinding (dst->rdi, src->rsi, n->rdx).
        // El selector convierte el STORE param->alloca en `MOV rXX, param` y
        // lista la alloca como operando del INLINE_ASM (input).
        auto make_binding = [&](const char *reg, ir::IrType ty,
                                ir::IrValueId param,
                                const char *dbg) -> ir::IrValueId {
            ir::IrValueId slot = fn_->new_value(ir::IrType::PTR);
            fn_->values[slot].is_host_ptr = true;
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA;
                al.type = ir::IrType::I8;
                al.dst = slot;
                al.imm = 8;
                al.host_alloca = true;
                al.source_line = ln;
                emit(current_block_, std::move(al));
            }
            {
                ir::AsmRegBinding b{slot, reg, ty, false, dbg};
                b.reg_class = reg; // registro concreto.
                fn_->asm_reg_bindings.push_back(std::move(b));
            }
            // STORE param -> alloca (carga el input en el reg fijado).
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {param, slot};
            st.source_line = ln;
            emit(current_block_, std::move(st));
            return slot;
        };
        const ir::IrValueId s_dst =
            make_binding("rdi", ir::IrType::U64, p_dst, "__mc_dst");
        const ir::IrValueId s_src =
            make_binding("rsi", ir::IrType::U64, p_src, "__mc_src");
        const ir::IrValueId s_n =
            make_binding("rdx", ir::IrType::U64, p_n, "__mc_n");

        // Cuerpo NASM.  rdi=dst, rsi=src, rdx=n.  rax = scratch del byte de
        // cola.
        const std::string asm_body =
            ".chunk:\n"
            "cmp rdx, 0x20\n" // mientras queden >= 32 bytes
            "jb .tail\n"
            "vmovdqu ymm0, [rsi]\n" // 32 B src -> ymm0
            "vmovdqu [rdi], ymm0\n" // ymm0 -> 32 B dst
            "add rsi, 0x20\n"
            "add rdi, 0x20\n"
            "sub rdx, 0x20\n"
            "jmp .chunk\n"
            ".tail:\n"
            "test rdx, rdx\n" // cola (< 32) byte a byte
            "jz .done\n"
            ".tloop:\n"
            "mov al, [rsi]\n"
            "mov [rdi], al\n"
            "inc rsi\n"
            "inc rdi\n"
            "dec rdx\n"
            "jnz .tloop\n"
            ".done:\n"
            "vzeroupper\n";
        {
            ir::IrInstr ia{};
            ia.op = ir::IrOp::INLINE_ASM;
            ia.type = ir::IrType::VOID;
            ia.dst = ir::IR_NO_VALUE;
            ia.source_line = ln;
            ia.func_name = asm_body;
            ia.preserve = true; // volatile

            // Operandos: las 3 allocas binding (inputs).
            ia.operands = {s_dst, s_src, s_n};

            // Clobbers: rax (scratch de la cola) + memoria.  ymm0 lo asume el
            // ABI (caller-saved); rdi/rsi/rdx son operandos.  flags por las
            // comparaciones del loop.
            std::vector<std::string> clob = {"rax", "memory"};
            uint64_t q = 0;
            q |= 1ull << 0; // volatile
            q |= 1ull << 4; // clobbers memory
            q |= 1ull << 5; // clobbers flags
            const uint64_t asm_id = (uint64_t)fn_->asm_clobber_lists.size();
            fn_->asm_clobber_lists.push_back(std::move(clob));
            q |= (asm_id & 0xFFFFFFull) << 8;
            ia.imm = q;
            emit(current_block_, std::move(ia));
        }

        // RET void.
        {
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = ir::IrType::VOID;
            ret.dst = ir::IR_NO_VALUE;
            ret.source_line = ln;
            emit(current_block_, std::move(ret));
        }
        block_terminated_ = true;
        out_mod_->add_function(std::move(hf));
        out_mod_->register_native_import("vrt", "inline_asm_exec");
    }

    // --- 3. __vx_memcpy_init(): setea el fp segun el bit AVX2 --------------
    {
        ir::IrFunction hf;
        hf.name = "__vx_memcpy_init";
        hf.ret_type = ir::IrType::VOID;
        const ir::IrBlockId e = hf.new_block("entry");
        fn_ = &hf;
        current_block_ = e;
        block_terminated_ = false;

        // Helper local: STORE &<fn_name> al global fp en el bloque actual.
        // (Sin BR; el caller decide el terminador.)  Reusado por el camino
        // de override (RET directo) y por las ramas del dispatch cpuid.
        auto emit_store_fp = [&](const std::string &fn_name) {
            ir::IrValueId v_addr = emit_label_addr(fn_name, ln);
            ir::IrValueId v_gaddr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_gaddr].is_host_ptr = true;
            {
                ir::IrInstr la{};
                la.op = ir::IrOp::STR_LIT_ADDR;
                la.type = ir::IrType::PTR;
                la.dst = v_gaddr;
                la.imm = fp_slot;
                la.source_line = ln;
                emit(current_block_, std::move(la));
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_addr, v_gaddr};
            st.source_line = ln;
            emit(current_block_, std::move(st));
        };

        // CPU dispatch Inc 4: si el usuario declaro @HelperOverride(memcpy),
        // el fp apunta a SU funcion de forma INCONDICIONAL (sin leer cpuid).
        // Esto reemplaza el memcpy del build entero por el del usuario.
        if (!memcpy_override_.empty()) {
            emit_store_fp(memcpy_override_);
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = ir::IrType::VOID;
            ret.dst = ir::IR_NO_VALUE;
            ret.source_line = ln;
            emit(current_block_, std::move(ret));
            block_terminated_ = true;
            out_mod_->add_function(std::move(hf));
            fn_ = saved_fn;
            current_block_ = saved_block;
            block_terminated_ = saved_terminated;
            return fp_slot;
        }

        // feat = LOAD i64 [__vx_cpu_features].
        const uint64_t feat_slot = ensure_cpu_features_global();
        ir::IrValueId v_faddr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_faddr].is_host_ptr = true;
        {
            ir::IrInstr la{};
            la.op = ir::IrOp::STR_LIT_ADDR;
            la.type = ir::IrType::PTR;
            la.dst = v_faddr;
            la.imm = feat_slot;
            la.source_line = ln;
            emit(current_block_, std::move(la));
        }
        ir::IrValueId v_feat = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_feat;
            ld.operands = {v_faddr};
            ld.source_line = ln;
            emit(current_block_, std::move(ld));
        }
        // has_avx2 = (feat >> 4) & 1.  (bit4 = AVX2.)
        ir::IrValueId v_sh = fn_->new_value(ir::IrType::I64);
        {
            ir::IrValueId v4 = emit_const(ir::IrType::I64, 4, ln);
            ir::IrInstr sh{};
            sh.op = ir::IrOp::SHR; // logico (feat es bitmask)
            sh.type = ir::IrType::I64;
            sh.dst = v_sh;
            sh.operands = {v_feat, v4};
            sh.source_line = ln;
            emit(current_block_, std::move(sh));
        }
        ir::IrValueId v_bit = fn_->new_value(ir::IrType::I64);
        {
            ir::IrValueId v1 = emit_const(ir::IrType::I64, 1, ln);
            ir::IrInstr an{};
            an.op = ir::IrOp::AND;
            an.type = ir::IrType::I64;
            an.dst = v_bit;
            an.operands = {v_sh, v1};
            an.source_line = ln;
            emit(current_block_, std::move(an));
        }
        ir::IrValueId v_has = fn_->new_value(ir::IrType::BOOL);
        {
            ir::IrValueId v0 = emit_const(ir::IrType::I64, 0, ln);
            ir::IrInstr cm{};
            cm.op = ir::IrOp::CMP_NE;
            cm.type = ir::IrType::BOOL;
            cm.dst = v_has;
            cm.operands = {v_bit, v0};
            cm.source_line = ln;
            emit(current_block_, std::move(cm));
        }

        // Ramas: avx2 -> fp=&avx2 ; base -> fp=&base ; join -> RET.
        const ir::IrBlockId bb_avx2 = fn_->new_block("avx2");
        const ir::IrBlockId bb_base = fn_->new_block("base");
        const ir::IrBlockId bb_join = fn_->new_block("join");
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR_COND;
            br.type = ir::IrType::VOID;
            br.dst = ir::IR_NO_VALUE;
            br.operands = {v_has};
            br.target_block = bb_avx2;
            br.false_block = bb_base;
            br.source_line = ln;
            emit(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(bb_avx2);
            fn_->blocks[current_block_].succs.push_back(bb_base);
            fn_->blocks[bb_avx2].preds.push_back(current_block_);
            fn_->blocks[bb_base].preds.push_back(current_block_);
        }

        // Helper: en el bloque actual, STORE &<fn_name> al global fp + BR join.
        auto store_fp_and_join = [&](const std::string &fn_name) {
            emit_store_fp(fn_name);
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.type = ir::IrType::VOID;
            br.dst = ir::IR_NO_VALUE;
            br.target_block = bb_join;
            br.source_line = ln;
            emit(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(bb_join);
            fn_->blocks[bb_join].preds.push_back(current_block_);
        };

        current_block_ = bb_avx2;
        store_fp_and_join("__vx_memcpy_avx2");
        current_block_ = bb_base;
        store_fp_and_join("__vx_memcpy_base");

        current_block_ = bb_join;
        {
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = ir::IrType::VOID;
            ret.dst = ir::IR_NO_VALUE;
            ret.source_line = ln;
            emit(current_block_, std::move(ret));
        }
        block_terminated_ = true;
        out_mod_->add_function(std::move(hf));
    }

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    return fp_slot;
}

// ---------------------------------------------------------------------
// AUTO multiversion (--float-isa auto): despacha el MAIN por cpuid.
//
// Problema: main es el entry; el _start stub lo llama por NOMBRE.  Si lo
// multiversionaramos directamente (main$sse2/avx2/avx512) nadie correria el
// init (cpuid) antes de elegir la variante.  Fix: reducir "multiversionar
// main" a "despachar un helper":
//   1. El main del usuario se RENOMBRA a __vx_main_body (un helper VEC
//      normal; el driver lo compila 3x: $sse2/$avx2/$avx512).
//   2. Se sintetiza un main fino = { <inits> ; r = CALLIND [__vx_main_body$fp]
//      (args...) ; ret r }.  Los inits (cpu_init + auto_init) los prepone
//      run() en su entry, asi corren ANTES del CALLIND que lee el fp.
//   3. __vx_auto_init() elige la variante por cpuid (AVX512F bit7 > AVX2 bit4
//      > SSE2) y la guarda en __vx_main_body$fp.
// El fp se referencia por INDICE (STR_LIT_ADDR), no por nombre -> no hace
// falta trampolin de bytes crudos ni reloc DATA_REL32: todo es IR estandar
// (CALLIND + LABEL_ADDR + LOAD/STORE), PURE_NATIVE.
void Lowering::ensure_auto_multiversion(ir::IrModule &out_module) {
    if (!native_poo_ || !aot_auto_vec_) return; // solo AOT --float-isa auto
    if (auto_dispatch_emitted_) return;         // idempotente

    // Detector de ops VEC_* (idem al driver): solo despachamos lo vectorizado.
    auto fn_has_vec = [](const ir::IrFunction &f) -> bool {
        for (const auto &b : f.blocks)
            for (const auto &in : b.instrs) {
                const auto op = in.op;
                if (op == ir::IrOp::VEC_BINOP || op == ir::IrOp::VEC_UNOP ||
                    op == ir::IrOp::VEC_FMA || op == ir::IrOp::VEC_BINOP_S ||
                    op == ir::IrOp::VEC_BCAST || op == ir::IrOp::VEC_ACC_ZERO ||
                    op == ir::IrOp::VEC_ACC_ADD ||
                    op == ir::IrOp::VEC_ACC_FMA ||
                    op == ir::IrOp::VEC_ACC_STORE ||
                    op == ir::IrOp::VEC_ACC_COMBINE)
                    return true;
            }
        return false;
    };

    // Recolectar TODAS las funciones con ops VEC (main + helpers).  Capturar
    // firma + RENOMBRAR en sitio ANTES de cualquier add_function (que
    // realocaria out_module.functions e invalidaria indices/punteros).  El
    // renombrado NO anñade funciones, asi que el primer bucle es seguro.
    struct PInfo {
        ir::IrType ty;
        bool host;
    };
    struct MvEntry {
        std::string wrapper_name; // nombre que ven los callers (el original)
        std::string
            body_name; // cuerpo multiversionado (el driver lo compila 3x)
        ir::IrType ret;
        std::vector<PInfo> params;
        uint64_t fp_slot = 0; // se rellena en la fase de wrappers
    };
    std::vector<MvEntry> mv;
    for (size_t i = 0; i < out_module.functions.size(); ++i) {
        ir::IrFunction &f = out_module.functions[i];
        if (!fn_has_vec(f)) continue;
        MvEntry e;
        e.wrapper_name = f.name;
        // main mantiene el nombre historico __vx_main_body; los helpers usan
        // <nombre>$mv.  El driver suffija $sse2/$avx2/$avx512 a estos nombres.
        e.body_name =
            (f.name == "main") ? std::string("__vx_main_body") : f.name + "$mv";
        e.ret = f.ret_type;
        for (ir::IrValueId pid : f.params)
            e.params.push_back({f.values[pid].type, f.values[pid].is_host_ptr});
        f.name = e.body_name; // renombrado en sitio (sin add_function)
        mv.push_back(std::move(e));
    }
    if (mv.empty()) return; // ninguna funcion vectorizada -> nada que despachar

    auto_dispatch_emitted_ = true;
    cpu_dispatch_used_ = true;
    // Garantizar el global de features + __vx_cpu_init (puede anñadir una
    // funcion -> realoc, pero ya no tenemos referencias vivas a las funciones).
    (void)ensure_cpu_features_global();

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;
    const uint32_t ln = 0;

    // Por cada funcion VEC: slot fp <body>$fp + wrapper sintetico (nombre
    // original) que hace CALLIND a la variante elegida.  Los callers siguen
    // llamando por nombre -> caen en el wrapper -> despacho transparente.
    for (auto &e : mv) {
        {
            std::vector<uint8_t> zero(8, 0);
            e.fp_slot = static_cast<uint64_t>(
                out_module.static_data.push_back(std::move(zero)));
            auto &m = out_module.static_data.meta_at(e.fp_slot);
            m.section_name = ".data";
            m.flags |= ir::IrModule::SD_FLAG_NON_DEDUP |
                       ir::IrModule::SD_FLAG_FORCE_EMIT;
            m.shared_key = e.body_name + "$fp";
        }
        ir::IrFunction w;
        w.name = e.wrapper_name;
        w.ret_type = e.ret;
        std::vector<ir::IrValueId> sparams;
        for (const auto &pi : e.params) {
            const ir::IrValueId pv = w.new_value(pi.ty);
            w.values[pv].is_param = true;
            w.values[pv].is_host_ptr = pi.host;
            w.params.push_back(pv);
            sparams.push_back(pv);
        }
        const ir::IrBlockId be = w.new_block("entry");
        fn_ = &w;
        current_block_ = be;
        block_terminated_ = false;

        // v_fpaddr = &<body>$fp ; v_fp = LOAD i64 [v_fpaddr].
        ir::IrValueId v_fpaddr = w.new_value(ir::IrType::PTR);
        w.values[v_fpaddr].is_host_ptr = true;
        {
            ir::IrInstr la{};
            la.op = ir::IrOp::STR_LIT_ADDR;
            la.type = ir::IrType::PTR;
            la.dst = v_fpaddr;
            la.imm = e.fp_slot;
            la.source_line = ln;
            w.append(current_block_, std::move(la));
        }
        ir::IrValueId v_fp = w.new_value(ir::IrType::PTR);
        w.values[v_fp].is_host_ptr = true;
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_fp;
            ld.operands = {v_fpaddr};
            ld.source_line = ln;
            w.append(current_block_, std::move(ld));
        }
        // CALLIND v_fp(args...) -> r.
        ir::IrValueId v_ret = ir::IR_NO_VALUE;
        {
            ir::IrInstr ci{};
            ci.op = ir::IrOp::CALLIND;
            ci.type = e.ret;
            ci.func_ptr = v_fp;
            ci.operands = sparams;
            if (e.ret != ir::IrType::VOID) {
                v_ret = w.new_value(e.ret);
                ci.dst = v_ret;
            } else {
                ci.dst = ir::IR_NO_VALUE;
            }
            ci.source_line = ln;
            w.append(current_block_, std::move(ci));
        }
        {
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = e.ret;
            ret.dst = ir::IR_NO_VALUE;
            if (e.ret != ir::IrType::VOID) ret.operands.push_back(v_ret);
            ret.source_line = ln;
            w.append(current_block_, std::move(ret));
        }
        block_terminated_ = true;
        out_module.add_function(std::move(w));
    }

    // __vx_auto_init(): un solo cpuid -> tres ramas (AVX512F bit7 > AVX2 bit4 >
    // SSE2); cada rama setea el fp de TODAS las funciones VEC a su variante del
    // ancho elegido.  (La decision de ISA es global a la CPU -> una sola vez.)
    {
        ir::IrFunction hf;
        hf.name = "__vx_auto_init";
        hf.ret_type = ir::IrType::VOID;
        const ir::IrBlockId e = hf.new_block("entry");
        fn_ = &hf;
        current_block_ = e;
        block_terminated_ = false;

        // STORE &<variante> al fp dado, en el bloque actual (sin terminador).
        auto emit_store_fp = [&](const std::string &variant, uint64_t fp_slot) {
            ir::IrValueId v_addr = emit_label_addr(variant, ln);
            ir::IrValueId v_gaddr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_gaddr].is_host_ptr = true;
            {
                ir::IrInstr la{};
                la.op = ir::IrOp::STR_LIT_ADDR;
                la.type = ir::IrType::PTR;
                la.dst = v_gaddr;
                la.imm = fp_slot;
                la.source_line = ln;
                emit(current_block_, std::move(la));
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_addr, v_gaddr};
            st.source_line = ln;
            emit(current_block_, std::move(st));
        };

        // feat = LOAD i64 [__vx_cpu_features].
        const uint64_t feat_slot = ensure_cpu_features_global();
        ir::IrValueId v_faddr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_faddr].is_host_ptr = true;
        {
            ir::IrInstr la{};
            la.op = ir::IrOp::STR_LIT_ADDR;
            la.type = ir::IrType::PTR;
            la.dst = v_faddr;
            la.imm = feat_slot;
            la.source_line = ln;
            emit(current_block_, std::move(la));
        }
        ir::IrValueId v_feat = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_feat;
            ld.operands = {v_faddr};
            ld.source_line = ln;
            emit(current_block_, std::move(ld));
        }
        // bit_set(n): has = ((feat >> n) & 1) != 0.
        auto bit_set = [&](int n) -> ir::IrValueId {
            ir::IrValueId v_sh = fn_->new_value(ir::IrType::I64);
            {
                ir::IrValueId vn = emit_const(ir::IrType::I64, (uint64_t)n, ln);
                ir::IrInstr sh{};
                sh.op = ir::IrOp::SHR;
                sh.type = ir::IrType::I64;
                sh.dst = v_sh;
                sh.operands = {v_feat, vn};
                sh.source_line = ln;
                emit(current_block_, std::move(sh));
            }
            ir::IrValueId v_bit = fn_->new_value(ir::IrType::I64);
            {
                ir::IrValueId v1 = emit_const(ir::IrType::I64, 1, ln);
                ir::IrInstr an{};
                an.op = ir::IrOp::AND;
                an.type = ir::IrType::I64;
                an.dst = v_bit;
                an.operands = {v_sh, v1};
                an.source_line = ln;
                emit(current_block_, std::move(an));
            }
            ir::IrValueId v_has = fn_->new_value(ir::IrType::BOOL);
            {
                ir::IrValueId v0 = emit_const(ir::IrType::I64, 0, ln);
                ir::IrInstr cm{};
                cm.op = ir::IrOp::CMP_NE;
                cm.type = ir::IrType::BOOL;
                cm.dst = v_has;
                cm.operands = {v_bit, v0};
                cm.source_line = ln;
                emit(current_block_, std::move(cm));
            }
            return v_has;
        };

        const ir::IrBlockId bb_512 = fn_->new_block("pick512");
        const ir::IrBlockId bb_not512 = fn_->new_block("not512");
        const ir::IrBlockId bb_2 = fn_->new_block("pick2");
        const ir::IrBlockId bb_sse = fn_->new_block("picksse");
        const ir::IrBlockId bb_join = fn_->new_block("join");

        auto branch = [&](ir::IrValueId cond, ir::IrBlockId t,
                          ir::IrBlockId f) {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR_COND;
            br.type = ir::IrType::VOID;
            br.dst = ir::IR_NO_VALUE;
            br.operands = {cond};
            br.target_block = t;
            br.false_block = f;
            br.source_line = ln;
            emit(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(t);
            fn_->blocks[current_block_].succs.push_back(f);
            fn_->blocks[t].preds.push_back(current_block_);
            fn_->blocks[f].preds.push_back(current_block_);
        };
        // En el bloque actual: setea el fp de TODAS las entries a la variante
        // del sufijo dado + BR a join.
        auto store_all_and_join = [&](const char *suffix) {
            for (const auto &en : mv)
                emit_store_fp(en.body_name + suffix, en.fp_slot);
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.type = ir::IrType::VOID;
            br.dst = ir::IR_NO_VALUE;
            br.target_block = bb_join;
            br.source_line = ln;
            emit(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(bb_join);
            fn_->blocks[bb_join].preds.push_back(current_block_);
        };

        // entry: has512 = bit7 ; br has512 -> pick512 : not512.
        ir::IrValueId v512 = bit_set(7);
        branch(v512, bb_512, bb_not512);
        current_block_ = bb_512;
        store_all_and_join("$avx512");
        current_block_ = bb_not512;
        ir::IrValueId v2 = bit_set(4);
        branch(v2, bb_2, bb_sse);
        current_block_ = bb_2;
        store_all_and_join("$avx2");
        current_block_ = bb_sse;
        store_all_and_join("$sse2");
        // join: RET void.
        current_block_ = bb_join;
        {
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = ir::IrType::VOID;
            ret.dst = ir::IR_NO_VALUE;
            ret.source_line = ln;
            emit(current_block_, std::move(ret));
        }
        block_terminated_ = true;
        out_module.add_function(std::move(hf));
    }

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
}

void Lowering::emit_memcpy_dispatched(ir::IrValueId dst, ir::IrValueId src,
                                      ir::IrValueId len, uint32_t line) {
    // Asegura el global fp + variantes + init (idempotente) y marca el uso
    // para que el wiring prepone __vx_memcpy_init en main.
    const uint64_t fp_slot = ensure_memcpy_dispatch();

    // v_fpaddr = &__vx_memcpy_fp ; v_fp = LOAD i64 [v_fpaddr].
    ir::IrValueId v_fpaddr = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_fpaddr].is_host_ptr = true;
    {
        ir::IrInstr la{};
        la.op = ir::IrOp::STR_LIT_ADDR;
        la.type = ir::IrType::PTR;
        la.dst = v_fpaddr;
        la.imm = fp_slot;
        la.source_line = line;
        emit(current_block_, std::move(la));
    }
    ir::IrValueId v_fp = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_fp].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_fp;
        ld.operands = {v_fpaddr};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    // CALLIND v_fp(dst, src, len) -> void.
    ir::IrInstr ci{};
    ci.op = ir::IrOp::CALLIND;
    ci.type = ir::IrType::VOID;
    ci.dst = ir::IR_NO_VALUE;
    ci.func_ptr = v_fp;
    ci.operands = {dst, src, len};
    ci.source_line = line;
    emit(current_block_, std::move(ci));
}

// ---------------------------------------------------------------------
// CPU dispatch (Inc 5a): strcmp/strlen multi-versionados por tabla de
// punteros.  Foundation para que una libreria stdlib provea variantes SIMD
// via @HelperOverride(strcmp)/(strlen).  A DIFERENCIA de memcpy, el
// compilador NO hace cpuid aqui: el default es el BASELINE escalar
// (__vx_strcmp_base / __vx_strlen_base, la impl actual del compilador);
// la variante SIMD vendra de una lib importada (Inc 5c) via @HelperOverride.
//
// Tres piezas:
//   1. Globals __vx_strcmp_fp / __vx_strlen_fp (u64 en ".data").
//   2. Baselines __vx_strcmp_base / __vx_strlen_base (los renombrados
//      ensure_strcmp_helper / ensure_strlen_helper; siempre presentes,
//      llamables por nombre para que un override delegue a ellos).
//   3. __vx_strdisp_init(): setea cada fp al override del usuario (si
//      declarado @HelperOverride) o al baseline.
//
// run() prepone `call __vx_strdisp_init` al entry de main (junto al resto
// de inits).  Los call sites de strcmp/strlen bajan a `call [fp]` (CALLIND)
// en native_poo_.  Todo PURE_NATIVE (CALL/CALLIND/LABEL_ADDR/LOAD/STORE).
// ---------------------------------------------------------------------
void Lowering::ensure_strdisp() {
    cpu_dispatch_used_ = true;
    if (strdisp_emitted_) return;
    strdisp_emitted_ = true;

    // 1. Globals fp (8 bytes zero-init) en ".data" (writable: el init les
    //    hace STORE en runtime).  NON_DEDUP + FORCE_EMIT como los demas fp.
    auto make_fp_slot = [&](const char *shared_key) -> uint64_t {
        std::vector<uint8_t> zero(8, 0);
        const uint64_t slot = static_cast<uint64_t>(
            out_mod_->static_data.push_back(std::move(zero)));
        auto &m = out_mod_->static_data.meta_at(slot);
        m.section_name = ".data";
        m.flags |=
            ir::IrModule::SD_FLAG_NON_DEDUP | ir::IrModule::SD_FLAG_FORCE_EMIT;
        // Global de programa: unificar el slot cross-module en el merge.
        m.shared_key = shared_key;
        return slot;
    };
    strcmp_fp_slot_ = make_fp_slot("__vx_strcmp_fp");
    strlen_fp_slot_ = make_fp_slot("__vx_strlen_fp");

    // 2. Asegurar los baselines (emiten __vx_strcmp_base / __vx_strlen_base).
    (void)ensure_strcmp_helper();
    (void)ensure_strlen_helper();

    // 3. __vx_strdisp_init(): para cada fp, STORE &<variante> al global.
    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;
    const uint32_t ln = 0;

    ir::IrFunction hf;
    hf.name = "__vx_strdisp_init";
    hf.ret_type = ir::IrType::VOID;
    const ir::IrBlockId e = hf.new_block("entry");
    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;

    // STORE &<fn_name> al global fp del slot dado.
    auto emit_store_fp = [&](uint64_t fp_slot, const std::string &fn_name) {
        ir::IrValueId v_addr = emit_label_addr(fn_name, ln);
        ir::IrValueId v_gaddr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_gaddr].is_host_ptr = true;
        {
            ir::IrInstr la{};
            la.op = ir::IrOp::STR_LIT_ADDR;
            la.type = ir::IrType::PTR;
            la.dst = v_gaddr;
            la.imm = fp_slot;
            la.source_line = ln;
            emit(current_block_, std::move(la));
        }
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_addr, v_gaddr};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    };

    // fp = override del usuario si lo hay; si no, el baseline.
    emit_store_fp(strcmp_fp_slot_, strcmp_override_.empty()
                                       ? std::string("__vx_strcmp_base")
                                       : strcmp_override_);
    emit_store_fp(strlen_fp_slot_, strlen_override_.empty()
                                       ? std::string("__vx_strlen_base")
                                       : strlen_override_);
    {
        ir::IrInstr ret{};
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::VOID;
        ret.dst = ir::IR_NO_VALUE;
        ret.source_line = ln;
        emit(current_block_, std::move(ret));
    }
    block_terminated_ = true;
    out_mod_->add_function(std::move(hf));

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
}

std::string Lowering::ensure_strcmp_helper() {
    // Vesta Embed Inc 4: helper de comparacion lexicografica de strings
    // value-type nativos.  Firma:
    //   i64 __vx_strcmp(u8* pa, i64 la, u8* pb, i64 lb)
    // Devuelve -1/0/1 (memcmp + tie-break por longitud):
    //   1. min = (la < lb) ? la : lb.
    //   2. for (i = 0; i < min; i++): comparar pa[i] vs pb[i] como bytes
    //      unsigned (0..255).  El primer byte que difiere decide:
    //      pa[i] < pb[i] -> -1 ; pa[i] > pb[i] -> 1.
    //   3. Si los min bytes coinciden, el mas CORTO es menor:
    //      la < lb -> -1 ; la > lb -> 1 ; la == lb -> 0.
    // Vive en una funcion APARTE con loop -> el optimizer NO foldea la
    // comparacion byte-a-byte mid-expression con operandos constantes y
    // el inliner no la re-inlinea (is_inlineable exige 1 bloque).  Usa
    // slots ALLOCA para el indice (mem2reg los promueve en O2) y evita
    // PHIs manuales.  Todas las ops son PURE_NATIVE.
    //
    // CPU dispatch Inc 5a: este es el BASELINE escalar (`__vx_strcmp_base`)
    // al que apunta __vx_strcmp_fp por defecto.  Es llamable por nombre desde
    // Vesta (un override puede delegar a el).
    const std::string name = "__vx_strcmp_base";
    if (strcmp_helper_emitted_) return name;
    strcmp_helper_emitted_ = true;

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::I64;
    const ir::IrValueId p_pa = hf.new_value(ir::IrType::PTR, "%pa");
    hf.values[p_pa].is_param = true;
    hf.values[p_pa].is_host_ptr = true;
    hf.params.push_back(p_pa);
    const ir::IrValueId p_la = hf.new_value(ir::IrType::I64, "%la");
    hf.values[p_la].is_param = true;
    hf.params.push_back(p_la);
    const ir::IrValueId p_pb = hf.new_value(ir::IrType::PTR, "%pb");
    hf.values[p_pb].is_param = true;
    hf.values[p_pb].is_host_ptr = true;
    hf.params.push_back(p_pb);
    const ir::IrValueId p_lb = hf.new_value(ir::IrType::I64, "%lb");
    hf.values[p_lb].is_param = true;
    hf.params.push_back(p_lb);
    const ir::IrBlockId e = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;

    const uint32_t ln = 0;

    // Helpers locales (mismo patron que emit_native_itoa_to_buf).
    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v;
        ad.operands = {base, off};
        ad.source_line = ln;
        emit(current_block_, std::move(ad));
        return v;
    };
    auto new_slot = [&]() -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v;
        al.imm = 8;
        al.host_alloca = true;
        al.source_line = ln;
        emit(current_block_, std::move(al));
        return v;
    };
    auto load_i64 = [&](ir::IrValueId addr) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v;
        ld.operands = {addr};
        ld.source_line = ln;
        emit(current_block_, std::move(ld));
        return v;
    };
    auto store_i64 = [&](ir::IrValueId addr, ir::IrValueId val) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    };
    auto load_byte = [&](ir::IrValueId addr) -> ir::IrValueId {
        // LOAD U8 -> zero-extend a i64 (byte unsigned 0..255).
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::U8;
        ld.dst = v;
        ld.operands = {addr};
        ld.source_line = ln;
        emit(current_block_, std::move(ld));
        return v;
    };
    auto bin = [&](ir::IrOp op, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr in{};
        in.op = op;
        in.type = ir::IrType::I64;
        in.dst = v;
        in.operands = {a, b};
        in.source_line = ln;
        emit(current_block_, std::move(in));
        return v;
    };
    auto cmp = [&](ir::IrOp op, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr in{};
        in.op = op;
        in.type = ir::IrType::I64;
        in.dst = v;
        in.operands = {a, b};
        in.source_line = ln;
        emit(current_block_, std::move(in));
        return v;
    };
    auto new_block = [&]() -> ir::IrBlockId { return fn_->new_block(); };
    auto br = [&](ir::IrBlockId target) {
        ir::IrInstr b{};
        b.op = ir::IrOp::BR;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.target_block = target;
        b.source_line = ln;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(target);
        fn_->blocks[target].preds.push_back(current_block_);
    };
    auto br_cond = [&](ir::IrValueId cond, ir::IrBlockId t_true,
                       ir::IrBlockId t_false) {
        ir::IrInstr b{};
        b.op = ir::IrOp::BR_COND;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.operands = {cond};
        b.target_block = t_true;
        b.false_block = t_false;
        b.source_line = ln;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(t_true);
        fn_->blocks[current_block_].succs.push_back(t_false);
        fn_->blocks[t_true].preds.push_back(current_block_);
        fn_->blocks[t_false].preds.push_back(current_block_);
    };
    auto ret = [&](ir::IrValueId v) {
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::I64;
        rt.dst = ir::IR_NO_VALUE;
        rt.operands = {v};
        rt.source_line = ln;
        emit(current_block_, std::move(rt));
    };

    ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, ln);
    ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, ln);
    ir::IrValueId v_neg1 = emit_const(ir::IrType::I64, (uint64_t)(-1), ln);

    // min = (la < lb) ? la : lb.  Bloques: bb_minA / bb_minB / join.
    ir::IrValueId s_min = new_slot();
    {
        ir::IrValueId la_lt_lb = cmp(ir::IrOp::CMP_LT, p_la, p_lb); // signed
        ir::IrBlockId bb_minA = new_block();
        ir::IrBlockId bb_minB = new_block();
        ir::IrBlockId bb_minJ = new_block();
        br_cond(la_lt_lb, bb_minA, bb_minB);
        current_block_ = bb_minA;
        store_i64(s_min, p_la);
        br(bb_minJ);
        current_block_ = bb_minB;
        store_i64(s_min, p_lb);
        br(bb_minJ);
        current_block_ = bb_minJ;
    }

    // i = 0.
    ir::IrValueId s_i = new_slot();
    store_i64(s_i, v_zero);

    // Loop: while (i < min) { ca=pa[i]; cb=pb[i]; if(ca!=cb) ret cmp; i++; }
    ir::IrBlockId bb_hdr = new_block();
    br(bb_hdr);
    current_block_ = bb_hdr;
    {
        ir::IrValueId v_i = load_i64(s_i);
        ir::IrValueId v_min = load_i64(s_min);
        ir::IrValueId i_lt = cmp(ir::IrOp::CMP_LT, v_i, v_min); // signed
        ir::IrBlockId bb_body = new_block();
        ir::IrBlockId bb_tail = new_block();
        br_cond(i_lt, bb_body, bb_tail);

        // bb_body: comparar bytes.
        current_block_ = bb_body;
        {
            ir::IrValueId v_i2 = load_i64(s_i);
            ir::IrValueId v_a_at = ptr_add(p_pa, v_i2);
            ir::IrValueId v_b_at = ptr_add(p_pb, v_i2);
            ir::IrValueId v_ca = load_byte(v_a_at);
            ir::IrValueId v_cb = load_byte(v_b_at);
            ir::IrValueId ne = cmp(ir::IrOp::CMP_NE, v_ca, v_cb);
            ir::IrBlockId bb_diff = new_block();
            ir::IrBlockId bb_cont = new_block();
            br_cond(ne, bb_diff, bb_cont);

            // bb_diff: ret (ca < cb) ? -1 : 1.  Bytes 0..255 -> CMP_LT
            //          unsigned == signed (ambos positivos en i64).
            current_block_ = bb_diff;
            {
                ir::IrValueId lt = cmp(ir::IrOp::CMP_LT, v_ca, v_cb);
                ir::IrBlockId bb_lt = new_block();
                ir::IrBlockId bb_gt = new_block();
                br_cond(lt, bb_lt, bb_gt);
                current_block_ = bb_lt;
                ret(v_neg1);
                current_block_ = bb_gt;
                ret(v_one);
            }

            // bb_cont: i++ ; volver al header.
            current_block_ = bb_cont;
            {
                ir::IrValueId v_i3 = load_i64(s_i);
                ir::IrValueId v_i4 = bin(ir::IrOp::ADD, v_i3, v_one);
                store_i64(s_i, v_i4);
            }
            br(bb_hdr);
        }

        // bb_tail: prefijos iguales -> el mas corto es menor.
        current_block_ = bb_tail;
        {
            ir::IrValueId la_lt = cmp(ir::IrOp::CMP_LT, p_la, p_lb);
            ir::IrBlockId bb_short = new_block();
            ir::IrBlockId bb_chk_gt = new_block();
            br_cond(la_lt, bb_short, bb_chk_gt);
            current_block_ = bb_short;
            ret(v_neg1);
            current_block_ = bb_chk_gt;
            {
                ir::IrValueId la_gt = cmp(ir::IrOp::CMP_GT, p_la, p_lb);
                ir::IrBlockId bb_long = new_block();
                ir::IrBlockId bb_eq = new_block();
                br_cond(la_gt, bb_long, bb_eq);
                current_block_ = bb_long;
                ret(v_one);
                current_block_ = bb_eq;
                ret(v_zero);
            }
        }
    }

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));
    return name;
}

ir::IrValueId Lowering::build_native_string_interp(ast::StringLitExpr *slit) {
    // Vesta Embed Inc 2: interpolacion native.  Construimos un value-string
    // owned partiendo de un buffer vacio + appendeando cada parte (literal
    // o ${expr}).  El resultado es un slot {ptr,len,cap} de 24 bytes; el
    // caller registra su STRING_FREE.  Layout del literal: parts[0] +
    // exprs[0] + parts[1] + ... + parts[N] (N+1 parts para N exprs).
    const int line = slit->loc.line;
    const uint32_t ln = static_cast<uint32_t>(line);

    // Helper: addr = base + off (host).
    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v;
        ad.operands = {base, off};
        ad.source_line = ln;
        emit(current_block_, std::move(ad));
        return v;
    };

    // 1. Slot resultado vacio.  String Inc 5 (SSO): arrancamos como SSO
    //    vacio (len=0) -> CERO malloc inicial.  Los appends posteriores
    //    crecen inline (SSO) o transicionan a HEAP via
    //    build_native_string_append_inplace.  El slot vive en host stack.
    const ir::IrValueId v_slot = fn_->new_value(ir::IrType::PTR);
    if (native_poo_) fn_->values[v_slot].is_host_ptr = true;
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_slot;
        al.imm = 24;
        al.host_alloca = native_poo_;
        al.source_line = ln;
        emit(current_block_, std::move(al));
    }
    // String Inc 5 (SSO): zero-init qword0/1 + qword2 = (0 << 56) -> SSO
    // vacio (len 0, flag bit alto 0, data inline definida).  Los appends
    // crecen desde aqui.  Cubre los 24 bytes (sin uninitialised reads).
    emit_zero_native_str_slot(v_slot, ln);
    emit_str_meta_sso(v_slot, emit_const(ir::IrType::I64, 0, ln), ln);

    // Helper: appendear un literal de compile-time conocido.  Escribimos
    // sus bytes a un buffer scratch en stack (ALLOCA) y appendeamos via
    // build_native_string_append_inplace.  Para literales pequenos esto
    // es directo (bytes a STORE inline).
    auto append_literal = [&](const std::string &part) {
        if (part.empty()) return;
        const uint64_t plen = static_cast<uint64_t>(part.size());
        // Buffer scratch de plen bytes (sin nul; append no lo necesita).
        ir::IrValueId v_scratch = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr al{};
            al.op = ir::IrOp::ALLOCA;
            al.type = ir::IrType::I8;
            al.dst = v_scratch;
            al.imm = plen;
            al.host_alloca = native_poo_;
            al.source_line = ln;
            emit(current_block_, std::move(al));
        }
        if (native_poo_) fn_->values[v_scratch].is_host_ptr = true;
        // STOREs empaquetados (qword/dword/word/byte) de los bytes.
        std::vector<uint8_t> data(part.begin(), part.end());
        auto store_chunk = [&](uint64_t off, uint64_t val, ir::IrType ty) {
            ir::IrValueId v_dst =
                (off == 0)
                    ? v_scratch
                    : ptr_add(v_scratch, emit_const(ir::IrType::I64, off, ln));
            ir::IrValueId v_val = emit_const(ty, val, ln);
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ty;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_val, v_dst};
            st.source_line = ln;
            emit(current_block_, std::move(st));
        };
        auto pack = [&](uint64_t pos, int n) -> uint64_t {
            uint64_t v = 0;
            for (int k = 0; k < n; ++k)
                v |= static_cast<uint64_t>(data[pos + k]) << (8 * k);
            return v;
        };
        uint64_t pos = 0;
        for (; pos + 8 <= plen; pos += 8)
            store_chunk(pos, pack(pos, 8), ir::IrType::I64);
        if (pos + 4 <= plen) {
            store_chunk(pos, pack(pos, 4), ir::IrType::I32);
            pos += 4;
        }
        if (pos + 2 <= plen) {
            store_chunk(pos, pack(pos, 2), ir::IrType::I16);
            pos += 2;
        }
        if (pos + 1 <= plen) {
            store_chunk(pos, pack(pos, 1), ir::IrType::U8);
            pos += 1;
        }
        ir::IrValueId v_plen = emit_const(ir::IrType::I64, plen, ln);
        build_native_string_append_inplace(v_slot, v_scratch, v_plen, ln);
    };

    // Helper: appendear un ${expr}.  Segun el tipo del expr:
    //   string -> append de sus bytes (ptr/len del slot del expr).
    //   char   -> 1 byte (el char ya es un valor 0-255).
    //   int    -> itoa inline a un buffer scratch de 24 bytes.
    //   bool   -> "true"/"false" via branch + append literal.
    // BUG-3: extractor del KIND del format spec (ignora alineacion).
    auto fmt_kind_of = [](const std::string &fmt) -> std::string {
        size_t i = 0;
        while (i < fmt.size()) {
            while (i < fmt.size() && (fmt[i] == ' ' || fmt[i] == '\t'))
                ++i;
            if (i >= fmt.size()) break;
            if (fmt[i] == '<' || fmt[i] == '>') {
                ++i;
                while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
                    ++i;
                if (i < fmt.size() && fmt[i] != ':') ++i;
            } else {
                size_t start = i;
                while (i < fmt.size() && fmt[i] != ':' && fmt[i] != ' ' &&
                       fmt[i] != '\t')
                    ++i;
                std::string kw = fmt.substr(start, i - start);
                if (kw == "char" || kw == "hex" || kw == "bin" || kw == "oct" ||
                    kw == "dec" || kw == "ptr" || kw == "bool" || kw == "gc")
                    return kw;
            }
            while (i < fmt.size() && (fmt[i] == ' ' || fmt[i] == '\t'))
                ++i;
            if (i < fmt.size() && fmt[i] == ':') ++i;
        }
        return std::string();
    };
    auto append_expr = [&](ast::Expr *ex, const std::string &fmt) -> bool {
        if (!ex) return false;
        // String literal anidado (no interpolado) -> tratamos su texto
        // como literal directo.
        if (ex->kind == ast::NodeKind::StringLitExpr) {
            auto *sl = static_cast<ast::StringLitExpr *>(ex);
            if (!sl->is_interpolated()) {
                append_literal(sl->value);
                return true;
            }
        }
        const PrimitiveKind ek = ex->result_type.kind;
        // BUG-3: `${int:char}` -> codificar el valor como codepoint UTF-8 via
        // __vx_ctoa (paridad con interp/JIT), no como decimal.
        if (!fmt.empty() && fmt_kind_of(fmt) == "char" &&
            (is_integral(ek) || ek == PrimitiveKind::CHAR)) {
            ir::IrValueId v_cp = lower_expr(ex);
            if (v_cp == ir::IR_NO_VALUE) return false;
            // Promover a i64 para el helper (cp puede ser hasta 0x10FFFF).
            if (ek != PrimitiveKind::I64 && ek != PrimitiveKind::U64) {
                ir::IrValueId v64 = fn_->new_value(ir::IrType::I64);
                ir::IrInstr ext{};
                ext.op =
                    is_signed_integral(ek) ? ir::IrOp::SEXT : ir::IrOp::ZEXT;
                ext.type = ir::IrType::I64;
                ext.dst = v64;
                ext.operands = {v_cp};
                ext.source_line = ln;
                emit(current_block_, std::move(ext));
                v_cp = v64;
            }
            // scratch de 4 bytes (max UTF-8).
            ir::IrValueId v_scr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA;
                al.type = ir::IrType::I8;
                al.dst = v_scr;
                al.imm = 4;
                al.host_alloca = native_poo_;
                al.source_line = ln;
                emit(current_block_, std::move(al));
            }
            if (native_poo_) fn_->values[v_scr].is_host_ptr = true;
            const std::string ctoa_fn = ensure_ctoa_helper();
            ir::IrValueId v_len = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr ca{};
                ca.op = ir::IrOp::CALL;
                ca.type = ir::IrType::I64;
                ca.dst = v_len;
                ca.func_name = ctoa_fn;
                ca.operands = {v_scr, v_cp};
                ca.source_line = ln;
                emit(current_block_, std::move(ca));
            }
            build_native_string_append_inplace(v_slot, v_scr, v_len, ln);
            return true;
        }
        if (ek == PrimitiveKind::STRING) {
            // El expr produce un value-string (PTR a slot).  Inc 5 (SSO):
            // (ptr, len) via accesores flag-aware.  Append de sus bytes.
            ir::IrValueId v_src = lower_expr(ex);
            if (v_src == ir::IR_NO_VALUE) return false;
            ir::IrValueId v_sptr = emit_native_str_data_ptr(v_src, ln);
            ir::IrValueId v_slen = emit_native_str_len(v_src, ln);
            build_native_string_append_inplace(v_slot, v_sptr, v_slen, ln);
            return true;
        }
        if (ek == PrimitiveKind::CHAR) {
            ir::IrValueId v_ch = lower_expr(ex);
            if (v_ch == ir::IR_NO_VALUE) return false;
            // Buffer scratch de 1 byte con el char.
            ir::IrValueId v_scr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA;
                al.type = ir::IrType::I8;
                al.dst = v_scr;
                al.imm = 1;
                al.host_alloca = native_poo_;
                al.source_line = ln;
                emit(current_block_, std::move(al));
            }
            if (native_poo_) fn_->values[v_scr].is_host_ptr = true;
            {
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE;
                st.type = ir::IrType::U8;
                st.dst = ir::IR_NO_VALUE;
                st.operands = {v_ch, v_scr};
                st.source_line = ln;
                emit(current_block_, std::move(st));
            }
            build_native_string_append_inplace(
                v_slot, v_scr, emit_const(ir::IrType::I64, 1, ln), ln);
            return true;
        }
        // Enteros (i8..i64/u8..u64): itoa decimal inline a un scratch de
        // 24 bytes, luego append de los `len` bytes escritos.
        if (is_integral(ek)) {
            ir::IrValueId v_int = lower_expr(ex);
            if (v_int == ir::IR_NO_VALUE) return false;
            // El itoa trabaja en i64: promover si es mas estrecho.
            if (ek != PrimitiveKind::I64 && ek != PrimitiveKind::U64) {
                ir::IrValueId v64 = fn_->new_value(ir::IrType::I64);
                ir::IrInstr ext{};
                ext.op =
                    is_signed_integral(ek) ? ir::IrOp::SEXT : ir::IrOp::ZEXT;
                ext.type = ir::IrType::I64;
                ext.dst = v64;
                ext.operands = {v_int};
                ext.source_line = ln;
                emit(current_block_, std::move(ext));
                v_int = v64;
            }
            // scratch de 24 bytes (suficiente para i64 con signo).
            ir::IrValueId v_scr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA;
                al.type = ir::IrType::I8;
                al.dst = v_scr;
                al.imm = 24;
                al.host_alloca = native_poo_;
                al.source_line = ln;
                emit(current_block_, std::move(al));
            }
            if (native_poo_) fn_->values[v_scr].is_host_ptr = true;
            // CALL al helper itoa (no inline): evita el const-fold
            // mid-expression que daba longitudes erroneas con argumento
            // constante (el itoa vive en una funcion aparte con loops).
            const std::string itoa_fn =
                ensure_itoa_helper(is_signed_integral(ek));
            ir::IrValueId v_len = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr ca{};
                ca.op = ir::IrOp::CALL;
                ca.type = ir::IrType::I64;
                ca.dst = v_len;
                ca.func_name = itoa_fn;
                ca.operands = {v_scr, v_int};
                ca.source_line = ln;
                emit(current_block_, std::move(ca));
            }
            build_native_string_append_inplace(v_slot, v_scr, v_len, ln);
            return true;
        }
        // bool: "true" (4) / "false" (5) via helper btoa (no inline,
        // branch -> fold-safe con argumento constante).
        if (ek == PrimitiveKind::BOOL) {
            ir::IrValueId v_b = lower_expr(ex);
            if (v_b == ir::IR_NO_VALUE) return false;
            // Promover el bool a i64 para el helper (cmp != 0).
            ir::IrValueId v_b64 = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr ext{};
                ext.op = ir::IrOp::ZEXT;
                ext.type = ir::IrType::I64;
                ext.dst = v_b64;
                ext.operands = {v_b};
                ext.source_line = ln;
                emit(current_block_, std::move(ext));
            }
            // scratch de 8 bytes (cabe "false" + margen).
            ir::IrValueId v_scr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA;
                al.type = ir::IrType::I8;
                al.dst = v_scr;
                al.imm = 8;
                al.host_alloca = native_poo_;
                al.source_line = ln;
                emit(current_block_, std::move(al));
            }
            if (native_poo_) fn_->values[v_scr].is_host_ptr = true;
            const std::string btoa_fn = ensure_btoa_helper();
            ir::IrValueId v_len = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr ca{};
                ca.op = ir::IrOp::CALL;
                ca.type = ir::IrType::I64;
                ca.dst = v_len;
                ca.func_name = btoa_fn;
                ca.operands = {v_scr, v_b64};
                ca.source_line = ln;
                emit(current_block_, std::move(ca));
            }
            build_native_string_append_inplace(v_slot, v_scr, v_len, ln);
            return true;
        }
        // Tipo no soportado en interpolacion native (float/struct/class/
        // enum) -- Inc 2b futuro.  Diagnostico claro.
        error_at(ex->loc,
                 "interpolacion `${expr}` native (AOT): solo se soportan "
                 "todavia string, char, int y bool.  float (y struct/class/"
                 "enum) quedan para un follow-up (requieren codegen de "
                 "bloques anidados en la construccion del string).");
        return false;
    };

    const size_t ne = slit->interp_exprs.size();
    const size_t np = slit->interp_parts.size();
    // parts[0].
    if (np > 0) append_literal(slit->interp_parts[0]);
    for (size_t i = 0; i < ne; ++i) {
        const std::string fmt_i = (i < slit->interp_formats.size())
                                      ? slit->interp_formats[i]
                                      : std::string();
        if (!append_expr(slit->interp_exprs[i].get(), fmt_i))
            return ir::IR_NO_VALUE;
        if (i + 1 < np) append_literal(slit->interp_parts[i + 1]);
    }
    return v_slot;
}

void Lowering::emit_word_copy_loop(ir::IrValueId dst_base,
                                   ir::IrValueId src_base, ir::IrValueId v_len,
                                   uint32_t source_line) {
    // Copia v_len bytes de src_base -> dst_base.  Estrategia: dos loops.
    //   (1) Loop de PALABRA: mientras i + 8 <= len, copia un qword (LOAD
    //       i64 + STORE i64) y avanza i += 8.  ~8x menos iteraciones que
    //       byte-a-byte.
    //   (2) Loop de COLA: copia los <8 bytes restantes byte-a-byte
    //       (mientras i < len).
    // El contador i vive en un ALLOCA de 8 bytes (mem2reg lo promueve a
    // PHI en O2).  Las direcciones src/dst se recalculan con ADD por
    // iteracion.  Sin registros fijos -> cero impacto en el regalloc.
    // Correctness: nunca lee/escribe fuera de [base, base+len) (el qword
    // solo corre cuando i+8 <= len; la cola cubre el resto exacto).  El
    // buffer destino tiene cap = total+1 bytes -> margen suficiente.

    // Helper local: addr = base + off (off es un IrValue I64).
    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_addr].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr;
        ad.operands = {base, off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v_addr;
    };

    // Slot del contador i = 0 (compartido por ambos loops).
    const ir::IrValueId v_i_slot = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_i_slot;
        al.imm = 8;
        al.source_line = source_line;
        emit(current_block_, std::move(al));
    }
    {
        ir::IrValueId v_z = emit_const(ir::IrType::I64, 0, source_line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_z, v_i_slot};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    }

    // limit8 = len - 7 (el qword corre mientras i < limit8, i.e. i+8 <= len).
    // Para len < 8 -> limit8 <= 0 -> el loop de palabra no entra (i=0 >= 0
    // no se cumple con CMP_LT signed) y todo se copia por la cola.
    ir::IrValueId v_seven = emit_const(ir::IrType::I64, 7, source_line);
    ir::IrValueId v_limit8 = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr su{};
        su.op = ir::IrOp::SUB;
        su.type = ir::IrType::I64;
        su.dst = v_limit8;
        su.operands = {v_len, v_seven};
        su.source_line = source_line;
        emit(current_block_, std::move(su));
    }

    // ---- Loop 1: copia de palabra (8 bytes/iter). ----
    {
        const ir::IrBlockId hdr = fn_->new_block("wcopy_w_hdr");
        const ir::IrBlockId body = fn_->new_block("wcopy_w_body");
        const ir::IrBlockId done = fn_->new_block("wcopy_w_done");
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.target_block = hdr;
            br.source_line = source_line;
            emit(current_block_, std::move(br));
        }
        fn_->blocks[current_block_].succs.push_back(hdr);
        fn_->blocks[hdr].preds.push_back(current_block_);

        // hdr: i = load slot ; cond = i < limit8 ; br body, done
        current_block_ = hdr;
        ir::IrValueId v_i = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_i;
            ld.operands = {v_i_slot};
            ld.source_line = source_line;
            emit(current_block_, std::move(ld));
        }
        ir::IrValueId v_cond = fn_->new_value(ir::IrType::BOOL);
        {
            ir::IrInstr cmp{};
            cmp.op = ir::IrOp::CMP_LT; // signed; len/i no negativos
            cmp.type = ir::IrType::BOOL;
            cmp.dst = v_cond;
            cmp.operands = {v_i, v_limit8};
            cmp.source_line = source_line;
            emit(current_block_, std::move(cmp));
        }
        {
            ir::IrInstr brc{};
            brc.op = ir::IrOp::BR_COND;
            brc.operands = {v_cond};
            brc.target_block = body;
            brc.false_block = done;
            brc.source_line = source_line;
            emit(current_block_, std::move(brc));
        }
        fn_->blocks[hdr].succs.push_back(body);
        fn_->blocks[hdr].succs.push_back(done);
        fn_->blocks[body].preds.push_back(hdr);
        fn_->blocks[done].preds.push_back(hdr);

        // body: w = load.i64 src+i ; store.i64 w -> dst+i ; i += 8 ; -> hdr
        current_block_ = body;
        ir::IrValueId v_src = ptr_add(src_base, v_i);
        ir::IrValueId v_dst = ptr_add(dst_base, v_i);
        ir::IrValueId v_w = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_w;
            ld.operands = {v_src};
            ld.source_line = source_line;
            emit(current_block_, std::move(ld));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_w, v_dst};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        }
        ir::IrValueId v_i8 = fn_->new_value(ir::IrType::I64);
        {
            ir::IrValueId v_8 = emit_const(ir::IrType::I64, 8, source_line);
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_i8;
            ad.operands = {v_i, v_8};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_i8, v_i_slot};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        }
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.target_block = hdr;
            br.source_line = source_line;
            emit(current_block_, std::move(br));
        }
        fn_->blocks[body].succs.push_back(hdr);
        fn_->blocks[hdr].preds.push_back(body);

        current_block_ = done;
        block_terminated_ = false;
    }

    // ---- Loop 2: cola byte-a-byte (mientras i < len). ----
    {
        const ir::IrBlockId hdr = fn_->new_block("wcopy_b_hdr");
        const ir::IrBlockId body = fn_->new_block("wcopy_b_body");
        const ir::IrBlockId done = fn_->new_block("wcopy_b_done");
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.target_block = hdr;
            br.source_line = source_line;
            emit(current_block_, std::move(br));
        }
        fn_->blocks[current_block_].succs.push_back(hdr);
        fn_->blocks[hdr].preds.push_back(current_block_);

        // hdr: i = load slot ; cond = i < len ; br body, done
        current_block_ = hdr;
        ir::IrValueId v_i = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_i;
            ld.operands = {v_i_slot};
            ld.source_line = source_line;
            emit(current_block_, std::move(ld));
        }
        ir::IrValueId v_cond = fn_->new_value(ir::IrType::BOOL);
        {
            ir::IrInstr cmp{};
            cmp.op = ir::IrOp::CMP_LT;
            cmp.type = ir::IrType::BOOL;
            cmp.dst = v_cond;
            cmp.operands = {v_i, v_len};
            cmp.source_line = source_line;
            emit(current_block_, std::move(cmp));
        }
        {
            ir::IrInstr brc{};
            brc.op = ir::IrOp::BR_COND;
            brc.operands = {v_cond};
            brc.target_block = body;
            brc.false_block = done;
            brc.source_line = source_line;
            emit(current_block_, std::move(brc));
        }
        fn_->blocks[hdr].succs.push_back(body);
        fn_->blocks[hdr].succs.push_back(done);
        fn_->blocks[body].preds.push_back(hdr);
        fn_->blocks[done].preds.push_back(hdr);

        // body: byte = load.u8 src+i ; store.u8 byte -> dst+i ; i += 1 ; -> hdr
        current_block_ = body;
        ir::IrValueId v_src = ptr_add(src_base, v_i);
        ir::IrValueId v_dst = ptr_add(dst_base, v_i);
        ir::IrValueId v_byte = fn_->new_value(ir::IrType::U8);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::U8;
            ld.dst = v_byte;
            ld.operands = {v_src};
            ld.source_line = source_line;
            emit(current_block_, std::move(ld));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::U8;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_byte, v_dst};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        }
        ir::IrValueId v_i1 = fn_->new_value(ir::IrType::I64);
        {
            ir::IrValueId v_1 = emit_const(ir::IrType::I64, 1, source_line);
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_i1;
            ad.operands = {v_i, v_1};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_i1, v_i_slot};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        }
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.target_block = hdr;
            br.source_line = source_line;
            emit(current_block_, std::move(br));
        }
        fn_->blocks[body].succs.push_back(hdr);
        fn_->blocks[hdr].preds.push_back(body);

        current_block_ = done;
        block_terminated_ = false;
    }
}

ir::IrValueId Lowering::load_native_string_field(ir::IrValueId v_slot,
                                                 uint64_t byte_off,
                                                 bool as_host,
                                                 uint32_t source_line) {
    ir::IrValueId v_addr = v_slot;
    if (byte_off > 0) {
        ir::IrValueId v_off =
            emit_const(ir::IrType::I64, byte_off, source_line);
        v_addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr;
        ad.operands = {v_slot, v_off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }
    const ir::IrType rt = as_host ? ir::IrType::PTR : ir::IrType::I64;
    ir::IrValueId v_val = fn_->new_value(rt);
    if (as_host) fn_->values[v_val].is_host_ptr = true;
    ir::IrInstr ld{};
    ld.op = ir::IrOp::LOAD;
    ld.type = ir::IrType::I64;
    ld.dst = v_val;
    ld.operands = {v_addr};
    ld.source_line = source_line;
    emit(current_block_, std::move(ld));
    return v_val;
}

// -----------------------------------------------------------------------
// String Inc 5 (SSO): accesores flag-aware del value-string nativo.
//
// Layout union de 24 bytes con flag en el bit alto del byte [23]:
//   HEAP (byte[23]&0x80 != 0): ptr@0, len@8, cap en bytes[16..22] (56b).
//   SSO  (byte[23]&0x80 == 0): data inline en bytes[0..21], len en
//                              byte[23]&0x7F, nul en byte[len].
// Los accesores son BRANCHLESS (mascara aritmetica) -> sin CFG, barato y
// robusto en el regalloc (sin PHIs).
// -----------------------------------------------------------------------

ir::IrValueId Lowering::emit_native_str_is_heap(ir::IrValueId v_slot,
                                                uint32_t source_line) {
    // is_heap = (byte[23] >> 7) & 1.  Cargamos el byte [23] (u8
    // zero-extended) y lo desplazamos 7 bits.  Resultado I64 0 o 1.
    ir::IrValueId v_off = emit_const(ir::IrType::I64, 23, source_line);
    ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_addr].is_host_ptr = fn_->values[v_slot].is_host_ptr;
    {
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr;
        ad.operands = {v_slot, v_off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }
    ir::IrValueId v_b23 = fn_->new_value(ir::IrType::U8);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::U8;
        ld.dst = v_b23;
        ld.operands = {v_addr};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
    }
    // is_heap = b23 >> 7  (logico; b23 es 0..255 zero-extended).
    ir::IrValueId v_seven = emit_const(ir::IrType::I64, 7, source_line);
    ir::IrValueId v_is_heap = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr sh{};
        sh.op = ir::IrOp::SHR;
        sh.type = ir::IrType::I64;
        sh.dst = v_is_heap;
        sh.operands = {v_b23, v_seven};
        sh.source_line = source_line;
        emit(current_block_, std::move(sh));
    }
    return v_is_heap;
}

ir::IrValueId Lowering::emit_native_str_is_owned(ir::IrValueId v_slot,
                                                 uint32_t source_line) {
    // owned = (byte[23] >> 7) & ~(byte[23] >> 6) & 1, sin ramas:
    //   b23 >> 6 da 0b11 para un prestado (bits 7 y 6) y 0b10 para uno
    //   propio, asi que basta comparar con 2.  Se hace con aritmetica para no
    //   introducir un salto en el camino de salida de cada scope.
    //
    //   propio    -> (b23 >> 6) == 0b10 = 2 -> owned = 1
    //   prestado  -> (b23 >> 6) == 0b11 = 3 -> owned = 0
    //   SSO       -> (b23 >> 6) == 0b00 = 0 -> owned = 0 (no hay buffer)
    ir::IrValueId v_off = emit_const(ir::IrType::I64, 23, source_line);
    ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_addr].is_host_ptr = fn_->values[v_slot].is_host_ptr;
    {
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr;
        ad.operands = {v_slot, v_off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }
    ir::IrValueId v_b23 = fn_->new_value(ir::IrType::U8);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::U8;
        ld.dst = v_b23;
        ld.operands = {v_addr};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
    }
    ir::IrValueId v_six = emit_const(ir::IrType::I64, 6, source_line);
    ir::IrValueId v_top2 = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr sh{};
        sh.op = ir::IrOp::SHR;
        sh.type = ir::IrType::I64;
        sh.dst = v_top2;
        sh.operands = {v_b23, v_six};
        sh.source_line = source_line;
        emit(current_block_, std::move(sh));
    }
    ir::IrValueId v_dos = emit_const(ir::IrType::I64, 2, source_line);
    ir::IrValueId v_owned = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr cm{};
        cm.op = ir::IrOp::CMP_EQ;
        cm.type = ir::IrType::I64;
        cm.dst = v_owned;
        cm.operands = {v_top2, v_dos};
        cm.source_line = source_line;
        emit(current_block_, std::move(cm));
    }
    return v_owned;
}

ir::IrValueId Lowering::emit_native_str_data_ptr_inline(ir::IrValueId v_slot,
                                                        uint32_t source_line) {
    // data_ptr = is_heap ? LOAD ptr@0 : &slot.
    // Branchless: slot + is_heap*(ptr0 - slot).
    //   - SSO  (is_heap=0): slot + 0 = slot           (data inline @0).
    //   - HEAP (is_heap=1): slot + (ptr0 - slot) = ptr0.
    ir::IrValueId v_is_heap = emit_native_str_is_heap(v_slot, source_line);
    // ptr0 cargado SIEMPRE (slot+0 es memoria valida en ambos modos; en
    // SSO son bytes de data, pero solo los usamos si is_heap=1).  Lo
    // tratamos como I64 para la aritmetica de mascara.
    ir::IrValueId v_ptr0 = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_ptr0;
        ld.operands = {v_slot};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
    }
    // slot como I64 (la direccion del slot).  BITCAST PTR->I64.
    ir::IrValueId v_slot_i = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr bc{};
        bc.op = ir::IrOp::BITCAST;
        bc.type = ir::IrType::I64;
        bc.dst = v_slot_i;
        bc.operands = {v_slot};
        bc.source_line = source_line;
        emit(current_block_, std::move(bc));
    }
    // diff = ptr0 - slot.
    ir::IrValueId v_diff = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr s{};
        s.op = ir::IrOp::SUB;
        s.type = ir::IrType::I64;
        s.dst = v_diff;
        s.operands = {v_ptr0, v_slot_i};
        s.source_line = source_line;
        emit(current_block_, std::move(s));
    }
    // masked = diff & (-is_heap)  (AND, no MUL: valgrind sabe x&0=0 es
    // definido aunque diff use ptr0 con bits de data inline SSO).
    ir::IrValueId v_mask = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ng{};
        ng.op = ir::IrOp::NEG;
        ng.type = ir::IrType::I64;
        ng.dst = v_mask;
        ng.operands = {v_is_heap};
        ng.source_line = source_line;
        emit(current_block_, std::move(ng));
    }
    ir::IrValueId v_masked = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr an{};
        an.op = ir::IrOp::AND;
        an.type = ir::IrType::I64;
        an.dst = v_masked;
        an.operands = {v_diff, v_mask};
        an.source_line = source_line;
        emit(current_block_, std::move(an));
    }
    // data = slot + masked.
    ir::IrValueId v_data = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_data].is_host_ptr = true;
    {
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_data;
        ad.operands = {v_slot_i, v_masked};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }
    return v_data;
}

ir::IrValueId Lowering::emit_native_str_len_inline(ir::IrValueId v_slot,
                                                   uint32_t source_line) {
    // len = is_heap ? LOAD len@8 : (byte[23] & 0x7F).
    // Branchless: sso_len + (diff & -is_heap), donde diff = heap_len -
    // sso_len.
    ir::IrValueId v_is_heap = emit_native_str_is_heap(v_slot, source_line);
    // sso_len = byte[23] & 0x7F.
    ir::IrValueId v_off23 = emit_const(ir::IrType::I64, 23, source_line);
    ir::IrValueId v_addr23 = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_addr23].is_host_ptr = fn_->values[v_slot].is_host_ptr;
    {
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr23;
        ad.operands = {v_slot, v_off23};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }
    ir::IrValueId v_b23 = fn_->new_value(ir::IrType::U8);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::U8;
        ld.dst = v_b23;
        ld.operands = {v_addr23};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
    }
    ir::IrValueId v_mask7f = emit_const(ir::IrType::I64, 0x7F, source_line);
    ir::IrValueId v_sso_len = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr an{};
        an.op = ir::IrOp::AND;
        an.type = ir::IrType::I64;
        an.dst = v_sso_len;
        an.operands = {v_b23, v_mask7f};
        an.source_line = source_line;
        emit(current_block_, std::move(an));
    }
    // heap_len = LOAD len@8.
    ir::IrValueId v_heap_len =
        load_native_string_field(v_slot, 8, /*as_host=*/false, source_line);
    // diff = heap_len - sso_len.
    ir::IrValueId v_diff = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr s{};
        s.op = ir::IrOp::SUB;
        s.type = ir::IrType::I64;
        s.dst = v_diff;
        s.operands = {v_heap_len, v_sso_len};
        s.source_line = source_line;
        emit(current_block_, std::move(s));
    }
    // masked = diff & (-is_heap)  (AND, no MUL: heap_len puede ser un
    // LOAD de bytes no inicializados en modo SSO; x&0=0 es definido).
    ir::IrValueId v_mask = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ng{};
        ng.op = ir::IrOp::NEG;
        ng.type = ir::IrType::I64;
        ng.dst = v_mask;
        ng.operands = {v_is_heap};
        ng.source_line = source_line;
        emit(current_block_, std::move(ng));
    }
    ir::IrValueId v_masked = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr an{};
        an.op = ir::IrOp::AND;
        an.type = ir::IrType::I64;
        an.dst = v_masked;
        an.operands = {v_diff, v_mask};
        an.source_line = source_line;
        emit(current_block_, std::move(an));
    }
    // len = sso_len + masked.
    ir::IrValueId v_len = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_len;
        ad.operands = {v_sso_len, v_masked};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }
    return v_len;
}

ir::IrValueId Lowering::emit_native_str_data_ptr(ir::IrValueId v_slot,
                                                 uint32_t source_line) {
    // CALL __vx_strdata(s) -> u8* (la logica branchless vive en el helper;
    // ver ensure_strdata_helper / el comentario del blacklist del inliner).
    const std::string name = ensure_strdata_helper();
    ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true;
    ir::IrInstr ca{};
    ca.op = ir::IrOp::CALL;
    ca.type = ir::IrType::PTR;
    ca.dst = v;
    ca.func_name = name;
    ca.operands = {v_slot};
    ca.source_line = source_line;
    emit(current_block_, std::move(ca));
    return v;
}

ir::IrValueId Lowering::emit_native_str_len(ir::IrValueId v_slot,
                                            uint32_t source_line) {
    // Fuera de native_poo_ (no deberia ocurrir: el value-string solo existe
    // en AOT native) -> CALL directo al baseline, sin dispatch (el init NO se
    // prepone en main no-native, asi que el fp quedaria a null).
    if (!native_poo_) {
        const std::string name = ensure_strlen_helper();
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ca{};
        ca.op = ir::IrOp::CALL;
        ca.type = ir::IrType::I64;
        ca.dst = v;
        ca.func_name = name;
        ca.operands = {v_slot};
        ca.source_line = source_line;
        emit(current_block_, std::move(ca));
        return v;
    }
    // CPU dispatch Inc 5a: strlen(s) -> i64 DESPACHADO por tabla de punteros:
    // `call [__vx_strlen_fp]`.  El fp apunta al baseline (__vx_strlen_base)
    // o al @HelperOverride(strlen) del usuario.  ensure_strdisp() es
    // idempotente y marca cpu_dispatch_used_ para wirear el init en main.
    ensure_strdisp();
    const uint64_t fp_slot = strlen_fp_slot_;
    // v_fpaddr = &__vx_strlen_fp ; v_fp = LOAD i64 [v_fpaddr].
    ir::IrValueId v_fpaddr = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_fpaddr].is_host_ptr = true;
    {
        ir::IrInstr la{};
        la.op = ir::IrOp::STR_LIT_ADDR;
        la.type = ir::IrType::PTR;
        la.dst = v_fpaddr;
        la.imm = fp_slot;
        la.source_line = source_line;
        emit(current_block_, std::move(la));
    }
    ir::IrValueId v_fp = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_fp].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_fp;
        ld.operands = {v_fpaddr};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
    }
    // CALLIND v_fp(s) -> i64.
    ir::IrValueId v = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ci{};
    ci.op = ir::IrOp::CALLIND;
    ci.type = ir::IrType::I64;
    ci.dst = v;
    ci.func_ptr = v_fp;
    ci.operands = {v_slot};
    ci.source_line = source_line;
    emit(current_block_, std::move(ci));
    return v;
}

// -------------------------------------------------------------------------
// Vesta Embed Inc 6 (encoding UTF-8): conteo de code-points + conversion a
// UTF-16 (.length() / .wstr()).  Ambos como helpers IR aparte (loop) ->
// fuera del const-fold y del inliner (prefijo __vx_str), self-contained
// (solo malloc en utf16, overridable) -> funciona freestanding.
// -------------------------------------------------------------------------
std::string Lowering::ensure_str_cplen_helper() {
    // i64 __vx_str_cplen(u8* p, i64 byte_len):
    //   count = 0;
    //   for (i = 0; i < byte_len; i++)
    //     if ((p[i] & 0xC0) != 0x80) count++;   // no es byte de continuacion
    //   ret count;
    // Para ASCII puro coincide con byte_len (cada byte < 0x80).
    const std::string name = "__vx_str_cplen";
    if (str_cplen_helper_emitted_) return name;
    str_cplen_helper_emitted_ = true;

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::I64;
    const ir::IrValueId p_p = hf.new_value(ir::IrType::PTR, "%p");
    hf.values[p_p].is_param = true;
    hf.values[p_p].is_host_ptr = true;
    hf.params.push_back(p_p);
    const ir::IrValueId p_blen = hf.new_value(ir::IrType::I64, "%blen");
    hf.values[p_blen].is_param = true;
    hf.params.push_back(p_blen);
    const ir::IrBlockId entry = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = entry;
    block_terminated_ = false;
    const uint32_t ln = 0;

    // Toolkit local (mismo patron que ensure_strcmp_helper).
    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v;
        ad.operands = {base, off};
        ad.source_line = ln;
        emit(current_block_, std::move(ad));
        return v;
    };
    auto new_slot = [&]() -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v;
        al.imm = 8;
        al.host_alloca = true;
        al.source_line = ln;
        emit(current_block_, std::move(al));
        return v;
    };
    auto load_i64 = [&](ir::IrValueId addr) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v;
        ld.operands = {addr};
        ld.source_line = ln;
        emit(current_block_, std::move(ld));
        return v;
    };
    auto store_i64 = [&](ir::IrValueId addr, ir::IrValueId val) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    };
    auto load_byte = [&](ir::IrValueId addr) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::U8;
        ld.dst = v;
        ld.operands = {addr};
        ld.source_line = ln;
        emit(current_block_, std::move(ld));
        return v;
    };
    auto bin = [&](ir::IrOp op, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr in{};
        in.op = op;
        in.type = ir::IrType::I64;
        in.dst = v;
        in.operands = {a, b};
        in.source_line = ln;
        emit(current_block_, std::move(in));
        return v;
    };
    auto br = [&](ir::IrBlockId target) {
        ir::IrInstr b{};
        b.op = ir::IrOp::BR;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.target_block = target;
        b.source_line = ln;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(target);
        fn_->blocks[target].preds.push_back(current_block_);
    };
    auto br_cond = [&](ir::IrValueId cond, ir::IrBlockId t_true,
                       ir::IrBlockId t_false) {
        ir::IrInstr b{};
        b.op = ir::IrOp::BR_COND;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.operands = {cond};
        b.target_block = t_true;
        b.false_block = t_false;
        b.source_line = ln;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(t_true);
        fn_->blocks[current_block_].succs.push_back(t_false);
        fn_->blocks[t_true].preds.push_back(current_block_);
        fn_->blocks[t_false].preds.push_back(current_block_);
    };

    ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, ln);
    ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, ln);
    ir::IrValueId v_c0 = emit_const(ir::IrType::I64, 0xC0, ln);
    ir::IrValueId v_80 = emit_const(ir::IrType::I64, 0x80, ln);

    // i = 0 ; count = 0.
    ir::IrValueId s_i = new_slot();
    ir::IrValueId s_cnt = new_slot();
    store_i64(s_i, v_zero);
    store_i64(s_cnt, v_zero);

    // header: while (i < byte_len)
    ir::IrBlockId bb_hdr = fn_->new_block();
    br(bb_hdr);
    current_block_ = bb_hdr;
    ir::IrValueId v_i = load_i64(s_i);
    ir::IrValueId i_lt = bin(ir::IrOp::CMP_LT, v_i, p_blen);
    ir::IrBlockId bb_body = fn_->new_block();
    ir::IrBlockId bb_exit = fn_->new_block();
    br_cond(i_lt, bb_body, bb_exit);

    // body: b = p[i]; if ((b & 0xC0) != 0x80) count++; i++.
    current_block_ = bb_body;
    ir::IrValueId v_i2 = load_i64(s_i);
    ir::IrValueId v_at = ptr_add(p_p, v_i2);
    ir::IrValueId v_b = load_byte(v_at);
    ir::IrValueId v_hi = bin(ir::IrOp::AND, v_b, v_c0);
    ir::IrValueId is_cont = bin(ir::IrOp::CMP_EQ, v_hi, v_80);
    ir::IrBlockId bb_inc = fn_->new_block(); // no-continuacion -> count++
    ir::IrBlockId bb_adv = fn_->new_block(); // avanza i
    // si is_cont (==0x80) saltar el count++; si no, contarlo.
    br_cond(is_cont, bb_adv, bb_inc);
    current_block_ = bb_inc;
    {
        ir::IrValueId v_c = load_i64(s_cnt);
        store_i64(s_cnt, bin(ir::IrOp::ADD, v_c, v_one));
    }
    br(bb_adv);
    current_block_ = bb_adv;
    {
        ir::IrValueId v_i3 = load_i64(s_i);
        store_i64(s_i, bin(ir::IrOp::ADD, v_i3, v_one));
    }
    br(bb_hdr);

    // exit: ret count.
    current_block_ = bb_exit;
    {
        ir::IrValueId v_cnt = load_i64(s_cnt);
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::I64;
        rt.dst = ir::IR_NO_VALUE;
        rt.operands = {v_cnt};
        rt.source_line = ln;
        emit(current_block_, std::move(rt));
    }
    block_terminated_ = true;

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));
    return name;
}

ir::IrValueId Lowering::emit_native_str_cplen(ir::IrValueId v_ptr,
                                              ir::IrValueId v_blen,
                                              uint32_t source_line) {
    const std::string name = ensure_str_cplen_helper();
    ir::IrValueId v = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ca{};
    ca.op = ir::IrOp::CALL;
    ca.type = ir::IrType::I64;
    ca.dst = v;
    ca.func_name = name;
    ca.operands = {v_ptr, v_blen};
    ca.source_line = source_line;
    emit(current_block_, std::move(ca));
    return v;
}

std::string Lowering::ensure_str_to_utf16_helper() {
    // u16* __vx_str_to_utf16(u8* p, i64 byte_len):
    //   out = malloc((byte_len + 1) * 2)   // cota superior: ASCII = 1
    //   unit/byte i = 0; ob = 0;                       // i=byte idx, ob=output
    //   byte off while (i < byte_len):
    //     b0 = p[i]
    //     if      b0 < 0x80: cp = b0;                                     i+=1
    //     elif    b0 < 0xE0: cp = ((b0&0x1F)<<6)|c(1);                    i+=2
    //     elif    b0 < 0xF0: cp = ((b0&0x0F)<<12)|(c(1)<<6)|c(2);         i+=3
    //     else:              cp = ((b0&0x07)<<18)|(c(1)<<12)|(c(2)<<6)|c(3);
    //     i+=4
    //       (c(k) = p[i+k] & 0x3F)
    //     if cp < 0x10000: out[ob]=cp; ob+=2
    //     else: cp-=0x10000; out[ob]=0xD800|(cp>>10);
    //     out[ob+2]=0xDC00|(cp&0x3FF); ob+=4
    //   out[ob] = 0   // NUL u16
    //   ret out
    // Asume UTF-8 bien formado (el value-string se construye de literales/
    // concat validos).  El CALLER es dueno del buffer (transitorio para FFI).
    const std::string name = "__vx_str_to_utf16";
    if (str_to_utf16_helper_emitted_) return name;
    str_to_utf16_helper_emitted_ = true;

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::PTR;
    const ir::IrValueId p_p = hf.new_value(ir::IrType::PTR, "%p");
    hf.values[p_p].is_param = true;
    hf.values[p_p].is_host_ptr = true;
    hf.params.push_back(p_p);
    const ir::IrValueId p_blen = hf.new_value(ir::IrType::I64, "%blen");
    hf.values[p_blen].is_param = true;
    hf.params.push_back(p_blen);
    const ir::IrBlockId entry = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = entry;
    block_terminated_ = false;
    const uint32_t ln = 0;

    // Toolkit local.
    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v;
        ad.operands = {base, off};
        ad.source_line = ln;
        emit(current_block_, std::move(ad));
        return v;
    };
    auto new_slot = [&]() -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v;
        al.imm = 8;
        al.host_alloca = true;
        al.source_line = ln;
        emit(current_block_, std::move(al));
        return v;
    };
    auto load_i64 = [&](ir::IrValueId addr) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v;
        ld.operands = {addr};
        ld.source_line = ln;
        emit(current_block_, std::move(ld));
        return v;
    };
    auto store_i64 = [&](ir::IrValueId addr, ir::IrValueId val) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    };
    auto load_byte_at = [&](ir::IrValueId base,
                            ir::IrValueId off) -> ir::IrValueId {
        ir::IrValueId a = ptr_add(base, off);
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::U8;
        ld.dst = v;
        ld.operands = {a};
        ld.source_line = ln;
        emit(current_block_, std::move(ld));
        return v;
    };
    auto store_u16 = [&](ir::IrValueId addr, ir::IrValueId val) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I16;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    };
    auto bin = [&](ir::IrOp op, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr in{};
        in.op = op;
        in.type = ir::IrType::I64;
        in.dst = v;
        in.operands = {a, b};
        in.source_line = ln;
        emit(current_block_, std::move(in));
        return v;
    };
    auto cst = [&](uint64_t k) -> ir::IrValueId {
        return emit_const(ir::IrType::I64, k, ln);
    };
    auto br = [&](ir::IrBlockId target) {
        ir::IrInstr b{};
        b.op = ir::IrOp::BR;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.target_block = target;
        b.source_line = ln;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(target);
        fn_->blocks[target].preds.push_back(current_block_);
    };
    auto br_cond = [&](ir::IrValueId cond, ir::IrBlockId t_true,
                       ir::IrBlockId t_false) {
        ir::IrInstr b{};
        b.op = ir::IrOp::BR_COND;
        b.type = ir::IrType::VOID;
        b.dst = ir::IR_NO_VALUE;
        b.operands = {cond};
        b.target_block = t_true;
        b.false_block = t_false;
        b.source_line = ln;
        emit(current_block_, std::move(b));
        fn_->blocks[current_block_].succs.push_back(t_true);
        fn_->blocks[current_block_].succs.push_back(t_false);
        fn_->blocks[t_true].preds.push_back(current_block_);
        fn_->blocks[t_false].preds.push_back(current_block_);
    };

    // out = malloc((byte_len + 1) * 2).
    ir::IrValueId v_units = bin(ir::IrOp::ADD, p_blen, cst(1));
    ir::IrValueId v_bytes = bin(ir::IrOp::SHL, v_units, cst(1)); // *2
    ir::IrValueId v_out = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_out].is_host_ptr = true;
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::RAW_ALLOC;
        al.type = ir::IrType::PTR;
        al.dst = v_out;
        al.operands = {v_bytes};
        al.source_line = ln;
        emit(current_block_, std::move(al));
    }

    // i = 0 ; ob = 0 ; cp slot.
    ir::IrValueId s_i = new_slot();
    ir::IrValueId s_ob = new_slot();
    ir::IrValueId s_cp = new_slot();
    store_i64(s_i, cst(0));
    store_i64(s_ob, cst(0));

    // header: while (i < byte_len).
    ir::IrBlockId bb_hdr = fn_->new_block();
    br(bb_hdr);
    current_block_ = bb_hdr;
    ir::IrValueId v_i = load_i64(s_i);
    ir::IrValueId i_lt = bin(ir::IrOp::CMP_LT, v_i, p_blen);
    ir::IrBlockId bb_dec = fn_->new_block();
    ir::IrBlockId bb_end = fn_->new_block();
    br_cond(i_lt, bb_dec, bb_end);

    // bb_dec: b0 = p[i] ; 4-way segun rango.
    current_block_ = bb_dec;
    ir::IrValueId v_i0 = load_i64(s_i);
    ir::IrValueId v_b0 = load_byte_at(p_p, v_i0);
    ir::IrBlockId bb_emit = fn_->new_block(); // tras decodificar cp + avanzar i
    auto cont = [&](uint64_t k) -> ir::IrValueId {
        // (p[i + k] & 0x3F)
        ir::IrValueId off = bin(ir::IrOp::ADD, load_i64(s_i), cst(k));
        return bin(ir::IrOp::AND, load_byte_at(p_p, off), cst(0x3F));
    };
    // if b0 < 0x80
    {
        ir::IrValueId lt80 = bin(ir::IrOp::CMP_LT, v_b0, cst(0x80));
        ir::IrBlockId bb_1 = fn_->new_block();
        ir::IrBlockId bb_n1 = fn_->new_block();
        br_cond(lt80, bb_1, bb_n1);
        // 1 byte.
        current_block_ = bb_1;
        store_i64(s_cp, v_b0);
        store_i64(s_i, bin(ir::IrOp::ADD, load_i64(s_i), cst(1)));
        br(bb_emit);
        // else.
        current_block_ = bb_n1;
        ir::IrValueId ltE0 = bin(ir::IrOp::CMP_LT, v_b0, cst(0xE0));
        ir::IrBlockId bb_2 = fn_->new_block();
        ir::IrBlockId bb_n2 = fn_->new_block();
        br_cond(ltE0, bb_2, bb_n2);
        // 2 bytes: cp = ((b0&0x1F)<<6) | c(1).
        current_block_ = bb_2;
        {
            ir::IrValueId hi =
                bin(ir::IrOp::SHL, bin(ir::IrOp::AND, v_b0, cst(0x1F)), cst(6));
            store_i64(s_cp, bin(ir::IrOp::OR, hi, cont(1)));
            store_i64(s_i, bin(ir::IrOp::ADD, load_i64(s_i), cst(2)));
        }
        br(bb_emit);
        // else.
        current_block_ = bb_n2;
        ir::IrValueId ltF0 = bin(ir::IrOp::CMP_LT, v_b0, cst(0xF0));
        ir::IrBlockId bb_3 = fn_->new_block();
        ir::IrBlockId bb_4 = fn_->new_block();
        br_cond(ltF0, bb_3, bb_4);
        // 3 bytes: cp = ((b0&0x0F)<<12) | (c(1)<<6) | c(2).
        current_block_ = bb_3;
        {
            ir::IrValueId hi = bin(
                ir::IrOp::SHL, bin(ir::IrOp::AND, v_b0, cst(0x0F)), cst(12));
            ir::IrValueId mid = bin(ir::IrOp::SHL, cont(1), cst(6));
            store_i64(s_cp,
                      bin(ir::IrOp::OR, bin(ir::IrOp::OR, hi, mid), cont(2)));
            store_i64(s_i, bin(ir::IrOp::ADD, load_i64(s_i), cst(3)));
        }
        br(bb_emit);
        // 4 bytes: cp = ((b0&0x07)<<18)|(c(1)<<12)|(c(2)<<6)|c(3).
        current_block_ = bb_4;
        {
            ir::IrValueId hi = bin(
                ir::IrOp::SHL, bin(ir::IrOp::AND, v_b0, cst(0x07)), cst(18));
            ir::IrValueId m1 = bin(ir::IrOp::SHL, cont(1), cst(12));
            ir::IrValueId m2 = bin(ir::IrOp::SHL, cont(2), cst(6));
            ir::IrValueId acc = bin(ir::IrOp::OR, bin(ir::IrOp::OR, hi, m1),
                                    bin(ir::IrOp::OR, m2, cont(3)));
            store_i64(s_cp, acc);
            store_i64(s_i, bin(ir::IrOp::ADD, load_i64(s_i), cst(4)));
        }
        br(bb_emit);
    }

    // bb_emit: codificar cp a UTF-16 (BMP o par suplente) + ob += unidades.
    current_block_ = bb_emit;
    ir::IrValueId v_cp = load_i64(s_cp);
    ir::IrValueId is_bmp = bin(ir::IrOp::CMP_LT, v_cp, cst(0x10000));
    ir::IrBlockId bb_bmp = fn_->new_block();
    ir::IrBlockId bb_ast = fn_->new_block();
    br_cond(is_bmp, bb_bmp, bb_ast);
    // BMP: out[ob] = cp ; ob += 2.
    current_block_ = bb_bmp;
    {
        ir::IrValueId v_ob = load_i64(s_ob);
        store_u16(ptr_add(v_out, v_ob), v_cp);
        store_i64(s_ob, bin(ir::IrOp::ADD, v_ob, cst(2)));
    }
    br(bb_hdr);
    // Astral: cp2 = cp - 0x10000 ; hi/lo surrogates.
    current_block_ = bb_ast;
    {
        ir::IrValueId cp2 = bin(ir::IrOp::SUB, v_cp, cst(0x10000));
        ir::IrValueId hi =
            bin(ir::IrOp::OR, cst(0xD800), bin(ir::IrOp::SHR, cp2, cst(10)));
        ir::IrValueId lo =
            bin(ir::IrOp::OR, cst(0xDC00), bin(ir::IrOp::AND, cp2, cst(0x3FF)));
        ir::IrValueId v_ob = load_i64(s_ob);
        store_u16(ptr_add(v_out, v_ob), hi);
        ir::IrValueId v_ob2 = bin(ir::IrOp::ADD, v_ob, cst(2));
        store_u16(ptr_add(v_out, v_ob2), lo);
        store_i64(s_ob, bin(ir::IrOp::ADD, v_ob, cst(4)));
    }
    br(bb_hdr);

    // bb_end: out[ob] = 0 (NUL u16) ; ret out.
    current_block_ = bb_end;
    {
        ir::IrValueId v_ob = load_i64(s_ob);
        store_u16(ptr_add(v_out, v_ob), cst(0));
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::PTR;
        rt.dst = ir::IR_NO_VALUE;
        rt.operands = {v_out};
        rt.source_line = ln;
        emit(current_block_, std::move(rt));
    }
    block_terminated_ = true;

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));
    return name;
}

ir::IrValueId Lowering::emit_native_str_to_utf16(ir::IrValueId v_ptr,
                                                 ir::IrValueId v_blen,
                                                 uint32_t source_line) {
    const std::string name = ensure_str_to_utf16_helper();
    ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true;
    ir::IrInstr ca{};
    ca.op = ir::IrOp::CALL;
    ca.type = ir::IrType::PTR;
    ca.dst = v;
    ca.func_name = name;
    ca.operands = {v_ptr, v_blen};
    ca.source_line = source_line;
    emit(current_block_, std::move(ca));
    return v;
}

ir::IrValueId Lowering::emit_strcmp_dispatched(ir::IrValueId pa,
                                               ir::IrValueId la,
                                               ir::IrValueId pb,
                                               ir::IrValueId lb,
                                               uint32_t source_line) {
    // CPU dispatch Inc 5a: strcmp(pa, la, pb, lb) -> i64 (-1/0/1) DESPACHADO
    // por tabla de punteros: `call [__vx_strcmp_fp]`.  El fp apunta al
    // baseline (__vx_strcmp_base) o al @HelperOverride(strcmp) del usuario.
    // Solo se llama desde el path native_poo_ de lower_binary.  Idempotente +
    // marca cpu_dispatch_used_ para wirear el init en main.
    ensure_strdisp();
    const uint64_t fp_slot = strcmp_fp_slot_;
    // v_fpaddr = &__vx_strcmp_fp ; v_fp = LOAD i64 [v_fpaddr].
    ir::IrValueId v_fpaddr = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_fpaddr].is_host_ptr = true;
    {
        ir::IrInstr la_i{};
        la_i.op = ir::IrOp::STR_LIT_ADDR;
        la_i.type = ir::IrType::PTR;
        la_i.dst = v_fpaddr;
        la_i.imm = fp_slot;
        la_i.source_line = source_line;
        emit(current_block_, std::move(la_i));
    }
    ir::IrValueId v_fp = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_fp].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_fp;
        ld.operands = {v_fpaddr};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
    }
    // CALLIND v_fp(pa, la, pb, lb) -> i64.
    ir::IrValueId v = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ci{};
    ci.op = ir::IrOp::CALLIND;
    ci.type = ir::IrType::I64;
    ci.dst = v;
    ci.func_ptr = v_fp;
    ci.operands = {pa, la, pb, lb};
    ci.source_line = source_line;
    emit(current_block_, std::move(ci));
    return v;
}

std::string Lowering::ensure_strdata_helper() {
    // u8* __vx_strdata(u8* s): data_ptr branchless (is_heap ? ptr@0 : &s).
    // Funcion APARTE (no inline) -> una sola CALL por uso; el blacklist del
    // inliner (prefijo __vx_str) impide re-inlinearla.
    const std::string name = "__vx_strdata";
    if (strdata_helper_emitted_) return name;
    strdata_helper_emitted_ = true;

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::PTR;
    const ir::IrValueId p_s = hf.new_value(ir::IrType::PTR, "%s");
    hf.values[p_s].is_param = true;
    hf.values[p_s].is_host_ptr = true;
    hf.params.push_back(p_s);
    const ir::IrBlockId e = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;

    ir::IrValueId v_data = emit_native_str_data_ptr_inline(p_s, 0);
    {
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::PTR;
        rt.operands.push_back(v_data);
        rt.source_line = 0;
        emit(current_block_, std::move(rt));
    }

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));
    return name;
}

std::string Lowering::ensure_strlen_helper() {
    // i64 __vx_strlen_base(u8* s): len branchless (is_heap ? len@8 :
    // byte[23]&0x7F).
    //
    // CPU dispatch Inc 5a: BASELINE escalar al que apunta __vx_strlen_fp por
    // defecto.  Llamable por nombre desde Vesta (un override puede delegar a
    // el).
    const std::string name = "__vx_strlen_base";
    if (strlen_helper_emitted_) return name;
    strlen_helper_emitted_ = true;

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::I64;
    const ir::IrValueId p_s = hf.new_value(ir::IrType::PTR, "%s");
    hf.values[p_s].is_param = true;
    hf.values[p_s].is_host_ptr = true;
    hf.params.push_back(p_s);
    const ir::IrBlockId e = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;

    ir::IrValueId v_len = emit_native_str_len_inline(p_s, 0);
    {
        ir::IrInstr rt{};
        rt.op = ir::IrOp::RET;
        rt.type = ir::IrType::I64;
        rt.operands.push_back(v_len);
        rt.source_line = 0;
        emit(current_block_, std::move(rt));
    }

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));
    return name;
}

void Lowering::emit_native_str_free_if_heap(ir::IrValueId v_slot,
                                            uint32_t source_line) {
    // free(is_heap ? ptr@0 : 0).  Branchless via AND-mask:
    //   mask = -is_heap   (SSO: 0 ; HEAP: ~0)
    //   to_free = ptr0 & mask  (SSO: ptr0 & 0 = 0 ; HEAP: ptr0).
    // Usamos AND (no MUL) porque valgrind sabe que `x & 0 = 0` es definido
    // aunque x tenga bits no inicializados (en SSO ptr0 = data inline);
    // MUL propagaba la indefinicion a free() -> "Conditional jump depends
    // on uninitialised value".  free(0) es no-op -> seguro para SSO y
    // move-out.
    // Se pregunta por PROPIO, no por "tiene puntero": una vista sobre
    // .rodata tambien tiene puntero, y liberarlo seria pasarle al asignador
    // una direccion que nunca le pidio.
    ir::IrValueId v_is_heap = emit_native_str_is_owned(v_slot, source_line);
    ir::IrValueId v_ptr0 = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_ptr0;
        ld.operands = {v_slot};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
    }
    // mask = -is_heap  (0 - is_heap): 0 -> 0 ; 1 -> ~0.
    ir::IrValueId v_mask = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ng{};
        ng.op = ir::IrOp::NEG;
        ng.type = ir::IrType::I64;
        ng.dst = v_mask;
        ng.operands = {v_is_heap};
        ng.source_line = source_line;
        emit(current_block_, std::move(ng));
    }
    ir::IrValueId v_to_free_i = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr an{};
        an.op = ir::IrOp::AND;
        an.type = ir::IrType::I64;
        an.dst = v_to_free_i;
        an.operands = {v_ptr0, v_mask};
        an.source_line = source_line;
        emit(current_block_, std::move(an));
    }
    ir::IrValueId v_to_free = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_to_free].is_host_ptr = true;
    {
        ir::IrInstr bc{};
        bc.op = ir::IrOp::BITCAST;
        bc.type = ir::IrType::PTR;
        bc.dst = v_to_free;
        bc.operands = {v_to_free_i};
        bc.source_line = source_line;
        emit(current_block_, std::move(bc));
    }
    ir::IrInstr rf{};
    rf.op = ir::IrOp::RAW_FREE;
    rf.type = ir::IrType::VOID;
    rf.dst = ir::IR_NO_VALUE;
    rf.operands = {v_to_free};
    rf.source_line = source_line;
    emit(current_block_, std::move(rf));
}

void Lowering::emit_zero_native_str_slot(ir::IrValueId v_slot,
                                         uint32_t source_line) {
    // Zerar los bytes 0..22 del slot DEJANDO byte[23] para el caller.
    // Cubre todo lo que el move (copia de los 3 qwords) y los accesores
    // flag-aware podrian leer no inicializado en modo SSO:
    //   - qword0 (ptr@0 / data[0..7])  via STORE i64=0.
    //   - qword1 (len@8 / data[8..15]) via STORE i64=0.
    //   - bytes 16..22 (cap-low en HEAP / padding SSO) via I32+I16+U8.
    // byte[23] (flag/len SSO) NO se toca aqui: lo escribe el caller con un
    // STORE U8.  CRITICO: nunca usamos un STORE i64 a offset 16 porque el
    // patch U8 posterior a byte[23] crearia un solape parcial que el
    // store-forwarding del optimizer mal-resuelve (un LOAD i64 de offset
    // 16 forwarda el i64=0 e ignora el U8) -- rompia el MOVE de SSO.  Con
    // I32+I16+U8 (sin i64 que cubra byte[23]) el move lee bytes 16..22=0 +
    // byte[23]=len -> definido y correcto.
    auto ptr_add = [&](uint64_t off) -> ir::IrValueId {
        if (off == 0) return v_slot;
        ir::IrValueId v_off = emit_const(ir::IrType::I64, off, source_line);
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = fn_->values[v_slot].is_host_ptr;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v;
        ad.operands = {v_slot, v_off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v;
    };
    auto store0 = [&](uint64_t off, ir::IrType ty) {
        ir::IrValueId v_z = emit_const(ty, 0, source_line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ty;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_z, ptr_add(off)};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };
    store0(0, ir::IrType::I64);  // bytes 0..7   (ptr@0 / data)
    store0(8, ir::IrType::I64);  // bytes 8..15  (len@8 / data)
    store0(16, ir::IrType::I32); // bytes 16..19 (cap-lo HEAP / data SSO)
    store0(20, ir::IrType::I16); // bytes 20..21
    store0(22, ir::IrType::U8);  // byte  22  (byte[23] lo pone el caller)
    // byte[23] (flag/len) lo escribe el caller con un STORE U8.  El move
    // usa MEMCPY (emit_native_str_move_copy), no i64 LOADs, asi que el
    // solape parcial de los stores no afecta la copia (lee la memoria).
}

void Lowering::emit_str_meta_sso(ir::IrValueId v_slot, ir::IrValueId v_len,
                                 uint32_t source_line) {
    // SSO: byte[23] = len (flag bit alto 0).  STORE U8 -- NO toca bytes
    // 16..22 (que pueden contener DATA inline para len > 16).  El
    // store-forwarding del move se resuelve via MEMCPY (no i64 LOADs),
    // ver emit_native_str_move_copy.
    ir::IrValueId v_addr23 = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_addr23].is_host_ptr = fn_->values[v_slot].is_host_ptr;
    {
        ir::IrValueId v_off = emit_const(ir::IrType::I64, 23, source_line);
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v_addr23;
        ad.operands = {v_slot, v_off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
    }
    ir::IrInstr st{};
    st.op = ir::IrOp::STORE;
    st.type = ir::IrType::U8;
    st.dst = ir::IR_NO_VALUE;
    st.operands = {v_len, v_addr23};
    st.source_line = source_line;
    emit(current_block_, std::move(st));
}

void Lowering::emit_str_meta_heap(ir::IrValueId v_slot, ir::IrValueId v_cap,
                                  uint32_t source_line) {
    // HEAP: cap en bytes 16..22, byte[23] = 0x80 (flag HEAP).  En HEAP
    // qword2 no contiene data -> escribimos cap como i64 a offset 16 (que
    // toca byte[23]=0 si cap < 2^56) y luego byte[23]=0x80 (U8).  El move
    // de un HEAP usa MEMCPY (no i64 LOADs) -> sin solape de forwarding.
    auto ptr_add = [&](uint64_t off) -> ir::IrValueId {
        ir::IrValueId v_off = emit_const(ir::IrType::I64, off, source_line);
        ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = fn_->values[v_slot].is_host_ptr;
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = v;
        ad.operands = {v_slot, v_off};
        ad.source_line = source_line;
        emit(current_block_, std::move(ad));
        return v;
    };
    auto store = [&](ir::IrValueId addr, ir::IrValueId val, ir::IrType ty) {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ty;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {val, addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    };
    store(ptr_add(16), v_cap, ir::IrType::I64);
    store(ptr_add(23), emit_const(ir::IrType::U8, 0x80, source_line),
          ir::IrType::U8);
}

void Lowering::emit_native_str_move_copy(ir::IrValueId v_dst_slot,
                                         ir::IrValueId v_src_slot,
                                         uint32_t source_line) {
    // Copia los 24 bytes del value-string @p v_src_slot a @p v_dst_slot
    // via MEMCPY (rep movsb), NO via 3 LOAD/STORE i64.  Asi evita el
    // store-forwarding del optimizer sobre qword2 (que mezcla data inline
    // + byte[23] via stores parciales) -- los i64 LOADs lo mal-resolvian
    // (perdian la longitud SSO).  MEMCPY lee la MEMORIA directamente.
    ir::IrValueId v_24 = emit_const(ir::IrType::I64, 24, source_line);
    ir::IrInstr mc{};
    mc.op = ir::IrOp::MEMCPY;
    mc.type = ir::IrType::I8;
    mc.dst = ir::IR_NO_VALUE;
    mc.operands = {v_dst_slot, v_src_slot, v_24};
    mc.source_line = source_line;
    emit(current_block_, std::move(mc));
}

void Lowering::emit_native_str_invalidate_moved(ir::IrValueId v_slot,
                                                uint32_t source_line) {
    // ptr@0 = old_ptr0 & (is_heap - 1).
    //   HEAP (is_heap=1): mask = 0     -> ptr@0 = 0  (free posterior no-op).
    //   SSO  (is_heap=0): mask = ~0    -> ptr@0 sin cambio (data inline).
    ir::IrValueId v_is_heap = emit_native_str_is_heap(v_slot, source_line);
    ir::IrValueId v_old_ptr0 = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_old_ptr0;
        ld.operands = {v_slot};
        ld.source_line = source_line;
        emit(current_block_, std::move(ld));
    }
    // mask = is_heap - 1.
    ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, source_line);
    ir::IrValueId v_mask = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr s{};
        s.op = ir::IrOp::SUB;
        s.type = ir::IrType::I64;
        s.dst = v_mask;
        s.operands = {v_is_heap, v_one};
        s.source_line = source_line;
        emit(current_block_, std::move(s));
    }
    // new_ptr0 = old_ptr0 & mask.
    ir::IrValueId v_new = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr an{};
        an.op = ir::IrOp::AND;
        an.type = ir::IrType::I64;
        an.dst = v_new;
        an.operands = {v_old_ptr0, v_mask};
        an.source_line = source_line;
        emit(current_block_, std::move(an));
    }
    ir::IrInstr st{};
    st.op = ir::IrOp::STORE;
    st.type = ir::IrType::I64;
    st.dst = ir::IR_NO_VALUE;
    st.operands = {v_new, v_slot};
    st.source_line = source_line;
    emit(current_block_, std::move(st));
}

ir::IrValueId Lowering::emit_gc_handle_for_ptr(ir::IrValueId v_host_ptr,
                                               uint32_t source_line) {
    // GC_HANDLE_FOR_PTR devuelve un GcHandle uint32 zero-extended a i64.
    // No es is_host_ptr (es un indice opaco), no es is_gc_object (no es
    // el host_ptr al payload).
    const ir::IrValueId v_h = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::GC_HANDLE_FOR_PTR;
    ins.type = ir::IrType::I64;
    ins.dst = v_h;
    ins.operands = {v_host_ptr};
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
    return v_h;
}

void Lowering::emit_monitor_op(ir::IrValueId v_obj_or_handle, bool enter,
                               uint32_t source_line) {
    // @SyncImpl: si el programa define monitor_enter/monitor_exit,
    // `synchronized` rutea a esas funciones en LOS 3 MODOS (interp/JIT/AOT).
    // Mecanismo, no politica: la impl del usuario decide el layout del lock. El
    // operando es el host_ptr al ObjectHeader (lower_synchronized ya pasa v_obj
    // sin convertir a GcHandle cuando hay override).
    const std::string &sync_ovr =
        enter ? sync_enter_override_ : sync_exit_override_;
    if (!sync_ovr.empty()) {
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALL;
        ins.func_name = sync_ovr;
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.operands = {v_obj_or_handle};
        ins.is_call_site = true;
        ins.source_line = source_line;
        emit(current_block_, std::move(ins));
        return;
    }
    if (native_poo_) {
        // AOT/bare: monitor reentrante inline en el objeto (palabra en obj+16),
        // sin GC ni handle table.  Baja a CALL a la primitiva nativa
        // (__vx_monenter/__vx_monexit) que el auto-bundle de vx_sync.vx
        // fusiona en el .o.  v_obj_or_handle es el host_ptr al ObjectHeader.
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALL;
        ins.func_name = enter ? "__vx_monenter" : "__vx_monexit";
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.operands = {v_obj_or_handle};
        ins.source_line = source_line;
        emit(current_block_, std::move(ins));
        return;
    }
    // Resto de tiers (Full/JIT/interp): IR op MONENTER/MONEXIT sobre el handle.
    ir::IrInstr ins{};
    ins.op = enter ? ir::IrOp::MONENTER : ir::IrOp::MONEXIT;
    ins.type = ir::IrType::VOID;
    ins.dst = ir::IR_NO_VALUE;
    ins.operands = {v_obj_or_handle};
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
}

void Lowering::emit_mvtake(ir::IrValueId v_dst_addr, ir::IrValueId v_src_addr,
                           uint32_t source_line) {
    ir::IrInstr ins{};
    ins.op = ir::IrOp::MVTAKE_IR;
    ins.type = ir::IrType::VOID;
    ins.dst = ir::IR_NO_VALUE;
    ins.operands = {v_dst_addr, v_src_addr};
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
}

void Lowering::emit_gc_set_finalizer(ir::IrValueId v_box, uint32_t kind,
                                     uint32_t source_line,
                                     ir::IrValueId v_dtor_addr) {
    // Registra (kind 1/2/3) el finalizador GC del box con recurso interno.
    module_has_gc_finalizers_ = true; // habilita el finalize_all al exit (AOT)
    if (native_poo_) {
        // AOT: CALL vx_gc_register_finalizer(payload, kind, aux) de
        // libvesta_gc.  El runner nativo ejecuta el deleter/dtor por CALL
        // directo cuando el sweep colecte el objeto (o el shutdown lo
        // finalice). aux = vaddr/func_ptr del <Clase>____dtor (kind==3), 0 para
        // UNIQUE/ SHARED (su deleter vive dentro del box).  El auto-link de
        // libvesta_gc.a se dispara al detectar el simbolo vx_gc_*.
        const ir::IrValueId v_kind = emit_const(
            ir::IrType::I64, static_cast<int64_t>(kind), source_line);
        const ir::IrValueId v_aux =
            (kind == 3 && v_dtor_addr != ir::IR_NO_VALUE)
                ? v_dtor_addr
                : emit_const(ir::IrType::I64, 0, source_line);
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALL;
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.func_name = "vx_gc_register_finalizer";
        ins.operands = {v_box, v_kind, v_aux};
        ins.is_call_site = true;
        ins.source_line = source_line;
        emit(current_block_, std::move(ins));
        return;
    }
    // interp/JIT: opcode gcfinal (1/2) o gcfinalc (3 = CLASS_DTOR).
    ir::IrInstr ins{};
    ins.op = ir::IrOp::GC_SET_FINALIZER;
    ins.type = ir::IrType::VOID;
    ins.dst = ir::IR_NO_VALUE;
    if (kind == 3 && v_dtor_addr != ir::IR_NO_VALUE)
        ins.operands = {v_box, v_dtor_addr};
    else
        ins.operands = {v_box};
    ins.imm = kind;
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
}

// ---- Sprint 2:  Z + reflexion + static + AOP ----

ir::IrValueId Lowering::emit_findmethod(ir::IrValueId v_params, uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true; // MethodInfo* host
    ir::IrInstr ins{};
    ins.op = ir::IrOp::FINDMETHOD;
    ins.type = ir::IrType::PTR;
    ins.dst = v;
    ins.operands = {v_params};
    ins.is_call_site = true;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_findfield(ir::IrValueId v_params, uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true;
    ir::IrInstr ins{};
    ins.op = ir::IrOp::FINDFIELD;
    ins.type = ir::IrType::PTR;
    ins.dst = v;
    ins.operands = {v_params};
    ins.is_call_site = true;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_gc_allocp(ir::IrValueId v_size, uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true;
    ir::IrInstr ins{};
    if (native_poo_) {
        // AOT: usar el GC nativo (libvesta_gc) -> CALL vx_gc_alloc_ptr(size),
        // igual que __new_<Class>_gc.  Asi shared<T> aloca su control block sin
        // la VM; el GC gestiona el lifetime (stackmaps).  El auto-link de
        // libvesta_gc.a se dispara al detectar vx_gc_*.
        ins.op = ir::IrOp::CALL;
        ins.func_name = "vx_gc_alloc_ptr";
    } else {
        ins.op = ir::IrOp::GC_ALLOCP;
    }
    ins.type = ir::IrType::PTR;
    ins.dst = v;
    ins.operands = {v_size};
    ins.is_call_site = true;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_gc_promote(ir::IrValueId v_src, uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true;
    ir::IrInstr ins{};
    ins.op = ir::IrOp::GC_PROMOTE;
    ins.type = ir::IrType::PTR;
    ins.dst = v;
    ins.operands = {v_src};
    ins.is_call_site = true;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_gc_demote(ir::IrValueId v_src, uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true;
    ir::IrInstr ins{};
    ins.op = ir::IrOp::GC_DEMOTE;
    ins.type = ir::IrType::PTR;
    ins.dst = v;
    ins.operands = {v_src};
    ins.is_call_site = true;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_atomic_ld_i64(ir::IrValueId v_addr, uint32_t line,
                                           ir::IrType wt) {
    const ir::IrValueId v = fn_->new_value(wt);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::ATOMIC_LD;
    ins.type = wt; // ancho del atomico (1/2/4/8 -> mode del ctrl-byte)
    ins.dst = v;
    ins.operands = {v_addr};
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

void Lowering::emit_atomic_st_i64(ir::IrValueId v_addr, ir::IrValueId v_val,
                                  uint32_t line, ir::IrType wt) {
    ir::IrInstr ins{};
    ins.op = ir::IrOp::ATOMIC_ST;
    // El resultado es VOID pero el ancho viaja en `type` (el ir_emitter lo lee
    // del tipo del valor almacenado; aqui lo fijamos directamente).
    ins.type = wt;
    ins.dst = ir::IR_NO_VALUE;
    ins.operands = {v_addr, v_val};
    ins.source_line = line;
    emit(current_block_, std::move(ins));
}

ir::IrValueId Lowering::emit_atomic_cas_i64(ir::IrValueId v_addr,
                                            ir::IrValueId v_exp,
                                            ir::IrValueId v_des, uint32_t line,
                                            ir::IrType wt) {
    const ir::IrValueId v = fn_->new_value(wt);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::ATOMIC_CAS;
    ins.type = wt;
    ins.dst = v;
    ins.operands = {v_addr, v_exp, v_des};
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_atomic_add_i64(ir::IrValueId v_addr,
                                            ir::IrValueId v_delta,
                                            uint32_t line, ir::IrType wt) {
    const ir::IrValueId v = fn_->new_value(wt);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::ATOMIC_ADD;
    ins.type = wt;
    ins.dst = v;
    ins.operands = {v_addr, v_delta};
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_getstatic(ir::IrValueId v_cls, uint64_t offset,
                                       uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::GETSTATIC;
    ins.type = ir::IrType::I64;
    ins.dst = v;
    ins.operands = {v_cls};
    ins.imm = offset;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

void Lowering::emit_setstatic(ir::IrValueId v_cls, ir::IrValueId v_val,
                              uint64_t offset, uint32_t line) {
    ir::IrInstr ins{};
    ins.op = ir::IrOp::SETSTATIC;
    ins.type = ir::IrType::VOID;
    ins.dst = ir::IR_NO_VALUE;
    ins.operands = {v_cls, v_val};
    ins.imm = offset;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
}

ir::IrValueId Lowering::emit_proceed(uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::I64);

    /* `proceed()` con destino conocido se emite como una llamada normal.
     *
     * La instruccion `proceed` no lleva operandos: reusa los registros vivos y
     * lee a donde ir del MARCO, que se lo prepara el despacho al recorrer la
     * cadena.  Eso obliga a entrar por el despacho aunque se sepa todo.
     *
     * Y se sabe todo: un advice tiene un solo objetivo -- el pointcut es
     * `Clase.metodo` exacto, sin comodines -- y una sola posicion en su cadena,
     * asi que su `proceed` tiene UN destino, que es el siguiente `@Around` o el
     * metodo.  Se calcula en @c run() y se llama directo, pasando lo mismo que
     * pasaria el despacho: los parametros de este advice, que son los de la
     * llamada original (el receptor incluido, en el primero).
     *
     * Ademas de ahorrarse la indireccion, esto es lo que permite despues TEJER
     * la cadena en el sitio de llamada: un `@Around` tejido se invoca directo,
     * sin marco, y entonces la instruccion `proceed` no tendria de donde leer.
     *
     * Se conserva la forma antigua para lo que no se pueda atribuir, que sigue
     * despachandose por la cadena en ejecucion. */
    auto it = fn_ ? proceed_target_.find(fn_->name) : proceed_target_.end();
    if (it != proceed_target_.end() && !it->second.empty()) {
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALL;
        ins.type = ir::IrType::I64;
        ins.dst = v;
        ins.func_name = it->second;
        ins.operands = fn_->params; // this + args, tal cual llegaron
        ins.is_call_site = true;
        ins.source_line = line;
        emit(current_block_, std::move(ins));
        return v;
    }

    ir::IrInstr ins{};
    ins.op = ir::IrOp::PROCEED;
    ins.type = ir::IrType::I64;
    ins.dst = v;
    ins.is_call_site = true;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

// ---- Sprint 3: label-addr + CLI args + async helper fusion ----

ir::IrValueId Lowering::emit_label_addr(const std::string &label_name,
                                        uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::LABEL_ADDR;
    ins.type = ir::IrType::PTR;
    ins.dst = v;
    ins.func_name = label_name; // se interpreta como @Absolute("code.<name>")
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_getpid(uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ins{};
    if (native_poo_) {
        // AOT: pid() -> CALL __vx_pid (vx_async.vx, devuelve
        // __vasync_current_pid).  Sin la VM; el runtime cooperativo lo provee.
        ins.op = ir::IrOp::CALL;
        ins.func_name = "__vx_pid";
        ins.is_call_site = true;
    } else {
        ins.op = ir::IrOp::GETPID;
    }
    ins.type = ir::IrType::I64;
    ins.dst = v;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_getargc(uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::GETARGC;
    ins.type = ir::IrType::I64;
    ins.dst = v;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_getarg(ir::IrValueId v_idx, uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true; // host_ptr al string del arg
    ir::IrInstr ins{};
    ins.op = ir::IrOp::GETARG;
    ins.type = ir::IrType::PTR;
    ins.dst = v;
    ins.operands = {v_idx};
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

void Lowering::emit_fulfill_hlt(ir::IrValueId v_fut, ir::IrValueId v_val,
                                uint32_t line) {
    ir::IrInstr ins{};
    ins.op = ir::IrOp::FULFILL_HLT;
    ins.type = ir::IrType::VOID;
    ins.dst = ir::IR_NO_VALUE;
    ins.operands = {v_fut, v_val};
    ins.source_line = line;
    emit(current_block_, std::move(ins));
}

// ---- Sprint 4: meta-OOP ----

ir::IrValueId Lowering::emit_findclass(ir::IrValueId v_params, uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true;
    ir::IrInstr ins{};
    ins.op = ir::IrOp::FINDCLASS;
    ins.type = ir::IrType::PTR;
    ins.dst = v;
    ins.operands = {v_params};
    ins.is_call_site = true;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

ir::IrValueId Lowering::emit_defclass(ir::IrValueId v_params, uint32_t line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = true;
    ir::IrInstr ins{};
    ins.op = ir::IrOp::DEFCLASS;
    ins.type = ir::IrType::PTR;
    ins.dst = v;
    ins.operands = {v_params};
    ins.is_call_site = true;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
    return v;
}

void Lowering::emit_deffield(ir::IrValueId v_cls, ir::IrValueId v_params,
                             uint32_t line) {
    ir::IrInstr ins{};
    ins.op = ir::IrOp::DEFFIELD;
    ins.type = ir::IrType::VOID;
    ins.dst = ir::IR_NO_VALUE;
    ins.operands = {v_cls, v_params};
    ins.source_line = line;
    emit(current_block_, std::move(ins));
}

void Lowering::emit_defmethod(ir::IrValueId v_cls, ir::IrValueId v_params,
                              uint32_t line) {
    ir::IrInstr ins{};
    ins.op = ir::IrOp::DEFMETHOD;
    ins.type = ir::IrType::VOID;
    ins.dst = ir::IR_NO_VALUE;
    ins.operands = {v_cls, v_params};
    ins.source_line = line;
    emit(current_block_, std::move(ins));
}

void Lowering::emit_addadvice(ir::IrValueId v_target, ir::IrValueId v_advice,
                              uint64_t kind, uint32_t line) {
    ir::IrInstr ins{};
    ins.op = ir::IrOp::ADDADVICE;
    ins.type = ir::IrType::VOID;
    ins.dst = ir::IR_NO_VALUE;
    ins.operands = {v_target, v_advice};
    ins.imm = kind;
    ins.source_line = line;
    emit(current_block_, std::move(ins));
}

ir::IrValueId Lowering::emit_findclass_by_name(uint64_t name_idx,
                                               uint32_t name_len,
                                               uint32_t line) {
    // 1. ALLOCA 16 bytes para FindClassParams.
    ir::IrValueId v_params = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_params;
        al.imm = 16;
        al.source_line = line;
        emit(current_block_, std::move(al));
    }
    // 2. LABEL_ADDR @Absolute("code.s_<idx>") -> name_addr.
    ir::IrValueId v_name_addr =
        emit_label_addr("s_" + std::to_string(name_idx), line);
    // 3. STORE name_addr at [v_params + 0].
    {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {v_name_addr, v_params};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
    // 4. STORE name_len at [v_params + 8].
    ir::IrValueId v_name_len =
        emit_const(ir::IrType::I64, static_cast<uint64_t>(name_len), line);
    ir::IrValueId v_off8 = emit_const(ir::IrType::I64, 8, line);
    ir::IrValueId v_params8 = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr add{};
        add.op = ir::IrOp::ADD;
        add.type = ir::IrType::I64;
        add.dst = v_params8;
        add.operands = {v_params, v_off8};
        add.source_line = line;
        emit(current_block_, std::move(add));
    }
    {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {v_name_len, v_params8};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
    // 5. FINDCLASS -> ClassInfo*.
    return emit_findclass(v_params, line);
}

ir::IrValueId Lowering::emit_getproc(uint32_t source_line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    ir::IrInstr ip{};
    ip.op = ir::IrOp::GETPROC;
    ip.type = ir::IrType::PTR;
    ip.dst = v;
    ip.source_line = source_line;
    emit(current_block_, std::move(ip));
    return v;
}

/**
 * @brief  MC.17.2 -- obtiene (o aloca) el slot de @c static_data
 * para un comptime global.
 *
 * Lookup en @c comptime_global_slots_; si no esta, lee el valor
 * inicial desde @c tc_.comptime_const_values_, emite un slot de 8
 * bytes con esos bits y registra el mapping name -> idx.
 *
 * Solo soporta valores int (i64/u64/bool) en v1.  Strings/structs
 * requeririan inicializacion en @c __module_init via STRMAKE/etc.,
 * lo que es un sprint adicional.
 *
 * @return Indice valido (`s_<idx>` referenciable via STR_LIT_ADDR),
 *         o @c UINT64_MAX si el global no es soportado en v1.
 */
// L2.2: slot para global runtime no-const.  Zero-init en static_data;
// @c __module_init emite las instrucciones que copian el init real (e.g.
// un STRMAKE para strings, una constante para ints) al slot.
//
// CRITICO: usar push_back directo (no intern_static_data) para evitar
// dedup -- cada global necesita SU PROPIO slot aunque comparta bytes
// iniciales (typicamente todos los globals empiezan en {0,0,...,0} y
// si los dedupeamos colisionan en el mismo storage).
uint64_t Lowering::get_or_create_runtime_global_slot(const std::string &name,
                                                     uint64_t nbytes) {
    auto it = runtime_global_slots_.find(name);
    if (it != runtime_global_slots_.end()) return it->second;
    if (nbytes < 8) nbytes = 8; // minimo un qword (alineacion)
    std::vector<uint8_t> bytes(static_cast<size_t>(nbytes), 0); // zero-init
    const uint64_t idx = static_cast<uint64_t>(
        out_mod_->static_data.push_back(std::move(bytes)));
    auto &gmeta = out_mod_->static_data.meta_at(idx);
    // Marcar el slot como NON_DEDUP para que el merge cross-module
    // no colapse multiples globals con bytes iniciales identicos.
    gmeta.flags |= ir::IrModule::SD_FLAG_NON_DEDUP;
    // Un global es UNO en todo el programa, identificado por su nombre (ya
    // mangled por modulo/namespace, `lib__counter`).  El merge cross-module
    // unifica por `shared_key`, asi que el modulo que DEFINE el global y el
    // que solo lo USA (que crea su slot al vuelo, sin ver el AST del dep)
    // acaban compartiendo el mismo storage en vez de tener uno cada uno.
    gmeta.shared_key = name;
    // AOT: un global runtime es MUTABLE -> debe vivir en .data (rw), no en
    // .rodata (r): escribir a un slot read-only segfaultea en nativo.  El
    // interp/JIT ignoran section_name; solo lo consume el codegen AOT.
    gmeta.section_name = ".data";
    runtime_global_slots_[name] = idx;
    return idx;
}

uint64_t Lowering::shared_global_slot_for(const std::string &mangled_label,
                                          const Type &t) {
    // Tamano real del tipo: el slot del consumidor y el del dep se unifican por
    // shared_key y gana el PRIMERO que aparezca en el merge, asi que los dos
    // tienen que pedir el mismo tamano (con un global array, uno de 8 bytes
    // recortaria el storage).  Minimo un qword, como todo global.
    uint64_t nbytes = static_cast<uint64_t>(size_of_type(t));
    if (nbytes < 8) nbytes = 8;
    return get_or_create_runtime_global_slot(mangled_label, nbytes);
}

bool Lowering::imported_global_slot_of(ast::FieldAccessExpr *e,
                                       uint64_t &out_slot) {
    // property_kind=4 marca `ns.X`; ns_index dice CUAL namespace (el
    // sentinel 0xFFFFFFFF = sin resolver).
    if (e == nullptr || e->property_kind != 4 || e->ns_index == 0xFFFFFFFFu)
        return false;
    const auto &nss = tc_.imported_namespaces();
    if (e->ns_index >= nss.size()) return false;
    const auto &ns = nss[e->ns_index];
    auto it_sym = ns.by_name.find(e->field_name);
    if (it_sym == ns.by_name.end()) return false;
    const auto &sym = ns.symbols[it_sym->second];
    if (sym.kind != 1) return false;       // no es variable/constante
    if (sym.has_const_value) return false; // se inlinea como CONST
    if (sym.mangled_label.empty()) return false;
    // Simbolo de un namespace de ESTE modulo: es local, no importado.  Su
    // storage (o su ausencia, p.ej. un global de tipo funcion) ya se decidio
    // con el tipo delante; aqui solo pasan los de otros modulos.
    if (local_global_names_.count(sym.mangled_label) != 0) return false;
    // Un comptime const string se materializa via STRMAKE desde el blob del
    // .vxi, no tiene storage que compartir.
    if (sym.var_type.kind == PrimitiveKind::STRING) {
        const auto &ics = tc_.imported_global_consts();
        auto it_ic = ics.find(e->field_name);
        if (it_ic != ics.end() && it_ic->second.is_str) return false;
    }
    out_slot = shared_global_slot_for(sym.mangled_label, sym.var_type);
    return true;
}

bool Lowering::ensure_imported_global_slot(const std::string &name) {
    if (runtime_global_slots_.count(name) != 0) return true;
    const auto &igs = tc_.imported_global_storage();
    auto it = igs.find(name);
    if (it == igs.end()) return false;
    const uint64_t slot =
        shared_global_slot_for(it->second.mangled_label, it->second.type);
    // Alias: en ESTE modulo el global se nombra `name` (el nombre local del
    // import), pero su storage es el slot del dep.
    runtime_global_slots_[name] = slot;
    return true;
}

uint64_t Lowering::get_or_create_tls_global_slot(const std::string &name,
                                                 uint64_t nbytes,
                                                 uint64_t init_value,
                                                 uint16_t alignment) {
    auto it = runtime_global_slots_.find(name);
    if (it != runtime_global_slots_.end()) return it->second;
    // Cada slot ocupa >=8 bytes (alineado a 8), igual que un global runtime:
    // los STORE a un global usan ancho de qword, asi que un slot de 4 bytes
    // empacado junto al siguiente seria pisado por el store de 8 bytes del
    // vecino.  El valor vive en los bytes bajos; el LOAD usa el ancho del tipo.
    if (nbytes < 8) nbytes = 8;
    nbytes = (nbytes + 7) & ~static_cast<uint64_t>(7); // multiplo de 8
    // Plantilla estatica con el valor inicial (LE en los primeros 8 bytes).
    std::vector<uint8_t> bytes(static_cast<size_t>(nbytes), 0);
    for (size_t i = 0; i < bytes.size() && i < 8; ++i)
        bytes[i] = static_cast<uint8_t>((init_value >> (i * 8)) & 0xFFu);
    const uint64_t idx = static_cast<uint64_t>(
        out_mod_->static_data.push_back(std::move(bytes)));
    auto &m = out_mod_->static_data.meta_at(idx);
    // NON_DEDUP (storage propio) + TLS (seccion SHF_TLS) + .tdata.
    m.flags |= ir::IrModule::SD_FLAG_NON_DEDUP | ir::IrModule::SD_FLAG_TLS;
    m.section_name = ".tdata";
    m.alignment = alignment < 8 ? 8 : alignment;
    // Registrar en runtime_global_slots_ para que lower_ident lo resuelva
    // como un global ordinario (STR_LIT_ADDR); el driver AOT deriva la
    // TLS-ness desde SD_FLAG_TLS y emite el acceso por thread pointer.
    runtime_global_slots_[name] = idx;
    return idx;
}

uint64_t Lowering::get_or_create_comptime_global_slot(const std::string &name) {
    auto it = comptime_global_slots_.find(name);
    if (it != comptime_global_slots_.end()) return it->second;
    const auto &cgv = tc_.comptime_const_values();
    auto cit = cgv.find(name);
    if (cit == cgv.end()) return UINT64_MAX;
    /* Solo int en v1 -- strings serializados requieren STRMAKE en
     * __module_init que no esta integrado todavia. */
    if (cit->second.is_str) return UINT64_MAX;
    /* Empaquetar el valor inicial como 8 bytes little-endian. */
    const uint64_t init_val = static_cast<uint64_t>(cit->second.value);
    std::vector<uint8_t> bytes(8);
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((init_val >> (i * 8)) & 0xFFu);
    }
    const uint64_t idx = out_mod_->intern_static_data(std::move(bytes));
    comptime_global_slots_[name] = idx;
    return idx;
}

ir::IrValueId Lowering::stringify_primitive_via_native(ir::IrValueId v_val,
                                                       const char *native_fn,
                                                       uint32_t source_line) {
    const int ln = static_cast<int>(source_line);
    /* 1. ALLOCA 32 bytes -- buffer en stack VM.  Suficiente para
     *    todos los tipos: i64=20+signo, hex=18, "false"=5, UTF-8 4 B. */
    ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_buf;
        al.imm = 32;
        al.source_line = ln;
        emit(current_block_, std::move(al));
    }
    /* 2. proc_ptr via getproc. */
    const ir::IrValueId v_proc = emit_getproc(ln);
    /* 3. CALLN al native: devuelve length escrita en buf. */
    /* Se dice lo que hace, porque aqui se sabe: la familia `*_to_vmbuf`
     * formatea `value` y deja los bytes en el buffer del SEGUNDO argumento.
     * Nada mas -- ni lee otra memoria, ni hace E/S pese al prefijo `vio_`, ni
     * puede lanzar, y dos llamadas iguales dan lo mismo.
     *
     * Sin decirlo, cada `${n}` de una interpolacion era una barrera total para
     * cuanto la rodeara (52 sitios solo en std.memory), que es lo unico honesto
     * ante una funcion nativa de la que no se sabe nada. */
    {
        ir::IrNativeEffects fx;
        fx.declarados = true;
        fx.escribe_apuntado = 1u << 1; // el buffer destino
        out_mod_->register_native_import(
            std::string("stdlib/native/io/vesta_io"), native_fn, fx);
    }
    ir::IrValueId v_len = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr cl{};
        cl.op = ir::IrOp::CALLN;
        cl.type = ir::IrType::I64;
        cl.dst = v_len;
        cl.func_name = std::string("stdlib/native/io/vesta_io:") + native_fn;
        cl.operands = {v_proc, v_buf, v_val};
        cl.source_line = ln;
        emit(current_block_, std::move(cl));
    }
    /* 4. STRMAKE desde buf vm_mem. */
    ir::IrValueId v_h = emit_strmake(v_buf, v_len, ln);
    return v_h;
}

ir::IrValueId Lowering::cast_if_needed(ir::IrValueId v, ir::IrType from,
                                       ir::IrType to, uint32_t source_line,
                                       bool is_explicit) {
    // Overload de compatibilidad: sin SourceLoc completo, el warning apunta al
    // inicio (columna 1) de la linea.  Los call sites de alto impacto pasan el
    // SourceLoc de la expresion para apuntar a su columna real.
    SourceLoc loc;
    loc.file = current_file_;
    loc.line = source_line;
    loc.column = 1;
    return cast_if_needed(v, from, to, loc, is_explicit);
}

ir::IrValueId Lowering::cast_if_needed(ir::IrValueId v, ir::IrType from,
                                       ir::IrType to, const SourceLoc &loc,
                                       bool is_explicit) {
    if (from == to || v == ir::IR_NO_VALUE) return v;
    // Un valor CONSTANTE compile-time que CABE en el tipo destino no pierde
    // informacion aunque el destino sea mas estrecho: `u8 x = 0x48` o
    // `this.mod = 3` (bitfield) o un valued-enum (`Enc.RET`, cuyo valor es una
    // constante) no deben avisar de narrowing.  Misma politica que C/C++: no
    // se avisa por `char c = 65`.  Suprime el warning tratando el cast como
    // explicito (la conversion de bits sigue emitiendose igual).
    if (!is_explicit && v < fn_->values.size() && fn_->values[v].is_const) {
        const uint64_t cv = fn_->values[v].const_val;
        bool fits = false;
        switch (to) {
        case ir::IrType::U8: fits = (cv <= 0xFFULL); break;
        case ir::IrType::U16: fits = (cv <= 0xFFFFULL); break;
        case ir::IrType::U32: fits = (cv <= 0xFFFFFFFFULL); break;
        case ir::IrType::BOOL: fits = (cv <= 1ULL); break;
        case ir::IrType::I8: {
            const int64_t s = (int64_t)cv;
            fits = (s >= -128 && s <= 255);
            break;
        }
        case ir::IrType::I16: {
            const int64_t s = (int64_t)cv;
            fits = (s >= -32768 && s <= 65535);
            break;
        }
        case ir::IrType::I32: {
            const int64_t s = (int64_t)cv;
            fits = (s >= -2147483648LL && s <= 4294967295LL);
            break;
        }
        default: break;
        }
        if (fits) is_explicit = true;
    }
    // Warning de seguridad para casts implicitos que pueden perder
    // informacion: narrowing entero, float -> int, int -> float (los
    // grandes pierden mantissa).  Solo se emite cuando el usuario NO
    // escribio el cast explicitamente: `i32 x = i64_val` avisa, pero
    // `i32 x = (i32) i64_val` no.  Misma politica que -Wconversion en
    // GCC/Clang.
    if (!is_explicit) {
        auto bytes_for = [](ir::IrType t) -> int {
            return static_cast<int>(ir::type_slot_bytes(t));
        };
        const bool from_is_float =
            (from == ir::IrType::F32 || from == ir::IrType::F64);
        const bool to_is_float =
            (to == ir::IrType::F32 || to == ir::IrType::F64);
        const bool from_is_int =
            (from == ir::IrType::I8 || from == ir::IrType::I16 ||
             from == ir::IrType::I32 || from == ir::IrType::I64 ||
             from == ir::IrType::U8 || from == ir::IrType::U16 ||
             from == ir::IrType::U32 || from == ir::IrType::U64 ||
             from == ir::IrType::BOOL);
        const bool to_is_int =
            (to == ir::IrType::I8 || to == ir::IrType::I16 ||
             to == ir::IrType::I32 || to == ir::IrType::I64 ||
             to == ir::IrType::U8 || to == ir::IrType::U16 ||
             to == ir::IrType::U32 || to == ir::IrType::U64 ||
             to == ir::IrType::BOOL);
        const int from_bytes = bytes_for(from);
        const int to_bytes = bytes_for(to);
        std::string warn_msg;
        if (from_is_float && to_is_int) {
            warn_msg = "conversion implicita float -> int trunca la parte "
                       "fraccionaria; "
                       "usa cast explicito si es intencional";
        } else if (from_is_int && to_is_float) {
            // Solo avisar para enteros grandes a f32 (perdida de mantissa).
            // i64/u64 -> f32: ~24 bits de mantissa, perdida garantizada para
            // magnitudes >2^24. i32/u32 -> f32: tambien puede perder.  i*->f64
            // es exacto hasta 2^53.
            if (to == ir::IrType::F32 && from_bytes >= 4) {
                warn_msg =
                    "conversion implicita int -> f32 puede perder precision; "
                    "usa cast explicito si es intencional";
            }
        } else if (from_is_int && to_is_int) {
            if (to_bytes < from_bytes) {
                warn_msg = "conversion implicita reduce el ancho del entero "
                           "(narrowing); usa cast explicito si es intencional";
            }
        } else if (from_is_float && to_is_float) {
            if (to == ir::IrType::F32 && from == ir::IrType::F64) {
                warn_msg = "conversion implicita f64 -> f32 reduce precision; "
                           "usa cast explicito si es intencional";
            }
        }
        if (!warn_msg.empty()) {
            diags_.warning(loc, warn_msg);
        }
    }

    // Elegir el opcode de conversion correcto segun categoria.
    ir::IrOp op = ir::IrOp::CAST;
    const bool from_float =
        (from == ir::IrType::F32 || from == ir::IrType::F64);
    const bool to_float = (to == ir::IrType::F32 || to == ir::IrType::F64);

    if (from_float && to_float) {
        op = (from == ir::IrType::F32 && to == ir::IrType::F64)
                 ? ir::IrOp::F32TOF64
                 : ir::IrOp::F64TOF32;
    } else if (from_float && !to_float) {
        // Heuristica: si el destino es signed -> FTOI; si unsigned -> FTOUI.
        const bool to_signed = (to == ir::IrType::I8 || to == ir::IrType::I16 ||
                                to == ir::IrType::I32 || to == ir::IrType::I64);
        op = to_signed ? ir::IrOp::FTOI : ir::IrOp::FTOUI;
    } else if (!from_float && to_float) {
        // Heuristica simetrica: si origen signed -> ITOF; si unsigned -> UITOF.
        const bool from_signed =
            (from == ir::IrType::I8 || from == ir::IrType::I16 ||
             from == ir::IrType::I32 || from == ir::IrType::I64);
        // Bug fix 2026-05-23: ITOF/UITOF baja a `fcvt rd_gp, f0` que
        // opera sobre el reg de 64 bits.  Si el operando es i8/i16/i32,
        // los bits altos no estan extendidos correctamente -> el float
        // resultante es incorrecto.  Para i32 -7 -> trunc deja
        // 0xFFFFFFF9 con bits altos = 0 -> ITOF lo lee como 4294967289
        // (no -7).  Fix: SEXT (signed) o ZEXT (unsigned) a i64 ANTES
        // del ITOF/UITOF.
        /* Esta tabla se escribia a mano y OMITIA F32, que caia en el default y
         * valia 8 en vez de 4.  No fallaba porque esta rama solo se recorre
         * cuando el origen NO es flotante -- estaba protegida por el contexto,
         * no por ser correcta.  El vocabulario unico ya no deja escribir eso. */
        auto bytes_of_local = [](ir::IrType t) -> int {
            return static_cast<int>(ir::type_slot_bytes(t));
        };
        if (bytes_of_local(from) < 8) {
            ir::IrValueId v_ext = fn_->new_value(ir::IrType::I64);
            ir::IrInstr ext{};
            ext.op = from_signed ? ir::IrOp::SEXT : ir::IrOp::ZEXT;
            ext.type = ir::IrType::I64;
            ext.dst = v_ext;
            ext.operands.push_back(v);
            ext.source_line = loc.line;
            emit(current_block_, std::move(ext));
            v = v_ext;
        }
        op = from_signed ? ir::IrOp::ITOF : ir::IrOp::UITOF;
    } else {
        // Entero -> entero: elegir TRUNC, ZEXT o SEXT segun el cambio
        // de ancho y la signedness de la fuente.  Sin esto, el
        // emitter recibia siempre CAST y emitia un mov plano que NO
        // truncaba ni extendia: `i32 x = i64_value` dejaba los 8
        // bytes originales en el registro (bug de truncacion).
        /* Misma historia que bytes_of_local: omitia F32 y sobrevivia porque la
         * rama excluye los flotantes.  Ahora lo contesta el vocabulario. */
        auto bytes_of = [](ir::IrType t) -> int {
            return static_cast<int>(ir::type_slot_bytes(t));
        };
        const int from_b = bytes_of(from);
        const int to_b = bytes_of(to);
        const bool from_signed =
            (from == ir::IrType::I8 || from == ir::IrType::I16 ||
             from == ir::IrType::I32 || from == ir::IrType::I64);
        if (to_b < from_b) {
            op = ir::IrOp::TRUNC;
        } else if (to_b > from_b) {
            op = from_signed ? ir::IrOp::SEXT : ir::IrOp::ZEXT;
        } else {
            // Mismo ancho: nada que extender ni truncar; un BITCAST
            // (mov plano) es lo correcto a nivel de bytecode.  Esto
            // cubre cambios de signedness sin reinterpretacion (e.g.
            // i32 -> u32) y casts entre PTR e i64.
            op = ir::IrOp::BITCAST;
        }
    }

    const ir::IrValueId dst = fn_->new_value(to);
    ir::IrInstr ins{};
    ins.op = op;
    ins.type = to;
    ins.dst = dst;
    ins.operands = {v};
    ins.source_line = loc.line;
    emit(current_block_, std::move(ins));
    return dst;
}

// ---------------------------------------------------------------------
// Scopes.
// ---------------------------------------------------------------------

void Lowering::push_scope() {
    scopes_.emplace_back();
}

void Lowering::pop_scope() {
    scopes_.pop_back();
}

void Lowering::bind(const std::string &name, ir::IrValueId v) {
    scopes_.back()[name] = v;
    // Debug-info (solo-LSP / dumps): si el valor SSA aun no tiene nombre de
    // fuente, etiquetarlo con el de la variable -> la pestana IR muestra "%t"
    // en vez de "%5", y el cache de modulo lo persiste para el inspector.  NO
    // sobreescribimos nombres ya puestos (params "%n", alias) para no
    // corromperlos.  Cosmetico: el optimizer/codegen indexan por ID, no por
    // nombre, y el @ir de produccion no serializa nombres -> cero efecto en
    // runtime/JIT.
    if (fn_ && v != ir::IR_NO_VALUE && v < fn_->values.size() &&
        !name.empty()) {
        std::string &vn = fn_->values[v].name;
        // new_value() rellena el nombre por defecto como "%<id>".  Solo
        // sobreescribimos ese auto-nombre (no params "%n" ni alias ya
        // nombrados) para no corromper nombres significativos.
        const bool is_default = (vn == "%" + std::to_string(fn_->values[v].id));
        if (is_default) vn = "%" + name;
    }
}

ir::IrValueId Lowering::lookup(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return ir::IR_NO_VALUE;
}

// Wrapper publico para que helpers estaticos (e.g.
// @c collect_spawn_captures_in_expr) puedan resolver un nombre en
// todos los scopes activos del lowering sin tener acceso directo a
// @c lookup (que es @c const private).  Delega a @c lookup.
ir::IrValueId Lowering::spawn_capture_resolve(const std::string &name) {
    return lookup(name);
}

void Lowering::update_scope(const std::string &name, ir::IrValueId v) {
    // Buscar de mas interno a global y actualizar in-place.  Esto evita
    // crear sombras accidentales (que pasaria si simplemente hicieramos
    // bind() sobre el scope actual en lugar del scope donde la variable
    // se declaro).
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            found->second = v;
            return;
        }
    }
    // Fallback: si no existe en ningun scope, registrar en el actual.
    // El type checker normalmente atrapa esto antes, pero defendemos
    // para no perder informacion en caso de un AST malformado.
    bind(name, v);
}

// ---------------------------------------------------------------------
// Address-taken locals: pre-pase + accesos via LOAD/STORE.
//
// Cuando el usuario escribe `&x`, x debe vivir en una direccion estable
// (memoria), no en un registro virtual SSA.  Hacemos un escaneo previo
// del cuerpo de cada funcion para detectar todas las variables cuya
// direccion se toma; lower_var_decl emite ALLOCA para esas, y los
// accesos lectura/escritura pasan por LOAD/STORE en lugar de scope SSA.
//
// El conjunto address_taken_locals_ es por-funcion (limpiado al inicio
// de lower_function via run() / lower_function).  Solo registramos los
// nombres; el control de scope inner-shadowing es resposabilidad del
// type checker en hitos posteriores (no usamos shadowing).
// ---------------------------------------------------------------------

void Lowering::scan_address_taken(ast::Stmt *s) {
    if (!s) return;
    // Recorrido recursivo de stmts y exprs.  Definimos lambdas locales
    // para mantener las dependencias contenidas.
    std::function<void(ast::Expr *)> visit_expr;
    std::function<void(ast::Stmt *)> visit_stmt;

    // Marca address-taken toda variable asignada en @p n (parte de un loop):
    // fuerza su ALLOCA en la declaracion (dominante), evitando el PHI
    // direccion-vs-valor de un loop anidado en una rama condicional.
    auto mark_loop_assigned = [&](const ast::Node *n) {
        if (!n) return;
        std::set<std::string> tmp;
        collect_assigned_vars(n, tmp);
        for (const auto &nm : tmp)
            address_taken_locals_.insert(nm);
    };

    // Profundidad de anidamiento en ramas CONDICIONALES (then/else de un if,
    // cuerpos de catch).  Solo promovemos las vars loop-carried a address-taken
    // cuando el loop esta DENTRO de una rama condicional: ahi su ALLOCA (creado
    // en el bloque del loop) no domina la rama hermana del if -> el merge
    // produce el PHI direccion-vs-valor (el bug).  Un loop a nivel de funcion
    // (cond_depth==0) crea su ALLOCA en un bloque que domina el exit -> no hay
    // rama hermana problematica; ademas promover ahi romperia al vectorizador
    // (que espera el contador/acumulador como PHI SSA).
    int cond_depth = 0;

    visit_expr = [&](ast::Expr *e) {
        if (!e) return;
        switch (e->kind) {
        case ast::NodeKind::UnaryExpr: {
            auto *u = static_cast<ast::UnaryExpr *>(e);
            if (u->op == ast::UnOp::AddrOf && u->operand &&
                u->operand->kind == ast::NodeKind::IdentExpr) {
                auto *id = static_cast<ast::IdentExpr *>(u->operand.get());
                address_taken_locals_.insert(id->name);
            }
            visit_expr(u->operand.get());
            return;
        }
        case ast::NodeKind::LambdaExpr: {
            // Captures mutables: las variables modificadas dentro
            // del cuerpo de la lambda deben ser address-taken en
            // el outer scope para que el modelo de captura-por-
            // referencia funcione.  El env block guarda el PUNTERO
            // al slot del owner; el helper de la lambda hace
            // LOAD/STORE indirectos sobre ese puntero, de modo
            // que las mutaciones se ven desde fuera del lambda.
            auto *lam = static_cast<ast::LambdaExpr *>(e);
            for (const auto &nm : lam->mutable_captures) {
                address_taken_locals_.insert(nm);
            }
            if (lam->body) visit_stmt(lam->body.get());
            return;
        }
        case ast::NodeKind::BinaryExpr: {
            auto *b = static_cast<ast::BinaryExpr *>(e);
            visit_expr(b->lhs.get());
            visit_expr(b->rhs.get());
            return;
        }
        case ast::NodeKind::AssignExpr: {
            auto *a = static_cast<ast::AssignExpr *>(e);
            visit_expr(a->target.get());
            visit_expr(a->value.get());
            return;
        }
        case ast::NodeKind::CallExpr: {
            auto *c = static_cast<ast::CallExpr *>(e);
            // Borrow checker: lend(x) / lend_mut(x) sobre una
            // variable local plain requiere tomar su direccion
            // (un borrow ES, en runtime, un host_ptr al slot
            // donde vive el local; cero overhead vs un T*).
            // Forzamos address-taken promotion para que el lowering
            // deje el local en stack via ALLOCA + LOAD/STORE en
            // lugar de en registro SSA puro.  Sin esto, lend(local)
            // devuelve un valor (no una direccion) y read_borrow/
            // write_borrow dereferencian basura.  EXCEPCION: si la
            // var ya es de tipo borrow<T>/borrow_mut<T> (es un
            // borrow_var, no un local plain), NO la promocionamos
            // (su SSA value ya es host_ptr; el lend lo bypassa).
            if (c->callee && c->callee->kind == ast::NodeKind::IdentExpr &&
                c->args.size() == 1 &&
                c->args[0]->kind == ast::NodeKind::IdentExpr) {
                auto *cid = static_cast<ast::IdentExpr *>(c->callee.get());
                if (cid->name == "lend" || cid->name == "lend_mut") {
                    auto *aid = static_cast<ast::IdentExpr *>(c->args[0].get());
                    const Type at = aid->result_type;
                    if (at.kind != PrimitiveKind::BORROW &&
                        at.kind != PrimitiveKind::BORROW_MUT &&
                        at.kind != PrimitiveKind::UNIQUE_PTR &&
                        at.kind != PrimitiveKind::SHARED_PTR) {
                        address_taken_locals_.insert(aid->name);
                    }
                }
            }
            visit_expr(c->callee.get());
            for (auto &arg : c->args)
                visit_expr(arg.get());
            return;
        }
        case ast::NodeKind::FieldAccessExpr: {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e);
            visit_expr(fa->base.get());
            return;
        }
        case ast::NodeKind::IndexExpr: {
            auto *ix = static_cast<ast::IndexExpr *>(e);
            visit_expr(ix->base.get());
            visit_expr(ix->index.get());
            return;
        }
        case ast::NodeKind::CastExpr: {
            // `(T)(&x)`: el cast ENVUELVE el `&x`.  Sin recursar en el
            // operando, el `&x` interno no se veia y `x` no se promocionaba a
            // address-taken
            // -> error "& sobre variable no promocionada".  Bug de deteccion.
            auto *ce = static_cast<ast::CastExpr *>(e);
            visit_expr(ce->operand.get());
            return;
        }
        case ast::NodeKind::TernaryExpr: {
            // `cond ? &a : &b` -- recursar en las 3 ramas por el mismo motivo.
            auto *te = static_cast<ast::TernaryExpr *>(e);
            visit_expr(te->cond.get());
            visit_expr(te->then_expr.get());
            visit_expr(te->else_expr.get());
            return;
        }
        default: return; // literales, IdentExpr puro, etc. no aportan
        }
    };

    visit_stmt = [&](ast::Stmt *st) {
        if (!st) return;
        switch (st->kind) {
        case ast::NodeKind::BlockStmt: {
            auto *b = static_cast<ast::BlockStmt *>(st);
            for (auto &child : b->body)
                visit_stmt(child.get());
            return;
        }
        case ast::NodeKind::VarDeclStmt: {
            auto *vd = static_cast<ast::VarDeclStmt *>(st);
            if (vd->init) visit_expr(vd->init.get());
            return;
        }
        case ast::NodeKind::ExprStmt: {
            auto *es = static_cast<ast::ExprStmt *>(st);
            visit_expr(es->expr.get());
            return;
        }
        case ast::NodeKind::IfStmt: {
            auto *si = static_cast<ast::IfStmt *>(st);
            visit_expr(si->cond.get());
            // then/else son ramas condicionales: un loop dentro de ellas es el
            // caso del bug (su ALLOCA no domina la rama hermana).
            ++cond_depth;
            visit_stmt(si->then_branch.get());
            visit_stmt(si->else_branch.get());
            --cond_depth;
            return;
        }
        case ast::NodeKind::WhileStmt: {
            auto *w = static_cast<ast::WhileStmt *>(st);
            // Toda variable ASIGNADA dentro de un loop es loop-carried: el
            // lowering le crea un ALLOCA para persistir su valor entre
            // iteraciones (linea ~6377).  Si el loop esta dentro de una rama
            // condicional, ese ALLOCA (creado en el bloque del loop) NO domina
            // la rama HERMANA -> el merge del `if` termina con un PHI que
            // mezcla la DIRECCION del alloca (rama del loop) con el VALOR
            // original (rama sin loop) -> un `load` posterior deref-ea un valor
            // como si fuera puntero (SIGSEGV / basura).  Marcarla address-taken
            // AQUI (pre-pase) fuerza el ALLOCA en su DECLARACION (que domina
            // todo) y todas las ramas la ven como memoria -> representacion
            // consistente. Cero coste: el optimizer re-promueve a SSA los
            // allocas que no escapan (mem2reg / promote_local_allocas).
            if (cond_depth > 0) {
                mark_loop_assigned(w->cond.get());
                mark_loop_assigned(w->body.get());
            }
            visit_expr(w->cond.get());
            visit_stmt(w->body.get());
            return;
        }
        case ast::NodeKind::DoWhileStmt: {
            auto *dw = static_cast<ast::DoWhileStmt *>(st);
            if (cond_depth > 0) {
                mark_loop_assigned(dw->body.get());
                mark_loop_assigned(dw->cond.get());
            }
            visit_stmt(dw->body.get());
            visit_expr(dw->cond.get());
            return;
        }
        case ast::NodeKind::ForStmt: {
            auto *f = static_cast<ast::ForStmt *>(st);
            // Ver la nota en WhileStmt: toda variable asignada dentro del loop
            // se marca address-taken para que su ALLOCA nazca en la declaracion
            // (que domina todo), evitando el PHI direccion-vs-valor cuando el
            // loop esta anidado en una rama condicional.
            if (cond_depth > 0) {
                mark_loop_assigned(f->cond.get());
                mark_loop_assigned(f->step.get());
                mark_loop_assigned(f->body.get());
            }
            visit_stmt(f->init.get());
            visit_expr(f->cond.get());
            visit_expr(f->step.get());
            visit_stmt(f->body.get());
            return;
        }
        case ast::NodeKind::TryStmt: {
            // Sin esta rama, las variables declaradas dentro de un
            // try/catch/finally no se promocionan a address-taken
            // aunque aparezca `&var` en el body (error: '&x' sobre
            // variable no promocionada).  Y el cascade de errores
            // "nombre no resuelto" surge porque el lowering del
            // var-decl falla al evaluar `&var` y deja el binding
            // sin registrar.
            auto *ts = static_cast<ast::TryStmt *>(st);
            // try/catch introducen ramas (el catch se alcanza por un edge
            // de excepcion): un loop dentro puede sufrir el mismo PHI mixto.
            ++cond_depth;
            visit_stmt(ts->body.get());
            for (auto &cc : ts->catches)
                visit_stmt(cc.body.get());
            if (ts->finally_body) visit_stmt(ts->finally_body.get());
            --cond_depth;
            return;
        }
        case ast::NodeKind::ReturnStmt: {
            auto *r = static_cast<ast::ReturnStmt *>(st);
            visit_expr(r->value.get());
            return;
        }
        case ast::NodeKind::SynchronizedStmt: {
            auto *sy = static_cast<ast::SynchronizedStmt *>(st);
            visit_expr(sy->target.get());
            visit_stmt(sy->body.get());
            return;
        }
        default: return;
        }
    };
    visit_stmt(s);
}

// ---------------------------------------------------------------------
// spawn_body_uses_coop: recorre el body de un `spawn { }` buscando el uso
// de primitivas de asincronia COOPERATIVA (mailbox / future / await).  Su
// presencia indica que el spawn es una TAREA cooperativa del scheduler de
// vx_async (single-thread, run-to-completion), NO un hilo real paralelo.
// Ver la doc de la declaracion en lowering.h.
// ---------------------------------------------------------------------
bool Lowering::spawn_body_uses_coop(ast::Stmt *s) {
    if (!s) return false;
    bool found = false;
    std::function<void(ast::Expr *)> visit_expr;
    std::function<void(ast::Stmt *)> visit_stmt;

    visit_expr = [&](ast::Expr *e) {
        if (!e || found) return;
        switch (e->kind) {
        case ast::NodeKind::UnaryExpr: {
            auto *u = static_cast<ast::UnaryExpr *>(e);
            // `await fut` -> primitiva cooperativa.
            if (u->op == ast::UnOp::Await) {
                found = true;
                return;
            }
            visit_expr(u->operand.get());
            return;
        }
        case ast::NodeKind::CallExpr: {
            auto *c = static_cast<ast::CallExpr *>(e);
            if (c->callee && c->callee->kind == ast::NodeKind::IdentExpr) {
                const std::string &nm =
                    static_cast<ast::IdentExpr *>(c->callee.get())->name;
                if (nm == "msgrecv" || nm == "msgsend" || nm == "fulfill" ||
                    nm == "future_alloc") {
                    found = true;
                    return;
                }
            }
            visit_expr(c->callee.get());
            for (auto &arg : c->args)
                visit_expr(arg.get());
            return;
        }
        case ast::NodeKind::BinaryExpr: {
            auto *b = static_cast<ast::BinaryExpr *>(e);
            visit_expr(b->lhs.get());
            visit_expr(b->rhs.get());
            return;
        }
        case ast::NodeKind::AssignExpr: {
            auto *a = static_cast<ast::AssignExpr *>(e);
            visit_expr(a->target.get());
            visit_expr(a->value.get());
            return;
        }
        case ast::NodeKind::FieldAccessExpr:
            visit_expr(static_cast<ast::FieldAccessExpr *>(e)->base.get());
            return;
        case ast::NodeKind::IndexExpr: {
            auto *ix = static_cast<ast::IndexExpr *>(e);
            visit_expr(ix->base.get());
            visit_expr(ix->index.get());
            return;
        }
        case ast::NodeKind::CastExpr:
            visit_expr(static_cast<ast::CastExpr *>(e)->operand.get());
            return;
        case ast::NodeKind::TernaryExpr: {
            auto *te = static_cast<ast::TernaryExpr *>(e);
            visit_expr(te->cond.get());
            visit_expr(te->then_expr.get());
            visit_expr(te->else_expr.get());
            return;
        }
        default: return;
        }
    };

    visit_stmt = [&](ast::Stmt *st) {
        if (!st || found) return;
        switch (st->kind) {
        case ast::NodeKind::BlockStmt: {
            auto *b = static_cast<ast::BlockStmt *>(st);
            for (auto &child : b->body)
                visit_stmt(child.get());
            return;
        }
        case ast::NodeKind::VarDeclStmt: {
            auto *vd = static_cast<ast::VarDeclStmt *>(st);
            if (vd->init) visit_expr(vd->init.get());
            return;
        }
        case ast::NodeKind::ExprStmt:
            visit_expr(static_cast<ast::ExprStmt *>(st)->expr.get());
            return;
        case ast::NodeKind::IfStmt: {
            auto *si = static_cast<ast::IfStmt *>(st);
            visit_expr(si->cond.get());
            visit_stmt(si->then_branch.get());
            visit_stmt(si->else_branch.get());
            return;
        }
        case ast::NodeKind::WhileStmt: {
            auto *w = static_cast<ast::WhileStmt *>(st);
            visit_expr(w->cond.get());
            visit_stmt(w->body.get());
            return;
        }
        case ast::NodeKind::DoWhileStmt: {
            auto *dw = static_cast<ast::DoWhileStmt *>(st);
            visit_stmt(dw->body.get());
            visit_expr(dw->cond.get());
            return;
        }
        case ast::NodeKind::ForStmt: {
            auto *f = static_cast<ast::ForStmt *>(st);
            visit_stmt(f->init.get());
            visit_expr(f->cond.get());
            visit_expr(f->step.get());
            visit_stmt(f->body.get());
            return;
        }
        case ast::NodeKind::TryStmt: {
            auto *ts = static_cast<ast::TryStmt *>(st);
            visit_stmt(ts->body.get());
            for (auto &cc : ts->catches)
                visit_stmt(cc.body.get());
            if (ts->finally_body) visit_stmt(ts->finally_body.get());
            return;
        }
        case ast::NodeKind::ReturnStmt:
            visit_expr(static_cast<ast::ReturnStmt *>(st)->value.get());
            return;
        case ast::NodeKind::SynchronizedStmt: {
            auto *sy = static_cast<ast::SynchronizedStmt *>(st);
            visit_expr(sy->target.get());
            visit_stmt(sy->body.get());
            return;
        }
        default: return;
        }
    };
    visit_stmt(s);
    return found;
}

// ---------------------------------------------------------------------
// scan_escaping_locals: pre-pase que recorre el body buscando
// patrones donde el handle de un local escapa del scope:
//
//   - return ident;            -> ident escapa via valor de retorno.
//   - this.field   = ident;    -> ident escapa via campo de objeto.
//   - obj.field    = ident;    -> idem.
//   - *ptr         = ident;    -> escapa via deref-store.
//   - arr[i]       = ident;    -> escapa via slot de array.
//   - p->field     = ident;    -> escapa via field deref.
//
// Los locales detectados se añaden a @c escaping_locals_; el cleanup
// automatico los omite y queda como responsabilidad del caller (o del
// futuro GC roots) liberar el handle.
//
// Conservador: solo detecta escape via los patrones listados.  Pasar el
// local como argumento a una funcion NO se considera escape (el callee
// tipicamente solo lee el handle; si retiene una copia es responsabilidad
// suya marcar el escape via su propio analisis).
// ---------------------------------------------------------------------
void Lowering::scan_escaping_locals(ast::Stmt *body) {
    if (!body) return;
    std::function<void(ast::Expr *)> visit_expr;
    std::function<void(ast::Stmt *)> visit_stmt;

    // Grafo de aliasing local-to-local: alias_graph[A] = {B, C, ...}
    // significa "A puede contener un valor que vino de B, C, ..." (a
    // traves de asignaciones `A = B;`).  Tras la primera pasada
    // propagamos el escape hacia atras: si A es escaping, todos los
    // que feed-en a A tambien escapan.
    std::unordered_map<std::string, std::vector<std::string>> alias_graph;

    // Helper: si @p e es IdentExpr, marca el nombre como escaping.
    auto mark_if_ident = [&](ast::Expr *e) {
        if (e && e->kind == ast::NodeKind::IdentExpr) {
            auto *id = static_cast<ast::IdentExpr *>(e);
            escaping_locals_.insert(id->name);
        }
    };
    // Ruta B: un valor de un tipo con copy-hook NO escapa al guardarse en un
    // campo -- es una COPIA (el store llama __clone__ sobre la copia del campo;
    // el origen conserva su propia copia y su dtor corre).  No lo marcamos como
    // escaping (no es un move).
    auto value_has_copy_hook = [&](ast::Expr *e) -> bool {
        if (!e || e->kind != ast::NodeKind::IdentExpr) return false;
        const Type &t = e->result_type;
        // H5: un shared<T> guardado en un campo es una COPIA (inc-on-store); el
        // origen conserva su propia referencia y su dtor decrementa.  No es un
        // move -> no lo marcamos escaping (mismo trato que un copy-hook).
        if (t.kind == PrimitiveKind::SHARED_PTR) return true;
        if (t.kind != PrimitiveKind::STRUCT) return false;
        auto it = tc_.struct_layouts().find(t.struct_name);
        return it != tc_.struct_layouts().end() && it->second.has_copy_hook;
    };

    visit_expr = [&](ast::Expr *e) {
        if (!e) return;
        switch (e->kind) {
        case ast::NodeKind::AssignExpr: {
            auto *a = static_cast<ast::AssignExpr *>(e);
            // El target NO escapa por la asignacion misma.  El value
            // SI escapa cuando el target es un campo/slot/deref:
            //   - FieldAccessExpr: this.x = value, obj.x = value
            //   - IndexExpr:       arr[i] = value, p[i] = value
            //   - UnaryExpr Deref: *p = value
            if (a->target) {
                switch (a->target->kind) {
                case ast::NodeKind::FieldAccessExpr:
                    if (!value_has_copy_hook(a->value.get()))
                        mark_if_ident(a->value.get());
                    break;
                case ast::NodeKind::IndexExpr:
                    mark_if_ident(a->value.get());
                    break;
                case ast::NodeKind::UnaryExpr: {
                    auto *u = static_cast<ast::UnaryExpr *>(a->target.get());
                    if (u->op == ast::UnOp::Deref) {
                        mark_if_ident(a->value.get());
                    }
                    break;
                }
                case ast::NodeKind::IdentExpr: {
                    // Asignacion local-a-local: `target = source`.
                    // No marcamos escape ahora; registramos en el
                    // grafo de alias para propagacion transitiva.
                    // Si `target` resulta escaping al final, `source`
                    // tambien lo sera.
                    auto *id_t = static_cast<ast::IdentExpr *>(a->target.get());
                    // Una variable que se reasigna (o a la que se le anade con
                    // `+=`) puede pasar a tener buffer propio, asi que su
                    // limpieza al salir del ambito NO se puede omitir.  Se
                    // apunta aqui, que es el unico sitio que ya recorre el
                    // cuerpo entero.
                    reassigned_locals_.insert(id_t->name);
                    if (a->value &&
                        a->value->kind == ast::NodeKind::IdentExpr) {
                        auto *id_v =
                            static_cast<ast::IdentExpr *>(a->value.get());
                        alias_graph[id_t->name].push_back(id_v->name);
                    }
                    break;
                }
                default: break;
                }
            }
            visit_expr(a->target.get());
            visit_expr(a->value.get());
            return;
        }
        case ast::NodeKind::BinaryExpr: {
            auto *b = static_cast<ast::BinaryExpr *>(e);
            visit_expr(b->lhs.get());
            visit_expr(b->rhs.get());
            return;
        }
        case ast::NodeKind::UnaryExpr: {
            auto *u = static_cast<ast::UnaryExpr *>(e);
            visit_expr(u->operand.get());
            return;
        }
        case ast::NodeKind::CallExpr: {
            auto *c = static_cast<ast::CallExpr *>(e);
            visit_expr(c->callee.get());
            for (auto &arg : c->args)
                visit_expr(arg.get());
            return;
        }
        case ast::NodeKind::FieldAccessExpr: {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e);
            visit_expr(fa->base.get());
            return;
        }
        case ast::NodeKind::IndexExpr: {
            auto *ix = static_cast<ast::IndexExpr *>(e);
            visit_expr(ix->base.get());
            visit_expr(ix->index.get());
            return;
        }
        default: return;
        }
    };

    visit_stmt = [&](ast::Stmt *st) {
        if (!st) return;
        switch (st->kind) {
        case ast::NodeKind::BlockStmt: {
            auto *b = static_cast<ast::BlockStmt *>(st);
            for (auto &child : b->body)
                visit_stmt(child.get());
            return;
        }
        case ast::NodeKind::VarDeclStmt: {
            auto *vd = static_cast<ast::VarDeclStmt *>(st);
            // `T target = source;` propaga alias para tracking transitivo.
            if (vd->init && vd->init->kind == ast::NodeKind::IdentExpr) {
                auto *id_v = static_cast<ast::IdentExpr *>(vd->init.get());
                alias_graph[vd->name].push_back(id_v->name);
                // Ruta B (move-only): `S b = a` de un struct GESTIONADO (con
                // dtor o campo destructible) SIN copy-hook es un MOVE (estilo
                // Rust): `b` toma el ownership y el dtor de `a` se SUPRIME. Sin
                // esto la copia bit a bit dejaria a `a` y `b` con el mismo
                // recurso -> doble free.  Para tipos con copy-hook NO es move
                // (la copia es real, ambos gestionan via __clone__).
                const Type &st_t = id_v->result_type;
                if (st_t.kind == PrimitiveKind::STRUCT) {
                    auto it = tc_.struct_layouts().find(st_t.struct_name);
                    if (it != tc_.struct_layouts().end()) {
                        const StructLayout &sl = it->second;
                        bool managed = sl.has_destructible_field;
                        if (!managed)
                            for (const auto &mm : sl.methods)
                                if (mm.is_destructor) {
                                    managed = true;
                                    break;
                                }
                        if (managed && !sl.has_copy_hook)
                            escaping_locals_.insert(id_v->name);
                    }
                }
            }
            if (vd->init) visit_expr(vd->init.get());
            return;
        }
        case ast::NodeKind::ExprStmt: {
            auto *es = static_cast<ast::ExprStmt *>(st);
            visit_expr(es->expr.get());
            return;
        }
        case ast::NodeKind::IfStmt: {
            auto *si = static_cast<ast::IfStmt *>(st);
            visit_expr(si->cond.get());
            visit_stmt(si->then_branch.get());
            visit_stmt(si->else_branch.get());
            return;
        }
        case ast::NodeKind::WhileStmt: {
            auto *w = static_cast<ast::WhileStmt *>(st);
            visit_expr(w->cond.get());
            visit_stmt(w->body.get());
            return;
        }
        case ast::NodeKind::DoWhileStmt: {
            auto *dw = static_cast<ast::DoWhileStmt *>(st);
            visit_stmt(dw->body.get());
            visit_expr(dw->cond.get());
            return;
        }
        case ast::NodeKind::ForStmt: {
            auto *f = static_cast<ast::ForStmt *>(st);
            visit_stmt(f->init.get());
            visit_expr(f->cond.get());
            visit_expr(f->step.get());
            visit_stmt(f->body.get());
            return;
        }
        case ast::NodeKind::TryStmt: {
            // Recursar tambien en try para detectar escapes de
            // locales dentro de body, catches y finally.
            auto *ts = static_cast<ast::TryStmt *>(st);
            visit_stmt(ts->body.get());
            for (auto &cc : ts->catches)
                visit_stmt(cc.body.get());
            if (ts->finally_body) visit_stmt(ts->finally_body.get());
            return;
        }
        case ast::NodeKind::SynchronizedStmt: {
            auto *sy = static_cast<ast::SynchronizedStmt *>(st);
            visit_expr(sy->target.get());
            visit_stmt(sy->body.get());
            return;
        }
        case ast::NodeKind::ReturnStmt: {
            auto *r = static_cast<ast::ReturnStmt *>(st);
            // return ident; -> ident escapa.
            mark_if_ident(r->value.get());
            visit_expr(r->value.get());
            return;
        }
        default: return;
        }
    };
    visit_stmt(body);

    // ----- Propagacion transitiva del escape via alias_graph -----
    // Si `target = source` y target ya esta marcado como escaping, source
    // tambien debe estarlo (aliasing semantico).  Iteramos hasta punto fijo.
    // Coste: O(N*M) donde N=#locales escaping, M=longitud cadena alias.
    // En la practica las cadenas son cortas (1-3 hops); converge rapido.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &kv : alias_graph) {
            const std::string &target = kv.first;
            if (escaping_locals_.count(target) == 0) continue;
            for (const std::string &source : kv.second) {
                if (escaping_locals_.insert(source).second) {
                    changed = true;
                }
            }
        }
    }
}

ir::IrValueId Lowering::read_local(const std::string &name, ir::IrType ir_ty,
                                   uint32_t source_line) {
    const ir::IrValueId v = lookup(name);
    if (v == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
    if (!address_taken_locals_.count(name)) return v;
    // Address-taken: el scope guarda la direccion de un ALLOCA;
    // emitimos un LOAD para obtener el valor actual.
    const ir::IrValueId dst = fn_->new_value(ir_ty);
    ir::IrInstr ins{};
    ins.op = ir::IrOp::LOAD;
    ins.type = ir_ty;
    ins.dst = dst;
    ins.operands = {v};
    ins.source_line = source_line;
    emit(current_block_, std::move(ins));
    // Limitacion (cerrada): si el local fue marcado como host-bearing
    // (al menos un write_local le grabo un valor con is_host_ptr=true),
    // el LOAD reconstruye el bit en el SSA value resultante.  Sin esto
    // el round-trip `T* p = malloc(); ...; LOAD &p` perderia el bit y
    // el siguiente LOAD/STORE indirecto emitiria mov en vez de movh.
    if (host_bearing_locals_.count(name)) {
        fn_->values[dst].is_host_ptr = true;
    }
    return dst;
}

void Lowering::write_local(const std::string &name, ir::IrValueId v,
                           ir::IrType ir_ty, uint32_t source_line) {
    // `static T x` local: la escritura va al slot global (gdata), no al
    // scope.  Persistente entre llamadas.  Los agregados no pasan por aqui
    // (se copian campo a campo via su direccion).
    {
        auto sit = static_local_slots_.find(name);
        if (sit != static_local_slots_.end() && !sit->second.aggregate) {
            ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr is{};
                is.op = ir::IrOp::STR_LIT_ADDR;
                is.type = ir::IrType::PTR;
                is.dst = addr;
                is.imm = sit->second.slot;
                is.source_line = source_line;
                emit(current_block_, std::move(is));
                fn_->values[addr].is_host_ptr = true;
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = sit->second.ld_type;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v, addr};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
            return;
        }
    }
    if (!address_taken_locals_.count(name)) {
        update_scope(name, v);
        // Si la variable tiene slot activo en un try, ADICIONALMENTE
        // emitir STORE al slot.  El motivo: cuando ocurre un @c throw
        // dentro del body del try, el handler en el catch necesita el
        // ultimo valor de la variable, pero do_throw restaura el RSP
        // y descarta cualquier @c push del save/restore alrededor de
        // los CALLs.  Sin este STORE redundante, el catch leeria un
        // registro con un valor obsoleto o corrupto.
        auto it_slot = try_spill_slots_.find(name);
        if (it_slot != try_spill_slots_.end()) {
            // Usar el tipo real del valor para que el STORE escriba
            // exactamente N bytes y no contamine los bytes altos
            // del slot 8-byte alloca.
            ir::IrType st_ty = ir_ty;
            if (v < fn_->values.size()) {
                st_ty = fn_->values[v].type;
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = st_ty;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v, it_slot->second};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        }
        return;
    }
    // Address-taken: emitir STORE a la direccion guardada en scope.
    const ir::IrValueId addr = lookup(name);
    if (addr == ir::IR_NO_VALUE) {
        update_scope(name, v); // fallback defensivo
        return;
    }
    ir::IrInstr st{};
    st.op = ir::IrOp::STORE;
    st.type = ir_ty;
    st.dst = ir::IR_NO_VALUE;
    st.operands = {v, addr}; // STORE: operands[0]=val, operands[1]=ptr
    st.source_line = source_line;
    emit(current_block_, std::move(st));
    // Limitacion (cerrada): registrar host-bearing si el valor escrito
    // proviene de heap host (malloc o aritmetica derivada).  read_local
    // consulta este set para propagar is_host_ptr al LOAD del slot.
    // Ademas marcamos el SSA value del slot (addr) con pointee_is_host_ptr
    // para que el caso indirecto @c &p; *pp tambien propague is_host_ptr
    // al destino del LOAD via el ir_emitter.  Sticky por simplicidad: una
    // vez marcado, el local queda host-bearing aunque despues le asignen
    // un valor VM.  Aceptable porque en la practica los locales mantienen
    // su naturaleza a lo largo de su vida.
    if (v != ir::IR_NO_VALUE && fn_->values[v].is_host_ptr) {
        host_bearing_locals_.insert(name);
        fn_->values[addr].pointee_is_host_ptr = true;
    }
}

// ---------------------------------------------------------------------
// Errores y helpers de diagnostico.
// ---------------------------------------------------------------------

void Lowering::unsupported(SourceLoc loc, const char *feature) {
    diags_.error(std::move(loc),
                 std::string("lowering: caracteristica aun no soportada: ") +
                     feature);
}

void Lowering::error_at(SourceLoc loc, std::string msg) {
    diags_.error(std::move(loc), std::move(msg));
}

// ---------------------------------------------------------------------
// Exportacion de metadata POO al IrModule (para port transpilers).
// ---------------------------------------------------------------------

void Lowering::export_classes_to_ir(ir::IrModule &out) {
    const auto &layouts = tc_.class_layouts();
    out.classes.reserve(layouts.size());
    for (const auto &kv : layouts) {
        const auto &cl = kv.second;
        // Saltar clases predefinidas en runtime (e.g. FatalError):
        // el port no debe re-emitirlas; el runtime las provee.
        if (cl.is_runtime_predefined) continue;

        ir::IrClass icls;
        icls.name = cl.name;
        icls.super_name = cl.super_name;
        icls.interfaces = cl.interface_names;
        icls.size_bytes = cl.size_bytes;
        icls.is_final = false; /* Vesta frontend lo trackea por metodo;
                agregado lo deducimos en transpiler
                via hierarchy analysis cuando es
                necesario.  Default false = seguro. */
        icls.is_interface = cl.is_interface;
        icls.is_aspect = cl.is_aspect;
        icls.has_destructor = cl.has_destructor;
        icls.has_destructible_field = cl.has_destructible_field;
        icls.is_runtime_predefined = false;

        // Convertir fields de instancia.  Mantenemos el orden del
        // ClassLayout (heredados primero, luego propios) -- el
        // transpiler los emite tal cual en el struct C.
        icls.fields.reserve(cl.fields.size());
        for (const auto &f : cl.fields) {
            ir::IrField ifld;
            ifld.name = f.name;
            ifld.type = ir_type_from_primitive(f.type.kind);
            ifld.offset = f.offset;
            ifld.size_bytes = f.size;
            ifld.is_static = false;
            /* Si el tipo del field es CLASS, registrar el nombre de la
             * clase apuntada -- el transpiler lo necesita para emitir
             * el tipo C correcto (`ClassY *` vs `void *`). */
            if (f.type.kind == PrimitiveKind::CLASS) {
                ifld.class_type_name = f.type.struct_name;
            }
            icls.fields.push_back(std::move(ifld));
        }

        // Static fields.
        icls.static_fields.reserve(cl.static_fields.size());
        for (const auto &f : cl.static_fields) {
            ir::IrField ifld;
            ifld.name = f.name;
            ifld.type = ir_type_from_primitive(f.type.kind);
            ifld.offset = f.offset;
            ifld.size_bytes = f.size;
            ifld.is_static = true;
            if (f.type.kind == PrimitiveKind::CLASS) {
                ifld.class_type_name = f.type.struct_name;
            }
            icls.static_fields.push_back(std::move(ifld));
        }

        // Convertir metodos.  El @c ir_fn_name sigue el mangling de
        // @c lower_class_methods: "<Class>__ctor" para constructores,
        // "<Class>__<name>" para el resto (destructor usa name="__dtor"
        // -> ir_fn_name="<Class>____dtor" con 4 underscores).
        icls.methods.reserve(cl.methods.size());
        for (const auto &m : cl.methods) {
            ir::IrMethod imeth;
            imeth.name = m.name;
            if (m.is_constructor) {
                imeth.ir_fn_name = cl.name + "__ctor";
            } else {
                // Si el metodo es heredado puro (no override), apuntar al
                // simbolo del defining_class para evitar emitir referencia
                // a un Class__method que no existe.  El transpiler C usa
                // este nombre como label de funcion.
                const std::string &defc = m.defining_class;
                const std::string &owner =
                    (!defc.empty() && defc != cl.name) ? defc : cl.name;
                imeth.ir_fn_name = owner + "__" + m.name;
            }
            imeth.return_type = ir_type_from_primitive(m.return_type.kind);
            imeth.param_types.reserve(m.param_types.size());
            for (const auto &pt : m.param_types) {
                imeth.param_types.push_back(ir_type_from_primitive(pt.kind));
            }
            imeth.vtable_index = static_cast<int32_t>(m.vtable_index);
            imeth.is_static = m.is_static;
            imeth.is_final = m.is_final;
            imeth.is_constructor = m.is_constructor;
            imeth.is_destructor = m.is_destructor;
            imeth.is_inline = m.is_inline;
            imeth.defining_class = m.defining_class;
            icls.methods.push_back(std::move(imeth));
        }

        out.classes.push_back(std::move(icls));
    }
}

// =====================================================================
// BugFix R1: super(args) y super.method(args)
// =====================================================================
//
// super(args) (SuperCallExpr): dentro de un ctor de clase derivada,
// invoca el ctor del super con this como receptor.  El this implicito
// se obtiene del primer parametro (lookup("this")).
//
// Implementacion correcta: emitir el opcode bytecode `callsuper`
// (0xFC) que dispatcha a la vtable de la SUPER class (no del
// receiver dinamico).  Esto evita:
//   (1) Recursion infinita en ctor: `callvirt this, 0` con un
//       Derived as receiver resolveria vtable[0]=Derived.__ctor
//       (no Base.__ctor) -> recursion.
//   (2) Override-en-medio: si Derived overridea un metodo de Base,
//       `super.foo()` desde Derived debe llamar Base.foo, NO
//       Derived.foo.  CALLVIRT lo haria mal; CALLSUPER lo hace bien.
//
// El IR no tiene un IrOp::CALLSUPER dedicado, asi que el lowering
// usa RAW_ASM con la sintaxis textual `callsuper r_cls, vtable_idx`
// del assembler de la VM.  El bloque RAW_ASM emite:
//   findclass <super_name> -> r_cls (host_ptr a ClassInfo del super)
//   mov r1, this
//   mov r2..rN, args
//   mov r15, argc
//   callsuper r_cls, vtable_idx
//
// super.method(args) (SuperMethodCallExpr): igual patron pero busca
// el metodo non-ctor por nombre en la cadena super (BFS) y emite
// callsuper con su vtable_index dentro de la SUPER class.  Si hay
// overrides intermedios entre Derived y la clase que define el
// metodo, el dispatch va a la clase mas cercana en la jerarquia
// super (Java's `super.method` semantica).
ir::IrValueId Lowering::lower_super_call_expr(ast::SuperCallExpr *e) {
    // Resolver this implicito.
    const ir::IrValueId v_this = lookup("this");
    if (v_this == ir::IR_NO_VALUE) {
        error_at(e->loc, "super(...): no se encontro 'this' en el scope");
        return ir::IR_NO_VALUE;
    }
    // Buscar el super_name del current_class_.
    if (current_class_lowering_.empty()) {
        error_at(e->loc, "super(...) fuera de cuerpo de clase");
        return ir::IR_NO_VALUE;
    }
    auto it = tc_.class_layouts().find(current_class_lowering_);
    if (it == tc_.class_layouts().end() || it->second.super_name.empty()) {
        error_at(e->loc, "super(...) en clase sin super");
        return ir::IR_NO_VALUE;
    }
    const std::string &super_name = it->second.super_name;
    auto it_s = tc_.class_layouts().find(super_name);
    if (it_s == tc_.class_layouts().end()) {
        error_at(e->loc, "super clase '" + super_name + "' desconocida");
        return ir::IR_NO_VALUE;
    }
    // Buscar el ctor PROPIO del super (no heredado de su super-super).
    // BugFix R1.fix: si el super tambien deriva de otra clase, sus
    // methods comienzan con la inherited ctor del super-super.  Sin
    // priorizar el ctor cuyo defining_class == super_name, el callsuper
    // dispatcharia a Mid.vtable[0] = Base.__ctor (inherited) en lugar
    // de Mid.__ctor (own), con la aridad de Base.__ctor en vez de Mid.
    const ClassMethodInfo *super_ctor = nullptr;
    for (const auto &m : it_s->second.methods) {
        if (m.is_constructor && m.defining_class == super_name) {
            super_ctor = &m;
            break;
        }
    }
    // Fallback: si no hay ctor propio en super, usar el primero.
    if (!super_ctor) {
        for (const auto &m : it_s->second.methods) {
            if (m.is_constructor) {
                super_ctor = &m;
                break;
            }
        }
    }
    if (!super_ctor) {
        error_at(e->loc, "super(...): la clase super '" + super_name +
                             "' no tiene constructor");
        return ir::IR_NO_VALUE;
    }
    // Bajar args.
    std::vector<ir::IrValueId> arg_vals;
    arg_vals.reserve(e->args.size());
    for (auto &a : e->args) {
        const ir::IrValueId av = lower_expr(a.get());
        if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        arg_vals.push_back(av);
    }
    // AOT (native_poo_): la clase super es estaticamente conocida -> CALL
    // DIRECTO a <super>__ctor(this, args).  Evita findclass + callsuper
    // (ambos runtime, no compilables en bare).  Habilita herencia en AOT.
    if (native_poo_) {
        const std::string sname = super_ctor->defining_class.empty()
                                      ? super_name
                                      : super_ctor->defining_class;
        ir::IrInstr ca{};
        ca.op = ir::IrOp::CALL;
        ca.type = ir::IrType::VOID;
        ca.dst = ir::IR_NO_VALUE;
        ca.func_name = sname + "__ctor";
        ca.operands.push_back(v_this);
        for (auto av : arg_vals)
            ca.operands.push_back(av);
        ca.source_line = e->loc.line;
        emit(current_block_, std::move(ca));
        return ir::IR_NO_VALUE;
    }
    // Resolver ClassInfo* del super via findclass inline (mismo patron
    // que forName).  Resultado en v_cls.  Luego emitir CALLSUPER IR.
    const uint64_t super_name_idx = intern_class_name(*out_mod_, super_name);
    const uint32_t super_name_len = static_cast<uint32_t>(super_name.size());
    // Sprint 5: findclass via IR ops.
    const ir::IrValueId v_cls =
        emit_findclass_by_name(super_name_idx, super_name_len, e->loc.line);
    // Emit CALLSUPER IR: layout = [cls, this, args...], imm=vtbl_idx.
    // El emisor IR coloca obj en r1, args en r2..r_{N+1}, cls en r13,
    // y emite `callsuper r13, vtable_idx`.  Sin RAW_ASM: el regalloc,
    // DCE y otros pases ven la operacion como un CALL real.
    ir::IrInstr cs{};
    cs.op = ir::IrOp::CALLSUPER;
    cs.type = ir::IrType::VOID;
    cs.dst = ir::IR_NO_VALUE;
    cs.operands.push_back(v_cls);
    cs.operands.push_back(v_this);
    for (auto av : arg_vals)
        cs.operands.push_back(av);
    cs.imm = static_cast<uint64_t>(super_ctor->vtable_index);
    cs.source_line = e->loc.line;
    emit(current_block_, std::move(cs));
    return ir::IR_NO_VALUE;
}

ir::IrValueId
Lowering::lower_super_method_call_expr(ast::SuperMethodCallExpr *e) {
    const ir::IrValueId v_this = lookup("this");
    if (v_this == ir::IR_NO_VALUE) {
        error_at(e->loc,
                 "super." + e->method_name + "(...): no se encontro 'this'");
        return ir::IR_NO_VALUE;
    }
    if (current_class_lowering_.empty()) {
        error_at(e->loc, "super.<metodo>(...) fuera de cuerpo de clase");
        return ir::IR_NO_VALUE;
    }
    auto it = tc_.class_layouts().find(current_class_lowering_);
    if (it == tc_.class_layouts().end() || it->second.super_name.empty()) {
        error_at(e->loc, "super.<metodo>(...) en clase sin super");
        return ir::IR_NO_VALUE;
    }
    // Buscar el metodo en la cadena super (BFS).
    std::string cur = it->second.super_name;
    const ClassMethodInfo *found = nullptr;
    for (int depth = 0; depth < 32; ++depth) {
        auto it_s = tc_.class_layouts().find(cur);
        if (it_s == tc_.class_layouts().end()) break;
        for (const auto &m : it_s->second.methods) {
            if (!m.is_constructor && m.name == e->method_name) {
                found = &m;
                break;
            }
        }
        if (found) break;
        if (it_s->second.super_name.empty()) break;
        cur = it_s->second.super_name;
    }
    if (!found) {
        error_at(e->loc, "super." + e->method_name + ": metodo no encontrado");
        return ir::IR_NO_VALUE;
    }
    std::vector<ir::IrValueId> arg_vals;
    arg_vals.reserve(e->args.size());
    for (auto &a : e->args) {
        const ir::IrValueId av = lower_expr(a.get());
        if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        arg_vals.push_back(av);
    }
    const ir::IrType ret_ir = ir_type_from_primitive(found->return_type.kind);
    const ir::IrValueId dst =
        (ret_ir == ir::IrType::VOID) ? ir::IR_NO_VALUE : fn_->new_value(ret_ir);
    // AOT (native_poo_): el metodo base es estaticamente conocido ->
    // CALL DIRECTO a <defining_class>__<metodo>(this, args).  Evita
    // findclass + callsuper (runtime).  Habilita super.metodo() en AOT.
    if (native_poo_) {
        const std::string owner = found->defining_class.empty()
                                      ? it->second.super_name
                                      : found->defining_class;
        ir::IrInstr ca{};
        ca.op = ir::IrOp::CALL;
        ca.type = ret_ir;
        ca.dst = dst;
        ca.func_name = owner + "__" + found->name;
        ca.operands.push_back(v_this);
        for (auto av : arg_vals)
            ca.operands.push_back(av);
        ca.source_line = e->loc.line;
        emit(current_block_, std::move(ca));
        return dst;
    }
    // Resolver ClassInfo* del super via findclass inline.
    const std::string &super_name = it->second.super_name;
    const uint64_t super_name_idx = intern_class_name(*out_mod_, super_name);
    const uint32_t super_name_len = static_cast<uint32_t>(super_name.size());
    // Sprint 5: findclass via IR ops.
    const ir::IrValueId v_cls =
        emit_findclass_by_name(super_name_idx, super_name_len, e->loc.line);
    // Emit CALLSUPER IR (mismo patron que super(args) ctor).
    ir::IrInstr cs{};
    cs.op = ir::IrOp::CALLSUPER;
    cs.type = ret_ir;
    cs.dst = dst;
    cs.operands.push_back(v_cls);
    cs.operands.push_back(v_this);
    for (auto av : arg_vals)
        cs.operands.push_back(av);
    cs.imm = static_cast<uint64_t>(found->vtable_index);
    cs.source_line = e->loc.line;
    emit(current_block_, std::move(cs));
    return dst;
}
} // namespace vx
