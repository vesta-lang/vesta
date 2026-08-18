/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file compilation_unit.h
 * @brief Quien genero cada cosa, con que, y donde se esta ejecutando.
 *
 * Cuando algo falla no basta con saber en que codigo fue: hace falta saber de
 * QUE compilacion salio.  Dos compilaciones del mismo fuente con distinto nivel
 * de optimizacion, o con distinta version del compilador, producen nodos
 * distintos que conviven en el mismo almacen -- que es justo lo que se busca al
 * guardar por contenido.  Sin esto se parecerian lo bastante como para
 * confundirlas.
 *
 * Hay aqui dos cosas de naturaleza opuesta, y conviene no mezclarlas:
 *
 *  - @ref CompilationUnit es un NODO: describe algo que ya ocurrio, no cambia
 *    nunca mas y se identifica por su contenido.  Va al almacen.
 *  - @ref ExecutionContext y @ref DebugSession NO son nodos: describen una
 *    ejecucion en curso, cambian mientras corre y no tienen identidad estable.
 *    No van al almacen, y meterlos habria roto la regla de que lo guardado es
 *    inmutable.
 */

#ifndef VXDBG_COMPILATION_UNIT_H
#define VXDBG_COMPILATION_UNIT_H

#include "vxdbg/backend_meta.h"
#include "vxdbg/ids.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vxdbg {

/// Version del esquema de esta capa.
static constexpr uint32_t UNIT_SCHEMA_VERSION = 1;

/**
 * @brief Para que maquina se compilo.
 *
 * Descompuesto y no como una sola cadena: "x86-64-windows" no dice si se uso
 * AVX-512 ni que convencion de llamada, y esas cosas cambian el codigo
 * generado.  Con los campos sueltos, el cache puede distinguir dos
 * compilaciones que solo se diferencian en las capacidades que aprovecharon.
 */
struct TargetSpec {
    std::string cpu;                   ///< "x86_64", "aarch64"
    std::string os;                    ///< "windows", "linux", "" si no hay
    std::string abi;                   ///< "msvc", "gnu", "sysv"
    std::string vendor;                ///< "pc", "apple", ""
    std::vector<std::string> features; ///< "avx512", "fma", "neon"
};

/**
 * @brief Quien tradujo el codigo.
 *
 * Dos versiones del mismo compilador generan codigo distinto del mismo fuente.
 * Sin saber cual fue, un nodo del cache puede reutilizarse cuando ya no
 * corresponde.
 */
struct CompilerIdentity {
    std::string vendor;             ///< quien lo hace
    std::string version;            ///< que version
    std::string git_revision;       ///< y exactamente que revision
    std::vector<std::string> flags; ///< con que ajustes se construyo
};

/// Que papel juega una dependencia en una compilacion.
struct Dependency {
    ContentHash hash;
    /// Que es ("stdlib", "plugin", "macro", "fichero generado", "perfil").
    /// Texto libre a proposito: el conjunto de cosas de las que puede depender
    /// una compilacion crece con el tiempo, y cerrarlo obligaria a cambiar el
    /// formato cada vez.
    std::string role;
};

/**
 * @brief Una compilacion concreta y todo lo que produjo.
 *
 * Se identifica por contenido: dos compilaciones con los mismos ajustes sobre
 * las mismas entradas son la misma y no se guardan dos veces.
 */
struct CompilationUnit {
    uint32_t schema_version = UNIT_SCHEMA_VERSION;

    std::string language; ///< lenguaje del fuente ("vesta", "c")
    CompilerIdentity compiler;
    TargetSpec target;
    std::string optimization; ///< con que ajustes ("O2", "O0 + debug")

    /// De que depende esta compilacion, entrada por entrada.  Es lo que permite
    /// responder si un nodo del cache sigue siendo valido: cambia cualquiera de
    /// estas y deja de serlo.
    std::vector<Dependency> inputs;

    /// Que produjo.  No hace falta para buscar -- todo se puede recorrer desde
    /// los nodos -- pero si para responder a "que salio exactamente de aqui",
    /// que es la pregunta al comparar dos compilaciones.
    std::vector<ModuleId> modules;
    std::vector<LanguageEntityId> root_entities;
    std::vector<IrFunctionId> ir_functions;
    std::vector<CodeId> code;
    /// Las bajadas que produjo.  Se enumeran igual que lo demas: una capa que
    /// no aparece aqui no se podria atribuir a esta compilacion, y con varias
    /// conviviendo en el almacen eso deja de ser una comodidad.
    std::vector<ContentHash> lowering_maps;

    /// Cuando se hizo, en segundos desde la epoca.  Informativo: NO entra en la
    /// huella, porque compilar dos veces lo mismo debe dar el mismo
    /// identificador y con el reloj dentro nunca lo daria.
    uint64_t timestamp = 0;

    /* Sin huella a proposito.  No es un nodo del grafo: describe QUIEN compilo
     * y con que, no un hecho del programa.  Dos personas que compilan
     * exactamente lo mismo producen los mismos nodos -- esa es la gracia del
     * cache por contenido -- y no necesitan otro identificador, necesitan otro
     * contexto.  Hashearla habria metido en el grafo algo que no es del
     * programa sino de quien lo construyo. */
};

/**
 * @brief Donde se ejecuta un tramo de pila.
 *
 * Un mismo programa mezcla varios: parte se resolvio al compilar, parte corre
 * interpretada y parte compilada al vuelo.  Marcar cada tramo con el suyo es lo
 * que permite contar en UNA sola traza que instanciar una plantilla llamo a
 * algo que se evaluo al compilar y que de ahi salio el fallo, sin necesitar una
 * herramienta distinta para cada mundo.
 */
enum class ExecutionKind : uint8_t {
    Comptime = 0,    ///< lo ejecuta el compilador
    Interpreter = 1, ///< bytecode interpretado
    Jit = 2,         ///< compilado durante la ejecucion
    Aot = 3,         ///< compilado por adelantado
    Native = 4,      ///< codigo ajeno (una biblioteca del sistema)
};

/**
 * @brief El contexto en que corre un tramo de ejecucion.
 *
 * NO es un nodo: describe algo que esta pasando, no algo que ya paso.  No tiene
 * huella y no se guarda en el almacen.
 */
struct ExecutionContext {
    ExecutionKind kind = ExecutionKind::Interpreter;
    UnitId unit; ///< de que compilacion salio el codigo
    /// Version del codigo, cuando se recompila durante la ejecucion.  Se llama
    /// revision y no generacion para no confundirla con las del recolector.
    uint32_t code_revision = 0;
    /// Para quien lo lea ("evaluando constante", "instanciando plantilla").  La
    /// pone quien crea el contexto; el formato no la interpreta, que es lo que
    /// evita meter aqui conceptos de ningun lenguaje.
    std::string description;
};

/**
 * @brief Una ejecucion completa: que se cargo y donde quedo.
 *
 * Una ejecucion carga varias compilaciones a la vez -- el programa, sus
 * complementos, las bibliotecas, lo que el compilador al vuelo genere -- y cada
 * una tiene su unidad.  Sin agruparlas, una direccion no se podria atribuir a
 * la compilacion correcta cuando hay varias en memoria.
 *
 * Tampoco es un nodo: vive lo que dure la ejecucion.
 */
struct DebugSession {
    /// Identifica la ejecucion.  No se deriva del contenido -- no hay
    /// contenido que valga, es algo que esta pasando -- sino que se asigna al
    /// arrancar.
    uint64_t id = 0;
    std::vector<UnitId> units; ///< compilaciones cargadas
    SessionMap placements;     ///< donde quedo el codigo de cada una
};

} // namespace vxdbg

#endif // VXDBG_COMPILATION_UNIT_H
