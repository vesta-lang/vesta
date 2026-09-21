/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/escape/fn_addr_escape.h
 * @brief Que direcciones de funcion NUESTRA tienen que ser REALES.
 *
 * Una funcion de Vesta tiene dos direcciones posibles en la maquina virtual:
 * la del BYTECODE (gratis, pero codigo nativo no puede saltar ahi) y la
 * NATIVA (la funcion se compila al vuelo, asi que cuesta, pero vale para
 * cruzar).  Las dos hacen falta, y por una razon que no es de rendimiento: la
 * frontera es de DOS direcciones.  No solo llamamos fuera -- tambien nos
 * llaman, y un `qsort` que recibe nuestro comparador tiene que poder saltar a
 * el.
 *
 * QUE DECIDE CUAL.  El USO, no el tipo.  Atarlo al tipo -- que `cfn` sea
 * siempre nativa -- obligaria en la maquina a usar `fn` para todo lo interno,
 * o sea `cfn` = FFI y `fn` = maquina: el mismo diseno atado al backend que se
 * descarto, llegando por detras.  `cfn` y `fn` son FORMA (puntero estilo C
 * contra lambda con entorno) y siguen significando lo mismo en los tres modos;
 * lo que cambia es el bajado, que es donde si puede cambiar.
 *
 * COMO SE FALLA.  Por defecto, la nativa: es correcta SIEMPRE.  Esto contesta
 * quien se puede abaratar a bytecode, y quien no se pueda demostrar se queda
 * en la cara cara.  Al reves -- suponer bytecode y equivocarse -- es lo que
 * hoy **devuelve cero en silencio**, porque la llamada indirecta de la maquina
 * interpreta la direccion como codigo suyo.
 *
 * LO QUE SUSTITUYE.  Hoy la eleccion es SINTACTICA: `(cfn)nombre` supone que
 * escapa y `&nombre` supone que no.  Acierta cuando el cast esta a la vista --
 * por eso los ejemplos de fibras, hilos y canales funcionan -- y falla cuando
 * el valor se lava por un entero, que es el caso que deja una syscall sin
 * ejecutar dandola por buena.
 */
#ifndef ANALYSIS_ESCAPE_FN_ADDR_ESCAPE_H
#define ANALYSIS_ESCAPE_FN_ADDR_ESCAPE_H

#include "analysis/escape/escape.h"
#include "analysis/facts/ir_facts.h"
#include "analysis/memory/points_to.h"
#include "ir/ssa_ir.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace analysis {

/**
 * @enum FnAddrCrossing
 * @brief POR DONDE cruza a codigo real una direccion de funcion.
 *
 * El motivo se guarda, y no solo el si/no, porque es lo que permite explicar
 * un veredicto -- y porque un analisis que no dice por que renuncia parece que
 * funciona.
 */
enum class FnAddrCrossing : uint8_t {
    None = 0,      ///< no cruza: se puede abaratar a bytecode.
    NativeCall,    ///< va de argumento a una llamada nativa.
    InlineAsm,     ///< la toca un bloque de ensamblador.
    EscapingStore, ///< se guarda donde codigo nativo puede leerla.
    Returned,      ///< se devuelve: donde acaba no se ve desde aqui.
    ViaCallee,     ///< se pasa a una funcion que la deja cruzar.
};

/// Nombre estable del motivo, para volcados.  NO es texto de usuario.
const char *fn_addr_crossing_name(FnAddrCrossing c);

/// UNA direccion de funcion que tiene que ser real, y por donde cruza.
struct FnAddrSite {
    ir::IrValueId value = ir::IR_NO_VALUE;          ///< la direccion.
    FnAddrCrossing crossing = FnAddrCrossing::None; ///< por donde cruza.
};

/**
 * @brief Las direcciones de funcion de UNA funcion que tienen que ser reales.
 *
 * En un vector ORDENADO por value-id y no en un mapa: son pocas por funcion,
 * se recorren enteras al volcarlas y se consultan por busqueda binaria, las
 * dos cosas sobre memoria contigua.  Y con NOMBRE, que es lo que permite leer
 * un sitio sin ir a buscar quien lo rellena.
 */
struct FnAddrEscape {
    /// Ordenado por @c FnAddrSite::value.  Lo que no esta aqui NO cruza.
    std::vector<FnAddrSite> sites;
    /// Indices de parametro por los que esta funcion deja cruzar una
    /// direccion que le pasen, ordenados.  Es lo que cierra el punto fijo:
    /// quien llama necesita saberlo para resolver lo suyo.
    std::vector<int32_t> crossing_params;
    /**
     * @brief Cuantas direcciones de funcion se vieron, crucen o no.
     *
     * Lo lleva el resultado porque el analisis YA las conto al recorrer: que
     * quien informe las vuelva a contar significa recorrer otra vez el modulo
     * entero -- y, peor, volver a decidir cuales son de funcion, que es
     * internar un nombre por instruccion.
     */
    size_t fn_addr_count = 0;

    /// @return por donde cruza @p v, o @c None si no cruza.
    FnAddrCrossing why(ir::IrValueId v) const {
        const auto it = std::lower_bound(
            sites.begin(), sites.end(), v,
            [](const FnAddrSite &s, ir::IrValueId x) { return s.value < x; });
        if (it == sites.end() || it->value != v) return FnAddrCrossing::None;
        return it->crossing;
    }
    /// @return true si @p v tiene que ser la direccion NATIVA.
    bool needs_native(ir::IrValueId v) const {
        return why(v) != FnAddrCrossing::None;
    }
    /// @return true si por el parametro @p idx cruza.
    bool param_crosses(int32_t idx) const {
        return std::binary_search(crossing_params.begin(),
                                  crossing_params.end(), idx);
    }
};

/**
 * @brief Oraculo interprocedural: ¿@p callee deja cruzar su parametro @p idx?
 *
 * Lo provee el punto fijo del modulo.  Para un callee DESCONOCIDO contesta
 * true, que es la respuesta correcta sin su cuerpo y no una renuncia: lo que
 * no se ve, cruza.
 */
struct CalleeCrossesParam {
    /// Contesta por (@p callee, @p idx).  Nulo = no hay a quien preguntar, y
    /// entonces se supone que cruza: lo que no se ve, cruza.
    bool (*ask)(void *ctx, const std::string &callee, int32_t idx) = nullptr;
    void *ctx = nullptr;
    bool valid() const { return ask != nullptr; }
};

/**
 * @brief Los nombres que son FUNCIONES del modulo.
 *
 * Hace falta porque `LABEL_ADDR` no es "la direccion de una funcion": es la
 * direccion de una ETIQUETA, y la mayoria son de datos -- el nombre de una
 * clase para `findclass`, un literal de cadena --.  Tratarlas todas como
 * funciones era contar como cruce cada cadena del programa: medido, 679 de 689.
 *
 * Y es la pregunta que de verdad decide, dicha en su forma: si el nombre al
 * que apunta esta DENTRO de nuestros modulos.  Vesta es un mundo semi-cerrado,
 * asi que esa lista existe y es exacta.
 */
struct ModuleFunctionNames {
    /// Nombres INTERNADOS y ordenados por puntero: comparar es comparar
    /// punteros, y buscar es una biseccion.
    std::vector<const std::string *> sorted;
    /// @return true si @p name es una funcion del modulo.
    bool contains(const std::string *name) const {
        return std::binary_search(sorted.begin(), sorted.end(), name);
    }
};

/// Los nombres de funcion de @p mod, listos para preguntar.
ModuleFunctionNames collect_module_function_names(const ir::IrModule &mod);

/**
 * @brief Los sitios de memoria que el programa ENTREGA a codigo nativo.
 *
 * Existe porque la pregunta no es "¿lo lee alguien de fuera?" sino "¿puede
 * LLAMARLA desde ahi?", que es mas estrecho.  El runtime de la maquina lee
 * constantemente memoria nuestra -- los parametros de `defmethod` llevan la
 * direccion de cada metodo -- y eso NO exige que sea real: la guarda y luego
 * salta a ella como bytecode.  Codigo nativo solo puede llamar lo que esta en
 * memoria que le hayamos entregado, y entregarla es pasarle su direccion a una
 * llamada nativa.
 *
 * Sin esta distincion la regla se traga los destructores y las tablas de
 * metodos: medido sobre el corpus, 679 de 689 cruces eran eso, y los de verdad
 * eran diez.
 */
struct NativeReach {
    /// Ids de global cuya direccion llega a una llamada nativa, ORDENADOS.
    std::vector<uint32_t> globals;
    /**
     * @brief Una nativa recibe un puntero que no se resuelve.
     *
     * Entonces no se puede afirmar de NINGuN global que no lo alcance, porque
     * ese puntero podria ser cualquiera.  No poder demostrarlo no es
     * demostrar lo contrario, asi que se contesta que si a todos.
     */
    bool unknown_handed = false;

    /// @return true si codigo nativo puede alcanzar el global @p id.
    bool reaches_global(uint32_t id) const {
        if (unknown_handed) return true;
        return std::binary_search(globals.begin(), globals.end(), id);
    }
};

/// Que memoria entrega @p mod a codigo nativo.  Se calcula UNA vez por modulo:
/// un global es el mismo en todas las funciones (@c AbstractLoc lo identifica
/// por su id de simbolo), asi que la respuesta tambien.
struct FnAddrInputs;
NativeReach compute_native_reach(const ir::IrModule &mod,
                                 const FnAddrInputs &in);

/**
 * @brief Resuelve, para @p fn, que direcciones de funcion tienen que ser
 *        reales.
 *
 * CONSUME la base de hechos, no la construye.
 *
 * @param fn      Funcion a mirar.
 * @param facts   Def-use.
 * @param esc     Escape de @p fn: dice que ranuras locales son observables
 *                desde fuera.  Guardar una direccion en una que NO escapa no
 *                la saca de Vesta; en una que si, puede leerla codigo nativo.
 * @param pt      Points-to, para saber a QUE ranura va un almacen.  Sin el no
 *                se puede distinguir guardar en una tabla local de guardar en
 *                un sitio que alguien de fuera lee, y habria que suponer lo
 *                segundo siempre.
 * @param reach   Que memoria entrega el programa a codigo nativo.  Sin esto
 *                habria que suponer que cualquier global es alcanzable, y
 *                entonces guardar la direccion de un destructor en la ficha de
 *                su clase contaria como cruzar -- que era 679 de 689.
 * @param callee  A quien preguntar por los parametros de un callee.
 * @return Lo que cruza, y por donde.
 */
FnAddrEscape compute_fn_addr_escape(const ir::IrFunction &fn,
                                    const IrFacts &facts, const EscapeInfo &esc,
                                    const PointsTo &pt,
                                    const NativeReach &reach,
                                    const ModuleFunctionNames &fns,
                                    const CalleeCrossesParam &callee);

/**
 * @brief Lo mismo para TODO el modulo, cerrando @c crossing_params por punto
 *        fijo del grafo de llamadas.
 *
 * Se hace sobre el modulo FUSIONADO a proposito: Vesta es un mundo
 * semi-cerrado -- todos los modulos son nuestros y el unico sitio donde se
 * pierde conocimiento es la frontera FFI --, asi que ahi conviven quien toma
 * la direccion y quien la hace cruzar.
 */
struct FnAddrEscapeOfFunction {
    /// Nombre INTERNADO de la funcion: la entrada no copia cadenas y
    /// compararlos es comparar punteros.
    const std::string *function = nullptr;
    FnAddrEscape escape; ///< lo que cruza en ella.
};

/// El resultado por funcion, ordenado por el puntero del nombre internado.
struct FnAddrEscapeModule {
    std::vector<FnAddrEscapeOfFunction> by_function;
    /// @return lo de @p name, o @c nullptr si no esta.
    const FnAddrEscape *find(const std::string *name) const;
};

/**
 * @brief De donde salen las tres entradas que el analisis consume.
 *
 * Punteros a funcion con contexto, no `std::function` ni lambdas: quien
 * contesta tiene NOMBRE y sale en el perfil, y aqui contestar no es gratis --
 * detras de cada una hay un analisis del modulo.
 */
struct FnAddrInputs {
    const IrFacts &(*facts)(void *ctx, const ir::IrFunction &fn) = nullptr;
    const EscapeInfo &(*escape)(void *ctx, const ir::IrFunction &fn) = nullptr;
    const PointsTo &(*points_to)(void *ctx, const ir::IrFunction &fn) = nullptr;
    void *ctx = nullptr;
    bool valid() const {
        return facts != nullptr && escape != nullptr && points_to != nullptr;
    }
};

FnAddrEscapeModule compute_fn_addr_escape_module(const ir::IrModule &mod,
                                                 FnAddrInputs in);

} // namespace analysis

#endif // ANALYSIS_ESCAPE_FN_ADDR_ESCAPE_H
