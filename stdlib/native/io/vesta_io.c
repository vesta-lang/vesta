/**
 * @file vesta_io.c
 * @brief Modulo de entrada/salida de la libreria estandar nativa de VestaVM.
 *
 * Todas las funciones que reciben cadenas o buffers operan directamente sobre
 * la memoria virtual de la VM mediante el par (proc_ptr, vm_addr, len).
 * No se requiere ningun vmcopy previo desde el bytecode.
 *
 * Convencion de llamada:
 *   - Argumentos en r1..r12 como uint64_t; ningun contexto implicito.
 *   - proc_ptr : valor uint64_t obtenido con la instruccion getproc.
 *   - vm_addr  : direccion virtual dentro del espacio de la VM.
 *   - len      : longitud en bytes del dato en memoria VM.
 *   - Floats/doubles : representacion de bits IEEE 754 en uint64_t.
 *   - Retorno en r0 como uint64_t.
 *
 * Patron tipico en .vel:
 * @code
 *   getproc r1                        ; r1 = proc_ptr
 *   mov     r2, @Absolute("all.str")  ; r2 = vm_addr del string
 *   mov     r3, 13                    ; r3 = longitud en bytes
 *   mov     r15, 3
 *   calln   @Method("vesta_io.dll:vio_println")
 * @endcode
 *
 * Importar desde bytecode .vel:
 * @code
 *   @Import {
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_print")      }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_println")     }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_print_int")   }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_print_uint")  }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_print_hex")   }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_print_float") }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_read_line")   }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_fopen")       }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_fclose")      }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_fread")       }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_fwrite")      }
 *       @Method { @Lib("vesta_io.dll")  @Name("vio_fflush")      }
 *   }
 * @endcode
 */

/**
 * @def VESTA_IO_DEBUG
 * @brief Activa el mensaje de carga del modulo en vesta_init cuando vale 1.
 *
 * Definir como 1 para depuracion; dejar en 0 para produccion.
 * Se puede sobreescribir desde la linea de compilacion con -DVESTA_IO_DEBUG=1.
 */
#ifndef VESTA_IO_DEBUG
#  define VESTA_IO_DEBUG 0
#endif

#include "../../../include/ffi/vesta_plugin.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/** @brief Referencia a la API del plugin; inicializada en vesta_init. */
static const VestaPluginAPI *g_api = NULL;

/* -----------------------------------------------------------------------
 * Utilidades internas
 * ----------------------------------------------------------------------- */

/**
 * @brief Lee @p len bytes de la memoria VM a un buffer, terminando en '\0'.
 *
 * Si @p len cabe en @p stack_buf se usa ese buffer de pila para evitar
 * una asignacion dinamica.  Cuando se usa heap, el llamador debe liberar
 * el puntero con vm_buf_free().
 *
 * @param proc_ptr  Valor uint64_t de getproc (ProcessVM*).
 * @param vm_addr   Direccion virtual dentro de la VM.
 * @param len       Numero de bytes a leer.
 * @param stack_buf Buffer de pila del llamador para cadenas cortas.
 * @param stack_sz  Tamano en bytes de stack_buf.
 * @return          Puntero al buffer relleno y terminado en '\0',
 *                  o NULL si la asignacion falla o g_api es NULL.
 */
static char *vm_buf_alloc(uint64_t proc_ptr, uint64_t vm_addr, uint64_t len,
                          char *stack_buf, size_t stack_sz) {
    if (!g_api) return NULL;
    char *buf = (len < stack_sz) ? stack_buf : (char *)malloc(len + 1);
    if (!buf) return NULL;
    g_api->vm_read_bytes(proc_ptr, vm_addr, buf, len);
    buf[len] = '\0';
    return buf;
}

/**
 * @brief Libera el buffer obtenido con vm_buf_alloc si fue heap-asignado.
 *
 * No hace nada si @p buf coincide con @p stack_buf (es el buffer de pila).
 *
 * @param buf       Puntero a liberar.
 * @param stack_buf Buffer de pila del llamador (referencia para comparar).
 */
static void vm_buf_free(char *buf, const char *stack_buf) {
    if (buf && buf != stack_buf) free(buf);
}

/* -----------------------------------------------------------------------
 * Punto de entrada del plugin
 * ----------------------------------------------------------------------- */

/**
 * @brief Punto de entrada del plugin; llamado una vez por VestaVM al cargar.
 *
 * Guarda la referencia a la API para uso en todas las funciones del modulo.
 * El mensaje de carga solo se emite si VESTA_IO_DEBUG esta definido como 1.
 *
 * @param api  Puntero a la tabla de funciones de VestaVM.
 */
VESTA_PLUGIN_EXPORT void vesta_init(const VestaPluginAPI *api) {
    g_api = api;
#if VESTA_IO_DEBUG
    if (api) api->log("[vesta_io] cargado");
#endif
}

/* -----------------------------------------------------------------------
 * Salida de texto desde memoria VM
 * ----------------------------------------------------------------------- */

/**
 * @brief Imprime una cadena de la memoria VM en stdout sin salto de linea.
 *
 * Lee @p len bytes desde @p vm_addr en el proceso @p proc_ptr y los
 * escribe en stdout mediante fputs.
 *
 * Patron en .vel (argc = 3):
 * @code
 *   getproc r1
 *   mov     r2, @Absolute("all.msg")
 *   mov     r3, 5
 *   mov     r15, 3
 *   calln   @Method("vesta_io.dll:vio_print")
 * @endcode
 *
 * @param proc_ptr  Puntero al proceso VM (resultado de getproc).
 * @param vm_addr   Direccion virtual del string en la VM.
 * @param len       Longitud en bytes del string (sin '\0').
 * @return          0 siempre.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print(uint64_t proc_ptr,
                                        uint64_t vm_addr,
                                        uint64_t len) {
    if (!g_api || !len) return 0;
    char  stack_buf[256];
    char *buf = vm_buf_alloc(proc_ptr, vm_addr, len, stack_buf, sizeof(stack_buf));
    if (!buf) return 0;
    fputs(buf, stdout);
    vm_buf_free(buf, stack_buf);
    return 0;
}

/**
 * @brief Imprime una cadena de la memoria VM en stdout con salto de linea.
 *
 * Identico a vio_print pero agrega '\n' al final mediante puts().
 * Si @p len es 0 imprime una linea vacia.
 *
 * Patron en .vel (argc = 3):
 * @code
 *   getproc r1
 *   mov     r2, @Absolute("all.msg")
 *   mov     r3, 12
 *   mov     r15, 3
 *   calln   @Method("vesta_io.dll:vio_println")
 * @endcode
 *
 * @param proc_ptr  Puntero al proceso VM (resultado de getproc).
 * @param vm_addr   Direccion virtual del string en la VM.
 * @param len       Longitud en bytes del string (sin '\0').
 * @return          0 siempre.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_println(uint64_t proc_ptr,
                                          uint64_t vm_addr,
                                          uint64_t len) {
    if (!g_api || !len) { puts(""); return 0; }
    char  stack_buf[256];
    char *buf = vm_buf_alloc(proc_ptr, vm_addr, len, stack_buf, sizeof(stack_buf));
    if (!buf) return 0;
    puts(buf);  /* puts anade '\n' automaticamente */
    vm_buf_free(buf, stack_buf);
    return 0;
}

/* -----------------------------------------------------------------------
 * Salida de valores numericos (sin acceso a memoria VM)
 * ----------------------------------------------------------------------- */

/**
 * @brief Imprime un entero de 64 bits con signo seguido de '\n'.
 *
 * Patron en .vel (argc = 1):
 * @code
 *   mov   r1, 42
 *   mov   r15, 1
 *   calln @Method("vesta_io.dll:vio_print_int")
 * @endcode
 *
 * @param n  Valor a imprimir interpretado como int64_t.
 * @return   0 siempre.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_int(uint64_t n) {
    printf("%lld\n", (long long)(int64_t)n);
    return 0;
}

/**
 * @brief Imprime un entero de 64 bits sin signo seguido de '\n'.
 *
 * @param n  Valor a imprimir como uint64_t.
 * @return   0 siempre.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_uint(uint64_t n) {
    printf("%llu\n", (unsigned long long)n);
    return 0;
}

/**
 * @brief Imprime un entero de 64 bits en hexadecimal con prefijo 0x y '\n'.
 *
 * Formato: 0xXXXXXXXXXXXXXXXX (16 digitos, mayusculas, relleno con ceros).
 *
 * @param n  Valor a imprimir.
 * @return   0 siempre.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_hex(uint64_t n) {
    printf("0x%016llX\n", (unsigned long long)n);
    return 0;
}

/**
 * @brief Imprime un double codificado como bits IEEE 754 seguido de '\n'.
 *
 * El llamador carga los bits del double como entero y los pasa directamente:
 * @code
 *   mov   r1, 0x4000000000000000  ; bits de 2.0
 *   mov   r15, 1
 *   calln @Method("vesta_io.dll:vio_print_float")
 * @endcode
 *
 * @param bits  Representacion de bits IEEE 754 del double a imprimir.
 * @return      0 siempre.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_print_float(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));  /* reinterpretar bits como double sin UB */
    printf("%g\n", d);
    return 0;
}

/* -----------------------------------------------------------------------
 * Entrada de texto hacia memoria VM
 * ----------------------------------------------------------------------- */

/**
 * @brief Lee una linea de stdin y la escribe en la memoria virtual de la VM.
 *
 * Lee hasta @p max_bytes - 1 bytes de stdin (incluye '\n' si cabe) y escribe
 * el resultado terminado en '\0' en la direccion virtual @p vm_addr del
 * proceso @p proc_ptr.
 *
 * @param proc_ptr   Puntero al proceso VM (resultado de getproc).
 * @param vm_addr    Direccion VM de destino donde escribir la linea leida.
 * @param max_bytes  Tamano del buffer de destino en bytes (incluye el '\0').
 * @return           Numero de bytes escritos en la VM sin contar el '\0',
 *                   o 0 en caso de EOF, error o max_bytes == 0.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_read_line(uint64_t proc_ptr,
                                            uint64_t vm_addr,
                                            uint64_t max_bytes) {
    if (!g_api || max_bytes == 0) return 0;

    char *host_buf = (char *)malloc(max_bytes);
    if (!host_buf) return 0;

    if (!fgets(host_buf, (int)max_bytes, stdin)) {
        /* EOF o error: escribir cadena vacia en la VM */
        host_buf[0] = '\0';
        g_api->vm_write_bytes(proc_ptr, vm_addr, host_buf, 1);
        free(host_buf);
        return 0;
    }

    uint64_t written = (uint64_t)strlen(host_buf);
    /* escribir incluyendo el '\0' para dejar la VM bien terminada */
    g_api->vm_write_bytes(proc_ptr, vm_addr, host_buf, written + 1);
    free(host_buf);
    return written;
}

/* -----------------------------------------------------------------------
 * Operaciones de fichero (rutas y datos en memoria VM)
 * ----------------------------------------------------------------------- */

/**
 * @brief Abre un fichero cuya ruta y modo residen en la memoria VM.
 *
 * Lee la cadena de ruta desde (@p proc_ptr, @p path_vm_addr, @p path_len) y
 * la cadena de modo desde (@p proc_ptr, @p mode_vm_addr, @p mode_len),
 * luego invoca fopen() en el host.
 *
 * Modos tipicos: "r", "w", "a", "rb", "wb", "ab", "r+", "w+".
 *
 * Patron en .vel (argc = 5):
 * @code
 *   getproc r1
 *   mov     r2, @Absolute("all.path")
 *   mov     r3, 8          ; longitud de la ruta
 *   mov     r4, @Absolute("all.mode")
 *   mov     r5, 2          ; longitud del modo, p.ej. "rb"
 *   mov     r15, 5
 *   calln   @Method("vesta_io.dll:vio_fopen")
 * @endcode
 *
 * @param proc_ptr      Puntero al proceso VM (resultado de getproc).
 * @param path_vm_addr  Direccion VM de la cadena de ruta.
 * @param path_len      Longitud en bytes de la ruta (sin '\0').
 * @param mode_vm_addr  Direccion VM de la cadena de modo de apertura.
 * @param mode_len      Longitud en bytes del modo (sin '\0'; tipicamente 1-3).
 * @return              Handle al FILE* como uint64_t, o 0 si falla.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_fopen(uint64_t proc_ptr,
                                        uint64_t path_vm_addr,
                                        uint64_t path_len,
                                        uint64_t mode_vm_addr,
                                        uint64_t mode_len) {
    if (!g_api) return 0;

    char  path_stack[512];
    char  mode_stack[8];
    char *path = vm_buf_alloc(proc_ptr, path_vm_addr, path_len,
                              path_stack, sizeof(path_stack));
    char *mode = vm_buf_alloc(proc_ptr, mode_vm_addr, mode_len,
                              mode_stack, sizeof(mode_stack));
    if (!path || !mode) {
        vm_buf_free(path, path_stack);
        vm_buf_free(mode, mode_stack);
        return 0;
    }

    FILE *f = fopen(path, mode);
    vm_buf_free(path, path_stack);
    vm_buf_free(mode, mode_stack);
    return (uint64_t)(uintptr_t)f;
}

/**
 * @brief Cierra el fichero representado por @p handle.
 *
 * Envuelve fclose(); no requiere acceso a la memoria VM.
 *
 * @param handle  FILE* como uint64_t obtenido de vio_fopen.
 *                Pasar 0 devuelve error sin llamar a fclose.
 * @return        0 si se cerro correctamente, distinto de 0 si hubo error.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_fclose(uint64_t handle) {
    if (!handle) return (uint64_t)-1;
    return (uint64_t)fclose((FILE *)(uintptr_t)handle);
}

/**
 * @brief Lee bytes del fichero @p handle y los escribe en la memoria VM.
 *
 * Lee hasta @p size bytes del fichero y los almacena en la direccion
 * virtual @p vm_addr del proceso @p proc_ptr.
 *
 * @param proc_ptr  Puntero al proceso VM (resultado de getproc).
 * @param vm_addr   Direccion VM de destino donde escribir los datos leidos.
 * @param size      Numero maximo de bytes a leer.
 * @param handle    FILE* como uint64_t obtenido de vio_fopen.
 * @return          Numero de bytes efectivamente leidos y escritos en la VM,
 *                  o 0 si algun argumento es invalido.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_fread(uint64_t proc_ptr,
                                        uint64_t vm_addr,
                                        uint64_t size,
                                        uint64_t handle) {
    if (!g_api || !handle || !size) return 0;

    char *host_buf = (char *)malloc(size);
    if (!host_buf) return 0;

    uint64_t n = (uint64_t)fread(host_buf, 1, (size_t)size,
                                 (FILE *)(uintptr_t)handle);
    if (n > 0) g_api->vm_write_bytes(proc_ptr, vm_addr, host_buf, n);
    free(host_buf);
    return n;
}

/**
 * @brief Lee bytes de la memoria VM y los escribe en el fichero @p handle.
 *
 * Lee @p size bytes desde la direccion virtual @p vm_addr del proceso
 * @p proc_ptr y los escribe en el fichero host.
 *
 * @param proc_ptr  Puntero al proceso VM (resultado de getproc).
 * @param vm_addr   Direccion VM origen de los datos a escribir en el fichero.
 * @param size      Numero de bytes a escribir.
 * @param handle    FILE* como uint64_t obtenido de vio_fopen.
 * @return          Numero de bytes escritos en el fichero, o 0 si error.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_fwrite(uint64_t proc_ptr,
                                         uint64_t vm_addr,
                                         uint64_t size,
                                         uint64_t handle) {
    if (!g_api || !handle || !size) return 0;

    char *host_buf = (char *)malloc(size);
    if (!host_buf) return 0;

    g_api->vm_read_bytes(proc_ptr, vm_addr, host_buf, size);
    uint64_t n = (uint64_t)fwrite(host_buf, 1, (size_t)size,
                                  (FILE *)(uintptr_t)handle);
    free(host_buf);
    return n;
}

/**
 * @brief Vacia el buffer de escritura del fichero @p handle.
 *
 * Equivalente a fflush(handle).  Pasar handle=0 vacia todos los streams
 * abiertos (equivale a fflush(NULL)).
 *
 * @param handle  FILE* como uint64_t, o 0 para vaciar todos los streams.
 * @return        0 si correcto, distinto de 0 si hay error.
 */
VESTA_PLUGIN_EXPORT uint64_t vio_fflush(uint64_t handle) {
    FILE *f = handle ? (FILE *)(uintptr_t)handle : NULL;
    return (uint64_t)fflush(f);
}
