/**
 * @file vesta.h
 * @brief API C estable (C-ABI) para embeber VestaVM en cualquier lenguaje.
 *
 * Esta cabecera define una interfaz puramente C (sin C++) pensada para
 * cargar desde Python (ctypes/cffi), Rust (FFI), C#, C, etc. via la
 * libreria compartida @c libvesta (vesta.dll / libvesta.so).
 *
 * Capacidades:
 *   - Compilar fuente Vesta (.vx) a bytecode .velb en memoria.
 *   - Ejecutar bytecode .velb en una instancia de la VM.
 *   - Compilar + ejecutar en una sola llamada (eval).
 *
 * Convencion de errores: toda funcion que puede fallar devuelve un
 * @c int (0 = exito, distinto de 0 = error).  En caso de error, si el
 * parametro @c out_err no es NULL, se rellena con un puntero a un mensaje
 * asignado en el heap que el llamante DEBE liberar con @c vesta_free.
 *
 * Convencion de memoria: cualquier buffer (@c out_velb) o cadena
 * (@c out_err) devuelto por esta API se libera con @c vesta_free.  No
 * mezclar con free/delete del llamante.
 *
 * Re-entrancia: la VM usa estado global (registries, caches).  Las
 * llamadas a esta API NO son thread-safe entre si; serializa el acceso
 * desde el lado del llamante si compartes el modulo entre hilos.
 */
#ifndef VESTA_CAPI_H
#define VESTA_CAPI_H

#include <stddef.h>

/* Macro de export/import portable entre Windows (MinGW/MSVC) y POSIX. */
#if defined(_WIN32)
#if defined(VESTA_BUILD_DLL)
#define VESTA_API __declspec(dllexport)
#else
#define VESTA_API __declspec(dllimport)
#endif
#else
#define VESTA_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct VestaTarget
 * @brief Descriptor de target para las vistas por OS/arquitectura del LSP y de
 *        los diagramas (IR, bytecode .vel, asm nativo AOT, JIT, diagramas).
 *
 * Cualquier campo NULL o "" usa el valor por defecto.  El @c os selecciona las
 * ramas de compilacion condicional @c @Target("os:...") del fuente; el @c arch
 * y @c format determinan el codegen nativo (AOT) para las vistas de asm.
 *
 * Ejemplos:
 *   - Ver el IR/asm que se genera para Linux x86-64: {"linux","x86-64","elf",1}
 *   - Ver el asm que se genera para Windows x86-32: {"windows","x86-32","pe",1}
 */
typedef struct {
    const char *os;   /**< "windows" | "linux" | "macos"; NULL/"" = host. */
    const char *arch; /**< "x86-64" | "x86-32"; NULL/"" = "x86-64". */
    const char *fmt;  /**< "pe" | "elf"; NULL/"" = derivado del os. */
    int native_poo;   /**< 1 = lowering AOT nativo; 0 = interp/VM (default). */
} VestaTarget;

/**
 * @brief Devuelve la cadena de version de la API/VM.
 *
 * El puntero devuelto es estatico (no liberar con @c vesta_free).
 *
 * @return Cadena terminada en NUL con la version (p.ej. "vesta-capi 1.0").
 */
VESTA_API const char *vesta_version(void);

/**
 * @brief Compila fuente Vesta a bytecode .velb en memoria.
 *
 * @param src        Codigo fuente Vesta terminado en NUL.
 * @param unit_name  Nombre logico del modulo (para diagnosticos); puede ser
 *                   NULL (se usa "main").
 * @param out_velb   [salida] Recibe un puntero al buffer con los bytes del
 *                   .velb.  El llamante lo libera con @c vesta_free.
 * @param out_len    [salida] Recibe el tamano en bytes de @c out_velb.
 * @param out_err    [salida, opcional] Si no es NULL y hay error, recibe un
 *                   mensaje en heap (liberar con @c vesta_free).
 * @return 0 si la compilacion fue exitosa; distinto de 0 en caso de error.
 */
VESTA_API int vesta_compile(const char *src, const char *unit_name,
                            unsigned char **out_velb, size_t *out_len,
                            char **out_err);

/**
 * @brief Ejecuta bytecode .velb en una instancia de la VM.
 *
 * @param velb      Puntero a los bytes del .velb.
 * @param len       Tamano en bytes de @c velb.
 * @param argc      Numero de argumentos del programa (puede ser 0).
 * @param argv      Vector de argumentos (puede ser NULL si @c argc == 0).
 * @param out_exit  [salida, opcional] Recibe el valor de retorno de main
 *                  (registro R0 del proceso principal).
 * @param out_err   [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si la ejecucion fue exitosa; distinto de 0 en caso de error.
 */
VESTA_API int vesta_run(const unsigned char *velb, size_t len, int argc,
                        const char *const *argv, int *out_exit, char **out_err);

/**
 * @brief Compila y ejecuta fuente Vesta en una sola llamada.
 *
 * @param src        Codigo fuente Vesta terminado en NUL.
 * @param unit_name  Nombre logico del modulo; puede ser NULL.
 * @param out_exit   [salida, opcional] Valor de retorno de main (R0).
 * @param out_err    [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si compilo y ejecuto correctamente; distinto de 0 en caso de
 *         error.
 */
VESTA_API int vesta_eval(const char *src, const char *unit_name, int *out_exit,
                         char **out_err);

/**
 * @brief Compila fuente Vesta a texto .vel (bytecode textual intermedio).
 *
 * Reusa el frontend Vesta (@c vx::compile_vx_source) y devuelve el texto
 * @c .vel que el ensamblador consume.  Util para inspeccionar la salida
 * del lowering sin pasar por el ensamblado.
 *
 * @param src        Codigo fuente Vesta terminado en NUL.
 * @param unit_name  Nombre logico del modulo; puede ser NULL (se usa "main").
 * @param out_vel    [salida] Recibe el texto .vel en heap (liberar con
 *                   @c vesta_free).
 * @param out_err    [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_compile_to_vel(const char *src, const char *unit_name,
                                   char **out_vel, char **out_err);

/**
 * @brief Compila fuente Vesta a texto del IR SSA (dump legible del IrModule).
 *
 * Activa @c CompileOptions::dump_ir y devuelve @c CompileResult::ir_text,
 * el mismo dump que produce @c --vx-emit-ir.
 *
 * @param src        Codigo fuente Vesta terminado en NUL.
 * @param unit_name  Nombre logico del modulo; puede ser NULL.
 * @param out_ir     [salida] Recibe el texto del IR en heap (liberar con
 *                   @c vesta_free).
 * @param out_err    [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_compile_to_ir(const char *src, const char *unit_name,
                                  char **out_ir, char **out_err);

/**
 * @brief Ensambla + linka texto .vel a bytecode .velb en memoria.
 *
 * Reusa el path @c --worker/--build (@c asm_multi_process::run_worker):
 * escribe el texto a un temporal, ensambla, linka, y lee los bytes del
 * @c .velb resultante.  No pasa por el preprocesador VPP (el .vel ya esta
 * desazucarado).
 *
 * @param vel_text   Texto .vel terminado en NUL.
 * @param out_velb   [salida] Recibe el buffer con los bytes del .velb en heap
 *                   (liberar con @c vesta_free).
 * @param out_len    [salida] Recibe el tamano en bytes de @c out_velb.
 * @param out_err    [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_assemble(const char *vel_text, unsigned char **out_velb,
                             size_t *out_len, char **out_err);

/**
 * @brief Desensambla un buffer de bytes nativos a texto legible (Capstone).
 *
 * @param bytes     Puntero a los bytes a desensamblar.
 * @param len       Numero de bytes.
 * @param arch      Nombre de la arquitectura ("X86-32", "X86-64", "ARM",
 *                  "AArch64").  NULL => "X86-64".
 * @param out_text  [salida] Recibe el listado de instrucciones en heap
 *                  (liberar con @c vesta_free).
 * @param out_err   [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_disasm(const unsigned char *bytes, size_t len,
                           const char *arch, char **out_text, char **out_err);

/**
 * @brief Genera un diagrama del pipeline Vesta para un fuente dado.
 *
 * @param src        Codigo fuente Vesta terminado en NUL.
 * @param unit_name  Nombre logico del modulo; puede ser NULL.
 * @param kind       Vista del diagrama: "ast" | "ir-pre" | "ir-post" | "vel".
 * @param format     Formato de salida: "mermaid" | "graphviz" | "html".
 * @param out_text   [salida] Recibe el texto del diagrama en heap (liberar
 *                   con @c vesta_free).
 * @param out_err    [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_diagram(const char *src, const char *unit_name,
                            const char *kind, const char *format,
                            char **out_text, char **out_err);

/* ------------------------------------------------------------------------- *
 * Vistas por OS/arquitectura (@c VestaTarget).                              *
 *                                                                           *
 * Permiten al LSP y a los diagramas mostrar el IR / bytecode / asm nativo / *
 * JIT / diagramas que el compilador genera para un target concreto (Linux o *
 * Windows, x86-64 o x86-32).  @c target == NULL equivale al host con        *
 * lowering interp/VM.  Todas devuelven el texto en heap (liberar con        *
 * @c vesta_free).                                                            *
 * ------------------------------------------------------------------------- */

/**
 * @brief IR SSA (dump legible) que se genera para @c target.
 *
 * Aplica las ramas @c @Target("os:...") del @c target->os y, con
 * @c target->native_poo, el lowering AOT nativo.  @c target == NULL => host.
 */
VESTA_API int vesta_compile_to_ir_t(const char *src, const char *unit_name,
                                    const VestaTarget *target, char **out_ir,
                                    char **out_err);

/**
 * @brief Bytecode textual .vel que se genera para @c target.
 *
 * El @c os selecciona las ramas @c @Target; el bytecode VM es arch-agnostico.
 * @c target == NULL => host.
 */
VESTA_API int vesta_compile_to_vel_t(const char *src, const char *unit_name,
                                     const VestaTarget *target, char **out_vel,
                                     char **out_err);

/**
 * @brief Asm NATIVO (AOT) desensamblado que se genera para @c target.
 *
 * Baja el modulo a codigo nativo para @c (os, arch, fmt) y lo desensambla con
 * Capstone, agrupado por funcion.  Es la vista donde el arch/OS importan de
 * verdad (x86-64 vs x86-32, PE vs ELF).  @c target->native_poo se fuerza a 1.
 */
VESTA_API int vesta_compile_to_asm_t(const char *src, const char *unit_name,
                                     const VestaTarget *target, char **out_asm,
                                     char **out_err);

/**
 * @brief Asm del codigo JIT-eado (vreg) desensamblado para @c target.
 *
 * El JIT es x86-64 host; el @c os solo afecta las ramas @c @Target.  Devuelve
 * el listado por funcion compilada.  Util para comparar JIT vs AOT vs interp.
 */
VESTA_API int vesta_compile_to_jit_t(const char *src, const char *unit_name,
                                     const VestaTarget *target, char **out_asm,
                                     char **out_err);

/**
 * @brief Diagrama del pipeline para @c target (mismas @c kind/@c format que
 *        @c vesta_diagram, mas @c kind "asm" para el nativo desensamblado).
 */
VESTA_API int vesta_diagram_t(const char *src, const char *unit_name,
                              const char *kind, const char *format,
                              const VestaTarget *target, char **out_text,
                              char **out_err);

/**
 * @brief Ejecuta un script VestaShellScript (.vsh) desde una cadena.
 *
 * Reusa el interprete tree-walking @c vsh::VshInterpreter (path @c --script).
 *
 * @param script    Codigo VSH terminado en NUL.
 * @param out_rc    [salida, opcional] Recibe 0 si el script termino sin
 *                  error, distinto de 0 si lanzo (informativo).
 * @param out_err   [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si el script ejecuto sin error; distinto de 0 en caso de error.
 */
VESTA_API int vesta_vsh_eval(const char *script, int *out_rc, char **out_err);

/**
 * @brief Compila fuente Vesta y devuelve a la vez el .vel, el IR y el .velb.
 *
 * Conveniencia que evita compilar tres veces el mismo fuente.  Cada salida
 * es opcional (pasar NULL para omitirla).  Las cadenas/buffer devueltos se
 * liberan con @c vesta_free.
 *
 * @param src        Codigo fuente Vesta terminado en NUL.
 * @param unit_name  Nombre logico del modulo; puede ser NULL.
 * @param out_vel    [salida, opcional] Texto .vel en heap.
 * @param out_ir     [salida, opcional] Texto del IR en heap.
 * @param out_velb   [salida, opcional] Bytes del .velb en heap.
 * @param out_velb_len [salida, opcional] Tamano de @c out_velb (requerido si
 *                   @c out_velb no es NULL).
 * @param out_err    [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_compile_full(const char *src, const char *unit_name,
                                 char **out_vel, char **out_ir,
                                 unsigned char **out_velb, size_t *out_velb_len,
                                 char **out_err);

/**
 * @brief Genera bytecode .velb a partir de texto del IR SSA.
 *
 * Cierra el ciclo de manipulacion del IR: el llamante puede obtener el
 * texto IR con @c vesta_compile_to_ir, transformarlo, y volver a producir
 * bytecode ejecutable con esta funcion.  Internamente parsea el texto IR
 * (@c ir::ir_parse), emite el @c .vel (@c ir::ir_emit_module) y lo ensambla
 * + linka al @c .velb (mismo path que @c vesta_assemble).
 *
 * @param ir_text   Texto del IR SSA terminado en NUL.
 * @param out_velb  [salida] Recibe el buffer con los bytes del .velb en heap
 *                  (liberar con @c vesta_free).
 * @param out_len   [salida] Recibe el tamano en bytes de @c out_velb.
 * @param out_err   [salida, opcional] Mensaje de error en heap si falla
 *                  (liberar con @c vesta_free).
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_ir_to_velb(const char *ir_text, unsigned char **out_velb,
                               size_t *out_len, char **out_err);

/**
 * @brief Valida que una cadena es JSON sintacticamente correcto.
 *
 * Usa el parser de nlohmann::json.  No produce salida util mas alla del
 * codigo de retorno; el error de parse (si lo hay) va en @c out_err.
 *
 * @param json_text Texto JSON terminado en NUL.
 * @param out_err   [salida, opcional] Mensaje de error de parse en heap si
 *                  el JSON es invalido (liberar con @c vesta_free).
 * @return 0 si el JSON es valido; distinto de 0 si es invalido o hay error.
 */
VESTA_API int vesta_json_validate(const char *json_text, char **out_err);

/**
 * @brief Reformatea (pretty-print o minifica) una cadena JSON.
 *
 * Parsea el JSON y lo vuelve a serializar.  Si @c indent es negativo, el
 * resultado es compacto (minificado, sin espacios).  Si @c indent es >= 0,
 * el resultado es legible con esa sangria por nivel.
 *
 * @param json_text Texto JSON terminado en NUL.
 * @param indent    Sangria por nivel: < 0 => compacto; >= 0 => pretty.
 * @param out_text  [salida] Recibe el JSON reformateado en heap (liberar con
 *                  @c vesta_free).
 * @param out_err   [salida, opcional] Mensaje de error de parse en heap si
 *                  falla (liberar con @c vesta_free).
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_json_format(const char *json_text, int indent,
                                char **out_text, char **out_err);

/**
 * @brief Abre una base de datos SQLite, ejecuta SQL y devuelve filas en JSON.
 *
 * Abre @c db_path (acepta @c ":memory:" para una BD en memoria), ejecuta
 * todas las sentencias del bloque @c sql, y recolecta las filas producidas
 * por las sentencias @c SELECT en un array JSON de objetos
 * @c {columna: valor}.  Si no hay filas, devuelve @c "[]".  La conexion se
 * cierra siempre, incluso en error.
 *
 * @param db_path   Ruta del fichero de BD (o @c ":memory:").
 * @param sql       Una o varias sentencias SQL terminadas en NUL.
 * @param out_json  [salida] Recibe el array JSON de filas en heap (liberar
 *                  con @c vesta_free).
 * @param out_err   [salida, opcional] Mensaje de error de SQLite en heap si
 *                  falla (liberar con @c vesta_free).
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_sqlite_exec(const char *db_path, const char *sql,
                                char **out_json, char **out_err);

/* =========================================================================
 * Compilacion incremental / cache direccionado por contenido (CAS).
 *
 * Expone a otros lenguajes/sistemas (C/C++/Python/Rust/...) los mecanismos
 * del driver incremental de Vesta: fingerprint de configuracion de build,
 * store direccionado por contenido, e indice semantico + claves Merkle por
 * simbolo (para invalidacion granular).  Todo es content-addressed -> apto
 * para cache compartido / distribuido entre maquinas.
 * ========================================================================= */

/**
 * @brief Configuracion de build que afecta a un artefacto compilado.
 *
 * Mirror C del @c vx::BuildConfig .  Los campos de "IR pre-optimize" afectan
 * al fingerprint del IR; el resto solo al del artefacto final.  Rellenar con
 * @c 0 / defaults lo no usado.
 */
typedef struct VestaBuildConfig {
    unsigned char asm_target_bits; /**< 64/32/16: lowering de inline-asm. */
    int native_poo;                /**< clases AOT (calloc/dtor) vs VM (GC). */
    int exceptions_enabled;        /**< lowering de try/catch en AOT. */
    const char *instrument_mode;   /**< "none"/"trace"/"profile" (NULL = ""). */
    const char *tgt_os;            /**< @Target OS (NULL = ""). */
    const char *tgt_arch;          /**< @Target arch (NULL = ""). */
    int opt_level;                 /**< 0..3 (solo artefacto final). */
    int emit_debug;                /**< info de debug (solo artefacto final). */
    unsigned char aot_vec_width;   /**< 16/32/64 SSE2/AVX/AVX512 (post-opt). */
    const char *profile_id;        /**< PGO / perfil (NULL = ""). */
} VestaBuildConfig;

/**
 * @brief Calcula los fingerprints de una configuracion de build.
 *
 * @param cfg          Configuracion (no NULL).
 * @param out_ir_fp    [salida] fingerprint de las dims del IR pre-optimize.
 * @param out_full_fp  [salida] fingerprint COMPLETO (artefacto final).
 * @return 0 si exito; !=0 si @p cfg o algun out es NULL.
 */
VESTA_API int vesta_build_fingerprint(const VestaBuildConfig *cfg,
                                      unsigned long long *out_ir_fp,
                                      unsigned long long *out_full_fp);

/** @brief Handle opaco a un store direccionado por contenido (CAS). */
typedef struct VestaCas VestaCas;

/**
 * @brief Abre (creando si falta) un CAS en @p dir.  Si @p dir es NULL, usa el
 *        store global por defecto ($VX_CAS_DIR / $VX_HOME/cas / ~/.vesta/cas).
 * @return Handle no-NULL en exito; NULL si no se pudo crear.  Liberar con
 *         @c vesta_cas_close .
 */
VESTA_API VestaCas *vesta_cas_open(const char *dir);

/** @brief Cierra un CAS.  Aceptar NULL es seguro. */
VESTA_API void vesta_cas_close(VestaCas *cas);

/** @brief @c 1 si existe un artefacto para @p key; @c 0 si no. */
VESTA_API int vesta_cas_has(VestaCas *cas, unsigned long long key);

/**
 * @brief Lee el artefacto de @p key.  @param out_data [salida] buffer en heap
 *        (liberar con @c vesta_free); @param out_len [salida] su longitud.
 * @return 0 si existe y se leyo; !=0 si no existe o error.
 */
VESTA_API int vesta_cas_get(VestaCas *cas, unsigned long long key,
                            unsigned char **out_data, size_t *out_len);

/**
 * @brief Escribe (atomico, idempotente) el artefacto de @p key.
 * @return 0 si exito; !=0 en error de I/O.
 */
VESTA_API int vesta_cas_put(VestaCas *cas, unsigned long long key,
                            const unsigned char *data, size_t len);

/**
 * @brief Indice semantico por-simbolo + claves Merkle de un fuente Vesta, en
 *        JSON.  Cada simbolo lleva su nombre cualificado, kind, hash de
 *        contenido, dependencias, visibilidad y su CLAVE MERKLE (que resume su
 *        contenido + el de sus dependencias -> apta como clave de cache).
 *
 * @param src        Fuente .vx (NUL-terminado).
 * @param unit_name  Nombre logico (NULL -> "main").
 * @param out_json   [salida] JSON en heap (liberar con @c vesta_free).
 * @return 0 si exito; !=0 si el parse fallo (out_json queda NULL).
 */
VESTA_API int vesta_merkle_keys_json(const char *src, const char *unit_name,
                                     char **out_json);

/**
 * @brief Libera memoria devuelta por la API (@c out_velb, @c out_err).
 *
 * Aceptar NULL es seguro (no-op).
 *
 * @param p Puntero previamente devuelto por la API.
 */
VESTA_API void vesta_free(void *p);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VESTA_CAPI_H */
