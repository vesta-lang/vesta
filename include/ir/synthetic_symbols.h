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

/**
 * @brief El bloque `comptime { }` numero @p index de un modulo.
 *
 * Con el modulo dentro del nombre: los bloques se numeran por modulo, y al
 * fusionar dos que tengan un `comptime { }` los dos serian `__ctblock_0`.
 * El raiz no lleva modulo, como el resto de sus simbolos.
 *
 * @param owner Modulo que lo declara; vacio en el raiz.
 * @param index Su numero dentro del modulo.
 * @return El simbolo: `__ctblock_N` o `__ctblock_<modulo>__N`.
 */
inline std::string ctblock_symbol(const std::string &owner, size_t index) {
    std::string out = kCtBlockPrefix;
    if (!owner.empty()) {
        out += owner;
        out += "__";
    }
    out += std::to_string(index);
    return out;
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
// Vistas sobre bytes (`@overlay`)
// ===========================================================================

/// Prefijo comun de todo lo que se fabrica para una vista.
inline constexpr const char kOverlayPrefix[] = "__ovl_";
/// El resolutor de un campo con `@offset { }`: devuelve su DIRECCION.
inline constexpr const char kOverlayResolverPrefix[] = "__ovl_resolve_";
/// El resolutor de un elemento de un array con `@element { }`.
inline constexpr const char kOverlayElementPrefix[] = "__ovl_element_";
/// Lo que calcula `extent(v)`: el tamano de la vista en bytes.
inline constexpr const char kOverlayExtentPrefix[] = "__ovl_extent_";
/**
 * @brief El nombre con que se liga, DENTRO de un resolutor, el puntero a la
 *        vista raiz.  No es un simbolo: es una variable local que el
 *        programa no puede nombrar, y la lee `parent<T>()`.
 */
inline constexpr const char kOverlayRootBinding[] = "__ovl_root";

/**
 * @brief El resolutor del campo @p field de la vista @p view.
 * @param view       Vista.
 * @param field      Campo.
 * @param is_element El de `@element` (por elemento) en vez del de `@offset`.
 * @return El simbolo.
 */
inline std::string overlay_resolver_symbol(const std::string &view,
                                           const std::string &field,
                                           bool is_element) {
    return (is_element ? kOverlayElementPrefix : kOverlayResolverPrefix) +
           view + "_" + field;
}
/// @brief El que calcula la extension de la vista @p view.  @return Simbolo.
inline std::string overlay_extent_symbol(const std::string &view) {
    return kOverlayExtentPrefix + view;
}
/**
 * @brief Si @p name es el resolutor de un campo `@offset { }`.  Los de
 *        `@element` NO: la pregunta la hace el inliner, que solo excluye
 *        estos.
 * @param name Nombre de funcion.
 * @return true si lo es.
 */
inline bool is_overlay_offset_resolver(const std::string &name) noexcept {
    return has_synthetic_prefix(name, kOverlayResolverPrefix);
}

// ===========================================================================
// Nombres con `$`: variantes y ranuras
// ===========================================================================
//
// Un identificador de Vesta no puede llevar `$`, asi que un sufijo con `$` no
// choca nunca con un nombre del programa.

/**
 * @brief El cuerpo de una funcion con varias versiones por ancho vectorial:
 *        la original pasa a elegir, y el cuerpo se llama `<f>$mv` (`main`
 *        conserva `rt::kMainBody`).
 */
inline constexpr const char kMultiVersionSuffix[] = "$mv";
/// La ranura donde se guarda la version elegida de un cuerpo: `<cuerpo>$fp`.
inline constexpr const char kChosenVersionSlotSuffix[] = "$fp";
/**
 * @brief Las variantes por ancho de un cuerpo multiversion.  Las ESCRIBE el
 *        bajado (al elegir, guarda la direccion de `<cuerpo><sufijo>`) y las
 *        COMPILA el nativo con esos mismos nombres: los dos lados leen de aqui.
 */
inline constexpr const char kVariantSse2Suffix[] = "$sse2";
inline constexpr const char kVariantAvx2Suffix[] = "$avx2";
inline constexpr const char kVariantAvx512Suffix[] = "$avx512";

/// @brief El cuerpo multiversion de @p fn.  @return El simbolo.
inline std::string multi_version_body_symbol(const std::string &fn) {
    return fn + kMultiVersionSuffix;
}
/// @brief La ranura de la version elegida de @p body.  @return La clave.
inline std::string chosen_version_slot(const std::string &body) {
    return body + kChosenVersionSlotSuffix;
}

/**
 * @brief La ranura global de una variable `static` local: `<fn>$static$<var>`.
 *        Lleva el nombre de la funcion para que dos funciones con un `static`
 *        del mismo nombre no compartan ranura.
 * @param fn  Funcion que la declara.
 * @param var Variable.
 * @return La clave de la ranura.
 */
inline std::string static_local_slot(const std::string &fn,
                                     const std::string &var) {
    return fn + "$static$" + var;
}
/// @brief La marca de "ya inicializada" de la ranura @p slot.  @return Clave.
inline std::string static_local_done_slot(const std::string &slot) {
    return slot + "$done";
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
 * usuario los cuerpos de un spawn remoto, de una macro, de una vista y del
 * runtime.
 *
 * El gancho de copia `__clone__` NO esta: lo escribe el programa (es un
 * metodo suyo con un nombre fijo, ver vx/method_names.h), y su simbolo empieza
 * por el nombre del struct, no por un prefijo.  La lista lo tuvo como
 * `__clone_` y no coincidia con nada.
 *
 * @param name Nombre de la funcion, ya renombrado.
 * @return true si la genero el compilador.
 */
bool is_compiler_generated(const std::string &name) noexcept;

} // namespace ir

#endif // IR_SYNTHETIC_SYMBOLS_H
