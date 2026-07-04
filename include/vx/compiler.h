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
 * @file compiler.h
 * @brief Facade del compilador Vex: encadena los pases
 * lex/parse/check/lower/emit.
 *
 * Esta es la interfaz que el dispatcher CLI llama cuando recibe
 * @c --vex archivo.vex.  Convierte el codigo fuente .vex en texto
 * .vel listo para ser pasado al ensamblador del proyecto.
 *
 * No realiza I/O por si misma (lo hace el caller); solo opera sobre
 * cadenas en memoria.  Esto facilita los tests unitarios sin tocar
 * el sistema de ficheros.
 */

#ifndef VEX_COMPILER_H
#define VEX_COMPILER_H

#include <map>
#include <unordered_map>
#include <string>
#include <vector>

#include "vx/diagnostic.h"
#include "port/port_options.h"

namespace vx {

/**
 * @struct CompileOptions
 * @brief Opciones del compilador Vex.
 */
struct CompileOptions {
    std::string module_name; ///< Nombre logico del modulo (por defecto "main").
    bool emit_debug =
        false;         ///< Emitir comentarios @line N en el .vel generado.
    /// Solo-LSP: si true, las funciones @c comptime (no-macro) tambien se
    /// bajan a IR como funciones normales para poder inspeccionar su codegen
    /// (JIT/AOT/bytecode del hover).  En compilacion normal estas funciones se
    /// evaluan en compile-time y se eliden -> OFF por defecto.
    bool emit_comptime_fns = false;
    int opt_level = 2; ///< 0..3, mapea a ir::OptLevel.  Default O2 (DCE + copy
                       ///< prop + const fold + unreachable + TCO).
    /// si true, ademas del .vel, generar el dump
    /// textual del @c IrModule completo (post-optimizacion) en
    /// @c CompileResult::ir_text.  Util para inspeccionar lo que el
    /// frontend produce antes del backend, debug y verificacion de
    /// fixes (PHIs, SSA, CALLCLOSURE, etc.).
    bool dump_ir = false;

    /// Cuando true, ademas del @c ir_module_cache_bytes (modulo POST-opt),
    /// llena @c CompileResult::ir_module_cache_bytes_preopt con el modulo
    /// IR completo ANTES de la optimizacion O2.  Lo consume el modo
    /// @c --analyze para reportar el coste PRE-opt (complejidad algoritmica
    /// del fuente) junto al POST-opt (complejidad efectiva tras inline/
    /// loop-elim).  Cero impacto en el codegen (rama de debug/analisis).
    bool emit_ir_preopt = false;

    /// Cuando true, llena @c CompileResult::mermaid_ast con un diagrama
    /// Mermaid del AST Vex post type-check.  Util para visualizar la
    /// estructura del codigo fuente: clases, herencia, anotaciones.
    bool dump_mermaid_ast = false;
    /// Cuando true, llena @c CompileResult::mermaid_ir_pre con el
    /// diagrama Mermaid del SSA IR ANTES de optimizar.  Captura el
    /// output crudo del lowering (todos los PHIs, todos los CONSTs,
    /// blocks como los emite el frontend).
    bool dump_mermaid_ir_pre = false;
    /// Cuando true, llena @c CompileResult::mermaid_ir_post con el
    /// diagrama Mermaid del SSA IR DESPUES de optimizar.  Permite
    /// comparar contra ir_pre para ver que hizo el optimizer (DCE,
    /// inline_loop_header, const fold, TCO).
    bool dump_mermaid_ir_post = false;
    /// Cuando true, llena @c CompileResult::mermaid_vel con el
    /// diagrama Mermaid del bytecode .vel final.  Independiente de
    /// dump_ir / dump_mermaid_ir_*: opera solo sobre el texto del .vel.
    bool dump_mermaid_vel = false;

    /// Variantes Graphviz (DOT) de los flags Mermaid.  Producen archivos
    /// .dot listos para `dot -Tpng/-Tsvg`, con la misma topologia y
    /// MAS informacion (tooltips, atributos arbitrarios, formas
    /// distintas por tipo de nodo).  Usar cuando Mermaid se quede corto
    /// para grafos grandes (>200 nodos) o se quiera exportar a PDF/SVG
    /// con control fino del layout.  Coexisten con los flags Mermaid:
    /// activar ambos genera ambos formatos en paralelo.
    bool dump_graphviz_ast = false;
    bool dump_graphviz_ir_pre = false;
    bool dump_graphviz_ir_post = false;
    bool dump_graphviz_vel = false;

    /// Variantes HTML interactivas (CSS+JS embebidos, sin dependencias).
    /// Producen un .html autocontenido por vista que el usuario abre en
    /// el navegador para analizar el flujo de codigo con pan/zoom, panel
    /// de detalle por nodo, busqueda y filtros de aristas.  Internamente
    /// reutilizan el generador Graphviz (paridad de info garantizada).
    /// Coexisten con los flags Mermaid/Graphviz.
    bool dump_html_ast = false;
    bool dump_html_ir_pre = false;
    bool dump_html_ir_post = false;
    bool dump_html_vel = false;

    /// --diagram-cost: anotar cada nodo-funcion de los diagramas IR (pre y
    /// post) con su coste Big-O (parcial + total) calculado por
    /// analyze::bigo.  Cero impacto si no hay diagramas IR habilitados.
    bool annotate_cost = false;

    /// Lenguaje destino del transpiler IR -> codigo fuente.  Vacio = no
    /// transpilar (default).  Valores soportados: "c" (Phase 1).
    /// Futuros: "java", "js", "rust", etc.
    std::string port_target;

    /// Opciones del transpiler (GC, EH, type style).  Solo se consulta
    /// si @c port_target != "".  Default segun PortOptions::PortOptions.
    port::PortOptions port_options;

    /// Fase 4 interop C: si @c true, genera el header C publico del modulo
    /// (structs C-compat + prototipos de funciones C-representables) en
    /// @c CompileResult::header_text.  Activado por `vex --emit-header`.
    bool emit_header = false;

    /// Instrumentacion para debugging: cuando esta activa, el lowering
    /// emite CALLs sinteticas a @c "vex_trace:enter" y @c "vex_trace:exit"
    /// al inicio y antes de cada @c RET de cada funcion del usuario.
    /// Como las trazas estan en el IR (no en un backend especifico),
    /// el bytecode VM, el JIT y todos los ports (C, futuro Java, JS)
    /// las heredan automaticamente.
    ///   "none" (default) - sin instrumentacion (cero overhead).
    ///   "trace"          - imprime "ENTER name" y "EXIT name = value"
    ///                      con indentacion segun call depth.
    ///   "profile"        - mide tiempo per-funcion (rdtsc); imprime
    ///                      estadisticas al exit del programa.
    std::string instrument_mode;

    /// Phase AOT.2.b: modo POO NATIVA (sin runtime VM).  Cuando true, el
    /// lowering de clases baja a layout estilo C-struct (offsets estaticos)
    /// + new->malloc(size)/alloca + ctor directo, SIN __module_init/
    /// ClassRegistry/GcHeap (no se emiten defclass/newobj/findclass/
    /// gc_deref_host).  Lo activa el driver @c -m aot.  Default false
    /// (ruta runtime historica, intacta para la VM/JIT).
    bool native_poo = false;

    /// C3 (AOT): habilita el mecanismo de excepciones NATIVO (setjmp/longjmp,
    /// sin runtime/GC/libc).  CONFIGURABLE: el usuario puede DESACTIVARLO
    /// (--no-exceptions) para kernels/freestanding donde no se quiere ningun
    /// runtime de excepciones; entonces un try/catch/throw da error claro.
    /// Default true.  Coste cero si el programa no usa excepciones (el runtime
    /// solo se emite si hay try/catch/throw).  Solo afecta a @c native_poo (AOT).
    bool exceptions_enabled = true;

    /// Bits del target para el ensamblado del inline-asm (@Naked / asm{}): 64
    /// (defecto), 32 (--aot-arch x86-32) o 16.  La validacion compile-time del
    /// asm debe usar el modo del TARGET, no del host -- si no, instrucciones de
    /// 32 bits (p.ej. `jmp ecx`) fallan en KS_MODE_64.
    uint8_t asm_target_bits = 64;

    /// Ancho del chunk SIMD (bytes) que el matcher del vectorizador hornea en
    /// AOT (native_poo): 16 (SSE2/defecto), 32 (AVX), 64 (AVX512).  En AOT lo
    /// fija el TARGET (--float-isa), no el host de build -> cross-compile
    /// correcto (no emitir AVX2 si el target es solo-SSE2) y ancho seleccionable.
    /// El codegen (vreg) deriva el mismo ancho de @c FloatIsa.  Fuera de AOT el
    /// matcher sigue usando el host (vec_chunk_isa) para portabilidad del .velb.
    uint8_t aot_vec_width = 16;

    /// --float-isa auto: el binario multiversiona las funciones vectorizadas y
    /// elige sse2/avx2/avx512 en runtime por cpuid.  El matcher hornea el chunk
    /// con estrategia DUAL para que UN IR compile a las 3 variantes: element-
    /// wise/unary/scalar-bcast a 64 (cada variante decompone 4x128/2x256/1x512),
    /// reduccion/FMA a 16 (el acumulador no splittea -> 128b en todas).
    bool aot_auto_vec = false;

    /// Fase 3.5 LSP: cuando true, @c compile_vx_source vuelca un snapshot
    /// de los valores @c comptime computados (constantes top-level) a
    /// @c CompileResult::comptime_values.  Estrictamente ADITIVO y gateado:
    /// con el default false el flujo de compilacion es EXACTAMENTE el
    /// historico (cero coste, cero cambio de codegen).  Lo consume el
    /// metodo @c vesta/comptimeValues del LSP, on-demand.
    bool dump_comptime_values = false;

    /// LSP "notebook" (valores runtime): cuando true, el lowering instrumenta
    /// cada declaracion/asignacion de variable ESCALAR (int/bool/char) emitiendo
    /// un CALLN a @c vesta_io:vio_lsp_value(source_line, valor) que vuelca
    /// @c __LSPVAL__:linea:valor a stderr.  El LSP compila con este flag, ejecuta
    /// el @c .velb en un subproceso con timeout y parsea esos marcadores para
    /// mostrar los valores reales de las variables inline.  Estrictamente
    /// ADITIVO y gateado: con el default false el codegen es EXACTAMENTE el
    /// historico (cero coste).  NUNCA se activa en compilacion normal.
    bool lsp_value_trace = false;
};

/**
 * @struct CompileResult
 * @brief Resultado de la compilacion Vex.
 *
 * @c ok == true implica @c vel_text valido y diagnosticos sin errores
 * (puede haber warnings).
 * @c ok == false implica errores en diagnostics; @c vel_text puede
 * estar vacio o ser parcial.
 */
struct CompileResult {
    bool ok = false;      ///< Exito global.
    std::string vel_text; ///< Texto .vel generado a partir del IR.
    std::string
        ir_text; ///< dump del IrModule (solo si CompileOptions::dump_ir).
    /// AOT.2.d: simbolos de override (@AllocatorOverride / @PanicHandler).
    /// Vacio = sin override (convencion calloc/free/abort).  El driver
    /// @c -m aot los pasa a @c AotLowerConfig y habilita LIBC_MAPPED en
    /// --freestanding para el rol cubierto.
    std::string aot_alloc_sym; ///< @AllocatorOverride que devuelve ptr.
    std::string aot_free_sym;  ///< @AllocatorOverride que devuelve void.
    std::string aot_panic_sym; ///< @PanicHandler.
    /// C-3: @StringConcat / @StringEq -- nombres de las funciones libres
    /// que reemplazan el `+` (concat) y `==` (eq) del string built-in.
    /// Vacios => comportamiento por defecto (STRCAT/STRCMP o value-string).
    std::string string_concat_override;
    std::string string_eq_override;
    /// @SyncImpl -- nombres de las funciones libres que reemplazan la
    /// primitiva de monitor de `synchronized` (enter/exit).  Vacios =>
    /// comportamiento por defecto (opcode MONENTER/MONEXIT en interp/JIT,
    /// __vex_monenter/monexit en AOT).  Cuando estan set, `synchronized`
    /// baja a un CALL a estas funciones en LOS 3 MODOS.
    std::string sync_enter_override;
    std::string sync_exit_override;
    /// CPU dispatch Inc 4: @HelperOverride(<helper>) -- mapea el nombre del
    /// helper objetivo (hoy solo "memcpy") al nombre de la fn libre del
    /// usuario que lo reemplaza.  Vacio => sin override (dispatch por cpuid).
    /// Disenado para escalar a strcmp/strlen/itoa sin tocar el schema.
    std::map<std::string, std::string> aot_helper_override_syms;
    /// Diagrama Mermaid del AST post type-check.  Llenado solo si
    /// @c CompileOptions::dump_mermaid_ast == true.  Vacio en caso
    /// contrario para no pagar el coste de generacion en builds prod.
    std::string mermaid_ast;
    std::string mermaid_ir_pre;  ///< Mermaid del IR pre-optimizacion
                                 ///< (dump_mermaid_ir_pre).
    std::string mermaid_ir_post; ///< Mermaid del IR post-optimizacion
                                 ///< (dump_mermaid_ir_post).
    std::string
        mermaid_vel; ///< Mermaid del bytecode .vel final (dump_mermaid_vel).
    /// Variantes Graphviz (DOT) llenas cuando los flags @c dump_graphviz_*
    /// estan activos.  Vacias en otro caso.  El contenido es texto DOT
    /// completo (con `digraph G { ... }`), listo para `dot -Tpng/-Tsvg`.
    std::string graphviz_ast;
    std::string graphviz_ir_pre;
    std::string graphviz_ir_post;
    std::string graphviz_vel;
    /// Variantes HTML interactivas (documento completo `<!DOCTYPE html>...`)
    /// llenas cuando los flags @c dump_html_* estan activos.  Vacias en
    /// otro caso.  Cada una es una pagina autocontenida lista para abrir.
    std::string html_ast;
    std::string html_ir_pre;
    std::string html_ir_post;
    std::string html_vel;
    Diagnostics diagnostics; ///< Errores y warnings acumulados.

    /**
     * @brief Opcion W: IR serializado en bytes para embebido en `.velb`.
     *
     * Llenado por @c compile_vx_source con el resultado de
     * @c ir::emit_ir_section sobre el @c IrModule optimizado.  El
     * caller (main.cpp + assembler) pasa estos bytes al Linker
     * via @c Linker::set_ir_section_bytes.  El Linker los appendea
     * a la seccion @c @ir del `.velb` v3.
     *
     * Asi habilita auto-JIT: al cargar un `.velb`,
     * el Loader deserializa esta seccion y mantiene un mapping
     * @c MethodInfo* -> @c IrFunction.  Cuando una funcion se
     * vuelve "hot" (invocation_count >= threshold), el JIT compila
     * el IR y se patcha @c MethodInfo::jit_code.
     *
     * Vacio si la compilacion no produjo IR (caso de errores).
     */
    std::vector<uint8_t> ir_section_bytes;

    /**
     * @brief Phase AOT: IR del modulo COMPLETO serializado (functions +
     * static_data + globals) via @c ir::emit_ir_module_cache (magic VXMC).
     *
     * A diferencia de @c ir_section_bytes (solo functions, lo consume el
     * JIT/loader), esto round-trippea tambien el @c static_data -- que el
     * codegen AOT necesita para materializar los literales en @c .rodata
     * (las refs @c STR_LIT_ADDR/@c LABEL_ADDR se resuelven contra el).  Lo
     * consume el driver @c -m aot con @c ir::parse_ir_module_cache.  NO toca
     * la seccion @c @ir del @c .velb (cero impacto en el path JIT).
     *
     * Vacio si la compilacion no produjo IR (caso de errores).
     */
    std::vector<uint8_t> ir_module_cache_bytes;

    /**
     * @brief Modo --analisis: IR del modulo completo serializado ANTES de
     * la optimizacion O2 (mismo formato magic VXMC que
     * @c ir_module_cache_bytes).  Llenado SOLO si
     * @c CompileOptions::emit_ir_preopt esta activo.
     *
     * El modo @c --analyze lo usa para computar el coste PRE-opt (la
     * complejidad algoritmica del codigo tal como se escribio) y
     * contrastarlo con el POST-opt (la complejidad efectiva del codigo
     * final, que puede ser MENOR si el optimizer elimina/folda loops).
     *
     * Vacio si la compilacion no produjo IR o si @c emit_ir_preopt es
     * false (caso comun en builds de produccion: cero coste extra).
     */
    std::vector<uint8_t> ir_module_cache_bytes_preopt;

    /// Codigo fuente generado por el transpiler IR -> lenguaje destino.
    /// Lleno solo si @c CompileOptions::port_target != "".  El contenido
    /// es C/Java/JS/etc segun el target elegido, listo para escribir
    /// a archivo y compilar con la toolchain nativa.
    std::string port_text;

    /// Fase 4 interop C: texto del header C publico generado, lleno solo si
    /// @c CompileOptions::emit_header.  Listo para escribir a `<out>.h`.
    std::string header_text;

    /// Warnings emitidos por el transpiler (IR ops no soportadas por
    /// el backend, etc.).  Vacio si no hubo issues.
    std::vector<std::string> port_warnings;

    /**
     * @brief expectaciones capturadas por el
     * TypeChecker al evaluar @Macros via el AST evaluator.
     *
     * Cada entrada describe un call site con su (nombre, args,
     * resultado AST esperado, ubicacion).  El caller las pasa a
     * @c ComptimeRuntime::record_expectation tras cargar el
     * bytecode con @c load_macros_from_bytes, y luego invoca
     * @c shadow_validate para comparar AST vs VM end-to-end.
     *
     * Si los conteos del shadow validate divergen, indica un bug
     * en la lowering del @Macro a IR (o en el AST evaluator).
     * Cuando todo coincide, MC.9 podra hacer el switch a VM-only
     * con confianza.
     */
    struct MacroExpectation {
        std::string macro_name;
        std::vector<uint64_t> args;
        std::string expected_str;
        std::string src_loc;
    };
    std::vector<MacroExpectation> macro_expectations;

    /**
     * @brief @c true si el modulo contiene AL MENOS
     * UNA declaracion @Macro que fue lowereada al IR.  Independiente
     * de @c macro_expectations (que solo se popula con call sites
     * cuyos args son codificables como uint64 directo).
     *
     * Usado por @c main.cpp como gate para disparar el two-phase
     * compile + cache populate.  Esto permite que macros con args
     * @c string (marshalados con @c runtime::make_string_flat) o
     * structs/arrays (futuros) tambien se beneficien del path VM.
     */
    bool has_lowerable_macros = false;

    /**
     * @brief por cada @Macro que el lowering rechazo
     * por usar features no soportados en el path VM (builtins
     * comptime-only, comptime globals, etc.), guarda
     * @c (macro_name, reason).  El main.cpp los imprime via
     * @c VESTA_MC_VERBOSE para diagnostico.
     */
    std::vector<std::pair<std::string, std::string>> macro_skip_reasons;

    /**
     * @brief Phase M5.B: rutas canonicas (absolutas + normalizadas) de
     * TODOS los modulos que participaron en el compile (root + deps
     * recursivos).  El main.cpp las usa para persistir el project
     * cache: tras un compile exitoso guarda
     * @c (paths, source_hashes, .velb final) para que el siguiente
     * compile pueda hacer cache hit instantaneo si nada cambio.
     *
     * Vacio si la compilacion fue single-file (sin imports) o si
     * @c compile_vx_source en lugar de @c compile_vx_project se uso.
     */
    std::vector<std::string> dep_paths;

    /**
     * @brief Fase 3.5 LSP: snapshot de los valores @c comptime computados.
     *
     * Cada entrada describe una constante @c comptime (top-level) con su
     * nombre, el ambito donde vive (best-effort), la clase de valor y una
     * representacion legible.  Llenado SOLO si
     * @c CompileOptions::dump_comptime_values esta activo (default false,
     * cero coste en builds normales).  Lo consume el metodo LSP
     * @c vesta/comptimeValues para mostrar al usuario los valores que el
     * compilador resolvio en tiempo de compilacion.
     */
    struct ComptimeValueSnapshot {
        std::string name;      ///< Nombre de la constante comptime.
        std::string scope;     ///< Ambito (best-effort; "" = global/desconocido).
        std::string type_kind; ///< "int"|"string"|"array"|"struct"|"type".
        std::string value_str; ///< Representacion legible del valor.
        SourceLoc loc;         ///< Ubicacion de la expresion (para hover);
                               ///< line==0 si no aplica (consts top-level).
        std::string builtin_kind; ///< "sizeof"/"alignof"/"kind"/"type_id"/
                                  ///< "typename" si proviene de un builtin; ""
                                  ///< para constantes comptime normales.
    };
    std::vector<ComptimeValueSnapshot> comptime_values;
};

/**
 * @brief Compila una cadena .vex a texto .vel.
 *
 * Pipeline interno:
 *
 *   .vex source
 *     -> Lexer (token stream)
 *     -> Parser  (AST)
 *     -> TypeChecker (rellena result_type, valida)
 *     -> Lowering (AST -> ir::IrModule)
 *     -> ir::ir_emit_module (IR -> texto .vel)
 *
 * No aplica VPP; el caller debe haberlo aplicado antes si quiere
 * habilitar la metaprogramacion.
 *
 * @param source   Codigo fuente Vex.
 * @param filename Nombre logico del fichero para diagnosticos.
 * @param opts     Opciones de compilacion.
 * @return CompileResult con el .vel y el set de diagnosticos.
 */
CompileResult compile_vx_source(const std::string &source,
                                 const std::string &filename,
                                 const CompileOptions &opts = {});

/**
 * @brief Compila un proyecto multi-modulo (Phase M.2.e).
 *
 * Resuelve los @c import del fichero raiz via @c ModuleGraph,
 * compila cada modulo en orden topologico (deps primero), inyecta
 * los .vexi entre dependientes, y emite un unico @c .vel con todas
 * las funciones mergeadas.
 *
 * Restricciones MVP:
 *   - Solo @c "only A, B" imports inyectan simbolos (alias y namespace
 *     son features posteriores).
 *   - Sin mangling automatico: el usuario es responsable de que los
 *     nombres de funciones no colisionen entre modulos.
 *   - Sin link separado: todos los modulos se mergean en un solo .vel.
 *
 * @param root_path Path absoluto o relativo del @c .vex raiz.
 * @param opts      Opciones de compilacion.
 * @param source_overlay Opcional: mapa path->texto en memoria (overlay).  Usado
 *        por el LSP para analizar el buffer del editor (root) resolviendo los
 *        imports del disco.  @c nullptr => todo del disco (ruta normal).
 * @param extra_search_paths Opcional: directorios extra donde resolver imports.
 *        Usado por el LSP para resolver imports relativos al root del proyecto
 *        (p.ej. `import "modules/buffer"`) cuando se analiza un fichero-modulo
 *        que no es la raiz (se le pasan sus directorios ancestros).
 * @return CompileResult con el @c .vel mergeado + diagnostics.
 */
CompileResult compile_vx_project(
    const std::string &root_path, const CompileOptions &opts = {},
    const std::unordered_map<std::string, std::string> *source_overlay =
        nullptr,
    const std::vector<std::string> *extra_search_paths = nullptr);

/**
 * @brief Detecta si el source @c .vex contiene @c import declaraciones
 * top-level.  Heuristica permisiva (string-scanner que respeta
 * comentarios y strings).  Usado por @c main.cpp para decidir si
 * dispatchar a @c compile_vx_project en lugar de @c compile_vx_source.
 *
 * @param source Texto Vex.
 * @return @c true si encuentra al menos un @c import top-level.
 */
bool vex_source_has_imports(const std::string &source);

} // namespace vx

#endif // VEX_COMPILER_H
