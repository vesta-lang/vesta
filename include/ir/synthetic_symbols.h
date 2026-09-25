/**
 * @file synthetic_symbols.h
 * @brief Los nombres de simbolo que genera el PROPIO compilador, no el
 *        programa: como se llaman y como se reconocen.
 *
 * Son una representacion compartida por capas muy distintas -- la bajada los
 * crea, el optimizador, los analisis, el precomputo, los backends y el
 * enlazador los reconocen --, y cada una los escribia a mano.  Escrito en nueve
 * ficheros, `__module_init` acabo con dos criterios distintos de "es este" (uno
 * miraba si el nombre EMPEZABA por el, otro si lo CONTENIA); y los prefijos se
 * quitaban con su longitud escrita a mano (`substr(8)`), que se rompe en
 * silencio si el prefijo cambia.  Vive en la capa del IR porque es la mas baja
 * que todos esos usuarios comparten.
 *
 * Todos empiezan por dos guiones bajos, que un nombre de Vesta no puede llevar:
 * es lo que hace segura la marca por prefijo.
 */
#ifndef IR_SYNTHETIC_SYMBOLS_H
#define IR_SYNTHETIC_SYMBOLS_H

#include <cstddef>
#include <string>

namespace ir {

// ===========================================================================
// Las dos operaciones sobre prefijos, escritas una vez
// ===========================================================================

/**
 * @brief Si @p name empieza por @p prefix (uno de los de este fichero).
 * @param name   Nombre.
 * @param prefix Prefijo.
 * @return true si lo lleva.
 */
inline bool has_synthetic_prefix(const std::string &name,
                                 const char *prefix) noexcept {
    return name.compare(0, std::char_traits<char>::length(prefix), prefix) ==
           0;
}

/**
 * @brief @p name sin @p prefix, si lo lleva.
 * @param name   Nombre.
 * @param prefix Prefijo.
 * @return El nombre sin el prefijo; @p name tal cual si no lo lleva.
 */
inline std::string strip_synthetic_prefix(const std::string &name,
                                          const char *prefix) {
    return has_synthetic_prefix(name, prefix)
               ? name.substr(std::char_traits<char>::length(prefix))
               : name;
}

// ===========================================================================
// El arranque de un modulo
// ===========================================================================

/**
 * @brief El nombre de la funcion de arranque de un modulo: la que registra sus
 *        clases, sus metodos y sus globales antes de `main`.
 *
 * El cargador la invoca por su `init_pc`; el nombre es como la reconocen los
 * demas.
 */
inline constexpr const char kModuleInit[] = "__module_init";

/**
 * @brief Si @p name es EXACTAMENTE la funcion de arranque de un modulo.
 * @param name Nombre de funcion.
 * @return true solo para la forma principal.
 */
inline bool is_module_init(const std::string &name) noexcept {
    return name == kModuleInit;
}

/**
 * @brief Si @p name es de la FAMILIA de la funcion de arranque: la principal,
 *        sus tandas (`__module_init_partN`) o la de una dependencia renombrada
 *        al fusionar (`__module_init_<modulo>`).
 *
 * Todas se ejecutan por el arranque y no por una llamada del programa, asi que
 * quien pregunta "es codigo de arranque" quiere esta, no @ref is_module_init.
 *
 * @param name Nombre de funcion.
 * @return true si empieza por @ref kModuleInit.
 */
inline bool is_module_init_family(const std::string &name) noexcept {
    return has_synthetic_prefix(name, kModuleInit);
}

/**
 * @brief El nombre de la tanda @p index de la funcion de arranque @p base.
 *
 * Un `__module_init` muy grande se parte en tandas que la principal llama por
 * nombre.
 *
 * @param base  Nombre de la funcion que se parte.
 * @param index Numero de tanda.
 * @return El nombre de la tanda.
 */
inline std::string module_init_part_name(const std::string &base,
                                         size_t index) {
    return base + "_part" + std::to_string(index);
}

// ===========================================================================
// El ayudante de construccion (`new X()`)
// ===========================================================================

/// Prefijo del ayudante que reserva y construye un objeto.
inline constexpr const char kNewHelperPrefix[] = "__new_";
/// Sufijo de la variante del ayudante que reserva en la memoria COMPARTIDA.
inline constexpr const char kNewHelperSharedSuffix[] = "_shared";
/// Sufijo de la variante del ayudante que reserva en el monton del RECOLECTOR.
inline constexpr const char kNewHelperGcSuffix[] = "_gc";

/**
 * @brief El principio del nombre del ayudante de construccion de @p cls.
 *
 * Detras pueden ir el discriminante del constructor, si la clase tiene varios,
 * y el sufijo de la variante: los pone quien conoce el constructor
 * (`vx::new_helper_symbol`).
 *
 * @param cls Nombre de la clase.
 * @return El nombre base del ayudante.
 */
inline std::string new_helper_base_name(const std::string &cls) {
    return kNewHelperPrefix + cls;
}

/**
 * @brief Si @p name es un ayudante de construccion de CUALQUIER variante,
 *        la compartida incluida.
 * @param name Nombre de la funcion.
 * @return true si lleva el prefijo de los ayudantes de construccion.
 */
inline bool is_any_new_helper(const std::string &name) noexcept {
    return has_synthetic_prefix(name, kNewHelperPrefix);
}

/**
 * @brief Si @p name es un ayudante de construccion, y cual es el trozo que
 *        sigue al prefijo.
 *
 * La variante compartida NO cuenta: registra el objeto en la tabla de
 * compartidos, un efecto observable, asi que quien pregunta "es un ayudante de
 * construccion corriente" no la quiere.
 *
 * El trozo que sigue al prefijo solo es el nombre de la clase cuando esta tiene
 * un unico constructor; para saber QUE clase construye, `ir::new_helper_class`.
 *
 * @param name      Nombre de la funcion.
 * @param out_class Si no es nulo y lo es, recibe lo que sigue al prefijo.
 * @return true si lo es.
 */
bool is_new_helper_name(const std::string &name, std::string *out_class);

/**
 * @brief Si @p name es la variante COMPARTIDA de un ayudante de construccion.
 * @param name Nombre de la funcion.
 * @return true si lleva el sufijo de la variante compartida.
 */
bool is_shared_new_helper(const std::string &name) noexcept;

// ===========================================================================
// Codigo que corre al COMPILAR
// ===========================================================================

/// Prefijo del cuerpo de una macro o funcion comptime bajado a funcion.
inline constexpr const char kMacroPrefix[] = "__macro_";
/// Prefijo de un bloque `comptime { }` bajado a funcion.
inline constexpr const char kCtBlockPrefix[] = "__ctblock_";

/**
 * @brief El simbolo con que se baja el cuerpo de la macro (o funcion comptime)
 *        @p name.
 * @param name Nombre de la macro tal como la conoce el programa.
 * @return El simbolo.
 */
inline std::string macro_symbol(const std::string &name) {
    return kMacroPrefix + name;
}

/**
 * @brief Si @p name es el cuerpo de una macro o funcion comptime: codigo que
 *        corre al COMPILAR, no en el programa.
 *
 * Quien juzga el programa -- limites, emision -- tiene que saltarselas:
 * juzgarlas es juzgar otro programa.
 *
 * @param name Nombre de funcion.
 * @return true si lleva el prefijo de las macros.
 */
inline bool is_macro_symbol(const std::string &name) noexcept {
    return has_synthetic_prefix(name, kMacroPrefix);
}

/**
 * @brief El nombre de la macro que baja @p name, sin el prefijo.
 * @param name Nombre de funcion.
 * @return El nombre sin el prefijo; @p name tal cual si no lo lleva.
 */
inline std::string macro_base_name(const std::string &name) {
    return strip_synthetic_prefix(name, kMacroPrefix);
}

/// @brief El bloque `comptime { }` numero @p index.  @return El simbolo.
inline std::string ctblock_symbol(size_t index) {
    return kCtBlockPrefix + std::to_string(index);
}

/// @brief Si @p name es un bloque `comptime { }`.  @return true si lo es.
inline bool is_ctblock_symbol(const std::string &name) noexcept {
    return has_synthetic_prefix(name, kCtBlockPrefix);
}

// ===========================================================================
// Cuerpos que el compilador saca a su propia funcion
// ===========================================================================

/// Prefijo del cuerpo de una lambda.
inline constexpr const char kLambdaPrefix[] = "__lambda_";
/// Prefijo del cuerpo de un `spawn`.
inline constexpr const char kSpawnPrefix[] = "__spawn_";
/// Prefijo del cuerpo de un `spawn` REMOTO.
inline constexpr const char kRemoteSpawnPrefix[] = "__rspawn_";
/// Prefijo del ayudante que envuelve el cuerpo de una funcion `@Async`.
inline constexpr const char kAsyncPrefix[] = "__async_";

/// @brief El cuerpo de la lambda numero @p index.  @return El simbolo.
inline std::string lambda_symbol(size_t index) {
    return kLambdaPrefix + std::to_string(index);
}
/// @brief Si @p name es el cuerpo de una lambda.  @return true si lo es.
inline bool is_lambda_symbol(const std::string &name) noexcept {
    return has_synthetic_prefix(name, kLambdaPrefix);
}
/// @brief El cuerpo del `spawn` numero @p index.  @return El simbolo.
inline std::string spawn_symbol(size_t index) {
    return kSpawnPrefix + std::to_string(index);
}
/// @brief El cuerpo del `spawn` remoto numero @p index.  @return El simbolo.
inline std::string remote_spawn_symbol(size_t index) {
    return kRemoteSpawnPrefix + std::to_string(index);
}
/// @brief El ayudante asincrono de la funcion @p fn.  @return El simbolo.
inline std::string async_helper_symbol(const std::string &fn) {
    return kAsyncPrefix + fn;
}

// ===========================================================================
// Plantillas y excepciones
// ===========================================================================

/// Prefijo de una plantilla generica exportada por su fuente.
inline constexpr const char kTemplatePrefix[] = "__tpl__";
/// El manejador de una excepcion que nadie captura.
inline constexpr const char kUncaught[] = "__uncaught";
/// Lo que informa de esa excepcion antes de terminar.
inline constexpr const char kUncaughtReport[] = "__uncaught_report";

/// @brief La plantilla generica @p mangled, ya con su espacio de nombres.
/// @return El simbolo.
inline std::string template_symbol(const std::string &mangled) {
    return kTemplatePrefix + mangled;
}
/// @brief @p name sin el prefijo de plantilla, si lo lleva.  @return Nombre.
inline std::string template_base_name(const std::string &name) {
    return strip_synthetic_prefix(name, kTemplatePrefix);
}

// ===========================================================================
// La pregunta de conjunto
// ===========================================================================

/**
 * @brief Si @p name es una funcion que se invento el COMPILADOR y no el
 *        programa.
 *
 * Es la pregunta de quien no quiere ver envoltorios -- la traza y los ganchos
 * de instrumentacion, que se piden para ver EL programa, y quien decide que
 * cuenta como codigo del usuario --.  Estaba escrita en seis sitios: uno con la
 * lista completa y cinco con una lista corta, que dejaban pasar como codigo del
 * usuario los cuerpos de un spawn remoto, de una macro, de una vista, de una
 * copia y del runtime.
 *
 * @param name Nombre de la funcion, ya renombrado.
 * @return true si la genero el compilador.
 */
bool is_compiler_generated(const std::string &name) noexcept;

} // namespace ir

#endif // IR_SYNTHETIC_SYMBOLS_H
