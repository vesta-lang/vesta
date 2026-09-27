/**
 * @file root_weaving.h
 * @brief Lo que el modulo RAIZ decide para todo el programa, recogido antes de
 *        compilar ningun modulo.
 *
 * Los sustitutos de ayudantes (`@HelperOverride`) y los ganchos (`@Hook`,
 * `@NoInstrument`) del raiz se tejen en los demas modulos, que se bajan ANTES
 * que el: por eso se recogen de los arboles sin aplanar, antes de compilar
 * nada.  Vivia como locales y lambdas de `compile_vx_project`; aqui tiene
 * dueno para que el camino de fichero suelto -- un proyecto de un modulo --
 * lo recoja igual.
 */
#ifndef VX_PROJECT_ROOT_WEAVING_H
#define VX_PROJECT_ROOT_WEAVING_H

#include "vx/unit/unit_env.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vx {

struct CompileResult;
struct ProjectModuleWork;

/**
 * @struct RootWeaving
 * @brief Lo que el raiz teje en todo el programa, con dueno.
 *
 * `RootWeavingEnv` solo apunta; esto es lo apuntado, y tiene que vivir
 * mientras se compilen los modulos.
 */
struct RootWeaving {
    /// `@HelperOverride` ya resueltos por precedencia: destino -> simbolo.
    std::unordered_map<std::string, std::string> helper_overrides;
    /// Los `@Hook` del raiz, con el nombre aplanado por el que se les llama.
    std::vector<RootHook> hooks;
    /// Las funciones del raiz marcadas `@NoInstrument`, por su nombre aplanado.
    std::vector<std::string> no_instrument;
    /// Un contador por gancho, que comparten todos los modulos.
    HookCounters hook_counters;
    /// Huella de lo que el tejido cambia en los demas modulos; 0 sin ganchos.
    uint64_t hooks_source_fp = 0;

    /**
     * @brief La vista que lee la compilacion de cada modulo.
     * @return Punteros a lo de aqui: vale mientras esto viva.
     */
    RootWeavingEnv view() const;
};

/**
 * @brief Recoge de @p work lo que el raiz (el ultimo) teje en todos.
 *
 * Los `@HelperOverride` salen de TODOS los modulos, porque una biblioteca
 * importada puede declararlos; si el raiz y un import sustituyen el mismo
 * ayudante gana el raiz, y dos del mismo nivel son un error (VX4016).  Los
 * ganchos salen solo del raiz: una biblioteca que usa un perfilador no
 * instrumenta a quien la usa.
 *
 * Tiene que llamarse con los arboles SIN aplanar ni renombrar: los nombres
 * se calculan aqui como quedaran despues.
 *
 * @param work Los modulos, en orden topologico (el raiz el ultimo).
 * @param res  Recibe el error si dos sustitutos chocan.
 * @param out  Lo recogido.
 * @return false si hubo un choque: el error ya esta dicho.
 */
bool collect_root_weaving(std::vector<ProjectModuleWork> &work,
                          CompileResult &res, RootWeaving &out);

} // namespace vx

#endif // VX_PROJECT_ROOT_WEAVING_H
