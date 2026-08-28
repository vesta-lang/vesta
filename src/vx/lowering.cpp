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
#include "util/thread_slot.h" // el estado por hilo NO va en thread_local
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
std::string macro_body_unsupported_reason(const TypeChecker &tc,
                                                 const ast::Stmt *s);

/* El estado por hilo del bajado de macros va en las ranuras propias del
 * proyecto (util/thread_slot.h) y no en `thread_local`.
 *
 * No es una preferencia: declararlas `extern thread_local` para que otra
 * unidad las alcanzara las hacia pasar por la capa emulada de este toolchain,
 * donde no se leia el nulo con el que nacen sino memoria sin sentido -- el
 * `if (puntero)` daba verdadero y se desreferenciaba --.  Reventaba al compilar
 * programas que ni siquiera tienen macros.  Esa cabecera ya existia y cuenta
 * esta misma historia: la via emulada les habia costado antes un cuelgue.
 *
 * La ranura es una global normal; lo que cambia por hilo es su CONTENIDO, asi
 * que no hay inicializador dinamico ni variable de guarda, que es de donde
 * venian los dos fallos. */
static util::ThreadSlot g_macro_force_lower_slot;
static util::ThreadSlot g_macro_visiting_slot;

std::unordered_set<std::string> *macro_force_lower() {
    return static_cast<std::unordered_set<std::string> *>(
        g_macro_force_lower_slot.get());
}
void set_macro_force_lower(std::unordered_set<std::string> *p) {
    g_macro_force_lower_slot.ensure();
    g_macro_force_lower_slot.set(p);
}
std::unordered_set<std::string> *macro_visiting() {
    return static_cast<std::unordered_set<std::string> *>(
        g_macro_visiting_slot.get());
}
void set_macro_visiting(std::unordered_set<std::string> *p) {
    g_macro_visiting_slot.ensure();
    g_macro_visiting_slot.set(p);
}

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
std::string macro_body_unsupported_reason(const TypeChecker &tc,
                                                 const ast::Stmt *s);

std::string macro_body_unsupported_reason_expr(const TypeChecker &tc,
                                                      const ast::Expr *e);

/* Force-lower de comptime helpers: cuando el estado de force-lower esta puesto, el
 * chequeo de lowereabilidad NO rechaza las llamadas a comptime fns no-macro,
 * sino que RECURRE en su body (chequeo transitivo) y, si son lowereables,
 * recolecta su nombre ahi para que @c lower_function las
 * baje a runtime (`code.<helper>`), permitiendo que el `__macro_<X>` que las
 * llama resuelva.  La guarda de ciclos va aparte.  Por hilo
 * porque M8 compila modulos en paralelo (cada thread con su propio contexto).
 * (Definidos arriba, antes de Lowering::run.) */

std::string macro_body_unsupported_reason_expr(const TypeChecker &tc,
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
                 * (con force-lower puesto): recurrir en su body; si es
                 * lowereable, recolectarla para bajarla a runtime y ACEPTAR la
                 * llamada.  Sin force-lower (call sites legacy): rechazar
                 * (AST-only), comportamiento previo. */
                auto *force_lower = macro_force_lower();
                auto *visiting = macro_visiting();
                if (force_lower && fn_it->second &&
                    fn_it->second->body) {
                    const std::string &hn = fn_it->first; // nombre registrado
                    if (visiting->count(hn)) {
                        return ""; // ciclo: asumir OK (el otro nivel decide)
                    }
                    visiting->insert(hn);
                    std::string sub = macro_body_unsupported_reason(
                        tc, fn_it->second->body.get());
                    visiting->erase(hn);
                    if (sub.empty()) {
                        force_lower->insert(hn);
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

std::string macro_body_unsupported_reason(const TypeChecker &tc,
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
bool macro_body_forwards_expr_capture_expr(const TypeChecker &tc,
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

bool macro_body_forwards_expr_capture(const TypeChecker &tc,
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
void annotate_macro_param_idents(
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
