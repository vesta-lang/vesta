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
 * @file lowering.h
 * @brief Pase de bajada AST de Vesta -> ir::IrModule (SSA).
 *
 * Subset cubierto en(intencionalmente reducido para validar el
 * pipeline end-to-end pronto):
 *
 *   - Declaracion de funciones top-level con parametros y retorno.
 *   - Variables locales con inicializador (UNA SOLA asignacion).
 *   - Expresiones aritmeticas, logicas, bitwise, comparacion.
 *   - if/else.
 *   - Llamadas a otras funciones del mismo modulo.
 *   - Recursion permitida.
 *
 * Diferido a hitos posteriores:
 *
 *   - while / for / do-while con estado mutable (requiere phi nodes en
 *     el bloque header, algoritmo de construccion de Braun et al.).
 *   - Asignacion a variables locales (idem: cada nueva asignacion crea
 *     un IrValueId distinto, hay que recolectar phi en bloques merge).
 *   - ++ y -- (variante del anterior).
 *   - Variables globales con estado mutable.
 *
 * Si el AST contiene una construccion no soportada, el lowering emite
 * un diagnostico explicito y aborta.  Esto evita generar IR incorrecto
 * silenciosamente.
 *
 * Decisiones de hardware:
 *   - Scope chain con std::vector<unordered_map>: lookup O(N_scopes)
 *     amortizado, dominado por la cache hot del scope actual.
 *   - Las constantes se rematerializan en cada uso (no se cachean).
 *     El optimizador IR (O1+) hace common subexpression elimination
 *     en la fase posterior, asi que duplicacion aqui es gratuita.
 *   - El lowering recorre el AST UNA sola vez; el type checker ya
 *     dejo el campo result_type relleno, evitando recomputos.
 */

#ifndef VX_LOWERING_H
#define VX_LOWERING_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/ssa_ir.h"
#include "vx/ast.h"
#include "vx/diagnostic.h"
#include "vx/type_checker.h"

namespace vx {

/**
 * @class Lowering
 * @brief Convierte un ModuleNode AST en un ir::IrModule SSA.
 *
 * Uso:
 * @code
 *   Lowering low(mod, tc, diags);
 *   ir::IrModule irmod;
 *   if (low.run(irmod, "modname")) { ... usar irmod ... }
 * @endcode
 */
class Lowering {
  public:
    /**
     * @brief Construye el lowering sobre un modulo AST y un diags.
     *
     * @param mod   AST a bajar.  El campo result_type de cada Expr
     *              debe estar relleno (ejecutar TypeChecker antes).
     * @param tc    TypeChecker que ya corrio sobre @p mod; se usa
     *              para consultar StructLayout y resolver tamanos
     *              de variables tipo struct.
     * @param diags Sumidero de errores.
     */
    Lowering(ast::ModuleNode &mod, const TypeChecker &tc, Diagnostics &diags);

    /**
     * @brief Ejecuta el pase y rellena @p out_module.
     *
     * @param out_module Modulo IR de salida.  Se sobreescriben todos
     *                   sus campos.
     * @param module_name Nombre logico del modulo (acaba en @c IrModule::name).
     * @return @c true si no hubo errores; el modulo es valido para emitir.
     */
    bool run(ir::IrModule &out_module, const std::string &module_name);

    /**
     * @brief Habilita instrumentacion para debugging.  Cuando esta
     * activa, el lowering emite @c CALLN sinteticas a
     * @c "vx_trace:enter" al inicio y a @c "vx_trace:exit" antes
     * de cada @c RET de cada funcion del usuario.  La instrumentacion
     * vive en el IR -> todos los backends (bytecode VM, JIT, port C,
     * futuros ports) la heredan automaticamente.
     *
     * @param mode @c "none" (default, sin instrumentacion),
     *             @c "trace" (enter/exit con nombre + depth),
     *             @c "profile" (timing per-funcion).
     */
    void set_instrument_mode(const std::string &mode) {
        instrument_mode_ = mode;
    }

    /// Phase AOT.2.b: activa el modo POO NATIVA (sin runtime VM).  Cuando
    /// esta activo, el lowering de clases baja a layout C-struct +
    /// new->malloc/alloca + ctor directo, SIN __module_init/registry/GC.
    void set_native_poo(bool on) { native_poo_ = on; }
    /// Bits del target para validar/ensamblar el inline-asm (@Naked / asm{}):
    /// 64 (defecto), 32 o 16.  Lo fija el driver AOT desde --aot-arch.
    void set_asm_target_bits(uint8_t bits) { asm_target_bits_ = bits; }
    /// Ancho del chunk SIMD (bytes) que hornea el matcher del vectorizador en
    /// AOT (16 SSE2 / 32 AVX / 64 AVX512).  Lo fija el driver desde --float-isa.
    void set_aot_vec_width(uint8_t w) { aot_vec_width_ = w; }
    /// --float-isa auto: chunk DUAL (element-wise 64, reduccion 16) para que un
    /// IR compile a las 3 variantes (multiversion por cpuid en runtime).
    void set_aot_auto_vec(bool on) { aot_auto_vec_ = on; }
    /// Solo-LSP: bajar tambien las funciones @c comptime (no-macro) a IR para
    /// poder inspeccionar su codegen.  Ver @c CompileOptions::emit_comptime_fns.
    void set_emit_comptime_fns(bool on) { emit_comptime_fns_ = on; }

    /// C-3: registra los nombres de las funciones libres marcadas con
    /// @StringConcat / @StringEq.  Cuando no estan vacios, el lowering
    /// del `+`/`==` entre strings (y de los builtins str_concat/
    /// str_equals) rutea a una CALL a esas funciones en vez del
    /// concat/cmp por defecto.  Aplica en native_poo_ y Full.
    void set_string_op_overrides(const std::string &concat,
                                 const std::string &eq) {
        string_concat_override_ = concat;
        string_eq_override_ = eq;
    }

    /// @SyncImpl: registra los nombres de las funciones Vesta que reemplazan
    /// la primitiva de monitor de `synchronized`.  Cuando NO estan vacios,
    /// @c emit_monitor_op emite un CALL a @p enter / @p exit en LOS 3 MODOS
    /// (interp/JIT/AOT) en vez del opcode MONENTER/MONEXIT (interp/JIT) o
    /// __vx_monenter/monexit (AOT).  El operando es el host_ptr al
    /// ObjectHeader (no el GcHandle): la impl del usuario decide el layout.
    void set_sync_impl_overrides(const std::string &enter,
                                 const std::string &exit) {
        sync_enter_override_ = enter;
        sync_exit_override_ = exit;
    }

    /// CPU dispatch Inc 4: registra el nombre de la fn libre marcada con
    /// @HelperOverride(memcpy).  Cuando NO esta vacio, __vx_memcpy_init
    /// apunta el fp directamente a esta fn (INCONDICIONAL, sin leer el
    /// bitmask de cpuid).  Solo aplica en native_poo_ (AOT).
    void set_memcpy_override(const std::string &fn_name) {
        memcpy_override_ = fn_name;
    }

    /// CPU dispatch Inc 5a: registra el nombre de la fn libre marcada con
    /// @HelperOverride(strcmp).  Cuando NO esta vacio, __vx_strdisp_init
    /// apunta el fp __vx_strcmp_fp a esta fn (INCONDICIONAL).  El default es
    /// __vx_strcmp_base (la impl escalar del compilador).  Solo native_poo_.
    /// Firma esperada: i64(u8*, i64, u8*, i64).
    void set_strcmp_override(const std::string &fn_name) {
        strcmp_override_ = fn_name;
    }

    /// CPU dispatch Inc 5a: registra el nombre de la fn libre marcada con
    /// @HelperOverride(strlen).  Cuando NO esta vacio, __vx_strdisp_init
    /// apunta el fp __vx_strlen_fp a esta fn (INCONDICIONAL).  El default es
    /// __vx_strlen_base.  Solo native_poo_.  Firma esperada: i64(u8*).
    void set_strlen_override(const std::string &fn_name) {
        strlen_override_ = fn_name;
    }

    /// Wrapper publico para que helpers estaticos del modulo (e.g.
    /// @c collect_spawn_captures_in_expr) puedan resolver un nombre
    /// recorriendo todos los scopes activos del lowering.
    /// @return @c IrValueId del binding o @c IR_NO_VALUE si no existe.
    ir::IrValueId spawn_capture_resolve_public(const std::string &name) {
        return spawn_capture_resolve(name);
    }

  private:
    // -----------------------------------------------------------------
    // Helpers de tipo y constante.
    // -----------------------------------------------------------------

    /**
     * @brief Convierte un PrimitiveKind del frontend a ir::IrType.
     */
    static ir::IrType ir_type_from_primitive(PrimitiveKind p) noexcept;

    /**
     * @brief Genera una instruccion CONST en el bloque actual.
     */
    ir::IrValueId emit_const(ir::IrType t, uint64_t imm, uint32_t source_line);

    /**
     * @brief Emite GETPROC y devuelve el SSA value (PTR al ProcessVM).
     *        Usado por las variantes GC-aware de las colecciones (cada
     *        variante @c *_gc del plugin recibe @c proc como primer arg
     *        para poder invocar @c gc_addref / @c gc_release sobre los
     *        slots que contienen GcHandles).
     */
    ir::IrValueId emit_getproc(uint32_t source_line);

    /**
     * @brief emite la secuencia ALLOCA + GETPROC +
     * CALLN(native_fn) + STRMAKE para convertir un valor primitivo
     * a string (StringObject GcHandle).
     *
     * Usado por interpolacion `${val}` y por los builtins `to_str`,
     * `chr`, `ord` que se aliasan al runtime path.  El @c native_fn
     * (e.g. "vio_int_to_vmbuf", "vstr_chr_to_vmbuf") debe respetar
     * la firma `(proc_ptr, vm_addr, value) -> length`.
     *
     * @param v_val      SSA value del valor primitivo a stringificar.
     * @param native_fn  Nombre de la funcion native en vesta_io.
     * @param source_line Linea para diagnostico.
     * @return GcHandle del StringObject resultante.
     */
    ir::IrValueId stringify_primitive_via_native(ir::IrValueId v_val,
                                                 const char *native_fn,
                                                 uint32_t source_line);

    /**
     * @brief Phase MC.17.2 -- obtiene (o aloca) el slot @c static_data
     * que materializa un comptime global como memoria runtime para
     * macros lowereados.  Ver @c comptime_global_slots_.
     *
     * @return idx valido o @c UINT64_MAX si el global no es int
     *         (strings/structs no soportados en v1).
     */
    uint64_t get_or_create_comptime_global_slot(const std::string &name);

    /**
     * @brief L2.2: allocate a slot en static_data para una variable
     * global runtime no-const.  El slot guarda el valor (i64-encoded:
     * GcHandle para STRING, valor escalar para i64/u64/etc.).
     *
     * Idempotente: si ya existe slot para @p name lo devuelve.
     * Init: el slot empieza zero-filled; @c __module_init lo inicializa.
     */
    uint64_t get_or_create_runtime_global_slot(const std::string &name,
                                               uint64_t bytes = 8);

    /**
     * @brief Reserva el slot de la PLANTILLA de un `thread_local` (TLS).
     *
     * El slot lleva la plantilla por-hilo (bytes de inicializacion estaticos,
     * NO via @c __module_init), marcado @c SD_FLAG_TLS + seccion @c .tdata
     * (SHF_TLS).  El codegen AOT lo emite en una seccion TLS y el acceso usa
     * el thread pointer (fs/gs + TPOFF) en vez de una direccion lineal.
     *
     * @param name   nombre del global.
     * @param bytes  tamano de la variable (>=1).
     * @param init_value valor inicial empaquetado LE (los primeros 8 bytes).
     */
    uint64_t get_or_create_tls_global_slot(const std::string &name,
                                           uint64_t bytes, uint64_t init_value,
                                           uint16_t alignment);

    /**
     * @brief Inserta una conversion de tipo si difiere; identidad si igual.
     *
     * @param is_explicit Si false (por defecto), emite un warning cuando
     *        la conversion es potencialmente perdida (narrowing,
     *        float<->int, etc.).  Las llamadas desde @c lower_cast_expr
     *        pasan true para silenciar el warning porque el usuario
     *        opto explicitamente por el cast.
     */
    ir::IrValueId cast_if_needed(ir::IrValueId v, ir::IrType from,
                                 ir::IrType to, uint32_t source_line,
                                 bool is_explicit = false);
    /**
     * @brief Igual que el anterior pero con el @c SourceLoc completo de la
     *        expresion culpable, para que el warning de conversion apunte a su
     *        COLUMNA real (no al inicio de la linea).  El overload de
     *        @c source_line delega en este con columna 1.
     */
    ir::IrValueId cast_if_needed(ir::IrValueId v, ir::IrType from,
                                 ir::IrType to, const SourceLoc &loc,
                                 bool is_explicit = false);

    // -----------------------------------------------------------------
    // Lowering por categoria.
    // -----------------------------------------------------------------

    void lower_function(ast::FunctionDecl *fd, ir::IrModule &out);

    /**
     * @brief Lowering de funciones marcadas con @Async.
     *
     * Genera DOS funciones IR a partir de la `FunctionDecl @Async`:
     *
     *   1. Wrapper publico con el nombre original (`fd->name`):
     *      - alloca un Future via @c future_alloc()
     *      - spawnea un child con la funcion sintetica `__async_<name>`
     *      - msgsend(child, fut_handle)
     *      - return fut_handle (i64)
     *
     *   2. Spawn helper sintetica `__async_<name>`:
     *      - msgrecv() -> handle del Future en SSA value (`async_fut_id_`)
     *      - body lowered del usuario; cada `return X` se intercepta en
     *        @c lower_return: emite `fulfill(async_fut_id_, X) + hlt`
     *        en lugar de RET.
     *      - Si el body cae naturalmente sin return: emite
     *        `fulfill(async_fut_id_, 0) + hlt` al final.
     *
     * El usuario llama `i64 fut = fn(); i64 r = await fut;` con la
     * misma firma que el wrapper expone.
     */
    void lower_async_function(ast::FunctionDecl *fd, ir::IrModule &out);
    void lower_block(ast::BlockStmt *b);
    void lower_stmt(ast::Stmt *s);
    void lower_var_decl(ast::VarDeclStmt *vd);
    void lower_if(ast::IfStmt *s);
    void lower_return(ast::ReturnStmt *s);
    void lower_while(ast::WhileStmt *s);
    /// Auto-vectorizacion (idioma memcpy): si @p s es exactamente el patron
    /// de copia de bytes/elementos `while (i < N) { dst[i] = src[i]; i++; }`
    /// sobre punteros HOST, lo reemplaza por un unico @c MEMCPY (el JIT/AOT lo
    /// bajan a @c rep @c movsb / SIMD; el interprete a un bucle host->host).
    /// Devuelve true si reconocio y bajo el idioma (el llamante debe @c return);
    /// false si no matchea (seguir con el lowering normal del while).
    bool try_lower_memcpy_idiom(ast::WhileStmt *s);
    /// Igual para @c for(T i=init; i<N; i++) dst[i]=src[i]; (la forma canonica
    /// del memcpy).  Cubre el for que DECLARA la var del loop en @c init
    /// (loop-local), evitando el writeback de scope post-loop.
    bool try_lower_memcpy_idiom_for(ast::ForStmt *s);
    /// Auto-vectorizacion aritmetica: @c for(T i=init; i<N; i++) c[i]=a[i] OP
    /// b[i]; con a/b/c punteros f64 HOST y OP in {+,-,*,/}.  Emite un loop
    /// principal que procesa W=2 elementos por iteracion via @c VEC_BINOP
    /// (SIMD packed en JIT/AOT, escalar por lane en interp) + una cola escalar
    /// re-bajando el cuerpo para el resto (N % W).  Devuelve true si matcheo.
    bool try_vectorize_elementwise_for(ast::Stmt *s);
    /// Auto-vectorizacion de DIFUSION ESCALAR (scalar broadcast):
    /// @c for(T i=init; i<N; i++) c[i] = a[i] OP scalar; con @c c/@c a punteros
    /// f64 HOST y @c scalar un valor f64 invariante del loop.  Tambien la forma
    /// conmutativa @c scalar OP a[i] (add/mul) y el compound @c c[i] OP= scalar.
    /// El escalar se difunde a todos los lanes (UNPCKLPD/VBROADCASTSD) en JIT/
    /// AOT, escalar por lane en interp.  Devuelve true si matcheo.
    bool try_vectorize_scalar_for(ast::Stmt *s);
    /// Auto-vectorizacion UNARIA: @c for(T i=init; i<N; i++) b[i] = OP a[i];
    /// con @c a/@c b punteros f64 HOST y OP in @c -a[i] (fneg), @c sqrt(a[i])
    /// (fsqrt), @c fabs(a[i]) (fabs).  Loop principal W=2 con @c VEC_UNOP (SIMD
    /// packed SQRTPD/XORPD/ANDPD en JIT/AOT, escalar por lane en interp) + cola
    /// escalar.  La copia pura la cubre el memcpy-idiom.  Devuelve true si match.
    bool try_vectorize_unary_for(ast::Stmt *s);
    /// Auto-vectorizacion de REDUCCION: @c for(T i=init; i<N; i++) acc = acc +
    /// a[i]; con @c acc escalar f64 y @c a puntero f64 HOST.  Usa un acumulador
    /// vectorial de W=2 lanes (slot host 16B) acumulado con @c VEC_BINOP
    /// (acc_slot += a_chunk), reduccion horizontal final + cola escalar.  El
    /// resultado se bindea a @c acc.  Devuelve true si matcheo.
    bool try_vectorize_reduction_for(ast::Stmt *s);
    /// Auto-vectorizacion COMPOUND (cadena lineal multi-op): @c for(...) c[i] =
    /// a[i] OP1 x OP2 y OP3 ... donde cada operando derecho (x, y, ...) es
    /// @c arr[i] (host, mismo tipo) o un escalar invariante f64, y la expresion
    /// es left-leaning `((a OP1 x) OP2 y) ...` (precedencia natural de
    /// @c a[i]*k + b[i]).  Emite el loop principal usando @c c como acumulador:
    /// @c c = a OP1 x ; @c c = c OP2 y ; ... (cadena de @c VEC_BINOP /
    /// @c VEC_BINOP_S por chunk) + cola escalar.  Solo f64/f32.  Cubre el patron
    /// axpy/FMA que los matchers de 1-op no aceptan.  Devuelve true si matcheo.
    bool try_vectorize_compound_for(ast::Stmt *s);
    /// Valida que @p asg sea exactamente @c dst[idx] = src[idx] con bases
    /// IdentExpr HOST ptr/array de igual tamano de elemento.  Compartido por
    /// las formas while/for.  Rellena las bases y el tamano de elemento.
    bool mc_match_copy_assign(ast::AssignExpr *asg, const std::string &idx_name,
                              ast::IdentExpr **out_dst, ast::IdentExpr **out_src,
                              size_t *out_esz);
    /// Emite el MEMCPY equivalente a la copia en @c current_block_.  @p v_idx
    /// es el SSA del indice inicial (ya resuelto por el llamante: lookup para
    /// el while, lower del init para el for).  Si @p idx_name_for_post no esta
    /// vacio, ademas escribe el idx post-loop = idx_init+count en ese nombre de
    /// scope (solo el while con idx externa lo necesita).  Devuelve false si
    /// @p v_idx no es un entero o si algun lower_expr fallo (defensivo).
    bool mc_emit_copy(ir::IrValueId v_idx, ast::Expr *limit,
                      ast::IdentExpr *dst_base, ast::IdentExpr *src_base,
                      size_t esz, uint32_t ln,
                      const std::string &idx_name_for_post);
    void lower_do_while(ast::DoWhileStmt *s);
    void lower_for(ast::ForStmt *s);
    void lower_try(ast::TryStmt *s);
    void lower_throw(ast::ThrowStmt *s);
    void lower_foreach(ast::ForEachStmt *s);
    void lower_synchronized(ast::SynchronizedStmt *s);
    void lower_asm(
        ast::AsmStmt *s); ///< Phase AS: baja a IrOp::INLINE_ASM (marker host).

    ir::IrValueId lower_expr(ast::Expr *e);
    ir::IrValueId lower_binary(ast::BinaryExpr *e);
    ir::IrValueId lower_unary(ast::UnaryExpr *e);
    ir::IrValueId lower_call(ast::CallExpr *e);
    ir::IrValueId lower_ident(ast::IdentExpr *e);
    ir::IrValueId lower_string_lit(ast::StringLitExpr *e);
    ir::IrValueId lower_assign(ast::AssignExpr *e);
    ir::IrValueId
    lower_ternary(ast::TernaryExpr *e); ///< A.38: cond ? then : else
    ir::IrValueId lower_field_access(ast::FieldAccessExpr *e);
    ir::IrValueId lower_index(ast::IndexExpr *e);

    /**
     * @brief Lowering de @c spawn @c { @c body @c }.
     *
     * Genera una funcion sintetica de nombre @c __spawn_<N>(void) que
     * contiene el body, terminada con @c hlt (no @c ret) porque los
     * procesos hijo terminan, no retornan.  En el sitio del spawn,
     * emite @c mov @c rN, @c @Absolute("code.__spawn_N") + @c spawn @c rN
     * y captura el PID encoded del hijo (que el opcode deposita en R0).
     *
     * @return SSA value con el PID encoded del hijo (i64).
     */
    ir::IrValueId lower_spawn_expr(ast::SpawnExpr *e);

    /**
     * @brief Genera la funcion sintetica del body de un @c spawn.
     *
     * Compila @p body como funcion top-level con nombre
     * @c __spawn_<N> donde N es @c spawn_func_counter_++.  Termina
     * con @c hlt.  La nueva @c IrFunction se agrega al modulo via
     * @c out_mod_->add_function.
     *
     * @return Nombre de la funcion generada (para usar en @c @Absolute).
     */
    std::string generate_spawn_helper(ast::BlockStmt *body,
                                      const SourceLoc &loc);

    /**
     * @brief lowering de @c rspawn(node) { body }.
     *
     * Genera helper @c __rspawn_<N> con `is_rspawn_body_=true` (cualquier
     * @c return X dentro del body se intercepta -> @c mov r0, X + hlt).
     * En el caller emite la instruccion bytecode @c rspawn r_fn, r_node y
     * captura R0 al SSA value (GcHandle del Future).
     *
     * @return SSA value con el GcHandle del Future (i64).
     */
    ir::IrValueId lower_rspawn_expr(ast::RSpawnExpr *e);

    /**
     * @brief genera la funcion sintetica del body de un @c rspawn.
     *
     * Identico a @c generate_spawn_helper salvo por el flag
     * @c is_rspawn_body_ activado durante el lowering del body.  El name
     * es @c __rspawn_<N>.
     */
    std::string generate_rspawn_helper(ast::BlockStmt *body,
                                       const SourceLoc &loc);

    /**
     * @brief closures: lowering de una @c LambdaExpr inline.
     *
     * Pipeline de tres fases:
     *   1. Genera la funcion sintetica @c __lambda_<N> via
     *      @c generate_lambda_helper, que toma los params declarados
     *      como parametros normales y los captures como prologue
     *      implicito que carga desde @c R14 + offset.
     *   2. Aloca un env block en la pila del caller (@c subsp rsp,
     *      8*nCaptures) y escribe los valores capturados ahi.  Si la
     *      lambda no captura nada (n=0), env_addr = 0 (sentinela).
     *   3. Aloca un slot de 16 bytes para el "function value" tipo
     *      @c fn(T) -> R: `[+0 fn_addr][+8 env_addr]`.  Devuelve el
     *      SSA value con la direccion de ese slot.
     *
     * Coste: con N captures, la creacion de la closure cuesta:
     *   - 1 ALLOCA de @c 8*N bytes (env block)
     *   - N STOREs (un qword por captura)
     *   - 1 ALLOCA de 16 bytes (function value slot)
     *   - 1 RAW_ASM @c mov rN, @c \@Absolute(label)
     *   - 2 STOREs (fn_addr + env_addr en el slot)
     * Total: ~5 + 2N instrucciones bytecode.  Cero allocaciones de
     * heap; cero overhead GC.
     *
     * Limitacion MVP: el env vive en la pila del caller, asi que la
     * closure solo es valida dentro del scope que la creo.  Para
     * closures que escapen (e.g. devolver una closure de una funcion)
     * habra que detectar el escape y promover el env a heap GC.
     *
     * @return SSA value con la direccion del function value (16 bytes).
     */
    ir::IrValueId lower_lambda_expr(ast::LambdaExpr *e);

    /**
     * @brief construye un function value (16 bytes) que
     *        apunta a una funcion top-level con env_addr=0.
     *
     * Mismo layout que @c lower_lambda_expr (slot de 16 bytes en
     * stack: fn_addr en +0, env_addr en +8) pero @c fn_addr es
     * @c @Absolute("code.<fn_name>") y env_addr es la constante 0.
     * Permite pasar funciones declaradas en top-level como argumento
     * a parametros tipados @c fn(...) -> ... sin requerir el wrapping
     * manual a una lambda local.
     *
     * @param fn_name Nombre de la funcion top-level.
     * @param line    Linea fuente para diagnostico.
     * @return SSA value PTR con la direccion del slot de 16 bytes.
     */
    ir::IrValueId emit_topfn_value(const std::string &fn_name, int line);

    /**
     * @brief Promueve un string literal a @c StringObject GC-managed.
     *
     * Emite la secuencia: STR_LIT_ADDR -> SSA value PTR; CONST(len) ->
     * SSA value I64; RAW_ASM "strmake {dst}, {src0}, {src1}" -> SSA
     * value I64 (GcHandle).  El handle se devuelve y puede bindearse
     * a una variable de tipo @c string.  Cero overhead vs el modelo
     * antiguo cuando NO se promueve (literal sigue siendo PTR puro).
     *
     * @param slit  Literal de string sin interpolacion.
     * @return SSA value I64 con el GcHandle del StringObject.
     */
    ir::IrValueId
    lower_string_literal_to_string_object(ast::StringLitExpr *slit);

    /**
     * @brief closures: genera la @c IrFunction sintetica para
     *        el body de una lambda.
     *
     * El helper se llama @c __lambda_<N> donde N es @c lambda_counter_++.
     * Su firma es la de la lambda (mismos params), pero el body lleva
     * un prologue que carga cada captura desde @c [r14 + 8*i] a un
     * SSA value local con el nombre del capture.  De esa forma el
     * resto del body (que el type checker validO referencias a
     * capturas como si fueran locales) puede usar las capturas
     * naturalmente sin distinguirlas de los params.
     *
     * Reusa el mismo patron de save/restore de scope que
     * @c generate_spawn_helper.
     *
     * @return Nombre del helper generado (para @c \@Absolute).
     */
    std::string generate_lambda_helper(ast::LambdaExpr *e);

    /**
     * @brief Overlay F3: sintetiza (una vez) la funcion resolvedora del offset
     *        de bloque de un campo -- `__ovl_resolve_<Struct>_<campo>(self)`.
     *
     * El body es el `@offset { ... }` del campo, lowered con `base` (= @c self)
     * y los campos hermanos ligados como locales; `return <dir>` se vuelve el
     * RET de la funcion.  Reusa TODO el control de flujo (if/else, multiples
     * return) sin ALLOCA-en-bucle; el optimizer puede inlinearla.  Devuelve el
     * nombre; @c generated_overlay_resolvers_ evita duplicados.
     */
    std::string generate_overlay_resolver(const StructLayout &lay,
                                          const StructFieldInfo &fi);
    /// Nombres de resolvedores de overlay ya sintetizados (dedup).
    std::unordered_set<std::string> generated_overlay_resolvers_;
    /**
     * @brief F4: baja el puntero de la vista RAIZ de una cadena de accesos
     *        overlay.  Camina @c e por sus bases (FieldAccess/Index) hasta la
     *        expresion que ya no es un acceso a campo/elemento (la vista raiz,
     *        p.ej. `pe` en `pe.Imports[i].name`) y la baja con @c lower_expr.
     *        Es el `root` que se enhebra a un resolver que usa `parent<T>()`.
     */
    ir::IrValueId lower_overlay_root(ast::Expr *e);

    /**
     * @brief ADTs: lowering de un constructor de variante de
     *        enum (`Color.Green(42)` o `Color.Red`).
     *
     * Estrategia:
     *   1. Aloca un slot en pila del caller con
     *      @c size_bytes = 8 + 8*max_payload_fields del enum.  Igual
     *      que para structs, usamos ALLOCA del IR -> @c subsp rsp, N.
     *   2. STORE i64 del @c tag en offset 0.
     *   3. Para cada payload arg: STORE i64 del valor en offset
     *      @c 8 + 8*i.  El valor se promociona a i64 si es mas
     *      estrecho (uniformidad del slot).
     *   4. Devuelve el SSA value con la direccion del slot (PTR).
     *
     * El call site del lowering reconoce este patron mirando
     * @c FieldAccessExpr::property_kind == 99 (marcado por el type
     * checker).  Para variantes sin payload se entra con
     * @c CallExpr::args vacio o directamente desde un FieldAccessExpr.
     *
     * @param enum_name      Nombre del enum (e.g. "Color").
     * @param variant_name   Nombre de la variante (e.g. "Green").
     * @param args           Args del CallExpr (vacio si sin payload).
     * @param loc            Para diagnosticos / metadata IR.
     * @return SSA value PTR al slot del enum recien construido.
     */
    ir::IrValueId
    lower_enum_constructor(const std::string &enum_name,
                           const std::string &variant_name,
                           const std::vector<std::unique_ptr<ast::Expr>> &args,
                           const SourceLoc &loc);

    /**
     * @brief ADTs: lowering de un @c MatchExpr.
     *
     * Estrategia:
     *   1. Lower del scrutinee -> SSA value PTR al slot del enum.
     *   2. LOAD i64 del tag (offset 0).
     *   3. Construye en stack una jumptable: array de @c uint64
     *      (8 bytes/entry) con la direccion de cada arm en orden de
     *      tag.  Para variantes no cubiertas en el match, la entry
     *      es la direccion del arm @c _ (default) o la de la
     *      siguiente instruccion tras el match (fall-through) si no
     *      hay default.
     *   4. RAW_ASM @c jumptable r_tag, r_table, count -> dispatch O(1).
     *   5. Para cada arm: emite un nuevo IrBlock que (a) lee los
     *      payloads del slot del scrutinee como SSA values nuevos,
     *      (b) los bindea con los nombres del patron, (c) lowers el
     *      body, (d) salta al merge_block.
     *   6. merge_block: continuacion tras el match.
     *
     * MVP simplificado: en lugar de @c jumptable bytecode (que
     * requiere ALLOCA + escritura de la tabla en stack + cuidado
     * con relocations), usamos una cadena de @c cmp + @c jmp.jeq al
     * label de cada arm.  Es O(N) en el numero de variantes pero
     * mas simple y robusto para empezar.  Optimizacion a jumptable
     * 0x27 queda como mejora futura (importante para enums grandes).
     *
     * @return @c IR_NO_VALUE (match es statement-like en MVP).
     */
    ir::IrValueId lower_match_expr(ast::MatchExpr *e);
    /// match sobre ESCALARES (enteros/chars), estilo switch.  Dispatch
    /// eficiente: SWITCH_DENSE (O(1)) si los casos son densos, BST balanceado
    /// (O(log N)) si dispersos, cadena lineal si pocos o con guards.
    /// Statement-like (VOID); el valor se produce con `return` en cada arm.
    ir::IrValueId lower_match_scalar(ast::MatchExpr *e);
    /// match sobre STRINGS.  Hiper-eficiente: computa el hash del scrutinee
    /// (STRHASH) una vez, despacha por los hashes de los literales (calculados
    /// en compile-time, mismo FNV-1a 32-bit que el runtime) via dispatch entero
    /// (BST O(log N) / lineal), y en el candidato hace UN STRCMP de verificacion
    /// (colisiones).  Tipico: 1 hash + 1 strcmp, no N comparaciones.
    ir::IrValueId lower_match_string(ast::MatchExpr *e);

    /**
     * @brief Lower @c CastExpr `(T) operand`.
     *
     * Casos cubiertos:
     *  - num <-> num (primitivos): delega en @c cast_if_needed.
     *  - PTR <-> PTR (incluyendo @c VirtualPtr<X> <-> @c X*): bitcast,
     *    el valor SSA mantiene el bit-pattern, pero @c is_host_ptr y
     *    @c pointee_is_host_ptr se ajustan al destino para que LOAD
     *    y STORE posteriores emitan @c mov vs @c movh segun la
     *    naturaleza del destino.
     *  - PTR <-> int (i64/u64) y viceversa: BITCAST IR op (preserva
     *    bits sin conversion numerica).
     *  - ARRAY -> PTR: decay (mismo bit-pattern); ARRAY <-> ARRAY:
     *    bitcast con propagacion de @c is_host_ptr.
     */
    ir::IrValueId lower_cast_expr(ast::CastExpr *e);

    // -----------------------------------------------------------------
    // POO: clases, new, this, getfield/setfield/callvirt sobre CLASS.
    // -----------------------------------------------------------------

    /**
     * @brief Compila los metodos de una clase como funciones IR
     *        independientes con nombre @c <Class>__<method> y un
     *        primer parametro implicito @c this de tipo PTR.
     *
     * Ademas registra en out_module la metadata necesaria para que
     * el module init pueda llamar @c defmethod con la direccion del
     * metodo (via label).
     */
    void lower_class_methods(ast::ClassDecl *cd, ir::IrModule &out);

    /**
     * @brief Baja los metodos de un struct a funciones libres.
     *
     * Cada metodo @c m del struct @c sd produce una @c IrFunction
     * @c <Struct>__<metodo> con un primer parametro implicito @c this
     * (PTR a la direccion del buffer del struct, memoria VM por
     * defecto -- @c is_host_ptr=false).  El dispatch en el call site
     * es CALL directo (sin vtable).  Soporta SRET (Optional/Result)
     * con retbuf hidden tras @c this, igual que las funciones libres.
     */
    void lower_struct_methods(ast::StructDecl *sd, ir::IrModule &out);

    /**
     * @brief NS.6-ext: baja los metodos de @c "extension Tipo { ... }" e
     * @c "impl Concept for Tipo { ... }" como funciones libres
     * @c <clave_layout>__<metodo> (dispatch estatico).  Reusa la emision de
     * @c lower_struct_methods via un @c StructDecl temporal; para targets
     * CLASE se activa @c ext_this_is_class_ para ligar @c this como objeto GC.
     */
    void lower_extension_methods(ir::IrModule &out);
    /// NS.6-ext: cuando @c true, @c lower_struct_methods liga @c this como
    /// host_ptr + objeto GC (target de una extension que es una CLASE).
    bool ext_this_is_class_ = false;

    /**
     * @brief Genera el bloque @c __module_init que registra todas las
     *        clases del modulo.  Se invoca al inicio de @c main.
     *
     * Construye la cadena RAW_ASM con la secuencia
     * @c findclass / @c defclass / @c deffield / @c defmethod usando
     * convenciones de registro fijas (r12-r15 reservados).  Los
     * nombres de clase/field/method se registran como bytes estaticos
     * via @c IrModule::intern_static_data.
     */
    std::string build_module_init_asm(ir::IrModule &out_module);

    /**
     * @brief Lower de @c new ClassName(args) -> CALL a la funcion
     *        auxiliar @c __new_<ClassName> generada por el frontend.
     *        Esa funcion encapsula findclass + newobj + callvirt 0.
     */
    ir::IrValueId lower_new_expr(ast::NewExpr *e);

    /**
     * @brief Genera la IrFunction auxiliar @c __new_<Class>(args) para
     *        cada clase declarada en el modulo y la añade a out.
     *        El cuerpo es un bloque RAW_ASM con findclass + newobj +
     *        callvirt 0 (ctor) + return GcHandle.
     */
    void generate_new_helpers(ir::IrModule &out);
    /// Emite, dentro del destructor del contenedor, la liberacion del env
    /// (RAW_ALLOC host) de un campo closure: `env = [this+offset+8]; if (env)
    /// RAW_FREE(env)`.  Modelo de ownership sin GC (el closure-en-campo se
    /// libera con su objeto, como un campo @c unique<T>).
    /// @param this_vid  SSA value del receptor (`this`, host_ptr al objeto).
    /// @param field_offset  Offset del campo closure (inicio del slot 16B).
    /// @param line  Linea fuente para la depuracion.
    void emit_free_closure_env_field(ir::IrValueId this_vid,
                                     uint32_t field_offset, uint32_t line);
    /// Libera el @c unique<T> almacenado en un campo del contenedor al exit del
    /// scope (ownership, sin GC): el campo guarda la direccion del slot Tier 1
    /// (16B [ptr][deleter]); cargamos el slot, y si != 0 dispatchamos el
    /// deleter dinamico (slot+8): deleter==0 -> RAW_FREE(ptr); !=0 -> CALLIND
    /// deleter(ptr).  Ops explicitas (universales en interp/JIT/AOT).
    /// @param this_vid  SSA value del receptor (host_ptr al contenedor).
    /// @param field_offset  Offset del campo @c unique<T> (8B, guarda el slot).
    /// @param line  Linea fuente.
    void emit_free_unique_field(ir::IrValueId this_vid, uint32_t field_offset,
                                uint32_t line);
    /// Invoca un metodo de struct (`<Struct>__<m>`) sobre un struct que vive en
    /// un campo HOST (p.ej. campo struct de una clase, cuyo payload es host).
    /// Los metodos de struct se compilan asumiendo `this` en memoria VM
    /// (interp/JIT); llamarlos con un `this` host hace que lean `this.campo` con
    /// `mov` (VM) sobre una direccion host -> basura.  En interp/JIT copiamos el
    /// campo a un temporal en VM-stack y llamamos el metodo sobre el temporal
    /// (lee `temp.campo` con VM correcto; los punteros internos son host y se
    /// deref-ean bien).  Valido para metodos que operan sobre los POINTEES
    /// (dtor: free del ptr; copy-hook: ++refcount via el ptr) sin necesidad de
    /// copy-back.  En AOT (native_poo_) el struct ya es host y el metodo
    /// host-this: CALL directo sobre @c field_addr.
    /// @param field_addr  host_ptr a la direccion del campo struct.
    /// @param struct_name  nombre del struct (para el tamano y el label).
    /// @param method_label  label del metodo (`<Struct>__<m>`).
    void emit_struct_method_on_host_field(ir::IrValueId field_addr,
                                          const std::string &struct_name,
                                          const std::string &method_label,
                                          uint32_t line);
    /// Copia memberwise (qword a qword) @p size_bytes desde @p src_addr a
    /// @p dst_addr.  Hereda la naturaleza host/VM de ambas direcciones para
    /// emitir mov/movh correctos.  Usado al copiar un agregado value-type
    /// (struct/array) -- e.g. inicializar un campo struct en un init-list.
    void emit_memberwise_copy(ir::IrValueId dst_addr, ir::IrValueId src_addr,
                              uint64_t size_bytes, uint32_t line);
    /// Rellena con CEROS @p size_bytes a partir de @p addr (STORE 0 en trozos
    /// de 8/4/2/1 bytes, sin desbordar).  Garantiza que todo struct/array en
    /// pila queda zero-inicializado por defecto (seguridad: nada de basura de
    /// la pila en campos no listados en el init).  @p addr es una direccion VM
    /// (ALLOCA); hereda su naturaleza para el STORE.
    void emit_zero_fill(ir::IrValueId addr, uint64_t size_bytes, uint32_t line);
    /// Emite los valores por defecto de los campos de @p lay (los `u8 a = 0x10`)
    /// sobre el struct ya alocado y zero-inicializado en @p base_addr.  Recurre
    /// en campos struct anidados que tengan defaults propios.  Se llama tras el
    /// zero-fill y ANTES del init-list explicito (que sobrescribe lo que toque).
    void emit_struct_field_defaults(ir::IrValueId base_addr,
                                    const StructLayout &lay, uint32_t line);
    /// Rellena los campos de un struct YA alocado en @p base_addr desde el
    /// init-list @p il segun el layout @p lay.  RECURSIVO: un campo de tipo
    /// struct inicializado con un init-list ANIDADO (`{.min = {.x=..,.y=..}}`)
    /// se rellena in-place en la direccion del campo (lower_expr no baja un
    /// InitListExpr como valor).  Un campo struct/array inicializado con una
    /// EXPRESION (otra variable, llamada, ...) usa copia memberwise; un campo
    /// escalar usa STORE.  Comparte la logica del init-list de struct de
    /// @c lower_var_decl para que ambos caminos (top-level y anidado) coincidan.
    void emit_struct_init_fields(ir::IrValueId base_addr,
                                 const StructLayout &lay, ast::InitListExpr *il,
                                 uint32_t line);
    /// Ruta B (H1 paso por valor): copia un struct con copy-hook para pasarlo
    /// por valor a una funcion.  Aloca una copia, memcpy del origen, invoca
    /// `copia.__clone__()` y devuelve la direccion de la copia.  El caller debe
    /// emitir el `~dtor` de la copia tras el CALL (la callee no la posee).
    ir::IrValueId emit_struct_arg_copy_clone(ir::IrValueId v_src,
                                             const std::string &struct_name,
                                             uint32_t line);
    /// Ruta B (H3 inc-on-copy): incrementa el refcount del bloque de control de
    /// un `shared<T>` al copiarlo (`b = a`, campo = a, paso por valor).  El slot
    /// guarda el host_ptr al ctrl; refcount en [ctrl+0].  No-op si ctrl==0.
    void emit_shared_refcount_inc(ir::IrValueId v_slot, uint32_t line);
    /// Ruta B (H3/H5 dec-on-drop): decrementa el refcount y libera (RAW_FREE) si
    /// cae a 0.  Lo usan el cleanup del scope y el dtor del contenedor (campo
    /// shared).  No-op si ctrl==0.
    void emit_shared_refcount_dec(ir::IrValueId v_slot, uint32_t line);
    /// Libera un slot Tier 1 (16B [ptr][deleter]) heap dado su VALOR (no via un
    /// campo): null-guard, dispatch del deleter (slot+8) + RAW_FREE(slot).  Lo
    /// usa @c emit_free_unique_field tras cargar el slot, y el reassign-free de
    /// un campo unique (que captura el slot viejo antes de sobreescribir).
    void emit_free_unique_slot(ir::IrValueId slot, uint32_t line);
    /// Genera los thunks Vesta `__cfnthunk_<fn>` para los externs cuya
    /// direccion se tomo como cfn (ver @c extern_cfn_thunks_).
    void generate_extern_cfn_thunks(ir::IrModule &out);
    /// Sintetiza, si @c needs_free_uniq_helper_, la funcion runtime
    /// `__vx_free_uniq(i64 slot)` que libera un slot Tier 1 de unique<T>
    /// (null-guard + dispatch del deleter + RAW_FREE del slot, reusando
    /// @c emit_free_unique_slot).  La usa el reassign-free de un campo unique
    /// como una sola CALL (el diamante del free vive DENTRO del helper, evitando
    /// la interaccion del diamante con el tailcall del dtor en el call site).
    void generate_free_uniq_helper(ir::IrModule &out);
    /// @c true si algun reassign de campo unique<T> requiere el helper.
    bool needs_free_uniq_helper_ = false;
    /// Devuelve el label a usar para `&fn` / promocion a cfn.  Si @c name es
    /// un extern, registra el thunk y devuelve `__cfnthunk_<fn>`; si no, el
    /// label mangled (o el nombre).
    std::string func_ref_label(const std::string &name,
                               const std::string &mangled);

    /**
     * @brief Genera la IrFunction @c __module_init que registra todas
     *        las clases del modulo via defclass / deffield / defmethod.
     */
    void generate_module_init_function(ir::IrModule &out);

    /**
     * @brief Exporta @c TypeChecker::class_layouts_ al @c IrModule::classes.
     *
     * Convierte el modelo interno del frontend Vesta a la representacion
     * portable del IR.  Cada @c ClassLayout produce un @c ir::IrClass
     * con sus fields/methods/super/interfaces.  Esta info la consumen
     * los transpilers (port-C, port-Java, ...) para emitir POO eficiente
     * sin tener que reconstruir el modelo desde @c __module_init.
     *
     * Clases @c is_runtime_predefined (FatalError, etc.) se omiten:
     * el runtime las define y los transpilers no deben re-emitirlas.
     */
    void export_classes_to_ir(ir::IrModule &out);

    /**
     * @brief Lower de @c this -> primer parametro del metodo en curso.
     */
    ir::IrValueId lower_this_expr(ast::ThisExpr *e);

    /**
     * @brief Lower de @c obj.field (lectura) cuando @c obj es CLASS.
     *        Emite GETFIELD con el offset del ClassLayout.
     */
    ir::IrValueId lower_class_field_load(ast::FieldAccessExpr *e);

    /**
     * @brief Lower de @c obj.field = v cuando @c obj es CLASS.
     *        Emite SETFIELD.
     */
    ir::IrValueId lower_class_field_store(ast::FieldAccessExpr *target,
                                          ir::IrValueId rhs,
                                          const SourceLoc &loc);

    /**
     * @brief Lower de @c obj.method(args) cuando @c obj es CLASS.
     *        Emite CALLVIRT con el indice del metodo en la vtable.
     */
    ir::IrValueId lower_class_method_call(ast::CallExpr *e);

    /**
     * @brief Lower de @c s.method(args) cuando @c s es STRUCT
     *        (value-type).  Emite CALL directo a @c <Struct>__<metodo>
     *        pasando la direccion del struct como primer argumento
     *        (@c this).  Sin vtable: dispatch estatico.  Soporta SRET
     *        (Optional/Result) con retbuf hidden tras @c this.
     */
    ir::IrValueId lower_struct_method_call(ast::CallExpr *e);

    /**
     * @brief Calcula el puntero al elemento indexado (base + i*sizeof(*base)).
     *
     * Helper compartido por @c lower_index (lectura) y la rama IndexExpr
     * de @c lower_assign (escritura).  Devuelve un IrValueId tipo PTR.
     */
    ir::IrValueId lower_index_addr(ast::IndexExpr *e);

    /**
     * @brief Tamano en bytes del tipo Vesta (consulta layout para STRUCT).
     *
     * @return Tamano del tipo, o 0 si no se puede determinar (e.g. void
     *         o struct desconocido).
     */
    size_t size_of_type(const Type &t) const;

    /**
     * @brief Calcula el IrValueId del puntero al campo @c e->field_name.
     *
     * Helper compartido por @c lower_field_access (lectura) y por la
     * rama FieldAccessExpr de @c lower_assign (escritura).  Suma el
     * offset del campo (resuelto via @c TypeChecker::struct_layouts())
     * al puntero base del struct.
     *
     * @return IrValueId de tipo @c PTR apuntando al campo, o
     *         @c IR_NO_VALUE si la base no era un struct valido.
     */
    ir::IrValueId lower_field_addr(ast::FieldAccessExpr *e);

    /**
     * @brief Construye un IrInstr binario en el bloque actual.
     *
     * Helper compartido por lower_binary() y lower_assign() (compound
     * assignments).  Decide el opcode por categoria flotante/integral y
     * con/sin signo, alineando el comportamiento entre asignaciones
     * compuestas y operaciones binarias normales.
     *
     * @param op       Opcode aritmetico/bitwise (mapeado a ADD/FADD/AND/etc.).
     * @param lhs_val  Valor izquierdo (ya bajado).
     * @param rhs_val  Valor derecho (ya bajado).
     * @param common   Tipo comun resultante (ya promovido).
     * @param loc      SourceLoc para anotacion de linea.
     * @return IrValueId del resultado.
     */
    ir::IrValueId emit_binop_ir(ast::BinOp op, ir::IrValueId lhs_val,
                                ir::IrValueId rhs_val, PrimitiveKind common,
                                const SourceLoc &loc);

    /**
     * @brief Si @p name es un builtin (println, print) emite la llamada
     *        FFI correspondiente a vesta_io y devuelve el IrValueId del
     *        retorno (IR_NO_VALUE si la firma es void).
     *
     * Devuelve un valor distinto de "false" mediante un puntero al
     * IrValueId asignado solo cuando se reconocio el builtin; en otro
     * caso devuelve @c std::nullopt.  Los argumentos ya estan en el AST
     * no bajados; el helper se encarga de emitir todo (inclusive la
     * pre-bajada de cada arg y el registro de los datos estaticos / imports).
     *
     * @return IR_NO_VALUE si el builtin coincide y se emitio (caso typico
     *         println/print devuelven void) o un IrValueId valido si en
     *         el futuro hay builtins que devuelven valor.  Devuelve
     *         IR_NO_VALUE - 1 (centinela) si el nombre NO es un builtin
     *         y el caller debe seguir con la ruta normal de CALL.
     */
    bool try_lower_builtin_call(ast::CallExpr *e, ir::IrValueId &out_value);

    // -----------------------------------------------------------------
    // Tabla de simbolos para variables locales y parametros.
    // -----------------------------------------------------------------

    void push_scope();
    void pop_scope();
    void bind(const std::string &name, ir::IrValueId v);
    ir::IrValueId lookup(const std::string &name) const;

    /**
     * @brief Actualiza el valor SSA asociado a @p name en el scope mas
     *        cercano que ya lo tenga definido.
     *
     * Si la variable no existe en ningun scope, ejecuta @c bind() en el
     * scope mas interno como fallback (no deberia ocurrir si el type
     * checker corrio antes; protege en caso de programas malformados).
     *
     * Es la primitiva basica del modelo SSA-construction de Braun:
     * para cada asignacion `x = e` anotamos un nuevo IrValueId como
     * "current value de x" en el scope donde x esta declarada.  Al
     * leer x mas tarde, lookup() devolvera ese ultimo valor.  Los
     * loops insertan PHI nodes en sus bloques header al sellar.
     */
    void update_scope(const std::string &name, ir::IrValueId v);

    /**
     * @brief Recorre el cuerpo de una funcion buscando @c &x donde x es
     *        un IdentExpr local, y rellena @c address_taken_locals_.
     *
     * Se llama una vez al inicio de @c lower_function antes de bajar el
     * body.  El conjunto resultante guia las decisiones de @c lower_var_decl
     * (ALLOCA en lugar de SSA), @c read_local y @c write_local
     * (LOAD/STORE en lugar de scope-update).
     */
    void scan_address_taken(ast::Stmt *s);

    /**
     * @brief Lectura de una variable local respetando promocion address-taken.
     *
     * Si @p name esta marcada como address-taken (@c address_taken_locals_),
     * emite un LOAD desde la direccion guardada en scope.  En caso
     * contrario devuelve el IrValueId SSA actual (lookup directo).
     *
     * @param name        Nombre de la variable.
     * @param ir_ty       Tipo IR esperado (para el LOAD; usado solo si
     *                    address-taken).
     * @param source_line Linea fuente para anotar la instruccion.
     * @return IrValueId con el valor leido.  IR_NO_VALUE si la variable
     *         no esta declarada.
     */
    ir::IrValueId read_local(const std::string &name, ir::IrType ir_ty,
                             uint32_t source_line);

    /**
     * @brief Escritura a una variable local respetando promocion address-taken.
     *
     * Si address-taken: emite STORE a la direccion del ALLOCA y deja
     * el value SSA del scope intacto (sigue apuntando a la addr).
     * Si no address-taken: actualiza el scope con el nuevo IrValueId.
     */
    void write_local(const std::string &name, ir::IrValueId v, ir::IrType ir_ty,
                     uint32_t source_line);

    // Helper para reportar features no soportadas por el lowering.
    void unsupported(SourceLoc loc, const char *feature);

    void error_at(SourceLoc loc, std::string msg);

    // -----------------------------------------------------------------
    // Datos.
    // -----------------------------------------------------------------

    ast::ModuleNode &mod_;
    const TypeChecker &tc_;
    Diagnostics &diags_;

    // contadores de @Macros lowered al IR.  Diagnostico
    // para que el desarrollador sepa cuantos @Macros se beneficiaron
    // del lowering y cuantos cayeron al evaluator AST por features
    // todavia no soportadas (introspect, comptime var, etc.).
    uint32_t macro_lowered_count_ = 0;
    uint32_t macro_skipped_count_ = 0;

    /// por cada @Macro que el lowering rechazo (usa
    /// builtins comptime-only no aliasables, comptime globals, etc.),
    /// guarda @c (macro_name, reason).  El compiler los propaga al
    /// @c CompileResult y main.cpp los imprime via
    /// @c VESTA_MC_VERBOSE para que el usuario entienda por que
    /// ciertos macros no se benefician del path VM.
    std::vector<std::pair<std::string, std::string>> macro_skip_reasons_;

  public:
    uint32_t macro_lowered_count() const noexcept {
        return macro_lowered_count_;
    }
    uint32_t macro_skipped_count() const noexcept {
        return macro_skipped_count_;
    }
    const std::vector<std::pair<std::string, std::string>> &
    macro_skip_reasons() const noexcept {
        return macro_skip_reasons_;
    }

  private:
    // Nombre del fichero fuente actual (para warnings que solo
    // tienen un source_line).  Se infiere del primer AST node con
    // loc no vacio durante run() y se mantiene durante todo el
    // lowering del modulo.  Si no se puede inferir, queda vacio
    // y los warnings se imprimen sin prefijo de fichero (siguen
    // siendo utiles porque contienen line:col).
    std::string current_file_;

    // Estado por funcion en curso.
    ir::IrModule *out_mod_ =
        nullptr; ///< Modulo IR de salida (para static_data e imports).
    ir::IrFunction *fn_ = nullptr; ///< Funcion en construccion.
    ir::IrBlockId current_block_ =
        ir::IR_NO_BLOCK; ///< Bloque actual donde insertar.
    bool block_terminated_ =
        false; ///< true si current_block_ ya tiene terminador.

    // Tabla de simbolos local: cada scope mapea nombre -> IrValueId.
    std::vector<std::unordered_map<std::string, ir::IrValueId>> scopes_;

    // Para CALL necesitamos saber el tipo de retorno de cada funcion;
    // el type checker ya valido las llamadas, asi que aqui solo
    // mantenemos un cache nombre -> IrType.
    std::unordered_map<std::string, ir::IrType> fn_return_types_;

    /// FFI declarativo: nombre de funcion -> libreria nativa
    /// (e.g. "user32.dll", "kernel32.dll" o "stdlib/native/io/vesta_io").
    /// Se llena en el pase 1 al recorrer @c ExternFnDecl.  Si una entrada
    /// existe para el callee de @c lower_call, emitimos
    /// @c CALLN @Method("<lib>:<name>") con args en R1..RN y registramos
    /// el import via @c out_mod_->register_native_import.  Sin entry,
    /// el flujo normal CALLVM (funcion Vesta local) sigue intacto.
    std::unordered_map<std::string, std::string> extern_lib_by_fn_name_;

    /// Externs cuyo `&fn` (o promocion a cfn) se uso como function value.
    /// Para cada uno generamos un thunk Vesta `__cfnthunk_<fn>` que reenvia
    /// al CALLN nativo, asi el cfn es invocable por CALLIND en cualquier
    /// backend (la direccion nativa no se puede llamar por callvmr directo).
    std::unordered_set<std::string> extern_cfn_thunks_;

    /// ADTs: nombre del enum que la funcion retorna (vacio si
    /// no retorna enum declarado).  Se usa en lower_call para
    /// alocar el retbuf con @c enum_layouts_[name].size_bytes y en
    /// lower_function para configurar @c sret_active_/@c sret_buf_size_.
    std::unordered_map<std::string, std::string> fn_ret_enum_name_;

    /// (gap O cerrado): conjunto de funciones que retornan un
    /// valor de tipo FUNCTION (function value).  Se trata como SRET
    /// con buf_size=16 (mismo layout que el slot de lambda: fn_addr
    /// en +0, env_addr en +8).  El env block apuntado por env_addr
    /// se aloca via RAW_ALLOC (heap) en lugar de ALLOCA (stack)
    /// cuando estamos dentro de una de estas funciones, asi el env
    /// sobrevive al RET y la closure retornada es invocable por el
    /// caller sin use-after-free.
    std::unordered_set<std::string> fn_returns_function_;

    /// Funciones cuyo return type es @c unique<T> o @c shared<T>.
    /// Igual que @c fn_returns_function_ pero con buffer SRET de 8
    /// bytes (slot del smart pointer).  El @c return p en el body
    /// copia los 8 bytes del slot local al retbuf del caller; el
    /// caller bindea la variable receptora directamente al retbuf,
    /// donde ya viven los datos correctos.  Cleanup del local NO
    /// se emite (escape detection lo detecta via "return ident"),
    /// pero el caller registra cleanup sobre el retbuf.
    std::unordered_set<std::string> fn_returns_smartptr_;

    /// Funciones que en @c native_poo_ (AOT Embed/Bare) declaran
    /// devolver `string`.  En native el `string` es VALUE-TYPE de 24
    /// bytes {ptr,len,cap}; un retorno por valor debe usar el ABI SRET
    /// (igual que un struct de 24 bytes): el caller aloca un retbuf de
    /// 24 bytes y lo pasa como primer arg hidden; el callee copia el
    /// value-string al retbuf y transfiere su ownership (no libera en
    /// el callee, lo posee el caller via su RAII).  Sin esto el callee
    /// retornaria en RAX un PTR a su slot local de 24 bytes que muere
    /// al RET -> basura -> segfault.  El path Full/JIT NO usa esto:
    /// ahi `string` es un GcHandle i64 retornado en registro.
    std::unordered_set<std::string> fn_returns_str_value_;

    /// indicador activo durante el lowering del body de una
    /// funcion que retorna FUNCTION.  Disparado en @c lower_function
    /// y consultado por @c lower_lambda_expr para alocar el env
    /// block en heap raw en lugar de stack.  Se restaura al salir.
    bool current_fn_returns_function_ = false;

    /// Activo mientras se baja un lambda-literal que se ALMACENA en un campo /
    /// slot de array / deref (escapa del scope actual a un objeto que puede
    /// sobrevivir al frame).  Disparado por @c lower_assign y consultado por
    /// @c lower_lambda_expr para alocar el env en heap (GC) en lugar de stack.
    bool current_lambda_store_escapes_ = false;

    /// Indica que la funcion actual declara devolver `string` a nivel
    /// fuente.  El IR tipo es I64 (handle a StringObject) por lo que
    /// el flag es necesario para auto-promover en `return "..."` el
    /// literal a StringObject via STRMAKE (mismo patron que
    /// `lower_var_decl` para `string s = "lit"`).  Se restaura al
    /// salir de `lower_function`.
    bool current_fn_returns_string_ = false;

    /// true mientras se baja el body de una funcion que en @c native_poo_
    /// retorna `string` (value-type) por SRET.  El @c lower_return usa este
    /// flag para construir el value-string nativo del `return <expr>` (p.ej.
    /// un literal -> build_native_string_from_literal) ANTES de copiar los
    /// 24 bytes al retbuf.  Sin el, `return "lit"` copiaria los bytes crudos
    /// de static_data en lugar de un {ptr,len,cap}.  Se restaura al salir.
    bool current_fn_sret_str_value_ = false;

    /// true si la funcion actual contiene algun `try { } catch`
    /// statement.  Se rellena con un pre-pase simple en lower_function.
    /// Cuando es true, NO emitimos el cleanup automatico release_handle
    /// para CLASS sin destructor: el bytecode tryenter/throw no preserva
    /// los GP regs, asi que un cleanup que lea un reg con el binding del
    /// local podria leer garbage si el catch handler corrio antes.
    /// Para funciones SIN try el cleanup es seguro y libera handles
    /// deterministicamente al exit del scope.
    bool current_fn_has_try_ = false;

    /// true si la funcion actual contiene algun loop
    /// (while/for/do-while).  Se rellena con un pre-pase en
    /// lower_function.  Cuando es true, el lower_var_decl marca
    /// AUTOMATICAMENTE como address-taken las vars CLASS / I64-GC
    /// declaradas en el top-level del body (depth scope <= 2).
    ///
    /// Razon: el regalloc puede clobbar el reg de una var GC del
    /// outer scope durante el body del loop (e.g. `newobj r1` reusa
    /// r1 que tenia `owned`).  Address-taken fuerza ALLOCA + STORE/
    /// LOAD por uso, garantizando que el binding sobreviva al loop.
    ///
    /// Esto permite que el cleanup CALL_DTOR / RAW_ASM al RET de la
    /// funcion lea del stack en lugar de un reg potencialmente
    /// corrupto.  Tambien habilita que el cleanup scope-local sea
    /// seguro.
    ///
    /// Coste: 1 STORE inicial + 1 LOAD por uso (~2 instr extra).
    /// Solo aplica a vars CLASS/GC en funciones con loops; el resto
    /// del codigo no tiene overhead.
    bool current_fn_has_loops_ = false;

    /// @c true cuando estamos lowereando el body de
    /// un @Macro.  Se usa para forzar que las VarDecl marcadas
    /// @c is_comptime se bajen como vars runtime regulares (el
    /// macro corre en VM; los locales se computan en cada
    /// invocacion).  Reset al entrar/salir de cada funcion.
    bool current_fn_is_macro_ = false;

    /// cache `name -> static_data_idx` para comptime
    /// globals referenciados por @Macros lowereados.  Cada global
    /// se materializa como un slot de 8 bytes en static_data del
    /// .velb, inicializado con el valor compile-time.  Los macros
    /// leen/escriben via @c STR_LIT_ADDR + LOAD/STORE i64.  El AST
    /// evaluator mantiene su propia copia en
    /// @c TypeChecker::comptime_const_values_ -- son dos espacios
    /// de memoria distintos pero cada pase del two-phase compile
    /// se mantiene consistente internamente.
    std::unordered_map<std::string, uint64_t> comptime_global_slots_;
    /// L2.2: slots para globales runtime no-const (string/int/etc.)
    std::unordered_map<std::string, uint64_t> runtime_global_slots_;

    /// thread_local con init != 0: (slot static_data, valor inicial 8B LE).  El
    /// lowering sintetiza __vx_tls_init (TLS callback del PE) que escribe estos
    /// valores en la copia por-hilo al attach del hilo -- el cargador de Windows
    /// no siempre copia la plantilla del TLS de una .dll a un consumidor minimal.
    std::vector<std::pair<uint64_t, uint64_t>> tls_nonzero_inits_;

    /// sret en call sites: cache nombre-de-funcion -> PrimitiveKind
    /// del tipo de retorno semantico (antes de la transformacion sret).
    /// Solo nos interesa distinguir OPTIONAL / RESULT del resto, porque
    /// solo esos dos kinds disparan el alocado del retbuf en el caller.
    /// Las claves que NO estan en el mapa o cuyo valor no sea
    /// OPTIONAL/RESULT corresponden a calls "normales" (la firma IR del
    /// callee usa @c fn_return_types_ y el resultado del CALL es el
    /// valor devuelto directamente).
    std::unordered_map<std::string, PrimitiveKind> fn_ret_kind_;

    /// Variables locales cuya direccion se ha tomado con '&' en alguna
    /// parte de la funcion actual.  Se rellena con scan_address_taken al
    /// inicio de lower_function y se limpia al terminarla.  Las entradas
    /// disparan ALLOCA en lower_var_decl y LOAD/STORE en read/write_local.
    std::unordered_set<std::string> address_taken_locals_;

    /// locales cuyo handle escapa del scope: se devuelven via
    /// @c return id, se asignan a un campo (@c this.x = id, @c obj.x = id,
    /// @c *p = id) o a un slot de array (@c arr[i] = id).  Para esos
    /// locales NO registramos auto-free al exit del scope porque el
    /// caller (o el padre) toma posesion del handle y debera liberarlo
    /// (o el GC lo gestionara via stack scanning conservativo).
    ///
    /// Conservador: si algun uso del local podria escapar segun los
    /// patrones detectados por @c scan_escaping_locals, se marca como
    /// escaping y queda fuera del cleanup automatico.  Falsos positivos
    /// (escape detectado pero no real) producen un leak intencional
    /// que el programador debe liberar via dispose() explicito.
    std::unordered_set<std::string> escaping_locals_;

    /// pre-pase ejecutado al inicio de @c lower_function que
    /// rellena @c escaping_locals_ recorriendo el body.  Reusable como
    /// helper de futuras analizadores de escape mas precisas.
    void scan_escaping_locals(ast::Stmt *body);

    /// Bug D fix: propagar @c is_gc_object a traves de todos los PHI
    /// nodes de la funcion hasta punto fijo.  Llamado al final de cada
    /// lowering de funcion (top-level, class methods, helpers
    /// sintetizados) justo antes de @c IrModule::add_function.
    void propagate_is_gc_object_through_phis(ir::IrFunction &fn);

    /// Limitacion A (cerrada): subset de @c address_taken_locals_ cuyo
    /// contenido es un puntero a memoria HOST (resultado de malloc o
    /// derivado).  Lo registra @c write_local cada vez que el valor
    /// escrito tiene @c is_host_ptr = true.  @c read_local consulta el
    /// set para propagar el bit al SSA value resultante del LOAD; sin
    /// esto, un LOAD de un local address-taken con tipo @c T* siempre
    /// emite @c mov (memoria VM) y corrompe la heap host.
    ///
    /// Es un best-effort sticky: si el local pasa por una asignacion
    /// con @c is_host_ptr = true se queda marcado para siempre.
    /// Asignaciones posteriores con valores VM no lo desmarcan.  En
    /// la practica los locales mantienen su naturaleza host/VM a lo
    /// largo de su vida, asi que esta semantica conservadora basta.
    ///
    /// NO cubre el caso indirecto @c i32** pp = &p; **pp = v.  Para
    /// ese caso haria falta un bit @c pointee_is_host_ptr en
    /// IrValue propagado a traves de @c &x.  Documentado como gap
    /// remanente; requiere acuerdo de diseno antes de implementarse.
    std::unordered_set<std::string> host_bearing_locals_;

    /// Conjunto de clases instanciadas alguna vez via @c new ClassName()
    /// con modificador @c shared (vd->is_shared).  Lo rellena
    /// @c lower_var_decl al detectar el patron.  @c generate_new_helpers
    /// lo consulta para emitir adicionalmente un helper
    /// @c __new_<X>_shared (que internamente usa @c newobjs en lugar de
    /// @c newobj).  Sin esto, una instancia compartida apuntaria al
    /// helper local-only y el child no podria deref el host_ptr.
    std::unordered_set<std::string> classes_used_shared_;

    /// gc<T> opt-in: clases instanciadas como @c gc<Class> -> generar el helper
    /// @c __new_<Class>_gc (aloca con @c vx_gc_alloc + marca is_gc_object, sin
    /// RAII).  El GC (libvesta_gc) colecta lo no alcanzable.
    std::unordered_set<std::string> classes_used_gc_;

    /// true si el modulo registro al menos un finalizador GC (gc<unique>/
    /// gc<shared>/gc<Clase> con recurso interno).  En AOT dispara la inyeccion
    /// de @c vx_gc_finalize_all antes de cada RET de main (cero fuga al exit).
    bool module_has_gc_finalizers_ = false;

    /// Vars locales declaradas con modificador @c shared.  El escape
    /// analyzer de @c spawn las omite del warning "objeto GC local-only
    /// capturado" (declarar @c shared es la solucion sugerida).
    std::unordered_set<std::string> shared_locals_;

    /// Slot stack del @c unique<T>/shared<T> que el caller pasa como
    /// retbuf SRET cuando devuelve smart pointer (signature @c VOID +
    /// retbuf hidden).  Si @c lower_return detecta que el return value
    /// es un @c CallExpr a @c unique_box/shared_box/_with, asigna este
    /// slot al lowering del builtin para que el smart pointer se
    /// construya IN-PLACE en el retbuf sin copia qword-a-qword al final.
    /// @c IR_NO_VALUE = sin in-place SRET (lowering normal).
    ir::IrValueId unique_box_target_slot_ = ir::IR_NO_VALUE;

    /// SSA values de las capturas del spawn body, en el orden en que
    /// fueron resueltas en el caller (sus nombres viven en
    /// @c spawn_captured_names_).  Usado por @c generate_spawn_helper
    /// para propagar @c is_host_ptr / @c is_gc_object a los params del
    /// helper hijo y por el escape analyzer del spawn capture.
    std::vector<ir::IrValueId> spawn_captured_ssa_values_;

    /// Nombres de las capturas del spawn body, paralelo a
    /// @c spawn_captured_ssa_values_.
    std::vector<std::string> spawn_captured_names_;

    /// Wrapper publico para que @c collect_spawn_captures_in_expr (que
    /// vive como helper estatico) pueda resolver un nombre en TODOS
    /// los scopes activos del lowering.  Equivale a @c lookup(name)
    /// pero accesible desde el contexto estatico.
    /// @return @c IrValueId del binding o @c IR_NO_VALUE si no existe.
    ir::IrValueId spawn_capture_resolve(const std::string &name);

    /// Emite la instruccion @c MVTAKE_IR (move-and-take) que copia un
    /// qword desde @c [v_src_addr] a @c [v_dst_addr] y zerifica el
    /// slot fuente en una sola operacion atomica.  Usado por
    /// @c move(p) sobre smart pointers para implementar move-ownership
    /// con la garantia de que la fuente queda invalidada.
    void emit_mvtake(ir::IrValueId v_dst_addr, ir::IrValueId v_src_addr,
                     uint32_t source_line);

    /// Emite GC_SET_FINALIZER %box, imm=kind.  Registra (kind 1/2/3) o
    /// desregistra (kind 0) el finalizador GC de un box con recurso interno.
    /// Para kind==3 (CLASS_DTOR) pasar @p v_dtor_addr con el vaddr del
    /// <Clase>____dtor concreto (dispatch estatico).
    void emit_gc_set_finalizer(ir::IrValueId v_box, uint32_t kind,
                               uint32_t source_line,
                               ir::IrValueId v_dtor_addr = ir::IR_NO_VALUE);

    /// Emite @c IrOp::LABEL_ADDR que se interpreta en el bajado a .vel
    /// como @c @Absolute("code.<label_name>"), produciendo la direccion
    /// absoluta del label resuelta por el linker.  Util para invocar
    /// helpers sintetizados, slots estaticos, etc.
    ir::IrValueId emit_label_addr(const std::string &label_name, uint32_t line);

    /// Construye en stack un @c FindClassParams (name_addr + name_len),
    /// invoca @c IrOp::FINDCLASS y devuelve el SSA value con el
    /// @c ClassInfo* resuelto (host_ptr).  @p name_idx referencia el
    /// slot de strings @c "s_<idx>" emitido previamente.
    ir::IrValueId emit_findclass_by_name(uint64_t name_idx, uint32_t name_len,
                                         uint32_t line);

    /// Emite @c IrOp::GC_HANDLE_FOR_PTR que toma un host_ptr al payload
    /// de un objeto GC y devuelve su @c GcHandle (uint32 zero-extended
    /// a i64).  Util cuando una operacion runtime requiere el handle
    /// (monitor, weak ref, drop) en lugar del puntero directo.
    ir::IrValueId emit_gc_handle_for_ptr(ir::IrValueId v_host_ptr,
                                         uint32_t source_line);

    // Monitor enter/exit.  En native_poo_ (AOT) baja a CALL nativo
    // (__vx_monenter/__vx_monexit, bundle-ado desde stdlib/vx/vx_sync.vx)
    // sobre el host_ptr del objeto; en el resto de tiers emite la IR op
    // MONENTER/MONEXIT (handle) que el runtime/JIT consume.
    void emit_monitor_op(ir::IrValueId v_obj_or_handle, bool enter,
                         uint32_t source_line);

    // --- Helpers de operaciones sobre cadenas (StringObject) ---
    ir::IrValueId emit_strmake(ir::IrValueId v_buf, ir::IrValueId v_len,
                               uint32_t source_line);
    /// Construye un `string` desde un literal (addr+len) eligiendo el repr
    /// segun el tier: value-string nativo (AOT, PURE_NATIVE, SSO) en
    /// native_poo_, o STRMAKE (GcHandle, Full/JIT/interp) en otro caso.  Usado
    /// por los builtins de introspeccion comptime (typename/underlying_of/...)
    /// que antes emitian STRMAKE incondicional -> RUNTIME_DEPENDENT en AOT.
    /// @p known_len = longitud compile-time (>=0) o -1 si solo se sabe en
    /// runtime (el value-string decide SSO/heap con una rama).
    ir::IrValueId emit_string_literal_repr(ir::IrValueId v_addr,
                                           ir::IrValueId v_len,
                                           int64_t known_len,
                                           uint32_t source_line);
    ir::IrValueId emit_strcat(ir::IrValueId v_a, ir::IrValueId v_b,
                              uint32_t source_line);
    ir::IrValueId emit_strraw(ir::IrValueId v_str, uint32_t source_line);
    ir::IrValueId emit_strconv(ir::IrValueId v_str, uint64_t enc_imm,
                               uint32_t source_line);
    ir::IrValueId emit_strgetbytes(ir::IrValueId v_str, uint32_t source_line);

    // --- Vesta Embed Inc 0: string value-type (solo native_poo_) ---
    /// Construye el repr value-string {ptr,len,cap} (24 bytes) en stack
    /// (ALLOCA) desde un literal: aloca buffer en heap (RAW_ALLOC len+1),
    /// copia los bytes del literal + nul final, y escribe los 3 campos
    /// del slot.  Devuelve el PTR al slot de 24 bytes (el "valor" del
    /// string, igual que un struct value-type).  Solo se usa en
    /// @c native_poo_ (AOT Embed/Bare); el path Full usa StringObject GC.
    ir::IrValueId build_native_string_from_literal(ast::StringLitExpr *slit,
                                                   uint32_t source_line);
    /// Vesta Embed: construye un value-string {ptr,len,cap} (24 bytes) en
    /// stack desde un valor @c char en runtime (@p v_char).  Aloca un
    /// buffer de 2 bytes (RAW_ALLOC), escribe el byte del char en
    /// buf[0] + nul en buf[1], y rellena los campos len=1, cap=2.
    /// Devuelve el PTR al slot.  Usado por el cast @c (string)<char>.
    /// Solo en @c native_poo_ (AOT Embed/Bare).
    ir::IrValueId build_native_string_from_char(ir::IrValueId v_char,
                                                uint32_t source_line);
    /// Carga el campo @p byte_off (0=ptr, 8=len, 16=cap) del slot
    /// value-string @p v_slot.  @p as_host marca el resultado como
    /// host_ptr (para el ptr@0 que viene de RAW_ALLOC).
    ir::IrValueId load_native_string_field(ir::IrValueId v_slot,
                                           uint64_t byte_off, bool as_host,
                                           uint32_t source_line);
    /// String Inc 5 (SSO): accesores flag-aware del value-string nativo.
    /// Layout union de 24 bytes con flag en el bit alto (0x80) del byte
    /// [23]:
    ///   HEAP (byte[23]&0x80 != 0): ptr@0 (8B), len@8 (8B), cap en
    ///     bytes[16..22] (56 bits), byte[23] bit alto=1.
    ///   SSO  (byte[23]&0x80 == 0): data en bytes[0..21] (max 22), nul en
    ///     byte[len], len en byte[23] bits bajos (0..22).
    /// @c emit_native_str_is_heap devuelve un I64 (0 o 1) = (byte[23]>>7).
    /// @c emit_native_str_data_ptr devuelve el host_ptr a los bytes: si
    /// HEAP -> LOAD ptr@0; si SSO -> &slot (la data vive inline en
    /// offset 0).  Branchless via mascara (slot + is_heap*(ptr0 - slot)).
    /// @c emit_native_str_len devuelve la longitud: si HEAP -> LOAD len@8;
    /// si SSO -> byte[23]&0x7F.  Branchless.  TODAS las ops de string
    /// usan estos accesores en vez de leer ptr@0/len@8 crudos.
    ir::IrValueId emit_native_str_is_heap(ir::IrValueId v_slot,
                                          uint32_t source_line);
    /// @c emit_native_str_data_ptr / @c emit_native_str_len emiten una CALL a
    /// los helpers @c __vx_strdata / @c __vx_strlen (una sola instruccion por
    /// uso).  La logica branchless (AND-mask heap/SSO) vive en el cuerpo del
    /// helper (@c *_inline), NO inline en cada call site: cada accesor inline
    /// expandia ~10 instrs, y sumar 4 longitudes + 4 punteros en una funcion
    /// reventaba el regalloc SysV (menos callee-saved que Win64) -> resultado
    /// erroneo en ELF.  Mismo patron que @c __vx_strcmp / itoa.  Los helpers
    /// estan en el blacklist del inliner (prefijo @c __vx_str) para no
    /// re-inlinearse.
    ir::IrValueId emit_native_str_data_ptr(ir::IrValueId v_slot,
                                           uint32_t source_line);
    ir::IrValueId emit_native_str_len(ir::IrValueId v_slot,
                                      uint32_t source_line);
    /// Cuerpos branchless de los accesores (emitidos dentro del helper).
    ir::IrValueId emit_native_str_data_ptr_inline(ir::IrValueId v_slot,
                                                  uint32_t source_line);
    ir::IrValueId emit_native_str_len_inline(ir::IrValueId v_slot,
                                             uint32_t source_line);
    /// Construyen (lazy, una vez) los helpers @c __vx_strdata / @c __vx_strlen
    /// y devuelven su nombre.  Firma: @c u8* __vx_strdata(u8* s) /
    /// @c i64 __vx_strlen(u8* s).
    std::string ensure_strdata_helper();
    std::string ensure_strlen_helper();
    /// Vesta Embed Inc 6 (encoding UTF-8): @c .length() cuenta CODE-POINTS (no
    /// bytes; @c .bytes() da los bytes via @c emit_native_str_len).  El helper
    /// @c __vx_str_cplen(u8* p, i64 byte_len) -> i64 recorre los bytes y suma
    /// 1 por cada byte que NO sea continuacion UTF-8 ((b & 0xC0) != 0x80).
    /// Para ASCII coincide con el conteo de bytes (cero cambio en los tests
    /// ASCII existentes).  @c emit_native_str_cplen emite la CALL.
    std::string ensure_str_cplen_helper();
    ir::IrValueId emit_native_str_cplen(ir::IrValueId v_ptr, ir::IrValueId v_blen,
                                        uint32_t source_line);
    /// Vesta Embed Inc 6: @c .wstr() devuelve un @c u16* NUL-terminado en
    /// UTF-16LE para FFI Win32 @c *W.  El helper
    /// @c __vx_str_to_utf16(u8* p, i64 byte_len) -> u16* aloca un buffer
    /// (@c RAW_ALLOC -> malloc/override), decodifica UTF-8 -> UTF-16 (pares
    /// suplentes para code-points astrales) y lo NUL-termina.  El CALLER es
    /// dueno del buffer (transitorio para FFI; liberar o aceptar la fuga en
    /// uso efimero, como en C).
    std::string ensure_str_to_utf16_helper();
    ir::IrValueId emit_native_str_to_utf16(ir::IrValueId v_ptr,
                                           ir::IrValueId v_blen,
                                           uint32_t source_line);
    /// String Inc 5 (SSO): libera el buffer del value-string SOLO si esta
    /// en modo HEAP (la data SSO es inline, no se libera).  Branchless:
    /// RAW_FREE(ptr0 * is_heap); free(0) es no-op.  Reemplaza el patron
    /// directo RAW_FREE(LOAD ptr@0) en TODOS los sitios de liberacion de
    /// strings nativos (cleanup STRING_FREE + frees de temporales).
    void emit_native_str_free_if_heap(ir::IrValueId v_slot,
                                      uint32_t source_line);
    /// String Inc 5 (SSO): tras un MOVE de @p v_slot (la data ya se copio
    /// al destino), invalida la fuente para evitar doble-free.  Si era
    /// HEAP -> escribe ptr@0=0 (su free posterior sera no-op); si era SSO
    /// -> deja byte[0..7] intacto (es data inline; no hay buffer que
    /// liberar y su free-if-heap ya salta).  Branchless: escribe
    /// ptr@0 = old_ptr0 & (is_heap - 1)  (HEAP: &0 -> 0; SSO: &~0 -> sin
    /// cambio).
    void emit_native_str_invalidate_moved(ir::IrValueId v_slot,
                                          uint32_t source_line);
    /// String Inc 5 (SSO): zero-inicializa los 24 bytes del slot
    /// value-string (3 STORE i64 = 0).  Evita que los accesores
    /// flag-aware lean bytes no inicializados del slot (ptr@0/len@8) en
    /// modo SSO -- valgrind los marcaria como "uninitialised value" aunque
    /// el resultado enmascarado sea correcto.  Llamar tras cada ALLOCA de
    /// 24 bytes de un value-string ANTES de escribir la data.
    void emit_zero_native_str_slot(ir::IrValueId v_slot, uint32_t source_line);
    /// String Inc 5 (SSO): escribe qword2 (bytes 16..23) del slot con UN
    /// STORE i64 entero -- evita el solape parcial i64(cap)+u8(flag) que el
    /// store-forwarding del optimizer mal-resuelve al hacer el move (LOAD
    /// i64 de offset 16).  @c emit_str_meta_sso pone (len << 56): byte[23]=
    /// len, bit alto 0 (SSO), bytes 16..22 = 0.  @c emit_str_meta_heap pone
    /// (cap & 0x00FFFFFFFFFFFFFF) | (0x80 << 56): cap en bytes 16..22 (56b),
    /// byte[23]=0x80 (flag HEAP).  @p v_len_or_cap es un IrValue I64.
    void emit_str_meta_sso(ir::IrValueId v_slot, ir::IrValueId v_len,
                           uint32_t source_line);
    void emit_str_meta_heap(ir::IrValueId v_slot, ir::IrValueId v_cap,
                            uint32_t source_line);
    /// String Inc 5 (SSO): copia los 24 bytes de un value-string de
    /// @p v_src_slot a @p v_dst_slot via MEMCPY (rep movsb) en vez de 3
    /// LOAD/STORE i64.  Evita el store-forwarding del optimizer sobre
    /// qword2 (data inline + byte[23] escritos con stores parciales) que
    /// los i64 LOADs mal-resolvian (perdian la longitud SSO en el move).
    void emit_native_str_move_copy(ir::IrValueId v_dst_slot,
                                   ir::IrValueId v_src_slot,
                                   uint32_t source_line);
    /// String Inc 5 (SSO): rellena el slot value-string @p v_slot (24
    /// bytes ya alocados) a partir de unos bytes recien producidos:
    /// @p v_src_ptr (host_ptr a la fuente) + @p v_len (longitud runtime).
    /// Decide SSO vs HEAP EN RUNTIME via branch: si len <= 22 copia la
    /// data INLINE a bytes[0..len) + nul + byte[23]=len (cero malloc); si
    /// len > 22 hace RAW_ALLOC(len+1), MEMCPY, nul, set ptr@0/len@8/
    /// cap@16 + byte[23] bit alto.  Usado por concat/slice/append/interp
    /// para obtener SSO en resultados runtime cortos.  Solo native_poo_.
    /// @p known_len >= 0 (Tier B str_make): la longitud es constante en
    /// compile-time -> decide SSO/HEAP SIN rama runtime (emite solo el cuerpo
    /// aplicable).  -1 = longitud runtime (rama CMP_GT como antes).
    void build_native_string_finalize(ir::IrValueId v_slot,
                                      ir::IrValueId v_src_ptr,
                                      ir::IrValueId v_len,
                                      uint32_t source_line,
                                      int64_t known_len = -1);
    /// str_make optimo (Vesta Embed): COPIA @p v_len bytes de @p v_ptr a un
    /// value-string PROPIO (slot 24B + buffer; RAII lo libera).  Sin GC.
    /// @p known_len >= 0 -> especializa (Tier B sin rama).  Solo native_poo_.
    ir::IrValueId build_native_string_from_buffer(ir::IrValueId v_ptr,
                                                  ir::IrValueId v_len,
                                                  uint32_t source_line,
                                                  int64_t known_len = -1);
    /// Vesta Embed Inc 1: concatena dos value-strings nativos @p v_a y
    /// @p v_b produciendo un NUEVO string owned (slot de 24 bytes en
    /// stack + buffer fresco en heap de total+1 bytes con ambos
    /// contenidos copiados y nul final).  Devuelve el PTR al slot
    /// resultado; el caller registra su STRING_FREE (es owned).  Solo
    /// en @c native_poo_.  @p v_a / @p v_b son PTR a slots value-string;
    /// no se consumen (el concat copia sus bytes).
    ir::IrValueId build_native_string_concat(ir::IrValueId v_a,
                                             ir::IrValueId v_b,
                                             uint32_t source_line);
    /// String Inc 3 (native_poo_): slice `s[a..b]` -> NUEVO string owned
    /// = copia de los bytes [a, b) del value-string @p v_src.  @p v_lo /
    /// @p v_hi son IrValue I64 (limites a y b).  @p inclusive=true para
    /// `s[a..=b]` (longitud b-a+1).  Aloca slot de 24 bytes en stack +
    /// buffer fresco en heap de (len+1) bytes, MEMCPY (rep movsb) de
    /// [src.ptr+a] por len bytes, nul-termina y rellena ptr/len/cap.  El
    /// caller registra su STRING_FREE (es owned).  v1 asume indices
    /// validos (a <= b <= src.len); negativos no soportados.
    ir::IrValueId build_native_string_slice(ir::IrValueId v_src,
                                            ir::IrValueId v_lo,
                                            ir::IrValueId v_hi, bool inclusive,
                                            uint32_t source_line);
    /// String Inc 3 (native_poo_): indexado simple `s[i]` -> el CHAR
    /// (byte) en la posicion @p v_idx del value-string @p v_src.  Carga
    /// el ptr@0 del slot y emite LOAD u8 de [ptr+i].  Devuelve un U8
    /// zero-extended (0-255).  v1 asume i valido (0 <= i < src.len).
    ir::IrValueId build_native_string_index_char(ir::IrValueId v_src,
                                                 ir::IrValueId v_idx,
                                                 uint32_t source_line);
    /// Copia @p v_len bytes desde @p src_base a @p dst_base con un loop de
    /// PALABRA: cuerpo principal de 8 bytes por iteracion (LOAD/STORE i64)
    /// mas un loop de cola para los <8 bytes restantes (LOAD/STORE u8).
    /// ~8x menos iteraciones que el copiado byte-a-byte sin necesitar
    /// registros fijos (rep movsb) -> cero riesgo en el regalloc.  Todas
    /// las ops son PURE_NATIVE (LOAD/STORE/ADD/SUB/CMP/BR) por lo que el
    /// codegen vreg-native (HOST_LEAF) las soporta y el interp/Full siguen
    /// funcionando.  @p src_base / @p dst_base son host_ptr; @p v_len es
    /// un IrValue I64 (>= 0).
    void emit_word_copy_loop(ir::IrValueId dst_base, ir::IrValueId src_base,
                             ir::IrValueId v_len, uint32_t source_line);

    // --- Vesta Embed Inc 2: mutacion += + interpolacion (solo native_poo_) ---
    /// Append in-place de @p v_app_len bytes (en @p v_app_ptr, host) al
    /// value-string cuyo slot {ptr,len,cap} apunta @p v_dst_slot.  Crece
    /// el buffer si la capacidad es insuficiente (RAW_ALLOC nuevo de
    /// new_len+1, MEMCPY de lo viejo, RAW_FREE del viejo, actualiza
    /// ptr@0/cap@16), copia los bytes nuevos al final, actualiza len@8 y
    /// nul-termina.  El slot se muta in-place (es owned mutable); NO
    /// crea slot nuevo.  Usado por `s += t` y por la interpolacion
    /// native.  Solo en @c native_poo_.  @p v_app_ptr / @p v_app_len no
    /// se consumen.
    void build_native_string_append_inplace(ir::IrValueId v_dst_slot,
                                             ir::IrValueId v_app_ptr,
                                             ir::IrValueId v_app_len,
                                             uint32_t source_line);
    /// Vesta Embed Inc 2: construye un value-string owned a partir de un
    /// literal interpolado @p slit ("texto ${a} mas ${b}").  Concatena las
    /// partes literales con cada @c ${expr} convertido a texto INLINE
    /// (string -> bytes directos, char -> 1 byte, int -> itoa decimal por
    /// div/mod, bool -> "true"/"false").  Devuelve el PTR al slot del
    /// value-string resultado (owned; el caller registra STRING_FREE).
    /// Solo en @c native_poo_.
    ir::IrValueId build_native_string_interp(ast::StringLitExpr *slit);
    /// Vesta Embed Inc 2: escribe en @p v_buf (host, >= 24 bytes) la
    /// representacion decimal ASCII de @p v_val (un I64).  @p is_signed
    /// controla el manejo del signo (emite '-' si negativo).  Devuelve el
    /// IrValue I64 con la longitud escrita (sin nul).  itoa INLINE via
    /// loop div/mod por 10 + inversion -- sin helper nativo (AOT bare no
    /// tiene plugin).  Solo en @c native_poo_.
    ir::IrValueId emit_native_itoa_to_buf(ir::IrValueId v_buf,
                                          ir::IrValueId v_val, bool is_signed,
                                          uint32_t source_line);
    /// Vesta Embed Inc 2: garantiza que el helper itoa nativo
    /// @c __vx_itoa_s (signed) / @c __vx_itoa_u (unsigned) este emitido
    /// como funcion IR independiente en @c out_mod_ (una sola vez por
    /// modulo y signedness).  Firma: @c (u8* buf, i64 val) -> i64 len.
    /// El cuerpo es el itoa de @c emit_native_itoa_to_buf, pero como
    /// funcion separada con loops -> el optimizer NO lo foldea cuando el
    /// argumento es una constante (el const-fold mid-expression del itoa
    /// INLINE producia longitudes erroneas).  Por tener varios bloques
    /// (loops) el inliner tampoco lo re-inlinea (is_inlineable exige 1
    /// bloque).  Devuelve el nombre del helper.  Solo en @c native_poo_.
    std::string ensure_itoa_helper(bool is_signed);
    /// Flags: el helper itoa signed/unsigned ya esta emitido en este
    /// modulo (indice 0=unsigned, 1=signed).  Evita duplicar la funcion.
    bool itoa_helper_emitted_[2] = {false, false};
    /// Vesta Embed Inc 2: helper bool->string nativo
    /// @c i64 __vx_btoa(u8* buf, i64 b): escribe "true" (4) o "false"
    /// (5) en @p buf y devuelve la longitud.  Como funcion APARTE con
    /// branch -> evita el const-fold mid-expression del append condicional.
    /// Devuelve el nombre del helper.  Solo en @c native_poo_.
    std::string ensure_btoa_helper();
    bool btoa_helper_emitted_ = false; ///< El helper btoa ya esta emitido.
    /// BUG-3 (`${cp:char}` en construccion native/AOT): helper codepoint ->
    /// UTF-8.  Firma @c i64 __vx_ctoa(u8* buf, i64 cp): escribe la
    /// codificacion UTF-8 (1..4 bytes) del codepoint en @p buf y devuelve la
    /// longitud.  Paridad byte-exacta con @c vio_char_to_vmbuf (interp/JIT).
    /// Solo en @c native_poo_.
    std::string ensure_ctoa_helper();
    bool ctoa_helper_emitted_ = false; ///< El helper ctoa ya esta emitido.
    /// Vesta Embed Inc 4: helper de comparacion lexicografica de strings
    /// value-type nativos.  Firma:
    /// @c i64 __vx_strcmp(u8* pa, i64 la, u8* pb, i64 lb).
    /// Devuelve -1/0/1 (memcmp + tie-break por longitud: a la izquierda
    /// del primer byte distinto decide; si un prefijo coincide, el mas
    /// corto es menor).  Como funcion APARTE con loop -> el optimizer no
    /// foldea la comparacion byte-a-byte mid-expression con operandos
    /// constantes (mismo motivo que itoa/btoa) y el inliner no la re-inlinea
    /// (is_inlineable exige 1 bloque).  Devuelve el nombre.  Solo en
    /// @c native_poo_.
    std::string ensure_strcmp_helper();
    bool strcmp_helper_emitted_ = false; ///< El helper strcmp ya esta emitido.
    bool strdata_helper_emitted_ = false; ///< El helper __vx_strdata emitido.
    bool strlen_helper_emitted_ = false; ///< El helper __vx_strlen emitido.
    bool str_cplen_helper_emitted_ = false; ///< El helper __vx_str_cplen emitido.
    bool str_to_utf16_helper_emitted_ = false; ///< __vx_str_to_utf16 emitido.

    /// CPU dispatch (cimiento): asegura que existan el global
    /// @c __vx_cpu_features (slot @c static_data de 8 bytes zero-init) y el
    /// helper @c __vx_cpu_init() que ejecuta @c cpuid al arranque y empaqueta
    /// un bitmask de features en ese slot.  Devuelve el indice del slot del
    /// global (para que @c cpu_features() lo lea via STR_LIT_ADDR + LOAD).
    /// Idempotente.  Solo en @c native_poo_ (AOT Bare/Embed): usa INLINE_ASM
    /// que es PURE_NATIVE.  El bitmask: bit0=SSE2 bit1=SSE4.2 bit2=POPCNT
    /// bit3=AVX bit4=AVX2 bit5=BMI1 bit6=BMI2 bit7=AVX512F bit8=ERMS.
    uint64_t ensure_cpu_features_global();
    bool cpu_init_emitted_ = false; ///< El helper __vx_cpu_init ya emitido.
    bool cpu_features_used_ =
        false; ///< Algun cpu_features() se uso -> wirear init en main.
    uint64_t cpu_features_slot_ =
        UINT64_MAX; ///< Slot static_data del global (UINT64_MAX = sin crear).
    bool cpu_dispatch_used_ =
        false; ///< Se uso ALGUN helper multi-versionado (memcpy dispatch) ->
               ///< wirear __vx_cpu_init en main aunque no se llame
               ///< cpu_features() (el init setea los fp).

    /// CPU dispatch (Inc 2): mecanismo de despacho por TABLA DE PUNTEROS.
    /// Asegura el global @c __vx_memcpy_fp (slot @c static_data de 8 bytes,
    /// seccion ".data") + las dos variantes @c __vx_memcpy_base (rep movsb,
    /// segura) y @c __vx_memcpy_avx2 (AVX2 32B + cola byte-a-byte).  El helper
    /// @c __vx_cpu_init setea el fp a la mejor variante segun el bit AVX2.
    /// Devuelve el indice del slot del global @c __vx_memcpy_fp.  Idempotente.
    /// Solo en @c native_poo_ (AOT).  Marca @c cpu_dispatch_used_.
    uint64_t ensure_memcpy_dispatch();
    bool memcpy_helpers_emitted_ =
        false; ///< Las variantes + el global fp ya estan emitidos.
    uint64_t memcpy_fp_slot_ =
        UINT64_MAX; ///< Slot static_data del global __vx_memcpy_fp.

    /// AUTO multiversion (--float-isa auto): si @c main tiene ops VEC_*, lo
    /// renombra a @c __vx_main_body (helper VEC normal que el driver compila
    /// 3x: $sse2/$avx2/$avx512) y sintetiza un @c main fino que (a) corre los
    /// inits y (b) hace @c CALLIND a traves del slot @c __vx_main_body$fp.
    /// Asi "multiversionar main" se reduce a "despachar un helper", sin tratar
    /// el entry como caso especial.  Construye ademas @c __vx_auto_init() que
    /// elige la variante por cpuid (AVX512F bit7 > AVX2 bit4 > SSE2) y la
    /// guarda en el fp.  Idempotente.  Solo @c native_poo_ + @c aot_auto_vec_.
    /// Marca @c cpu_dispatch_used_ + @c auto_dispatch_emitted_ para que
    /// @c run() prepone @c call __vx_auto_init al entry de main.
    void ensure_auto_multiversion(ir::IrModule &out_module);
    bool auto_dispatch_emitted_ =
        false; ///< El main sintetico + fp + __vx_auto_init ya se emitieron.

    /// Emite un memcpy(dst, src, len) DESPACHADO por la tabla de punteros:
    /// LOAD el fp del global + CALLIND.  Solo @c native_poo_.  En interp/JIT/
    /// Full el caller usa MEMCPY inline (rep movsb), sin cambio.
    void emit_memcpy_dispatched(ir::IrValueId dst, ir::IrValueId src,
                                ir::IrValueId len, uint32_t line);

    /// CPU dispatch Inc 5a: despacho de strcmp/strlen via tabla de punteros,
    /// foundation para que una libreria stdlib provea variantes SIMD via
    /// @HelperOverride.  Asegura (idempotente, una sola vez):
    ///   - global @c __vx_strcmp_fp (slot 8 B en ".data").
    ///   - global @c __vx_strlen_fp (slot 8 B en ".data").
    ///   - los helpers BASELINE @c __vx_strcmp_base / @c __vx_strlen_base
    ///     (la impl escalar del compilador; el dispatch los usa por defecto y
    ///     son llamables por nombre desde Vesta para que un override delegue).
    ///   - el helper @c __vx_strdisp_init() que setea ambos fp (override del
    ///     usuario si existe, si no el baseline).  El compilador NO hace cpuid
    ///     aqui: el default es baseline; la SIMD vendra de la lib importada.
    /// Marca @c cpu_dispatch_used_ para que @c run() prepone el init en main.
    /// Solo en @c native_poo_ (AOT).
    void ensure_strdisp();
    bool strdisp_emitted_ =
        false; ///< Los fp + baselines + init ya estan emitidos.
    uint64_t strcmp_fp_slot_ =
        UINT64_MAX; ///< Slot static_data del global __vx_strcmp_fp.
    uint64_t strlen_fp_slot_ =
        UINT64_MAX; ///< Slot static_data del global __vx_strlen_fp.

    /// Emite strcmp(pa, la, pb, lb) -> i64 (-1/0/1) DESPACHADO por la tabla de
    /// punteros (LOAD __vx_strcmp_fp + CALLIND).  Solo @c native_poo_.
    ir::IrValueId emit_strcmp_dispatched(ir::IrValueId pa, ir::IrValueId la,
                                         ir::IrValueId pb, ir::IrValueId lb,
                                         uint32_t source_line);

    // --- Reflexion / meta-OOP / Phase Z extras ---
    ir::IrValueId emit_findmethod(ir::IrValueId v_params, uint32_t line);
    ir::IrValueId emit_findfield(ir::IrValueId v_params, uint32_t line);
    ir::IrValueId emit_findclass(ir::IrValueId v_params, uint32_t line);
    ir::IrValueId emit_defclass(ir::IrValueId v_params, uint32_t line);
    void emit_deffield(ir::IrValueId v_cls, ir::IrValueId v_params,
                       uint32_t line);
    void emit_defmethod(ir::IrValueId v_cls, ir::IrValueId v_params,
                        uint32_t line);
    void emit_addadvice(ir::IrValueId v_target, ir::IrValueId v_advice,
                        uint64_t kind, uint32_t line);

    // --- GC primitives / Phase Z atomics ---
    ir::IrValueId emit_gc_allocp(ir::IrValueId v_size, uint32_t line);
    ir::IrValueId emit_gc_promote(ir::IrValueId v_src, uint32_t line);
    ir::IrValueId emit_gc_demote(ir::IrValueId v_src, uint32_t line);
    ir::IrValueId emit_atomic_ld_i64(ir::IrValueId v_addr, uint32_t line);
    void emit_atomic_st_i64(ir::IrValueId v_addr, ir::IrValueId v_val,
                            uint32_t line);
    ir::IrValueId emit_atomic_cas_i64(ir::IrValueId v_addr, ir::IrValueId v_exp,
                                      ir::IrValueId v_des, uint32_t line);
    ir::IrValueId emit_atomic_add_i64(ir::IrValueId v_addr,
                                      ir::IrValueId v_delta, uint32_t line);

    // --- Static fields + AOP proceed + Async fusion + Intrinsics ---
    ir::IrValueId emit_getstatic(ir::IrValueId v_cls, uint64_t offset,
                                 uint32_t line);
    void emit_setstatic(ir::IrValueId v_cls, ir::IrValueId v_val,
                        uint64_t offset, uint32_t line);
    ir::IrValueId emit_proceed(uint32_t line);
    ir::IrValueId emit_getpid(uint32_t line);
    ir::IrValueId emit_getargc(uint32_t line);
    ir::IrValueId emit_getarg(ir::IrValueId v_idx, uint32_t line);
    void emit_fulfill_hlt(ir::IrValueId v_fut, ir::IrValueId v_val,
                          uint32_t line);

    // --- Lowering helpers para expresiones nuevas ---
    ir::IrValueId lower_try_expr(ast::TryExpr *e);
    ir::IrValueId lower_super_call_expr(ast::SuperCallExpr *e);
    ir::IrValueId lower_super_method_call_expr(ast::SuperMethodCallExpr *e);

    /// Mapa de variables locales tipo `Class` cuyo origen es un
    /// `Class.forName("X")` con literal X.  Permite que el lowering
    /// de `cls.newInstance()` detecte la clase concreta en compile
    /// time y emita `new X()` (que SI llama al constructor via
    /// `__new_<X>` synthetic) en lugar del NEWOBJ raw que no llama
    /// al ctor.  Coste: cero (el helper `__new_<X>` ya existia).
    ///
    /// Llave: nombre del local Class.  Valor: nombre de la clase X
    /// que el local apunta.  Las re-asignaciones desde fuentes no
    /// trackeable (e.g. `cls = otroFn()`) borran la entrada.
    std::unordered_map<std::string, std::string> class_origin_of_local_;

    /// Mapa SSA value -> clase concreta cuando el valor proviene de un
    /// @c new Class() (o cadena MOV/PHI desde ese origen).  Permite
    /// devirtualizar en tiempo de compilacion las llamadas via
    /// interface receiver cuando el tipo concreto es estaticamente
    /// conocido: el dispatch baja a CALLVIRT directo con el vtable_idx
    /// del metodo en la CLASE concreta, sin necesidad del trio
    /// findclass+findmethod+callm runtime.
    ///
    /// Coste: cero runtime; ~50 LOC en el lowering para mantenerlo.
    /// Beneficio: tanto port C como JIT obtienen output devirtualizado
    /// para el caso comun de `Iface x = new Impl(); x.metodo()`.
    ///
    /// Llave: IrValueId.  Valor: nombre de clase concreta.  Vacio = no
    /// se conoce el tipo concreto estatico.
    std::unordered_map<ir::IrValueId, std::string> ssa_concrete_class_;

    /// Modo de instrumentacion: "none", "trace", "profile".  Cuando
    /// no es "none", el lowering envuelve cada funcion usuario con
    /// CALLs a @c vx_trace:enter y @c vx_trace:exit (o equivalente).
    std::string instrument_mode_ = "none";
    /// Phase AOT.2.b: modo POO nativa (sin runtime VM).  Ver set_native_poo.
    bool native_poo_ = false;
    /// Bits del target para validar el inline-asm (@Naked/asm{}); 64 por defecto.
    uint8_t asm_target_bits_ = 64;
    /// Ancho del chunk SIMD del vectorizador en AOT (16/32/64 bytes); 16 default.
    uint8_t aot_vec_width_ = 16;
    /// --float-isa auto: chunk dual para multiversion (ver set_aot_auto_vec).
    bool aot_auto_vec_ = false;
    /// Type matching de catch (AOT): por cada clase, su intervalo DFS [lo,hi]
    /// sobre el bosque de herencia.  is-a(A,B) <=> B.lo <= A.lo <= B.hi.  El
    /// throw transporta A.lo; cada catch(B) compara contra [B.lo,B.hi]
    /// (constantes en compile-time).  Vacio fuera de native_poo_.
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>>
        type_intervals_;
    /// Computa @c type_intervals_ via DFS del bosque de clases (super_name).
    void compute_type_intervals();
    /// Solo-LSP: bajar comptime fns (no-macro) a IR para inspeccion.
    bool emit_comptime_fns_ = false;
    /// C-3: nombres de los override del string built-in (vacios => default).
    std::string string_concat_override_;
    std::string string_eq_override_;
    /// @SyncImpl: nombres de las fns override de monitor enter/exit (vacios
    /// => default por tier).  Ver @c set_sync_impl_overrides.
    std::string sync_enter_override_;
    std::string sync_exit_override_;
    /// CPU dispatch Inc 4: fn libre @HelperOverride(memcpy) (vacio => sin
    /// override; el fp se elige por cpuid en __vx_memcpy_init).
    std::string memcpy_override_;
    /// CPU dispatch Inc 5a: fn libre @HelperOverride(strcmp) / (strlen)
    /// (vacio => sin override; el fp apunta al baseline en __vx_strdisp_init).
    std::string strcmp_override_;
    std::string strlen_override_;

    /// C-3: emite una CALL a una funcion libre override del string
    /// built-in (@StringConcat / @StringEq).  @p lhs / @p rhs son las
    /// expresiones operando; se materializan al repr `string` adecuado
    /// (StringObject handle i64 en Full, PTR a value-string en native).
    /// @p ret_ir es el tipo IR de retorno (I64 para concat, BOOL para eq).
    /// @p negate niega el resultado bool (para `!=` sobre @StringEq).
    /// Devuelve el IrValueId del resultado, o IR_NO_VALUE en error.
    ir::IrValueId emit_string_override_call(const std::string &fn_name,
                                            ast::Expr *lhs, ast::Expr *rhs,
                                            ir::IrType ret_ir, bool negate,
                                            uint32_t source_line);

    /// Helper: emite CALLN sintetica a @c "vx_trace:enter" con
    /// argumento puntero al string literal del nombre de la funcion.
    /// Usa @c out_mod_->intern_static_data para internar el nombre.
    void emit_instrument_enter(const std::string &fn_name, uint32_t line);

    /// Helper: emite CALLN sintetica a @c "vx_trace:exit" con
    /// argumentos (fn_name_ptr, return_value).  Si @c v_ret es
    /// @c IR_NO_VALUE (funcion void), se pasa @c 0.
    void emit_instrument_exit(const std::string &fn_name, ir::IrValueId v_ret,
                              uint32_t line);

    /// Spill slots activos durante el body y catches de un try.
    /// Para cada variable del scope outer que se asigna dentro del try,
    /// reservamos un slot (8 bytes en stack) y mantenemos un STORE
    /// duplicado en cada @c write_local.  El @c throw salta sin pop
    /// pero el slot vive en stack VM mas alla del rsp restore.  En el
    /// merge_bb hacemos LOAD del slot y bindeamos el nombre al SSA
    /// resultante -- asi el valor visible tras el try es siempre el
    /// ultimo escrito (body o catch) sin importar registros corruptos.
    ///
    /// Estructura: name -> SSA value PTR del slot.  Vacio fuera de un
    /// try.  El lower_try lo llena pre-body, lo limpia post-merge.
    std::unordered_map<std::string, ir::IrValueId> try_spill_slots_;

    /// stack de acciones de cleanup activas en el flujo actual.
    /// Cada entrada es codigo RAW_ASM que debe ejecutarse al SALIR del
    /// scope que la registro (sea por flujo normal, return o break).
    /// Lo usa @c synchronized para emitir @c tryleave + @c monexit
    /// cuando el body hace @c return temprano.  Las excepciones NO
    /// pasan por aqui: las maneja el @c tryenter+handler clasico.
    ///
    /// Pila LIFO: el cleanup mas reciente se ejecuta primero (orden
    /// inverso a su registro).  @c emit_cleanups_all() recorre la
    /// pila de tope a fondo, emitiendo cada bloque RAW_ASM en el
    /// bloque actual sin modificar el stack (para que el caller que
    /// abrio el scope haga su pop normal).
    struct CleanupAction {
        /// tipo de accion:
        ///   RAW_ASM: emitir un bloque @c IrOp::RAW_ASM con @c asm_text y
        ///            @c operands.  El regalloc trata el bloque como
        ///            opaco (no preserva regs caller-saved), asi que es
        ///            adecuado solo para cleanups que NO clobreen regs
        ///            de valores vivos del scope (e.g. monexit).
        ///   CALL_DTOR: emitir un @c IrOp::CALLVIRT real que el regalloc
        ///              trata como CALL normal (preserva los regs vivos
        ///              automaticamente).  Necesario para destructores
        ///              de clase y auto-free de colecciones cuando hay
        ///              valor de retorno vivo en el RET (lower_return
        ///              spills regs al stack alrededor del cleanup).
        ///   CALLN_FREE: emitir un @c IrOp::CALLN para liberar una
        ///               coleccion primitiva.  Usa @c func_name para el
        ///               nombre del simbolo nativo y @c needs_proc para
        ///               decidir si emite GETPROC + lo prepende como
        ///               primer argumento (variantes @c *_free_gc del
        ///               plugin de colecciones GC-aware).
        ///               El regalloc trata el CALLN como cualquier call,
        ///               preservando regs caller-saved automaticamente.
        ///   SMARTPTR_FREE: libera un @c unique<T> al exit del scope.
        ///                  Sigue el patron: cargar ptr del slot stack,
        ///                  saltar si es 0 (moved), invocar deleter
        ///                  literal (free, fclose, ~T()).  Para Tier 0
        ///                  el deleter es CALLN a @c free.  Para Tier 1
        ///                  con deleter custom usa el field +8 del slot.
        ///   SHAREDPTR_REL: decremento del refcount de un @c shared<T> al
        ///                  exit del scope.  Si llega a 0 invoca el
        ///                  deleter sobre payload (ctrl_block + 16).
        enum class Kind {
            RAW_ASM,    ///< Cleanup opaco (no preserva regs caller-saved).
            CALL_DTOR,  ///< CALLVIRT real al destructor de la clase.
            STRUCT_DTOR, ///< CALL directo al destructor de un STRUCT value-type
                        ///< (`<Struct>__dtor(addr)`).  Sin vtable: dispatch
                        ///< estatico.  Inlineable (un dtor trivial cuesta ~0
                        ///< tras el inliner).  Se registra solo si el struct
                        ///< tiene `~Struct()` y NO escapa (move-on-return /
                        ///< store suprime el cleanup via escaping_locals_).
            CALLN_FREE, ///< CALLN a libreria nativa (e.g. free de colecciones).
            SMARTPTR_FREE, ///< Liberar @c unique<T> al exit del scope.
            SHAREDPTR_REL, ///< Decrementar refcount de @c shared<T>.
            SYNC_EXIT, ///< Exit de @c synchronized {} : TRYLEAVE + MONEXIT como
                       ///< IR ops.
            NATIVE_FREE, ///< Phase AOT.2.b: RAW_FREE(obj) de una instancia de
                        ///< clase NATIVA (calloc) al exit del scope (RAII; sin
                        ///< GC). aot_lower lo convierte en call<free>.  Sin
                        ///< dangling.
            STRING_FREE, ///< Vesta Embed Inc 0: liberar el buffer de un
                        ///< string value-type (native_poo_) al exit del
                        ///< scope.  operands[0] = PTR al slot de 24 bytes
                        ///< {ptr,len,cap}.  Emite LOAD ptr@[slot+0] +
                        ///< RAW_FREE(ptr) (aot_lower -> call free; free(0)
                        ///< es no-op tras un move, sin doble-free).
            CLOSURE_ENV_FREE ///< Ownership: liberar el env+slot heap de los
                        ///< campos closure (lambda con captura) de un struct
                        ///< value-type que recibio su valor por move (init
                        ///< desde una call que retorna un struct con closure
                        ///< escapado).  operands[0] = PTR al struct (refresh
                        ///< por refresh_name); @c closure_field_offsets lista
                        ///< los offsets de los campos fn.  Emite, por campo,
                        ///< la misma secuencia que @c emit_free_closure_env_field
                        ///< (null-guard, free env + slot).  Sin GC; un solo
                        ///< free porque el productor suprime su cleanup
                        ///< (move-on-return via escaping_locals_).
        };
        Kind kind = Kind::RAW_ASM;
        // --- Comun ---
        std::vector<ir::IrValueId> operands; ///< valores SSA referenciados
        uint32_t source_line;
        /// si != "", @c emit_cleanups_all hace @c lookup(refresh_name)
        /// y reemplaza @c operands[0] con el SSA value ACTUAL del binding.
        std::string refresh_name;
        // --- RAW_ASM ---
        std::string asm_text; ///< plantilla con {src0..}
        // --- CALL_DTOR ---
        uint32_t dtor_vtable_index = 0;
        // --- NATIVE_FREE (AOT.2.d): dtor polimorfico ---
        /// @c true si la clase estatica tiene vtable y el dtor es virtual:
        /// el cleanup despacha @c ~T() por la vtable de la instancia (LOAD
        /// vtable de obj[0] + LOAD fn[idx] + CALLIND) en vez de un CALL
        /// directo al dtor estatico -> una ref base que posee una instancia
        /// derivada (@c Base b = new Derived()) ejecuta el dtor DERIVADO.
        bool native_dtor_virtual = false;
        // --- SMARTPTR_FREE con inner GC class (e.g. @c unique<Resource>) ---
        /// @c true si el contenido apunta a un objeto GC (no a memoria
        /// RAW_ALLOC).  Cuando se setea, el cleanup invoca el destructor
        /// del inner via @c CALLVIRT (@c inner_dtor_vtable_index) y NO
        /// hace @c RAW_FREE del host_ptr (rompedria el heap GC).
        bool inner_is_gc_class = false;
        /// Indice en la vtable del destructor del tipo contenido (>0).
        /// Usado solo si @c inner_is_gc_class.
        uint32_t inner_dtor_vtable_index = 0;
        /// Nombre IR del destructor del tipo contenido (@c <Class>____dtor),
        /// para CALL DIRECTO en native_poo (AOT) cuando el inner NO es
        /// polimorfico (tipo estatico == dinamico para @c unique<T>).
        std::string inner_dtor_func_name;
        /// @c true si el inner es polimorfico (tiene vtable): el dtor debe
        /// despacharse por la vtable de la instancia, no por el tipo estatico.
        bool inner_dtor_virtual = false;
        // --- CALLN_FREE / SMARTPTR_FREE / SHAREDPTR_REL ---
        std::string func_name;   ///< "lib:symbol" para CALLN
        bool needs_proc = false; ///< prepend GETPROC
        // --- SMARTPTR_FREE / SHAREDPTR_REL ---
        /// SSA value del PTR al slot del smart pointer (donde vive el ptr).
        /// Usado para LOAD del valor actual y CMP_EQ 0 (skip si moved).
        ir::IrValueId slot_addr = 0;
        /// Nombre del deleter.  Tres formatos:
        ///   "free"                  -> emite IrOp::RAW_FREE directo
        ///                              (deleter por defecto de unique_box).
        ///   "<vesta_fn_name>"       -> emite CALLVM @Method al simbolo
        ///                              Vesta declarado por el usuario.
        ///                              Cero overhead: 1 LOAD + 1 CALLVM.
        ///   "@extern:<lib>:<fn>"    -> emite CALLN al simbolo nativo
        ///                              de la libreria.  El prefijo "@extern:"
        ///                              discrimina extern vs Vesta.
        std::string literal_deleter;
        /// Tamano del slot del smart pointer (8 para Tier 0).
        uint32_t slot_size = 8;
        /// --- CLOSURE_ENV_FREE ---
        /// Offsets (en bytes) de los campos closure (fn con captura) del
        /// struct a liberar al exit del scope.
        std::vector<uint32_t> closure_field_offsets;
    };
    std::vector<CleanupAction> cleanup_stack_;
    /// Contador para etiquetas unicas en cleanups que emiten labels
    /// (smart pointer cleanups con branches internos).  Cada invocacion
    /// de emit_cleanups consume el siguiente valor; garantiza que dos
    /// cleanups no colisionen en el mismo label dentro de la misma
    /// funcion (caso comun: 2 vars unique<T> en el mismo scope).
    uint32_t cleanup_label_seq_ = 0;
    /// Canal compartido entre @c try_lower_builtin_call (unique_with /
    /// shared_with) y @c lower_var_decl para pasar el nombre del deleter
    /// custom al cleanup pendiente.  Vacio = usar el deleter por
    /// defecto ("free").  Se limpia tras consumirse en lower_var_decl.
    std::string pending_smartptr_deleter_;
    /// Deleter ESTATICO por variable @c unique<T> local.  Permite resolver el
    /// cleanup de un `move(a)` a una llamada DIRECTA al deleter concreto de `a`
    /// (free / <fn_label> / @extern:...) en vez de un dispatch dinamico que lee
    /// el slot+8 en runtime.  Clave = nombre de la variable; valor = mismo
    /// formato que @c CleanupAction::literal_deleter ("free", "<fn>",
    /// "@extern:<lib>:<fn>").  Ausente = deleter desconocido (p.ej. la variable
    /// vino de una factory opaca) -> el move cae a dispatch dinamico.
    std::unordered_map<std::string, std::string> unique_var_deleter_;
    /// Ownership: cuando un @c unique<T> se asigna a un CAMPO (de clase o
    /// struct), su slot Tier 1 (16B [ptr][deleter]) debe vivir en HEAP, no en
    /// stack: el campo guarda la direccion del slot y sobrevive al scope donde
    /// se creo el unique (igual que el env de un closure en un campo).  El dtor
    /// del contenedor libera el inner (via deleter) Y el slot heap.  Lo activa
    /// @c lower_assign antes de bajar el RHS; lo consumen unique_box/unique_with.
    bool unique_slot_to_heap_ = false;

    /// Stack de targets de break/continue para los loops anidados.
    /// Cada vez que entramos a un while/for/do-while se hace push de
    /// {continue_bb, break_bb}; al salir, pop.  BreakStmt emite
    /// `br break_bb` y ContinueStmt emite `br continue_bb` del top.
    ///
    /// continue_bb apunta al header del while (la cond se re-evalua) o
    /// al step_bb del for (ejecuta step y luego cond).  break_bb apunta
    /// al exit_bb del loop.  Vacio fuera de loops (BreakStmt y
    /// ContinueStmt en ese contexto son error de compilacion).
    struct LoopTargets {
        ir::IrBlockId continue_bb;
        ir::IrBlockId break_bb;
        /// Lista de bloques predecesores que saltaron al `continue_bb`
        /// via la sentencia `continue`.  Se rellena cada vez que el
        /// lowering procesa un `ContinueStmt` dentro del loop.  Al
        /// cerrar el loop, lower_while/for itera estos preds para
        /// completar los PHI nodes del header con los SSA values
        /// del scope al momento del continue (sin esto, las
        /// variables modificadas en el body antes del continue
        /// no se propagan correctamente al header del loop).
        std::vector<ir::IrBlockId> continue_preds;
        /// Snapshot del scope (todos los niveles) capturado en el
        /// instante de cada `continue`.  Indice paralelo a
        /// continue_preds.
        std::vector<std::vector<std::unordered_map<std::string, ir::IrValueId>>>
            continue_scopes;
        /// Lista de bloques predecesores que saltaron al `break_bb`
        /// via la sentencia `break`.  Equivalente a continue_preds
        /// pero para el exit_bb.  Sin esto, las variables modificadas
        /// en el body antes del break no se propagan al exit del loop
        /// (bug del PHI del exit con multiples paths que ven distintos
        /// SSA values).
        std::vector<ir::IrBlockId> break_preds;
        /// Snapshot del scope en el instante de cada `break`, paralelo
        /// a break_preds.  Usado por lower_while/for/do-while para
        /// emitir PHIs en el exit_bb cuando hay paths con valores
        /// distintos.
        std::vector<std::vector<std::unordered_map<std::string, ir::IrValueId>>>
            break_scopes;
    };
    std::vector<LoopTargets> loop_targets_;

    /// Mapa global de `goto` labels declaradas en la funcion actual.
    /// Cada `label:` declara un bloque con ese nombre; cada `goto label`
    /// emite BR al bloque resuelto via este mapa.  Se vacia al cerrar
    /// la funcion.  Si una label se usa antes de declararse, se
    /// crea el bloque en el primer goto y se reusa al verla.
    struct GotoEntry {
        ir::IrBlockId block;
        bool declared = false;   // true tras encontrar `label:`
        SourceLoc first_use_loc; // para diagnostico de undefined
    };
    std::unordered_map<std::string, GotoEntry> goto_labels_;

    /// contador monotono para nombrar funciones sinteticas
    /// generadas por @c spawn @c { @c body @c }.  Cada spawn produce
    /// @c __spawn_<N>; reset al inicio del modulo (no por funcion).
    size_t spawn_func_counter_ = 0;

    /// contador para nombres unicos de helpers
    /// @c __lambda_<N>.  Crece junto con cada @c LambdaExpr lowered;
    /// el reseteo es per-modulo (no per-funcion) para garantizar que
    /// dos lambdas distintas en funciones distintas tengan nombres
    /// diferentes.  Reusa @c pending_spawn_helpers_ para encolar los
    /// helpers; el flush a @c out_mod_ pasa al final de @c run().
    size_t lambda_counter_ = 0;

    /// contador per-funcion para nombres unicos de bloques del
    /// operador ternario (@c ter_then_<N> / @c ter_else_<N> /
    /// @c ter_merge_<N>).  Se incrementa por cada @c lower_ternary.
    size_t ternary_counter_ = 0;

    /// stack dinamico de comptime const en lowering.  Usado por
    /// @c lower_comptime_for para bindear el index a su valor const
    /// en cada iteracion del unroll.  @c lower_ident consulta este
    /// stack ANTES de las anotaciones AST -- permite override per-
    /// iteracion del unroll sin re-correr el type checker.
    struct ComptimeLocalEntry {
        bool is_str = false;
        int64_t value = 0;
        std::string str_value;
        ir::IrType ir_t = ir::IrType::I64;
    };
    std::vector<std::unordered_map<std::string, ComptimeLocalEntry>>
        lowering_comptime_scopes_;

    /**
     * @brief emite el cuerpo de un `comptime for` desenrollado.
     *
     * Evalua @c lo y @c hi en compile-time.  Por cada valor de i en
     * [lo, hi) (o [lo, hi] si inclusive), push scope con i->valor,
     * baja el body como stmt normal, pop scope.  Resultado: N copias
     * del body emitidas en secuencia, con i siendo una constante
     * conocida en cada uno (los IdentExpr de i se resuelven via
     * @c lowering_comptime_scopes_).
     */
    void lower_comptime_for(ast::ComptimeForStmt *s);

    /// helpers de spawn pendientes de añadir al modulo.  Las
    /// funciones se acumulan aqui durante el lowering del padre y se
    /// vuelcan a @c out_mod_ al final de @c run() para preservar el
    /// orden de funciones (main primero -> emisor IR la marca como
    /// entry point con @c hlt; spawn helpers despues -> @c ret).
    std::vector<ir::IrFunction> pending_spawn_helpers_;

    /// mapa de nombre de tipo @c @Introspect ->
    /// indice del chunk IntrospectInfo en @c static_data.  Poblado por
    /// @c emit_introspect_info_chunks() al final de @c run(); consultado
    /// por @c lower_call para resolver @c find_type("Name") a un
    /// @c @Absolute("code.s_<idx>") directo (cero overhead cuando el
    /// nombre es literal compile-time).
    std::unordered_map<std::string, uint64_t> introspect_idx_by_name_;

    /**
     * @brief emite chunks IntrospectInfo POD en
     * @c static_data para cada tipo marcado @c @Introspect.
     *
     * Invocado al final de @c run() tras lower_class_methods (que
     * rellena fields/methods).  Itera @c tc_.struct_layouts(),
     * @c class_layouts() y @c enum_layouts(); por cada layout con
     * @c is_introspect=true genera el chunk binario con header (24
     * bytes) + FieldInfo[] + nombres inline.  Indices guardados en
     * @c introspect_idx_by_name_ para resolver @c find_type literal.
     */
    void emit_introspect_info_chunks();

    /// si !=IR_NO_VALUE estamos bajando el body de una
    /// funcion @Async como spawn helper.  El SSA value contiene el
    /// handle del Future (resultado de msgrecv al inicio del helper).
    /// `lower_return` consulta este flag: si esta set, en vez de emitir
    /// RET, emite `fulfill(async_fut_id_, value) + hlt`.  Asi cada
    /// return del body resuelve el future del caller.
    ir::IrValueId async_fut_id_ = ir::IR_NO_VALUE;

    /// si true, estamos lowering el body de un `rspawn(node) { ... }`
    /// helper.  En este caso `lower_return` intercepta `return X` y emite
    /// `mov r0, X + hlt`.  El runtime distribuido captura R0 al detectar
    /// HALT en un proceso con `rspawn_future_id != 0` y envia
    /// VDP_FUTURE_FULFILL al nodo origen con ese valor.  No usa async_fut_id_
    /// porque el future vive en el nodo CALLER, no en el remoto.
    bool is_rspawn_body_ = false;

    /// Emite todos los cleanups activos del @c cleanup_stack_ en orden
    /// inverso (LIFO).  No modifica el stack.  Usado por @c lower_return
    /// para garantizar que las acciones de salida (e.g. monexit) corran
    /// antes del RET.
    void emit_cleanups_all();

    /**
     * @brief Emite los cleanups del rango [start, end) del @c cleanup_stack_
     *        en orden inverso (mas reciente primero).
     *
     * Variante de @c emit_cleanups_all que solo procesa una ventana del
     * stack.  Util para scope-local cleanup desde @c lower_block: el
     * scope recuerda la altura del stack al entrar (start = mark) y al
     * salir emite los cleanups que se registraron en su propio cuerpo
     * (end = stack actual), sin tocar los outer.  Asi un destructor
     * dentro de un loop body se ejecuta al exit de cada iteracion.
     *
     * NO modifica el cleanup_stack_; el caller hace @c resize(start)
     * tras esta llamada para popear las entradas ya emitidas.
     *
     * @param start Indice inicial (inclusive).
     * @param end   Indice final (exclusive); debe ser >= start.
     */
    void emit_cleanups_range(size_t start, size_t end);

    /// sret: si la funcion actual declara devolver Optional<T> o
    /// Result<V,E>, el tipo de retorno se transforma en void y el
    /// caller pasa un buffer hidden (retbuf) como primer argumento.
    /// @c sret_active_ indica si el patron esta en uso, y
    /// @c sret_retbuf_ guarda el SSA value del param hidden.  El
    /// lowering de @c return en estas funciones copia el contenido
    /// del Optional/Result construido al retbuf en vez de devolverlo
    /// como valor.  El de las funciones que NO devuelven Optional/
    /// Result es false / IR_NO_VALUE.
    bool sret_active_ = false;
    ir::IrValueId sret_retbuf_ = ir::IR_NO_VALUE;
    /// Tamano del Optional/Result que se devuelve (16 o 24).  Se
    /// usa para emitir el MEMCPY al retbuf en lower_return.
    uint64_t sret_buf_size_ = 0;

    /// Nombre de la clase contenedora durante el lowering de un metodo
    /// de instancia.  Lo consulta @c lower_class_field_load /
    /// @c lower_class_field_store para resolver offsets cuando el
    /// receptor de @c .field es @c this (caso comun) o cualquier
    /// expresion con tipo CLASS.  Vacio fuera de metodos.
    std::string current_class_lowering_;
};

} // namespace vx

#endif // VX_LOWERING_H
