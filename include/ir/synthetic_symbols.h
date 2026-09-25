/**
 * @file synthetic_symbols.h
 * @brief Los nombres de simbolo que genera el PROPIO compilador, no el
 *        programa: como se llaman y como se reconocen.
 *
 * Son una representacion compartida por capas muy distintas -- la bajada los
 * crea, el optimizador, los analisis, el precomputo, los backends y el
 * enlazador los reconocen --, y cada una los escribia a mano.  Escrito en nueve
 * ficheros, el mismo nombre acabo con dos criterios distintos de "es este" (uno
 * miraba si el nombre EMPEZABA por el, otro si lo CONTENIA) y con un analisis
 * que solo reconocia la forma principal y no sus tandas.  Vive en la capa del
 * IR porque es la mas baja que todos esos usuarios comparten.
 */
#ifndef IR_SYNTHETIC_SYMBOLS_H
#define IR_SYNTHETIC_SYMBOLS_H

#include <cstddef>
#include <string>

namespace ir {

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
    return name.compare(0, sizeof(kModuleInit) - 1, kModuleInit) == 0;
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
 * Todos empiezan por dos guiones bajos, que un nombre de Vesta no puede llevar:
 * es lo que hace segura la marca por prefijo.
 *
 * @param name Nombre de la funcion, ya renombrado.
 * @return true si la genero el compilador.
 */
bool is_compiler_generated(const std::string &name) noexcept;

} // namespace ir

#endif // IR_SYNTHETIC_SYMBOLS_H
