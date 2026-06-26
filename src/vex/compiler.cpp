/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file compiler.cpp
 * @brief Implementacion del facade del compilador Vex.
 */

#include "vex/compiler.h"

#include "analyze/bigo.h"
#include "ir/ir_emitter.h"
#include "ir/ir_optimizer.h"
#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include "vex/lexer.h"
#include "vex/lowering.h"
#include "vex/mermaid_diagrams.h"
#include "vex/graphviz_diagrams.h"
#include "vex/html_diagrams.h"
#include "vex/namespace_flatten.h"
#include "vex/parser.h"
#include "vex/type_checker.h"

#include "port/transpiler_base.h"
#include "port/c/c_backend.h"

#include <sstream>
#include <utility>

namespace vex {

/**
 * @brief Convierte un entero 0..3 al enum ir::OptLevel.
 *
 * Cualquier valor fuera de rango cae en O1 (default conservador).
 */
static ir::OptLevel opt_level_from_int(int n) noexcept {
    switch (n) {
    case 0: return ir::OptLevel::O0;
    case 1: return ir::OptLevel::O1;
    case 2: return ir::OptLevel::O2;
    case 3: return ir::OptLevel::O3;
    default: return ir::OptLevel::O1;
    }
}

CompileResult compile_vex_source(const std::string &source,
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

    // 1.5. Phase M.7.c: aplanar namespaces inline (`namespace ui { ... }`).
    //      Tras este pre-pass, el AST no tiene NamespaceDecl wrappers;
    //      los simbolos internos llevan prefix `<ns>__` (cero overhead
    //      runtime; compatible con port-c).  Cada namespace encontrado
    //      se registra en el TypeChecker como Symbol::Namespace para
    //      que el resolver de `ns.X` lo encuentre.
    auto inline_namespaces = flatten_namespaces(*mod);

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
    // estructura del programa Vex (clases, herencia, anotaciones,
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

    // 3. Lowering: AST -> ir::IrModule.  Pasamos el TypeChecker para
    // que el lowering pueda consultar StructLayout (offsets/tamanos)
    // sin recalcularlos.
    ir::IrModule irmod;
    Lowering lo(*mod, tc, res.diagnostics);
    if (!opts.instrument_mode.empty() && opts.instrument_mode != "none") {
        lo.set_instrument_mode(opts.instrument_mode);
    }
    lo.set_native_poo(opts.native_poo); // Phase AOT.2.b: POO nativa (-m aot)
    lo.set_asm_target_bits(opts.asm_target_bits); // arch del inline-asm @Naked
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
        // CPU dispatch Inc 4: @HelperOverride(<helper>).  Debe resolverse
        // ANTES del lowering porque afecta a la construccion de
        // __vex_memcpy_init (apunta el fp a la fn del usuario, saltando el
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
    // CPU dispatch Inc 4: pasar el override de "memcpy" (si lo hay) al
    // lowering para que __vex_memcpy_init apunte el fp a la fn del usuario.
    {
        auto it = res.aot_helper_override_syms.find("memcpy");
        if (it != res.aot_helper_override_syms.end())
            lo.set_memcpy_override(it->second);
    }
    // CPU dispatch Inc 5a: idem para strcmp / strlen (el __vex_strdisp_init
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
     * disparar el two-phase compile, porque incluye casos con args
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
    // flag @c --vex-emit-ir / @c CompileOptions::dump_ir.  Util para
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
    // --diagram-cost: si se pidio anotar el coste, calcular el analisis
    // Big-O sobre el IR PRE-opt (complejidad algoritmica del fuente) una
    // vez y reusarlo para los tres formatos de la vista pre.
    const bool want_cost_pre =
        opts.annotate_cost && (opts.dump_mermaid_ir_pre ||
                               opts.dump_graphviz_ir_pre || opts.dump_html_ir_pre);
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
        // --diagram-cost: analisis Big-O sobre el IR POST-opt (complejidad
        // efectiva del codigo final) calculado una vez para los tres formatos.
        const bool want_cost_post =
            opts.annotate_cost &&
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

    // Phase AS (AS.7): el backend bytecode/interp NO puede materializar
    // inline asm de la CPU host (no existe opcode VM para rdtsc/cpuid/
    // syscall/etc.).  Si el target es bytecode (port_target vacio) y el
    // IR contiene algun IrOp::INLINE_ASM, abortamos con un error claro
    // ANTES de emitir el .vel.  Los backends nativos (port-C hoy; JIT/AOT
    // en el futuro) interceptan INLINE_ASM antes de llegar aqui.
    // Phase AS inc.5: el inline-asm (IrOp::INLINE_ASM) ya NO se rechaza al
    // emitir bytecode.  El .velb se emite con las funciones inline-asm (su
    // cuerpo bytecode es un trap que el ir_emitter emite, ver mas abajo) y
    // su IR completo (con asm_reg_bindings) viaja en la seccion @ir.  El
    // loader las eager-compila a codigo nativo (JIT activo por defecto,
    // threshold 1500); el cuerpo bytecode-trap NUNCA se ejecuta bajo JIT.
    // Sin flags: `vm --vex prog.vex -o prog && vm --run prog.velb`.
    //
    // Phase AS inc.5g: PERO si el inline-asm liga un registro VECTORIAL
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
                            "examples_codes_vex/asm/06_sse2_paddd) "
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
            res.ir_module_cache_bytes_preopt =
                ir::emit_ir_module_cache(irmod);
        }
        ir::IrModule irmod_for_section = irmod;
        ir::ir_optimize(irmod_for_section, opt_level_from_int(opts.opt_level));
        res.ir_section_bytes = ir::emit_ir_section(irmod_for_section.functions);
        /* Phase AOT: modulo completo (functions + static_data + globals) para
         * que el driver -m aot materialice los literales en .rodata. */
        res.ir_module_cache_bytes = ir::emit_ir_module_cache(irmod_for_section);
    }

    ir::EmitResult eres = ir::ir_emit_module(irmod, emit_opts);
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

} // namespace vex
