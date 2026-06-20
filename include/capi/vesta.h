/**
 * @file vesta.h
 * @brief API C estable (C-ABI) para embeber VestaVM en cualquier lenguaje.
 *
 * Esta cabecera define una interfaz puramente C (sin C++) pensada para
 * cargar desde Python (ctypes/cffi), Rust (FFI), C#, C, etc. via la
 * libreria compartida @c libvesta (vesta.dll / libvesta.so).
 *
 * Capacidades:
 *   - Compilar fuente Vex (.vex) a bytecode .velb en memoria.
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
 * @brief Devuelve la cadena de version de la API/VM.
 *
 * El puntero devuelto es estatico (no liberar con @c vesta_free).
 *
 * @return Cadena terminada en NUL con la version (p.ej. "vesta-capi 1.0").
 */
VESTA_API const char *vesta_version(void);

/**
 * @brief Compila fuente Vex a bytecode .velb en memoria.
 *
 * @param src        Codigo fuente Vex terminado en NUL.
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
 * @brief Compila y ejecuta fuente Vex en una sola llamada.
 *
 * @param src        Codigo fuente Vex terminado en NUL.
 * @param unit_name  Nombre logico del modulo; puede ser NULL.
 * @param out_exit   [salida, opcional] Valor de retorno de main (R0).
 * @param out_err    [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si compilo y ejecuto correctamente; distinto de 0 en caso de
 *         error.
 */
VESTA_API int vesta_eval(const char *src, const char *unit_name, int *out_exit,
                         char **out_err);

/**
 * @brief Compila fuente Vex a texto .vel (bytecode textual intermedio).
 *
 * Reusa el frontend Vex (@c vex::compile_vex_source) y devuelve el texto
 * @c .vel que el ensamblador consume.  Util para inspeccionar la salida
 * del lowering sin pasar por el ensamblado.
 *
 * @param src        Codigo fuente Vex terminado en NUL.
 * @param unit_name  Nombre logico del modulo; puede ser NULL (se usa "main").
 * @param out_vel    [salida] Recibe el texto .vel en heap (liberar con
 *                   @c vesta_free).
 * @param out_err    [salida, opcional] Mensaje de error en heap si falla.
 * @return 0 si exito; distinto de 0 en caso de error.
 */
VESTA_API int vesta_compile_to_vel(const char *src, const char *unit_name,
                                   char **out_vel, char **out_err);

/**
 * @brief Compila fuente Vex a texto del IR SSA (dump legible del IrModule).
 *
 * Activa @c CompileOptions::dump_ir y devuelve @c CompileResult::ir_text,
 * el mismo dump que produce @c --vex-emit-ir.
 *
 * @param src        Codigo fuente Vex terminado en NUL.
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
 * @brief Genera un diagrama del pipeline Vex para un fuente dado.
 *
 * @param src        Codigo fuente Vex terminado en NUL.
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
 * @brief Compila fuente Vex y devuelve a la vez el .vel, el IR y el .velb.
 *
 * Conveniencia que evita compilar tres veces el mismo fuente.  Cada salida
 * es opcional (pasar NULL para omitirla).  Las cadenas/buffer devueltos se
 * liberan con @c vesta_free.
 *
 * @param src        Codigo fuente Vex terminado en NUL.
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
