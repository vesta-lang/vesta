/**
 * @file module_work.h
 * @brief Lo que el compilador lleva de CADA modulo mientras compila un
 *        proyecto.
 *
 * Era una estructura privada de `compiler_project.cpp`, y eso ataba a ese
 * fichero todo el codigo que la toca -- la cache de cada modulo, su frontend,
 * su bajada --, que por eso acabo dentro de una sola lambda de ~1.400 lineas.
 * Con su propia cabecera, cada una de esas piezas puede vivir en su fichero.
 */
#ifndef VX_PROJECT_MODULE_WORK_H
#define VX_PROJECT_MODULE_WORK_H

#include "analyze/fingerprint.h" // analyze::FunctionContracts
#include "ir/ssa_ir.h"
#include "util/name_pool.h"
#include "vx/ast.h"
#include "vx/diagnostic.h"
#include "vx/module/vxi_format.h"
#include "vx/type_checker.h"
#include "vxdbg/ids.h"   // vxdbg::LanguageEntityId
#include "vxdbg/roots.h" // vxdbg::SourceExtent

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vx {

/// Estructura de trabajo por modulo durante la compilacion del proyecto.
struct ProjectModuleWork {
    uint32_t module_id = 0;
    /// Ruta canonica de su fuente, INTERNADA: es su identidad, se copia mucho
    /// y vive lo que el proceso.
    util::InternedName canonical_path;
    /// Nombre del modulo, internado por lo mismo.
    util::InternedName module_name;
    /// El TEXTO del fuente.  No se interna: es contenido, no un nombre.
    std::string source;
    /// v18: el conjunto comptime de este modulo, tal como se extrajo de su AST.
    /// Se recolecta al compilarlo y se guarda en su `.vxi`, porque la proxima
    /// compilacion puede servirlo del cache y entonces no habra AST.
    std::string comptime_unit_source;
    uint64_t comptime_unit_hash = 0;
    ComptimeUnitNames comptime_unit_names;
    std::vector<std::string> comptime_unit_not_collected;
    std::unique_ptr<ast::ModuleNode> ast;
    std::unique_ptr<TypeChecker> tc;
    ir::IrModule ir;
    VxiModule vxi;
    bool ok = false;
    /// Los pares (simbolo, entidad) del grafo de depuracion de ESTE modulo.  Se
    /// juntan al final: el ejecutable contiene todos los modulos, asi que su
    /// mapa tiene que cubrirlos a todos.
    vxdbg::SymbolLinks vxdbg_symbols;
    ///  M.L20-full: Diagnostics local del modulo.  Cuando se
    /// paraleliza el compile (VX_PARALLEL_COMPILE=1), cada thread
    /// usa este diags propio en lugar del res.diagnostics compartido,
    /// evitando race conditions.  Post-join se mergean al global.
    Diagnostics diags;

    /**
     * @name Lo que de este modulo hace falta DESPUES de compilarlo
     *
     * CUATRO CONSUMIDORES TARDIOS TENIAN EN PIE EL AST ENTERO, y ninguno de
     * los cuatro necesitaba un AST: el tree-shake pregunta si el modulo
     * declara clases (un bit), los contratos son un mapa de nombre a siete
     * banderas, y la inyeccion diferida son un bit y dos cadenas.  Medido en
     * el pico de una compilacion de 144.000 lineas: 276 MB de frontend vivos
     * MIENTRAS SE EMITE el `.vel`, o sea sostenidos por preguntas que ya
     * estaban contestadas.
     *
     * Es la regla del ASA aplicada a la memoria y no al conocimiento: el hecho
     * se produce UNA vez, donde se sabe, y lo que se guarda es el hecho -- no
     * la estructura de la que salio.  Ver @c release_compiled_module.
     */
    ///@{
    /// Si declara alguna clase.  Lo mira el tree-shake: un dep con clases no
    /// se puede eliminar aunque sus simbolos importados no se usen.
    bool has_classes = false;
    /// Si le quedo codigo por inyectar, y cual.
    bool inject_pending = false;
    std::string inject_code;
    std::string inject_arg;
    /// Los contratos de huella declarados en su fuente, ya con la clave con la
    /// que el analizador vera la funcion.
    std::unordered_map<std::string, analyze::FunctionContracts> contracts;
    ///@}

    /**
     * @name El intermedio, cuando esta en disco y no en la RAM
     *
     * Un modulo ya compilado no vuelve a hablar hasta que se funden todos, asi
     * que sus cuerpos pueden bajar a disco mientras tanto.  DESALOJAR NO ES
     * BORRAR: los bytes estan escritos antes de soltar la memoria y volver a
     * traerlos reconstruye lo que habia.  Ver @c ir/module_spill.h.
     */
    ///@{
    /// Donde dejo la cache el `.vxir` de este modulo, si lo dejo.  Es el mismo
    /// fichero que sirve para recuperarlo, asi que un proyecto con cache no
    /// escribe nada extra por desalojar.
    std::string ir_cache_path;
    /// De donde se recupera.  Vacio = el intermedio esta en la RAM.
    std::string ir_spill_path;
    /// Cuantas funciones bajaron.  Si vuelven otras tantas, se grita.
    size_t ir_spilled_fns = 0;
    /// Cuanta RAM ocupaban, para descontarla del techo al soltarlas.
    size_t ir_footprint = 0;
    ///@}
};

} // namespace vx

#endif // VX_PROJECT_MODULE_WORK_H
