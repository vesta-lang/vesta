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
 * @file compiler.cpp
 * @brief Implementacion del facade del compilador Vesta.
 */

#include "vx/compiler.h"
#include "vx/c_header_gen.h" // Fase 4 interop C: vx --emit-header
#include "vx/vxdbg_emit.h"   // base de conocimiento de depuracion

#include "analyze/bigo.h"
#include "analyze/fingerprint.h" // verificacion de contratos de huella
#include "ir/ir_emitter.h"
#include "ir/ir_optimizer.h"
#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include "vx/lexer.h"
#include "vx/lowering.h"
#include "vx/diagram/mermaid_diagrams.h"
#include "vx/diagram/graphviz_diagrams.h"
#include "vx/diagram/html_diagrams.h"
#include "vx/module/namespace_flatten.h"
#include "vx/parser.h"
#include <iostream>

#include "vx/comptime/comptime_collect.h"
#include "vx/comptime/comptime_vm.h"
#include "ctpe/evaluable.h"
#include "jit/auto_jit.h"
#include <memory>

// Fwd-decl de run_worker (evita arrastrar assembler_multiprocess.h -> json.hpp,
// que no esta en el include path del frontend).  Firma exacta del header.
namespace asm_multi_process {
int run_worker(const std::string &file_name, const std::string &output_prefix,
                bool skip_preprocessor, bool keep_labels,
                const std::vector<uint8_t> *ir_section_bytes, bool emit_map);
}
#include "vx/type_checker.h"

#include "port/transpiler_base.h"
#include "port/c/c_backend.h"
#include "util/fs_utils.h" // FN.3: get_executable_path (auto-bundle fibras)

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vx {

/**
 * @brief Convierte un entero 0..3 al enum ir::OptLevel.
 *
 * Cualquier valor fuera de rango cae en O1 (default conservador).
 */
// Recolecta los contratos de huella declarados en el AST (recorriendo los
// NamespaceDecl) a un mapa por nombre de funcion.  Se lleva APARTE del IR por
// diseno: son metadata compile-time (modo --analyze) que el codegen no necesita.
static void collect_contracts_(
    const std::vector<std::unique_ptr<ast::Node>> &decls,
    std::unordered_map<std::string, analyze::FunctionContracts> &out) {
    for (const auto &d : decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            collect_contracts_(
                static_cast<const ast::NamespaceDecl *>(d.get())->decls, out);
            continue;
        }
        if (d->kind == ast::NodeKind::FunctionDecl) {
            const auto *fd = static_cast<const ast::FunctionDecl *>(d.get());
            analyze::FunctionContracts c;
            c.pure = fd->contract_pure;
            c.nothrow = fd->contract_nothrow;
            c.nopanic = fd->contract_nopanic;
            c.alloc_total = fd->contract_alloc;
            c.alloc_partial = fd->contract_alloc_partial;
            c.stack_total = fd->contract_stack;
            c.stack_partial = fd->contract_stack_partial;
            if (c.any()) out[fd->name] = c;
        }
        // Metodos de struct/clase: el mismo contrato sobre lo mismo.  Un metodo
        // baja a una IrFunction `Tipo__metodo`, asi que se registra con esa
        // clave -- la que el analizador vera.  Sin esto, un tipo cuya API son
        // METODOS podia DECLARAR sus contratos pero nadie los verificaba: peor
        // que no tenerlos, porque parecerian comprobados.
        auto tomar_metodos =
            [&](const std::string &tipo,
                const std::vector<std::unique_ptr<ast::ClassMethodDecl>> &ms) {
                for (const auto &m : ms) {
                    if (!m) continue;
                    analyze::FunctionContracts c;
                    c.pure = m->contract_pure;
                    c.nothrow = m->contract_nothrow;
                    c.nopanic = m->contract_nopanic;
                    c.alloc_total = m->contract_alloc;
                    c.alloc_partial = m->contract_alloc_partial;
                    c.stack_total = m->contract_stack;
                    c.stack_partial = m->contract_stack_partial;
                    if (c.any()) out[tipo + "__" + m->name] = c;
                }
            };
        // Los TEMPLATES genericos se saltan: no producen IR (solo lo hacen sus
        // instanciaciones), asi que su clave no tiene nada contra que
        // verificarse -- y como `Caja__leer` acaba casando por sufijo con
        // `Caja_i64__leer`, registrarla haria que cada incumplimiento se
        // reportase dos veces.  La monomorphizacion copia los contratos, asi
        // que cada instanciacion se verifica por su cuenta.
        if (d->kind == ast::NodeKind::StructDecl) {
            const auto *sd = static_cast<const ast::StructDecl *>(d.get());
            if (sd->type_params.empty() && !sd->is_specialization)
                tomar_metodos(sd->name, sd->methods);
        } else if (d->kind == ast::NodeKind::ClassDecl) {
            const auto *cd = static_cast<const ast::ClassDecl *>(d.get());
            if (cd->type_params.empty()) tomar_metodos(cd->name, cd->methods);
        }
    }
}

// Recolecta los contratos de TIPO (@pod/@no_heap/@size) declarados sobre
// struct/clase/enum a un mapa por nombre de tipo.  Mismo criterio que
// collect_contracts_ (compile-time, sin serializar).
static void collect_type_contracts_(
    const std::vector<std::unique_ptr<ast::Node>> &decls,
    std::unordered_map<std::string, analyze::TypeContracts> &out) {
    for (const auto &d : decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            collect_type_contracts_(
                static_cast<const ast::NamespaceDecl *>(d.get())->decls, out);
            continue;
        }
        auto take = [&](const std::string &name, bool pod, bool no_heap,
                        int64_t size) {
            analyze::TypeContracts c;
            c.pod = pod;
            c.no_heap = no_heap;
            c.size = size;
            if (c.any()) out[name] = c;
        };
        if (d->kind == ast::NodeKind::StructDecl) {
            const auto *sd = static_cast<const ast::StructDecl *>(d.get());
            take(sd->name, sd->contract_pod, sd->contract_no_heap,
                 sd->contract_size);
        } else if (d->kind == ast::NodeKind::ClassDecl) {
            const auto *cd = static_cast<const ast::ClassDecl *>(d.get());
            take(cd->name, cd->contract_pod, cd->contract_no_heap,
                 cd->contract_size);
        } else if (d->kind == ast::NodeKind::EnumDecl) {
            const auto *ed = static_cast<const ast::EnumDecl *>(d.get());
            take(ed->name, ed->contract_pod, ed->contract_no_heap,
                 ed->contract_size);
        }
    }
}

// Computa la huella de cada TIPO agregado (struct/clase/enum) a partir de los
// layouts ya resueltos del type checker.  @pod/@no_heap se componen sobre los
// tipos de campo via los clasificadores del type checker (type_is_managed /
// type_is_c_representable), que son la verdad de la frontera C.
static std::vector<analyze::TypeFingerprint>
compute_type_fingerprints_(const TypeChecker &tc) {
    std::vector<analyze::TypeFingerprint> out;
    using TF = analyze::TypeFingerprint;

    // Structs: value-types.  @pod = C-representable por valor + sin dtor.
    for (const auto &kv : tc.struct_layouts()) {
        const StructLayout &lay = kv.second;
        TF tf;
        tf.type_name = lay.name;
        tf.kind = TF::STRUCT;
        tf.size_bytes = lay.size_bytes;
        tf.align_bytes = lay.align_bytes;
        tf.field_count = static_cast<uint32_t>(lay.fields.size());
        bool has_dtor = lay.has_destructible_field;
        for (const auto &m : lay.methods)
            if (m.is_destructor) has_dtor = true;
        tf.has_destructor = has_dtor;
        bool no_heap = true, all_c_repr = true;
        for (const auto &f : lay.fields) {
            if (tc.type_is_managed(f.type)) no_heap = false;
            if (!tc.type_is_c_representable(f.type)) all_c_repr = false;
        }
        tf.no_heap = no_heap;
        tf.is_pod = all_c_repr && !has_dtor && no_heap;
        tf.is_reference = false;
        out.push_back(std::move(tf));
    }

    // Clases: tipos por REFERENCIA (viven en el heap gestionado) -> nunca @pod
    // ni @no_heap; @size verifica el tamano de la instancia.
    for (const auto &kv : tc.class_layouts()) {
        const ClassLayout &lay = kv.second;
        if (lay.is_interface) continue; // sin instancias.
        TF tf;
        tf.type_name = lay.name;
        tf.kind = TF::CLASS;
        tf.size_bytes = lay.size_bytes;
        tf.field_count = static_cast<uint32_t>(lay.fields.size());
        bool has_dtor = false;
        for (const auto &m : lay.methods)
            if (m.is_destructor) has_dtor = true;
        tf.has_destructor = has_dtor;
        tf.is_reference = true;
        tf.is_pod = false;
        tf.no_heap = false;
        out.push_back(std::move(tf));
    }

    // Enums: tagged unions inline (value-types).  @pod si ningun payload es
    // gestionado y todos son C-representables; un enum sin payload es @pod.
    for (const auto &kv : tc.enum_layouts()) {
        const EnumLayout &lay = kv.second;
        TF tf;
        tf.type_name = lay.name;
        tf.kind = TF::ENUM;
        tf.size_bytes = lay.size_bytes;
        tf.field_count = lay.max_payload_fields;
        bool no_heap = true, all_c_repr = true;
        for (const auto &v : lay.variants)
            for (const auto &ft : v.field_types) {
                if (tc.type_is_managed(ft)) no_heap = false;
                if (!tc.type_is_c_representable(ft)) all_c_repr = false;
            }
        tf.no_heap = no_heap;
        tf.is_pod = all_c_repr && no_heap;
        tf.is_reference = false;
        out.push_back(std::move(tf));
    }
    return out;
}

static ir::OptLevel opt_level_from_int(int n) noexcept {
    switch (n) {
    case 0: return ir::OptLevel::O0;
    case 1: return ir::OptLevel::O1;
    case 2: return ir::OptLevel::O2;
    case 3: return ir::OptLevel::O3;
    default: return ir::OptLevel::O1;
    }
}

CompileResult compile_vx_source(const std::string &source,
                                 const std::string &filename,
                                 const CompileOptions &opts) {
    CompileResult res;

    // 1. Lexer + Parser.  Si el lexer/parser ya reportan errores no
    // tiene sentido seguir: el AST seria parcial y los pases siguientes
    // generarian falsos positivos.
    Lexer lx(source, filename, res.diagnostics);
    Parser p(lx, res.diagnostics);
    auto mod = p.parse_program();
    if (!mod || res.diagnostics.has_errors()) {
        res.ok = false;
        return res;
    }

    // 1.5.  M.7.c: aplanar namespaces inline (`namespace ui { ... }`).
    //      Tras este pre-pass, el AST no tiene NamespaceDecl wrappers;
    //      los simbolos internos llevan prefix `<ns>__` (cero overhead
    //      runtime; compatible con port-c).  Cada namespace encontrado
    //      se registra en el TypeChecker como Symbol::Namespace para
    //      que el resolver de `ns.X` lo encuentre.
    auto inline_namespaces = flatten_namespaces(*mod);

    // P1 fase 1 (recolector): con los nombres ya mangled, identificar el
    // conjunto comptime del modulo (comptime fns/@Macro/consts + deps
    // transitivas).  Es la base del futuro artefacto comptime separado.  Hoy
    // solo diagnostico opt-in (VESTA_DUMP_COMPTIME_UNIT=1); no cambia codegen.
    if (std::getenv("VESTA_DUMP_COMPTIME_UNIT")) {
        const ComptimeUnit cu = collect_comptime_unit(*mod, source);
        dump_comptime_unit(cu, std::cerr);
    }

    // P1: eliminar el tree-walker tambien para los `comptime { }` de modulo.
    // Cada bloque se transforma en una comptime fn sintetica `__ctblock_N`
    // (que corre en la ComptimeVM como cualquier comptime fn) + una const que
    // la invoca.  Asi el `println`/buffers del bloque se ejecutan de verdad en
    // compile-time.  Los sinteticos se anexan al FINAL (preservan la semantica
    // de que el bloque corre tras todos los globales del modulo).
    {
        std::vector<std::unique_ptr<ast::Node>> kept;
        std::vector<std::unique_ptr<ast::Node>> synth;
        int ctblock_n = 0;
        for (auto &d : mod->decls) {
            if (d && d->kind == ast::NodeKind::ComptimeBlockStmt) {
                auto *cb = static_cast<ast::ComptimeBlockStmt *>(d.get());
                const std::string fname =
                    "__ctblock_" + std::to_string(ctblock_n++);
                // comptime i64 __ctblock_N() { <stmts>; return 0; }
                auto fn = std::make_unique<ast::FunctionDecl>();
                fn->name = fname;
                fn->is_comptime = true;
                fn->loc = cb->loc;
                auto rt = std::make_unique<ast::PrimitiveTypeNode>();
                rt->prim = PrimitiveKind::I64;
                fn->return_type = std::move(rt);
                auto body = std::make_unique<ast::BlockStmt>();
                for (auto &st : cb->stmts) body->body.push_back(std::move(st));
                auto ret = std::make_unique<ast::ReturnStmt>();
                auto zero = std::make_unique<ast::IntLitExpr>();
                zero->value = 0;
                ret->value = std::move(zero);
                body->body.push_back(std::move(ret));
                fn->body = std::move(body);
                // const i64 __ctblock_N_r = __ctblock_N();
                auto gv = std::make_unique<ast::GlobalVarDecl>();
                gv->name = fname + "_r";
                gv->is_const = true;
                gv->loc = cb->loc;
                auto gt = std::make_unique<ast::PrimitiveTypeNode>();
                gt->prim = PrimitiveKind::I64;
                gv->type = std::move(gt);
                auto call = std::make_unique<ast::CallExpr>();
                auto callee = std::make_unique<ast::IdentExpr>();
                callee->name = fname;
                call->callee = std::move(callee);
                call->loc = cb->loc;
                gv->init = std::move(call);
                synth.push_back(std::move(fn));
                synth.push_back(std::move(gv));
                // el ComptimeBlockStmt original se descarta.
            } else {
                kept.push_back(std::move(d));
            }
        }
        // Reasignar SIEMPRE: el loop ya movio cada decl a `kept`, dejando
        // `mod->decls` con punteros moved-from aunque no hubiera bloques.
        for (auto &s : synth) kept.push_back(std::move(s));
        mod->decls = std::move(kept);
    }

    // 2. TypeChecker: rellena result_type y valida semantica.
    TypeChecker tc(*mod, res.diagnostics);
    // Registrar namespaces inline en el checker ANTES de run().
    for (const auto &ins : inline_namespaces) {
        const uint32_t ns_idx =
            tc.register_imported_namespace(ins.name, ins.name);
        for (const auto &sym : ins.symbols) {
            TypeChecker::ImportedNamespace::Sym ns_sym;
            ns_sym.kind =
                (sym.kind == FlattenedNamespace::Sym::Function)
                    ? 0
                    : (sym.kind == FlattenedNamespace::Sym::Type ? 2 : 1);
            ns_sym.mangled_label = sym.mangled_label;
            // Para funciones, las firmas se rellenaran en check_function
            // del propio TypeChecker.  Aqui solo registramos la
            // existencia + mangled_label.  Esto basta porque la
            // funcion estara TAMBIEN en function_sigs_ (con el nombre
            // mangled como key), por lo que el lookup puede ir alli.
            tc.register_namespace_symbol(ns_idx, sym.public_name,
                                         std::move(ns_sym));
            // NS.2 round-trip: recordar el namespace declarado para el export.
            tc.register_declared_ns_symbol(sym.mangled_label, ins.name,
                                           sym.public_name);
        }
    }
    // Si el LSP pidio volcar valores comptime, activamos la captura
    // de las variables locales de los bloques `comptime { ... }`
    // ANTES de run() (para que check_stmt las acumule al evaluarlos).
    // Gateado: cero coste cuando dump_comptime_values esta off.
    if (opts.dump_comptime_values) {
        tc.set_capture_comptime_block_locals(true);
    }
    if (!tc.run()) {
        res.ok = false;
        return res;
    }

    // : copiar las "expectaciones" de @Macro capturadas
    // por el TypeChecker (al evaluar via AST) hacia la CompileResult.
    // El caller (main.cpp probe / shadow-eval mode) las reaplica
    // sobre una nueva ComptimeRuntime tras cargar el bytecode y
    // ejecuta shadow_validate.  Cero coste si no hay @Macros.
    const auto &ctr_ro = tc.comptime_runtime();
    const size_t n_exp = ctr_ro.expectation_count();
    res.macro_expectations.reserve(n_exp);
    for (size_t i = 0; i < n_exp; ++i) {
        auto v = ctr_ro.expectation_at(i);
        if (!v.macro_name || !v.args || !v.expected_str || !v.src_loc) {
            continue;
        }
        CompileResult::MacroExpectation e;
        e.macro_name = *v.macro_name;
        e.args = *v.args;
        e.expected_str = *v.expected_str;
        e.src_loc = *v.src_loc;
        res.macro_expectations.push_back(std::move(e));
    }

    // 2.4. (opcional) Volcar los valores comptime computados.  Estrictamente
    // gateado por @c dump_comptime_values (default false): si esta apagado,
    // NADA cambia respecto al flujo historico.  Solo LEEMOS las constantes
    // comptime top-level que el TypeChecker ya resolvio (sin tocar lowering
    // ni la logica de macros).  Lo consume el metodo LSP vesta/comptimeValues.
    if (opts.dump_comptime_values) {
        // Renderiza un ComptimeConst a (type_kind, value_str) legible.
        // Conservador: int -> decimal; string -> texto entre comillas;
        // array -> resumen "[n elementos]"; struct -> "{n campos}";
        // type -> nombre del tipo.
        for (const auto &kv : tc.comptime_const_values()) {
            const auto &name = kv.first;
            const auto &c = kv.second;
            CompileResult::ComptimeValueSnapshot snap;
            snap.name = name;
            snap.scope = ""; // top-level (global); best-effort.
            if (c.is_type) {
                snap.type_kind = "type";
                snap.value_str = type_to_string(c.type_val);
            } else if (c.is_str) {
                snap.type_kind = "string";
                snap.value_str = "\"" + c.str_value + "\"";
            } else if (c.is_array) {
                snap.type_kind = "array";
                snap.value_str =
                    "[" + std::to_string(c.array_vals.size()) + " elementos]";
            } else if (c.is_struct) {
                snap.type_kind = "struct";
                snap.value_str =
                    "{" + std::to_string(c.struct_fields.size()) + " campos}";
            } else {
                snap.type_kind = "int";
                snap.value_str = std::to_string(c.value);
            }
            res.comptime_values.push_back(std::move(snap));
        }
        // Ademas de las constantes top-level, volcamos las variables
        // locales que computaron los bloques `comptime { ... }` (arrays
        // y structs poblados por loops, etc).  El TypeChecker las captura
        // justo antes de salir de cada bloque.
        for (const auto &b : tc.comptime_block_snapshots()) {
            CompileResult::ComptimeValueSnapshot snap;
            snap.name = b.name;
            snap.scope = b.scope;
            snap.type_kind = b.type_kind;
            snap.value_str = b.value_str;
            res.comptime_values.push_back(std::move(snap));
        }
        // Y los valores que resolvieron los builtins de introspeccion
        // (sizeof<T>, alignof<T>, kind<T>, type_id<T>, typename<T>), con su
        // ubicacion para que el LSP los muestre por hover sobre la expresion.
        for (const auto &h : tc.comptime_builtin_hits()) {
            CompileResult::ComptimeValueSnapshot snap;
            snap.name = h.name;
            snap.scope = "";
            snap.type_kind = h.type_kind;
            snap.value_str = h.value_str;
            snap.loc = h.loc;
            // builtin_kind = el nombre antes del '<' (p.ej. "sizeof").
            const size_t lt = h.name.find('<');
            snap.builtin_kind =
                (lt != std::string::npos) ? h.name.substr(0, lt) : h.name;
            res.comptime_values.push_back(std::move(snap));
        }
    }

    // 2.5. (opcional) Diagrama Mermaid del AST post type-check.  Lo
    // generamos AHORA porque ya tenemos los result_type rellenos pero
    // antes de que el lowering altere el AST.  Util para ver la
    // estructura del programa Vesta (clases, herencia, anotaciones,
    // tipos resueltos) sin saturar con detalles de lowering.
    if (opts.dump_mermaid_ast) {
        res.mermaid_ast = mermaid_from_ast(*mod);
    }
    // Variante Graphviz del AST.  Se llena en paralelo si el flag
    // correspondiente esta activo.  Comparte el mismo punto de entrada
    // (post type-check, pre lowering) para garantizar paridad de info
    // con la version Mermaid.
    if (opts.dump_graphviz_ast) {
        res.graphviz_ast = graphviz_from_ast(*mod);
    }
    // Variante HTML interactiva del AST.  Reutiliza el generador
    // Graphviz internamente (paridad de info); produce un .html
    // autocontenido con pan/zoom + panel de detalle.
    if (opts.dump_html_ast) {
        res.html_ast = html_from_ast(*mod);
    }
    // Diagrama de tipos (classDiagram): clases/herencia/interfaces/structs/
    // enums/conceptos.  Vista de alto nivel de la POO, independiente del AST
    // detallado.  Cada formato se llena solo si su flag esta activo.
    if (opts.dump_mermaid_types) {
        res.mermaid_types = mermaid_types_from_ast(*mod);
    }
    if (opts.dump_graphviz_types) {
        res.graphviz_types = graphviz_types_from_ast(*mod);
    }
    if (opts.dump_html_types) {
        res.html_types =
            html_from_dot(graphviz_types_from_ast(*mod), "Tipos", "types");
    }

    // 3. Lowering: AST -> ir::IrModule.  Pasamos el TypeChecker para
    // que el lowering pueda consultar StructLayout (offsets/tamanos)
    // sin recalcularlos.
    ir::IrModule irmod;
    Lowering lo(*mod, tc, res.diagnostics);
    if (!opts.instrument_mode.empty() && opts.instrument_mode != "none") {
        lo.set_instrument_mode(opts.instrument_mode);
    }
    lo.set_native_poo(opts.native_poo); //  AOT.2.b: POO nativa (-m aot)
    lo.set_asm_target_bits(opts.asm_target_bits); // arch del inline-asm @Naked
    lo.set_aot_vec_width(opts.aot_vec_width); // ancho SIMD del target (--float-isa)
    lo.set_aot_auto_vec(opts.aot_auto_vec);   // --float-isa auto: chunk dual
    lo.set_emit_comptime_fns(opts.emit_comptime_fns); // solo-LSP: inspeccion
    // C-3: detectar @StringConcat / @StringEq ANTES del lowering.  A
    // diferencia de @AllocatorOverride (que reescribe IR post-lowering),
    // el override del string built-in debe afectar el lowering MISMO del
    // operador `+`/`==` (y de los builtins str_concat/str_equals), por lo
    // que se resuelve aqui y se pasa al Lowering via setter.
    for (auto &decl : mod->decls) {
        if (!decl || decl->kind != ast::NodeKind::FunctionDecl) continue;
        auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
        if (fd->is_string_concat_override) {
            if (!res.string_concat_override.empty()) {
                res.ok = false;
                res.diagnostics.error(
                    SourceLoc{opts.module_name, 0, 0},
                    "multiples @StringConcat: '" + res.string_concat_override +
                        "' y '" + fd->name + "'");
                return res;
            }
            res.string_concat_override = fd->name;
        }
        if (fd->is_string_eq_override) {
            if (!res.string_eq_override.empty()) {
                res.ok = false;
                res.diagnostics.error(
                    SourceLoc{opts.module_name, 0, 0},
                    "multiples @StringEq: '" + res.string_eq_override + "' y '" +
                        fd->name + "'");
                return res;
            }
            res.string_eq_override = fd->name;
        }
        // @SyncImpl: override de la primitiva de monitor de `synchronized`.
        // Debe resolverse ANTES del lowering (afecta al lowering MISMO del
        // bloque synchronized).  El ROL se selecciona por el nombre convenido
        // de la fn (ABI fijo): monitor_enter (adquiere) / monitor_exit
        // (libera).  Aceptamos el nombre exacto o cualquier sufijo tras un
        // separador de namespace '.' para permitir declararlas en un
        // namespace.  Firma esperada: void(<ptr> obj).
        if (fd->is_sync_impl) {
            const std::string &nm = fd->name;
            auto tail_is = [&](const std::string &suf) -> bool {
                if (nm.size() < suf.size()) return false;
                if (nm.compare(nm.size() - suf.size(), suf.size(), suf) != 0)
                    return false;
                if (nm.size() == suf.size()) return true;
                const size_t sep = nm.size() - suf.size() - 1;
                // Separador de namespace: '.' (fuente) o el mangling '__' que
                // aplica el aplanador de namespaces (namespace_flatten).  Tras
                // el flatten el nombre es 'ns__..__monitor_enter'.
                if (nm[sep] == '.') return true;
                if (nm[sep] == '_' && sep >= 1 && nm[sep - 1] == '_')
                    return true;
                return false;
            };
            if (tail_is("monitor_enter")) {
                if (!res.sync_enter_override.empty()) {
                    res.ok = false;
                    res.diagnostics.error(
                        SourceLoc{opts.module_name, 0, 0},
                        "multiples @SyncImpl monitor_enter: '" +
                            res.sync_enter_override + "' y '" + fd->name + "'");
                    return res;
                }
                res.sync_enter_override = fd->name;
            } else if (tail_is("monitor_exit")) {
                if (!res.sync_exit_override.empty()) {
                    res.ok = false;
                    res.diagnostics.error(
                        SourceLoc{opts.module_name, 0, 0},
                        "multiples @SyncImpl monitor_exit: '" +
                            res.sync_exit_override + "' y '" + fd->name + "'");
                    return res;
                }
                res.sync_exit_override = fd->name;
            } else {
                res.diagnostics.warning(
                    fd->loc,
                    "@SyncImpl en fn '" + fd->name +
                        "' cuyo nombre no termina en 'monitor_enter' ni "
                        "'monitor_exit'; la anotacion se ignora");
            }
        }
        // CPU dispatch Inc 4: @HelperOverride(<helper>).  Debe resolverse
        // ANTES del lowering porque afecta a la construccion de
        // __vx_memcpy_init (apunta el fp a la fn del usuario, saltando el
        // dispatch por cpuid).  El map escala a futuros helpers sin tocar el
        // schema; hoy solo "memcpy" es multi-versionado.
        if (!fd->helper_override_target.empty()) {
            const std::string &tgt = fd->helper_override_target;
            // CPU dispatch Inc 5a: helpers multi-versionados soportados.
            const bool is_multiversioned =
                (tgt == "memcpy" || tgt == "strcmp" || tgt == "strlen");
            if (!is_multiversioned) {
                // No-fatal: el helper objetivo no es (todavia) multi-
                // versionado.  Avisamos pero seguimos (el resto compila).
                res.diagnostics.warning(
                    fd->loc,
                    "@HelperOverride: helper '" + tgt +
                        "' no es multi-versionado (solo 'memcpy', 'strcmp', "
                        "'strlen' por ahora); la anotacion se ignora");
            } else {
                if (res.aot_helper_override_syms.count(tgt)) {
                    res.ok = false;
                    res.diagnostics.error(
                        fd->loc,
                        "multiples @HelperOverride(" + tgt + "): '" +
                            res.aot_helper_override_syms[tgt] + "' y '" +
                            fd->name + "'");
                    return res;
                }
                // Validacion de firma compatible por helper.  No es fatal (el
                // usuario manda), pero avisamos si no cuadra:
                //   memcpy -> void(u8*, u8*, u64)
                //   strcmp -> i64(u8*, i64, u8*, i64)
                //   strlen -> i64(u8*)
                bool ret_void =
                    !fd->return_type ||
                    (fd->return_type->kind ==
                         ast::NodeKind::PrimitiveTypeNode &&
                     static_cast<ast::PrimitiveTypeNode *>(
                         fd->return_type.get())
                             ->prim == PrimitiveKind::VOID);
                bool sig_ok = true;
                std::string expected;
                if (tgt == "memcpy") {
                    sig_ok = (fd->params.size() == 3 && ret_void);
                    expected = "void(u8*, u8*, u64)";
                } else if (tgt == "strcmp") {
                    sig_ok = (fd->params.size() == 4 && !ret_void);
                    expected = "i64(u8*, i64, u8*, i64)";
                } else { // strlen
                    sig_ok = (fd->params.size() == 1 && !ret_void);
                    expected = "i64(u8*)";
                }
                if (!sig_ok) {
                    res.diagnostics.warning(
                        fd->loc,
                        "@HelperOverride(" + tgt + "): firma esperada " +
                            expected +
                            "; la del override puede ser incompatible");
                }
                res.aot_helper_override_syms[tgt] = fd->name;
            }
        }
    }
    lo.set_string_op_overrides(res.string_concat_override,
                               res.string_eq_override);
    // @SyncImpl: exigir el PAR completo (enter + exit) o ninguno.  Un
    // override a medias dejaria el monitor sin liberar (o sin adquirir).
    if (res.sync_enter_override.empty() != res.sync_exit_override.empty()) {
        res.ok = false;
        res.diagnostics.error(
            SourceLoc{opts.module_name, 0, 0},
            std::string("@SyncImpl incompleto: se requiere el par "
                        "monitor_enter + monitor_exit (falta '") +
                (res.sync_enter_override.empty() ? "monitor_enter"
                                                 : "monitor_exit") +
                "')");
        return res;
    }
    lo.set_sync_impl_overrides(res.sync_enter_override, res.sync_exit_override);
    // CPU dispatch Inc 4: pasar el override de "memcpy" (si lo hay) al
    // lowering para que __vx_memcpy_init apunte el fp a la fn del usuario.
    {
        auto it = res.aot_helper_override_syms.find("memcpy");
        if (it != res.aot_helper_override_syms.end())
            lo.set_memcpy_override(it->second);
    }
    // CPU dispatch Inc 5a: idem para strcmp / strlen (el __vx_strdisp_init
    // apunta cada fp a la fn del usuario en lugar del baseline).
    {
        auto it = res.aot_helper_override_syms.find("strcmp");
        if (it != res.aot_helper_override_syms.end())
            lo.set_strcmp_override(it->second);
    }
    {
        auto it = res.aot_helper_override_syms.find("strlen");
        if (it != res.aot_helper_override_syms.end())
            lo.set_strlen_override(it->second);
    }
    const std::string mod_name =
        opts.module_name.empty() ? std::string("main") : opts.module_name;
    if (!lo.run(irmod, mod_name)) {
        res.ok = false;
        return res;
    }

    // Grafo de conocimiento del programa: los tipos, sus miembros y como se
    // relacionan, mas el mapa que liga los simbolos del artefacto con ellos.
    // Se emite AQUI y no antes porque los SIMBOLOS solo existen tras el
    // lowering, y sin ellos el grafo se queda sin puerta de entrada: una
    // direccion de ejecucion no podria llegar hasta el.
    //
    // No participa en la generacion de codigo: si falla, se avisa y la
    // compilacion sigue -- perder informacion de depuracion no es motivo para
    // no producir el programa.
    {
        VxdbgEmitStats st;
        std::string dbg_err;
        if (!emit_vxdbg_source(tc, lo.emitted_symbols(), filename, source,
                               opts.vxdbg_dir, st, dbg_err)) {
            std::cerr << "[vxdbg] no se pudo emitir: " << dbg_err << "\n";
        }
        res.vxdbg_artifact_map = st.artifact_map;
    }

    // -ffp-contract=off (CLI, per-modulo): fuerza IEEE estricto (sin contraccion
    // FMA) AND-eando la politica del modulo con el fp_contract por-funcion que ya
    // puso el lowering (@fp(strict) -> false).  Se aplica aqui, en la misma unidad
    // de traduccion que el optimizer/emitter que consumen irmod, para no depender
    // del global mutable ir_set_fma_contract_allowed (se duplica entre vm.exe, el
    // DLL y vmcore -> el setter de main.cpp puede tocar una copia distinta).
    if (!opts.fp_contract) {
        for (auto &fn : irmod.functions)
            fn.fp_contract = false;
    }

    // FN.3: auto-bundle del context-switch de fibra para el JIT.
    // En el path interp/JIT (native_poo == false), `fiber_swapctx` baja al
    // opcode VM SWAPCTX; el interp lo ejecuta directamente, pero el JIT emite
    // un CALL nativo al primitivo @Naked `__vx_swapctx` (context-switch host)
    // y `__fiber_trampoline` (arranque de fibra).  Esas dos funciones viven en
    // stdlib/vx/vx_fiber.vx y el opcode NO las referencia -> no estarian en
    // el .velb del usuario.  Cuando detectamos uso de SWAPCTX, compilamos
    // vx_fiber.vx (interp/JIT) y fusionamos SOLO esas dos funciones @Naked en
    // el modulo, para que su IR llegue a la seccion @ir del .velb y el JIT las
    // materialice (find_exe_with_fn -> compile_native_fn).  Son asm puro (sin
    // static_data / globals / native_imports), asi que basta con las funciones.
    // El path AOT (native_poo) tiene su propio auto-bundle en main.cpp; alli
    // fiber_swapctx baja a CALL __vx_swapctx (no al opcode) -> no entra aqui.
    // Recursion imposible: vx_fiber.vx DEFINE __vx_swapctx pero NO usa el
    // opcode SWAPCTX (usa fiber_switch/asm), asi que no re-dispara el bundle.
    if (!opts.native_poo) {
        bool uses_swapctx = false, defines_swapctx = false;
        for (const auto &f : irmod.functions) {
            if (f.name == "__vx_swapctx") defines_swapctx = true;
            for (const auto &b : f.blocks)
                for (const auto &ins : b.instrs)
                    if (ins.op == ir::IrOp::SWAPCTX) uses_swapctx = true;
        }
        if (uses_swapctx && !defines_swapctx) {
            std::vector<std::string> cands = {"stdlib/vx/vx_fiber.vx",
                                              "../stdlib/vx/vx_fiber.vx",
                                              "../../stdlib/vx/vx_fiber.vx"};
            const std::string exe = fs::get_executable_path();
            if (!exe.empty()) {
                std::filesystem::path ed =
                    std::filesystem::path(exe).parent_path();
                cands.push_back(
                    (ed / "stdlib" / "vx" / "vx_fiber.vx").string());
                cands.push_back(
                    (ed.parent_path() / "stdlib" / "vx" / "vx_fiber.vx")
                        .string());
            }
            std::string vf_path;
            for (const auto &c : cands)
                if (std::filesystem::exists(c)) {
                    vf_path = c;
                    break;
                }
            if (vf_path.empty()) {
                res.diagnostics.warning(
                    SourceLoc{mod_name, 0, 0},
                    "fiber_swapctx: no encuentro stdlib/vx/vx_fiber.vx; el "
                    "context-switch de fibra no estara disponible en JIT");
            } else {
                std::ifstream vff(vf_path);
                std::string vf_src((std::istreambuf_iterator<char>(vff)),
                                   std::istreambuf_iterator<char>());
                CompileOptions vf_opts;
                vf_opts.module_name = "vx_fiber";
                vf_opts.opt_level = 2;
                vf_opts.native_poo = false;
                vf_opts.asm_target_bits = opts.asm_target_bits;
                CompileResult vf_cr =
                    compile_vx_source(vf_src, vf_path, vf_opts);
                ir::IrModule vf_mod;
                if (vf_cr.ok && !vf_cr.ir_module_cache_bytes.empty() &&
                    ir::parse_ir_module_cache(vf_cr.ir_module_cache_bytes,
                                              vf_mod)) {
                    std::unordered_set<std::string> have;
                    for (const auto &f : irmod.functions)
                        have.insert(f.name);
                    for (auto &fn : vf_mod.functions) {
                        if (fn.name != "__vx_swapctx" &&
                            fn.name != "__fiber_trampoline")
                            continue;
                        if (have.count(fn.name)) continue;
                        irmod.functions.push_back(std::move(fn));
                    }
                    // Las funciones @Naked emiten un CALLN a `vrt:inline_asm_exec`
                    // (ejecutor de inline-asm del interp) en su bytecode; el
                    // linker necesita el native_import para resolverlo aunque en
                    // interp ese bytecode nunca se ejecute (la fibra corre por el
                    // opcode SWAPCTX / el JIT las materializa nativas).
                    for (const auto &ni : vf_mod.native_imports)
                        irmod.register_native_import(ni.lib, ni.name);
                }
            }
        }
    }

    // AOT.2.d: detectar @AllocatorOverride / @PanicHandler.  El allocador
    // que devuelve un puntero -> alloc_sym; el que devuelve void -> free_sym.
    for (auto &decl : mod->decls) {
        if (!decl || decl->kind != ast::NodeKind::FunctionDecl) continue;
        auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
        if (fd->is_panic_handler) res.aot_panic_sym = fd->name;
        if (fd->is_alloc_override) {
            bool ret_ptr = false;
            if (fd->return_type &&
                fd->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
                const auto pk =
                    static_cast<ast::PrimitiveTypeNode *>(fd->return_type.get())
                        ->prim;
                ret_ptr = (pk == PrimitiveKind::PTR);
            } else if (fd->return_type && fd->return_type->kind ==
                                              ast::NodeKind::PointerTypeNode) {
                ret_ptr = true;
            }
            if (ret_ptr)
                res.aot_alloc_sym = fd->name;
            else
                res.aot_free_sym = fd->name;
        }
    }

    /* : set @c has_lowerable_macros si el lowering emitio
     * al menos una IrFunction marcada @c is_macro_compiled.  Esto
     * es un gate mas robusto que @c macro_expectations.empty() para
     * disparar el two- compile, porque incluye casos con args
     * no codificables (string, struct, array) que no aparecerian
     * como expectations pero si pueden beneficiarse del VM path. */
    for (const auto &fn : irmod.functions) {
        if (fn.is_macro_compiled) {
            res.has_lowerable_macros = true;
            break;
        }
    }

    /* +: copiar las razones de skip del lowering a la
     * CompileResult para que main.cpp las imprima via VESTA_VERBOSE. */
    res.macro_skip_reasons = lo.macro_skip_reasons();

    // 3.5. (opcional) Volcar el IR pre-optimizacion al campo
    // @c res.ir_text para que el caller pueda inspeccionarlo con la
    // flag @c --vx-emit-ir / @c CompileOptions::dump_ir.  Util para
    // verificar que el frontend produce SSA correcto (PHI insertado
    // tras if/else, CALLCLOSURE con func_ptr+env, etc.) antes de
    // que el optimizador y el regalloc transformen el codigo.
    //
    // Para mostrar tambien el IR DESPUES de optimizar, hacemos una
    // copia del IrModule, le aplicamos las pasadas con el opt_level
    // configurado, y volcamos esa copia.  Asi NO afectamos a irmod
    // (que el emitter optimizara internamente otra vez con el mismo
    // opt_level; idempotente).  Esto es una herramienta de debug,
    // no se ejecuta cuando @c dump_ir es false (caso comun en builds
    // de produccion).
    // 3.5. (opcional) Diagrama Mermaid del IR pre-optimizacion.  Lo
    // generamos ANTES de tocar irmod para capturar el output crudo
    // del lowering (todos los PHIs, todos los CONSTs, blocks como
    // los emite el frontend, sin las transformaciones del optimizer).
    //
    // El coste Big-O (parcial + total) se anota SIEMPRE en los diagramas IR:
    // se calcula sobre el IR PRE-opt (complejidad algoritmica del fuente) una
    // vez y se reusa para los tres formatos de la vista pre.
    const bool want_cost_pre =
        (opts.dump_mermaid_ir_pre || opts.dump_graphviz_ir_pre ||
         opts.dump_html_ir_pre);
    analyze::ModuleCost mc_pre;
    if (want_cost_pre) {
        mc_pre = analyze::analyze_module(irmod);
        analyze::compose_interproc(mc_pre);
    }
    const analyze::ModuleCost *cost_pre = want_cost_pre ? &mc_pre : nullptr;
    if (opts.dump_mermaid_ir_pre) {
        res.mermaid_ir_pre = mermaid_from_ir_module(
            irmod, "Diagrama IR pre-optimizacion (frontend output)", cost_pre);
    }
    if (opts.dump_graphviz_ir_pre) {
        res.graphviz_ir_pre = graphviz_from_ir_module(
            irmod, "Diagrama IR pre-optimizacion (frontend output)", cost_pre);
    }
    if (opts.dump_html_ir_pre) {
        res.html_ir_pre = html_from_ir_module(
            irmod, "SSA IR pre-optimizacion (frontend output)", cost_pre);
    }
    // Si CUALQUIERA de las opciones pide informacion post-opt
    // (ir_text dump, mermaid_post o graphviz_post, port target),
    // generamos UNA copia optimizada y la reusamos para todos los
    // outputs.  Asi evitamos optimizar 2-3 veces el mismo IrModule
    // cuando el usuario pide multiples formatos.
    const bool need_post_opt = opts.dump_ir || opts.dump_mermaid_ir_post ||
                               opts.dump_graphviz_ir_post ||
                               opts.dump_html_ir_post ||
                               !opts.port_target.empty();
    if (need_post_opt) {
        ir::IrModule irmod_opt = irmod;
        ir::ir_optimize(irmod_opt, opt_level_from_int(opts.opt_level));
        if (opts.dump_ir) {
            std::ostringstream ir_oss;
            ir_oss << "// ============================================\n";
            ir_oss << "// SSA IR pre-optimizacion (frontend output)\n";
            ir_oss << "// ============================================\n";
            ir::ir_print(irmod, ir_oss);
            ir_oss << "\n// ============================================\n";
            ir_oss << "// SSA IR post-optimizacion (opt_level="
                   << opts.opt_level << ")\n";
            ir_oss << "// ============================================\n";
            ir::ir_print(irmod_opt, ir_oss);
            res.ir_text = ir_oss.str();
        }
        const std::string title = "Diagrama IR post-optimizacion (opt_level=" +
                                  std::to_string(opts.opt_level) + ")";
        // Coste Big-O SIEMPRE sobre el IR POST-opt (complejidad efectiva del
        // codigo final) calculado una vez para los tres formatos.
        const bool want_cost_post =
            (opts.dump_mermaid_ir_post || opts.dump_graphviz_ir_post ||
             opts.dump_html_ir_post);
        analyze::ModuleCost mc_post;
        if (want_cost_post) {
            mc_post = analyze::analyze_module(irmod_opt);
            analyze::compose_interproc(mc_post);
        }
        const analyze::ModuleCost *cost_post =
            want_cost_post ? &mc_post : nullptr;
        if (opts.dump_mermaid_ir_post) {
            res.mermaid_ir_post =
                mermaid_from_ir_module(irmod_opt, title, cost_post);
        }
        if (opts.dump_graphviz_ir_post) {
            res.graphviz_ir_post =
                graphviz_from_ir_module(irmod_opt, title, cost_post);
        }
        if (opts.dump_html_ir_post) {
            res.html_ir_post = html_from_ir_module(irmod_opt, title, cost_post);
        }

        // Fase 4 interop C: header C publico (structs C-compat + prototipos
        // de funciones C-representables).  Se genera del frontend (AST + tc),
        // no del IR: es la API Vesta tal cual la ve un programador C.
        if (opts.emit_header) {
            res.header_text = vx::generate_c_header(*mod, tc, mod_name);
        }

        // Port transpiler: convierte el IR optimizado a codigo fuente
        // del lenguaje destino (C, futuro: Java, JS, etc.).  Se ejecuta
        // ANTES de emitir el .vel para que el port reciba el mismo IR
        // que el bytecode (con opt_level aplicado).
        if (!opts.port_target.empty()) {
            if (opts.port_target == "c") {
                port::PortOptions popts = opts.port_options;
                // Si el caller no lleno module_name, usar el del compiler.
                if (popts.module_name.empty()) {
                    popts.module_name = mod_name;
                }
                if (popts.source_path.empty()) {
                    popts.source_path = filename;
                }
                port::CBackend backend(popts);
                port::Transpiler tx(irmod_opt, popts, backend);
                port::TranspileResult ptres = tx.run();
                if (ptres.ok) {
                    res.port_text = std::move(ptres.source_text);
                    res.port_warnings = std::move(ptres.warnings);
                } else {
                    SourceLoc loc;
                    loc.file = filename;
                    for (const auto &e : ptres.errors) {
                        res.diagnostics.error(SourceLoc{filename, 0, 0},
                                              std::string("port-c: ") + e);
                    }
                }
            } else {
                SourceLoc loc;
                loc.file = filename;
                res.diagnostics.error(
                    std::move(loc), std::string("port: target '") +
                                        opts.port_target +
                                        "' no soportado.  Targets validos: c");
            }
        }
    }

    //  AS (AS.7): el backend bytecode/interp NO puede materializar
    // inline asm de la CPU host (no existe opcode VM para rdtsc/cpuid/
    // syscall/etc.).  Si el target es bytecode (port_target vacio) y el
    // IR contiene algun IrOp::INLINE_ASM, abortamos con un error claro
    // ANTES de emitir el .vel.  Los backends nativos (port-C hoy; JIT/AOT
    // en el futuro) interceptan INLINE_ASM antes de llegar aqui.
    //  AS inc.5: el inline-asm (IrOp::INLINE_ASM) ya NO se rechaza al
    // emitir bytecode.  El .velb se emite con las funciones inline-asm (su
    // cuerpo bytecode es un trap que el ir_emitter emite, ver mas abajo) y
    // su IR completo (con asm_reg_bindings) viaja en la seccion @ir.  El
    // loader las eager-compila a codigo nativo (JIT activo por defecto,
    // threshold 1500); el cuerpo bytecode-trap NUNCA se ejecuta bajo JIT.
    // Sin flags: `vm --vx prog.vx -o prog && vm --run prog.velb`.
    //
    //  AS inc.5g: PERO si el inline-asm liga un registro VECTORIAL
    // (register("xmm0"/"ymm0"/"zmm0")), el JIT v1 no lo soporta (el regalloc
    // solo asigna el banco GP) -> en lugar de fallar SILENCIOSAMENTE en
    // runtime (el wrapper no compila -> trap hlt), abortamos AQUI con un
    // error claro + sugerencia.  (En --port c si funciona: GCC lo maneja.)
    if (opts.port_target.empty()) {
        for (const auto &fn : irmod.functions) {
            for (const auto &b : fn.asm_reg_bindings) {
                if (b.is_vector) {
                    res.diagnostics.error(
                        SourceLoc{filename, 0, 0},
                        "inline asm: register(\"" + b.reg +
                            "\") liga un "
                            "registro vectorial (xmm/ymm/zmm), no soportado en "
                            "el backend JIT todavia.  Usa el patron de memoria "
                            "(puntero GP + registro vectorial interno como "
                            "scratch; ver "
                            "examples_codes_vx/asm/06_sse2_paddd) "
                            "o compila con --port c.");
                }
            }
        }
        if (res.diagnostics.has_errors()) return res;
    }

    // 4. Emitir IR -> texto .vel.  Aqui es donde el optimizador IR
    // hace DCE / copy prop / etc segun opt_level y el regalloc lineal
    // asigna r0..r15 a los IrValue.
    ir::EmitOptions emit_opts;
    emit_opts.opt_level = opt_level_from_int(opts.opt_level);
    emit_opts.emit_comments = true;
    emit_opts.emit_debug = opts.emit_debug;
    // emit_opts.emit_stackmaps queda en su default (true): los stackmaps
    // precisos de raices GC se embeben SIEMPRE en el .velb.
    emit_opts.module_name = mod_name;

    /* serializar el IR OPTIMIZADO a bytes para que el JIT compile la
     * version post-optimizacion (incluyendo dead-alloc elim, DCE,
     * etc.) en lugar de la version frontend cruda.
     *
     * D.7.opt: corregido bug donde el .velb llevaba IR
     * UNOPTIMIZED.  El JIT auto-trigger compilaba @c pruebas con la
     * call a @c __new_Calculadora aunque el optimizer ya la habia
     * eliminado a nivel .vel.
     *
     * Genera @c irmod_opt aplicando el mismo opt_level que el .vel
     * pipeline para mantener consistencia entre @c .velb (JIT
     * source) y @c .vel (interp source). */
    {
        /* Modo --analyze: serializar el IR PRE-optimizacion (irmod tal cual
         * lo emite el lowering) antes de tocar nada.  Captura la complejidad
         * algoritmica del fuente; el analyzer la contrasta con la POST-opt. */
        if (opts.emit_ir_preopt) {
            // Plegar las ramas comptime-constantes (const fold + unreachable,
            // SIN inline): `is_float<T>()` es una CONSTANTE para cada
            // instanciacion, asi que la rama muerta del template no es parte
            // del cuerpo de esa instanciacion.  Resolucion de la
            // monomorphizacion, no optimizacion.  (Igual que la ruta de
            // proyecto.)
            ir::IrModule irmod_pre = irmod;
            for (auto &fn : irmod_pre.functions) {
                bool changed = true;
                while (changed) {
                    changed = false;
                    if (ir::ir_pass_const_fold(fn)) changed = true;
                    if (ir::ir_pass_unreachable(fn)) changed = true;
                }
            }
            res.ir_module_cache_bytes_preopt =
                ir::emit_ir_module_cache(irmod_pre);
        }
        // Contratos de huella (@pure/@nothrow/@nopanic/@alloc/@stack): recoger
        // del AST + guardarlos en el resultado (para --analyze) + VERIFICAR
        // contra la huella del IR PRE-opt (@c irmod, donde TODAS las funciones
        // existen -> enforcement completo; semantica source-level: source<=N =>
        // efectivo<=N, sound).  Sound/asimetrico: solo error si es demostrable.
        collect_contracts_(mod->decls, res.contracts);
        if (!res.contracts.empty()) {
            // Arch del TARGET activo (@Target/AOT cross-compile); vacio = host
            // de build (x86_64).  Selecciona la tabla de efectos con la que se
            // analiza cada bloque `asm { }` (x86_64/x86/arm64).
            std::string fp_os, fp_arch;
            vx::get_aot_condcomp_target(fp_os, fp_arch);
            if (fp_arch.empty()) fp_arch = "x86_64";
            auto fps = analyze::compute_module_fingerprints(irmod, fp_arch);
            analyze::compose_fingerprints(fps, &res.contracts);
            // En --analyze (`emit_ir_preopt`) NO se emite el error ni se aborta
            // (ver la nota en compiler_project.cpp): analyze mide, el build real
            // enforça.
            if (!opts.emit_ir_preopt) {
                auto checks = analyze::verify_contracts(fps, res.contracts);
                bool violated = false;
                for (const auto &ck : checks) {
                    if (ck.status != analyze::ContractCheck::VIOLATED) continue;
                    SourceLoc loc;
                    loc.file = filename;
                    res.diagnostics.error(std::move(loc),
                                          "contrato " + ck.contract +
                                              " incumplido en '" + ck.function +
                                              "': " + ck.detail);
                    violated = true;
                }
                if (violated) {
                    res.ok = false;
                    return res;
                }
            }
        }
        // Contratos de TIPO (@pod/@no_heap/@size): recoger + computar la huella
        // de los tipos (desde los layouts del type checker) + verificar.  La
        // huella se calcula SIEMPRE (para el reporte de --analyze); los checks
        // solo si hay contratos.  Decidibles del layout -> un VIOLATED es error.
        collect_type_contracts_(mod->decls, res.type_contracts);
        res.type_fingerprints = compute_type_fingerprints_(tc);
        if (!res.type_contracts.empty()) {
            auto tchecks = analyze::verify_type_contracts(res.type_fingerprints,
                                                          res.type_contracts);
            bool tviolated = false;
            for (const auto &ck : tchecks) {
                if (ck.status != analyze::ContractCheck::VIOLATED) continue;
                SourceLoc loc;
                loc.file = filename;
                res.diagnostics.error(std::move(loc),
                                      "contrato de tipo " + ck.contract +
                                          " incumplido en '" + ck.function +
                                          "': " + ck.detail);
                tviolated = true;
            }
            if (tviolated) {
                res.ok = false;
                return res;
            }
        }
        ir::IrModule irmod_for_section = irmod;
        // En modo --analyze (emit_ir_preopt) se optimiza SIN inline: el coste
        // PARCIAL es propiedad del cuerpo escrito, no del optimizador.  El
        // coste TOTAL lo compone el analizador via el callgraph.  Fuera de
        // --analyze, inline normal (no se genera .velb en --analyze).
        ir::ir_optimize(irmod_for_section, opt_level_from_int(opts.opt_level),
                        /*allow_inline=*/!opts.emit_ir_preopt);
        res.ir_section_bytes = ir::emit_ir_section(irmod_for_section.functions);
        /*  AOT: modulo completo (functions + static_data + globals) para
         * que el driver -m aot materialice los literales en .rodata. */
        res.ir_module_cache_bytes = ir::emit_ir_module_cache(irmod_for_section);
    }

    // --- CTPE (on por defecto; VESTA_NO_CTPE desactiva): precomputo del
    //     programa completo. ---
    // Si el modulo tiene candidatos (fn evaluable zero-param con retorno escalar,
    // p.ej. un `main` puro), se construye un ComptimeRuntime a partir del .velb
    // del modulo y el emisor pliega el resultado como CONST.  Es un dos-fases
    // AUTOCONTENIDO: emit sin plegar -> ensamblar a .velb temporal -> cargar el
    // runtime -> re-emitir con el runtime activo (el fold vive dentro del emisor).
    // Solo actua si hay candidato (main puro): el 99% de programas con I/O no lo
    // son -> sin coste.  Watchdog de 3s (VESTA_CTPE_MS) evita colgar el compile.
    std::unique_ptr<vx::ComptimeRuntime> ctpe_rt;
    // No en modulos con @Macro: el precomputo ya lo hace la maquinaria de macros
    // (comptime) y su two-phase (VESTA_MC_PREBUILT) choca con el two-phase de
    // CTPE.  Ademas los macros dejan un runtime comptime propio que conflictua.
    if (!std::getenv("VESTA_NO_CTPE") && opts.opt_level >= 2 &&
        !res.ir_section_bytes.empty() && !res.has_lowerable_macros) {
        ctpe::Evaluability ev = ctpe::compute_evaluability(irmod);
        std::vector<ctpe::Candidate> cands = ctpe::find_candidates(irmod, ev);
        res.has_ctpe_candidates = !cands.empty();
        if (res.has_ctpe_candidates) {
            // 1) Emit UNFOLDED -> temp .vel -> temp .velb (ensamblado).
            ir::EmitResult e1 = ir::ir_emit_module(irmod, emit_opts);
            if (e1.ok) {
                std::error_code ec;
                std::filesystem::create_directories(".cache/ctpe/tmp", ec);
                std::string base =
                    ".cache/ctpe/tmp/ctpe_" +
                    std::to_string(std::hash<std::string>{}(e1.vel_text));
                std::string tvel = base + ".vel";
                {
                    std::ofstream o(tvel, std::ios::binary);
                    o << e1.vel_text;
                }
                // Ensamblar el .vel a un .velb con su seccion @ir (para que el
                // runtime pueda compilar main en JIT y ejecutarlo).
                int rc = asm_multi_process::run_worker(
                    tvel, base, /*skip_preprocessor=*/true,
                    /*keep_labels=*/false, &res.ir_section_bytes,
                    /*emit_map=*/false);
                if (rc == 0) {
                    std::ifstream vf(base + ".velb", std::ios::binary);
                    std::vector<uint8_t> velb(
                        (std::istreambuf_iterator<char>(vf)),
                        std::istreambuf_iterator<char>());
                    if (!velb.empty()) {
                        // 2) Cargar el runtime; el re-emit de abajo pliega.  El
                        // handler de safepoint se activa AQUI (no en
                        // try_invoke_ctpe) porque main se JIT-compila durante
                        // load_macros_from_bytes -- antes de invocarlo.  Asi su
                        // codigo lleva los polls del watchdog.  Reset tras el emit.
                        jit::jit_set_ctpe_safepoint(jit::jit_safepoint_handler_addr());
                        ctpe_rt = std::make_unique<vx::ComptimeRuntime>();
                        if (ctpe_rt->load_macros_from_bytes(std::move(velb)))
                            emit_opts.ctpe_runtime = ctpe_rt.get();
                    }
                }
            }
        }
    }

    ir::EmitResult eres = ir::ir_emit_module(irmod, emit_opts);
    // Fin del modo CTPE: apagar los polls de safepoint para no afectar a los
    // compiles del JIT en runtime ni a los @Macro del lenguaje.
    jit::jit_set_ctpe_safepoint(0);
    if (!eres.ok) {
        // Volcar el error del emisor al sumidero unificado.
        SourceLoc loc;
        loc.file = filename;
        res.diagnostics.error(std::move(loc),
                              std::string("emisor IR fallo: ") + eres.error);
        res.ok = false;
        return res;
    }

    res.vel_text = std::move(eres.vel_text);

    // 5. (opcional) Diagrama Mermaid del bytecode .vel final.  Independiente
    // de las opciones IR/AST: parsea el texto del .vel para detectar labels
    // (bloques) y saltos.  Util para ver el output ultimo del frontend
    // antes de pasar al ensamblador, incluyendo opt como cmpjmp.cc, decjnz,
    // gcallocp que no aparecen en el IR (son fusion del emisor).
    if (opts.dump_mermaid_vel) {
        res.mermaid_vel = mermaid_from_vel_text(res.vel_text);
    }
    if (opts.dump_graphviz_vel) {
        res.graphviz_vel = graphviz_from_vel_text(res.vel_text);
    }
    if (opts.dump_html_vel) {
        res.html_vel = html_from_vel_text(res.vel_text);
    }

    res.ok = !res.diagnostics.has_errors();
    return res;
}

} // namespace vx
