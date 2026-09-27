/**
 * @file macro_report.h
 * @brief Lo que el compilador informa de cada `@Macro`: que genero y, si no
 *        se bajo a la maquina de compilacion, por que.
 *
 * Los dos caminos de compilacion -- fichero suelto y proyecto -- lo recogen
 * por aqui.  Solo lo hacia el de fichero suelto, asi que el servidor de
 * lenguaje no veia nada de un programa con `import`.
 *
 * El motivo de no bajar se guarda como DATO -- codigo del catalogo y el
 * nombre al que se refiere -- y no como frase: la frase se escribe al
 * mostrarla, en el idioma de quien la lee.  Antes era texto en espanol
 * escrito a mano que llegaba tal cual al editor.
 */
#ifndef VX_COMPTIME_MACRO_REPORT_H
#define VX_COMPTIME_MACRO_REPORT_H

#include "util/name_pool.h"
#include "util/named_alloc.h"

#include <cstdint>
#include <string>

namespace vx {

class TypeChecker;

namespace scratch {
struct MacroSkips;        ///< Los `@Macro` que no se bajaron.
struct MacroArgs;         ///< Los argumentos de una llamada a `@Macro`.
struct MacroExpectations; ///< Las llamadas a `@Macro` ya resueltas.
} // namespace scratch

/**
 * @struct MacroSkipReason
 * @brief El motivo por el que un cuerpo no se puede bajar, o ninguno.
 */
struct MacroSkipReason {
    /// Codigo `VXT...` del catalogo; nulo = el cuerpo SI se puede bajar.
    const char *code = nullptr;
    /// El nombre al que se refiere el motivo (una global, un builtin...), o
    /// vacio si el motivo no nombra nada.
    util::InternedName subject;
    /// La funcion comptime auxiliar por la que se llego al motivo, o vacio si
    /// esta en el propio cuerpo del `@Macro`.
    util::InternedName via;

    /// @brief @c true si no hay motivo: el cuerpo se puede bajar.
    /// @return Si falta el codigo.
    bool empty() const noexcept { return code == nullptr; }
};

/**
 * @struct MacroSkip
 * @brief Un `@Macro` que se quedo en el evaluador de arbol, y por que.
 */
struct MacroSkip {
    util::InternedName macro; ///< el `@Macro`, tal como se declaro
    MacroSkipReason why;      ///< el motivo
};

/// Los `@Macro` que se quedaron sin bajar, en el orden en que se vieron.
using MacroSkips = util::NamedVector<MacroSkip, scratch::MacroSkips>;

/**
 * @struct MacroExpectation
 * @brief Una llamada a un `@Macro` que el evaluador de arbol ya resolvio: con
 *        que argumentos y que codigo genero.
 *
 * Solo entran las llamadas cuyos argumentos caben en un entero de 64 bits.
 */
struct MacroExpectation {
    util::InternedName macro_name; ///< el `@Macro` llamado
    /// Los argumentos de la llamada, cada uno como entero de 64 bits.
    util::NamedVector<uint64_t, scratch::MacroArgs> args;
    std::string expected_str; ///< el codigo que genero: es TEXTO, no un nombre
    std::string src_loc;      ///< donde esta la llamada, ya escrito
};

/// Las llamadas a `@Macro` resueltas en una compilacion.
using MacroExpectations =
    util::NamedVector<MacroExpectation, scratch::MacroExpectations>;

/**
 * @brief Anade a @p out las llamadas a `@Macro` que @p tc resolvio.
 * @param tc  El comprobador que las evaluo.
 * @param out Donde se anaden.
 */
void collect_macro_expectations(const TypeChecker &tc, MacroExpectations &out);

/**
 * @brief La frase del motivo, del catalogo y en el idioma activo.
 * @param why El motivo; no puede estar vacio.
 * @return El texto.
 */
std::string macro_skip_text(const MacroSkipReason &why);

} // namespace vx

#endif // VX_COMPTIME_MACRO_REPORT_H
