/**
 * @file runtime_symbols.h
 * @brief Los nombres del CONTRATO entre el compilador y el runtime de Vesta:
 *        las funciones que el codigo generado llama sin que el programa las
 *        nombre, y las que el propio bajado fabrica para el programa.
 *
 * Todos empiezan por `__vx_`.  Hay dos familias, y conviene no confundirlas:
 *
 *  - Las que IMPLEMENTA el runtime, escrito en Vesta en `stdlib/vx/` y fundido
 *    en el objeto (`vx_exc.vx`, `vx_io.vx`, `vx_async.vx`...).  El compilador
 *    las LLAMA; si el nombre cambia en un lado y no en el otro, el enlace falla
 *    con un simbolo sin resolver.  Cada grupo dice de que fichero sale.
 *  - Las que FABRICA el bajado dentro del propio modulo: el despacho por CPU,
 *    los accesores de cadena, las conversiones a texto.  Aqui el compilador es
 *    a la vez quien las crea y quien las reconoce despues.
 *
 * Antes cada nombre estaba escrito a mano en cada sitio que lo usaba -- el
 * arranque del despacho por CPU lo reconocian por separado el bajado, el
 * proyecto, el nativo y el enlazador --, y en uno se comparaba con la longitud
 * del nombre escrita como numero.  Es solo cabecera a proposito: la incluye el
 * enlazador, que no enlaza con el resto del IR.
 */
#ifndef IR_RUNTIME_SYMBOLS_H
#define IR_RUNTIME_SYMBOLS_H

#include <string>

namespace ir {
namespace rt {

/** @brief El prefijo que llevan todos los nombres de este fichero. */
inline constexpr const char kPrefix[] = "__vx_";

/**
 * @brief Si @p name es un nombre del runtime (de cualquiera de las dos
 *        familias).
 * @param name Nombre de simbolo.
 * @return true si empieza por `__vx_`.
 */
inline bool is_runtime_symbol(const std::string &name) noexcept {
    return name.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0;
}

// ===========================================================================
// Implementados por el runtime (stdlib/vx/)
// ===========================================================================

// --- Excepciones: vx_exc.vx (setjmp/longjmp; `--no-exceptions` lo quita) ---
inline constexpr const char kSetjmp[] = "__vx_setjmp";         ///< guarda el contexto del try
inline constexpr const char kPushFrame[] = "__vx_push_frame";  ///< apila el marco del try
inline constexpr const char kPopFrame[] = "__vx_pop_frame";    ///< lo desapila
inline constexpr const char kGetValue[] = "__vx_get_value";    ///< la excepcion en vuelo
inline constexpr const char kGetType[] = "__vx_get_type";      ///< y su tipo
inline constexpr const char kThrow[] = "__vx_throw";           ///< lanza

// --- E/S y fallos: vx_io.vx ---
inline constexpr const char kWrite[] = "__vx_write";           ///< escribe (ptr, len)
inline constexpr const char kFlush[] = "__vx_flush";           ///< vacia la salida
inline constexpr const char kPad[] = "__vx_pad";               ///< relleno (caracter, cuantos)
inline constexpr const char kPanicNull[] = "__vx_panic_null";  ///< afirmacion de no-nulo fallida
inline constexpr const char kPrintI64[] = "__vx_print_i64";
inline constexpr const char kPrintU64[] = "__vx_print_u64";
inline constexpr const char kPrintHex[] = "__vx_print_hex";
inline constexpr const char kPrintBin[] = "__vx_print_bin";
inline constexpr const char kPrintOct[] = "__vx_print_oct";
inline constexpr const char kPrintBool[] = "__vx_print_bool";
inline constexpr const char kPrintChar[] = "__vx_print_char";
inline constexpr const char kPrintPtr[] = "__vx_print_ptr";
inline constexpr const char kPrintCstr[] = "__vx_print_cstr";
inline constexpr const char kPrintFloat[] = "__vx_print_float";

// --- Monitores: vx_sync.vx ---
inline constexpr const char kMonEnter[] = "__vx_monenter";
inline constexpr const char kMonExit[] = "__vx_monexit";

// --- Hilos del sistema: vx_thread.vx ---
inline constexpr const char kThreadRun[] = "__vx_thread_run";
inline constexpr const char kThreadJoinAll[] = "__vx_thread_join_all";

// --- Procesos, futuros y buzones: vx_async.vx ---
inline constexpr const char kSpawn[] = "__vx_spawn";           ///< encola una tarea; devuelve el pid
inline constexpr const char kSpawnArgv[] = "__vx_spawn_argv";  ///< idem, con argumentos
inline constexpr const char kFutureNew[] = "__vx_future_new";
inline constexpr const char kAwait[] = "__vx_await";
inline constexpr const char kFulfill[] = "__vx_fulfill";
inline constexpr const char kMsgSend[] = "__vx_msgsend";
inline constexpr const char kMsgRecv[] = "__vx_msgrecv";
inline constexpr const char kPid[] = "__vx_pid";

// --- Fibras: vx_fiber.vx ---
inline constexpr const char kSwapCtx[] = "__vx_swapctx";       ///< cambio de contexto cooperativo
/** @brief Donde arranca una fibra nueva.  Es del runtime aunque no lleve el
 *         prefijo `__vx_`: ::is_runtime_symbol no lo reconoce. */
inline constexpr const char kFiberTrampoline[] = "__fiber_trampoline";

// --- Asignador: std/alloc.vx ---
inline constexpr const char kMalloc[] = "__vx_malloc";
inline constexpr const char kFree[] = "__vx_free";

// --- Fallos del procesador: vx_fault.vx ---
inline constexpr const char kFaultInit[] = "__vx_fault_init";

// --- FFI en ejecucion: vx_ffi.vx ---
inline constexpr const char kDlopen[] = "__vx_dlopen";
inline constexpr const char kDlsym[] = "__vx_dlsym";

// ===========================================================================
// Puntos de entrada que pone el emisor o el cargador
// ===========================================================================

/**
 * @brief Alias de la funcion de reserva que haya elegido el programa (su
 *        `@AllocatorOverride` o la de la stdlib).  Es una etiqueta mas sobre
 *        el mismo codigo, no una funcion puente: el cargador la busca por este
 *        nombre para enchufar el asignador.
 */
inline constexpr const char kAllocEntry[] = "__vx_alloc_entry";
/** @brief El alias equivalente para liberar. */
inline constexpr const char kFreeEntry[] = "__vx_free_entry";

/**
 * @brief La funcion que el cargador de Windows llama en cada alta de hilo para
 *        copiar la plantilla de las `thread_local` con valor inicial.  La
 *        fabrica el bajado; hace tambien de punto de entrada de la `.dll`.
 */
inline constexpr const char kTlsInit[] = "__vx_tls_init";
/** @brief El indice TLS del modulo (simbolo externo de un PE). */
inline constexpr const char kTlsIndex[] = "__vx_tls_index";

// ===========================================================================
// Fabricados por el bajado dentro del modulo
// ===========================================================================

// --- Despacho por CPU ---
inline constexpr const char kCpuInit[] = "__vx_cpu_init";          ///< ejecuta cpuid
inline constexpr const char kCpuFeatures[] = "__vx_cpu_features";  ///< donde lo guarda
inline constexpr const char kMemcpyInit[] = "__vx_memcpy_init";    ///< elige la copia
inline constexpr const char kMemcpyBase[] = "__vx_memcpy_base";
inline constexpr const char kMemcpyAvx2[] = "__vx_memcpy_avx2";
inline constexpr const char kMemcpyFp[] = "__vx_memcpy_fp";        ///< la elegida
inline constexpr const char kStrDispInit[] = "__vx_strdisp_init";  ///< elige las de cadena
inline constexpr const char kStrcmpBase[] = "__vx_strcmp_base";
inline constexpr const char kStrlenBase[] = "__vx_strlen_base";
inline constexpr const char kStrcmpFp[] = "__vx_strcmp_fp";
inline constexpr const char kStrlenFp[] = "__vx_strlen_fp";

/**
 * @brief Cuantas funciones de arranque tiene el despacho por CPU.
 */
inline constexpr int kDispatchInitCount = 3;

/**
 * @brief Las funciones de arranque del despacho por CPU, EN EL ORDEN en que
 *        tienen que correr antes de `main`: primero se lee la CPU, y las otras
 *        dos eligen segun lo leido.
 *
 * Cada modulo trae las suyas; el nativo las exporta aunque empiecen por dos
 * guiones y el enlazador las recoge de cada objeto.
 */
inline constexpr const char *const kDispatchInits[kDispatchInitCount] = {
    kCpuInit, kMemcpyInit, kStrDispInit};

/**
 * @brief La posicion de @p name en ::kDispatchInits.
 * @param name Nombre de funcion.
 * @return Su indice (0 = la que corre primero), o -1 si no es una de ellas.
 */
inline int dispatch_init_index(const std::string &name) noexcept {
    for (int i = 0; i < kDispatchInitCount; ++i)
        if (name == kDispatchInits[i]) return i;
    return -1;
}

// --- Cadenas (Vesta Embed) ---
/**
 * @brief El prefijo comun de los accesores de cadena que fabrica el bajado
 *        (`__vx_strdata`, `__vx_str_cplen`, `__vx_strcmp_base`...).
 */
inline constexpr const char kStringHelperPrefix[] = "__vx_str";
inline constexpr const char kStrCpLen[] = "__vx_str_cplen";        ///< puntos de codigo
inline constexpr const char kStrToUtf16[] = "__vx_str_to_utf16";
inline constexpr const char kStrData[] = "__vx_strdata";

/**
 * @brief Si @p name es uno de los accesores de cadena.
 * @param name Nombre de funcion.
 * @return true si lleva ::kStringHelperPrefix.
 */
inline bool is_string_helper(const std::string &name) noexcept {
    return name.compare(0, sizeof(kStringHelperPrefix) - 1,
                        kStringHelperPrefix) == 0;
}

// --- Conversiones a texto ---
inline constexpr const char kItoaSigned[] = "__vx_itoa_s";
inline constexpr const char kItoaUnsigned[] = "__vx_itoa_u";
inline constexpr const char kCtoa[] = "__vx_ctoa";                 ///< caracter
inline constexpr const char kBtoa[] = "__vx_btoa";                 ///< booleano

// --- Arranque del programa ---
/** @brief Inicializa lo que el programa necesita antes de `main`. */
inline constexpr const char kAutoInit[] = "__vx_auto_init";
/**
 * @brief El cuerpo de `main` cuando el bajado lo separa en una version por
 *        CPU; `main` queda como el que elige.
 */
inline constexpr const char kMainBody[] = "__vx_main_body";

// --- Descriptores de clase en el binario nativo ---
/** @brief El mapa de campos de una clase: `__vx_fmap_<clase>`. */
inline std::string field_map_symbol(const std::string &cls) {
    return std::string("__vx_fmap_") + cls;
}
/** @brief El descriptor de tipo de una clase: `__vx_tdesc_<clase>`. */
inline std::string type_desc_symbol(const std::string &cls) {
    return std::string("__vx_tdesc_") + cls;
}

} // namespace rt
} // namespace ir

#endif // IR_RUNTIME_SYMBOLS_H
