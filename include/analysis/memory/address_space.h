/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/memory/address_space.h
 * @brief DE QUE MEMORIA es una direccion -- del anfitrion o de la maquina --,
 *        incluso cuando ha pasado por memoria.
 *
 * El intermedio ya lleva la respuesta por valor (@c ir::IrValue::is_host_ptr),
 * y el bajado la pone donde puede: del tipo, del origen, y propagandola por la
 * aritmetica de punteros.  Lo que NO sobrevive es el viaje por memoria: guardar
 * una direccion y leerla de vuelta la deja sin marca.  Hubo un intento
 * -- @c pointee_is_host_ptr -- y su propio comentario dice hasta donde llega:
 * UN nivel de indireccion, y solo para locales.  Un campo de un struct no
 * entra.
 *
 * LO QUE CUESTA QUE FALTE, y por que no es un detalle de precision: un valor
 * sin marca que se llama acaba en @c CALLIND -- una llamada indirecta DE LA
 * MAQUINA, que interpreta la direccion como codigo suyo --.  Llamar asi a una
 * direccion del proceso **no da un error: devuelve cero**.  El mismo programa
 * funciona o no segun si la direccion pasó por un campo.
 *
 * QUE HACE ESTE ANALISIS.  Propaga el espacio a traves de `store`/`load`, que
 * es el unico eslabon que no existia.  No resuelve direcciones por su cuenta:
 * dos direcciones son el mismo sitio cuando resuelven al MISMO
 * @c AbstractLoc -- misma raiz, mismo desplazamiento, mismo ancho --, y eso ya
 * lo contesta points-to.  Preguntarselo, y no re-derivarlo, es el primer
 * invariante del ASA: un hecho, un productor.
 *
 * ES INTRAPROCEDURAL A PROPOSITO.  Mira una funcion y su tabla points-to, nada
 * del resto del modulo, y por eso puede cachearse por el contenido de SU
 * funcion (igual que @c PointsTo, que lo dice en su cabecera).  Cruzar llamadas
 * obligaria a @c DomainInput::CallGraph, que PROHIBE la clave por funcion: lo
 * que se dijera de `f` cambiaria al tocar `g`.  Se deja fuera, y lo que venga
 * de una llamada se responde "no se" con su motivo.
 *
 * PEREZOSO POR FUERA Y POR DENTRO.  Por fuera, contesta por la funcion que se
 * le pregunta: un analisis puede ser perezoso por fuera y voraz por dentro
 * -- resolver el modulo entero para contestar por una funcion -- y eso NO
 * cuenta.  Por dentro, points-to se pide por ORACULO y solo al llegar a un
 * `load` que de verdad hay que emparejar: la inmensa mayoria de las funciones
 * no guardan ninguna direccion en memoria y no pagan la tabla.  Ni siquiera se
 * PREGUNTA en ese caso -- pedir lo vacio ya cuesta cerrojo, busqueda y
 * reservas, y eso esta medido al lado (`asm_of` en el optimizador: 10,35 s de
 * CPU para no devolver nada).
 */
#ifndef ANALYSIS_MEMORY_ADDRESS_SPACE_H
#define ANALYSIS_MEMORY_ADDRESS_SPACE_H

#include "analysis/asa/fact.h"       // UnknownReason: por que no se supo
#include "analysis/facts/ir_facts.h" // def-use
#include "analysis/memory/points_to.h"
#include "ir/ssa_ir.h"

#include <cstdint>
#include <vector>

namespace analysis {

/**
 * @brief En que memoria vive una direccion.
 *
 * Tres valores y no dos: "no se" es una respuesta distinta de las otras dos y
 * tiene que poder decirse, porque de ella depende que quien consulte NO
 * transforme nada.  Confundir "no lo se" con "es de la maquina" es lo que
 * convierte una duda en una llamada equivocada.
 */
enum class AddressSpace : uint8_t {
    Unknown = 0, ///< no se pudo decir; el motivo va aparte.
    Host,        ///< memoria del proceso anfitrion.
    Machine,     ///< memoria de la maquina virtual.
};

/// Nombre estable para volcados.  NO es texto de usuario.
const char *address_space_name(AddressSpace s);

/**
 * @brief Lo que se sabe de UN valor.
 *
 * El motivo viaja al lado del espacio y no en otra tabla, por lo mismo que en
 * el sello de un hecho: es la respuesta a la segunda pregunta de quien lee "no
 * se", y separarlos lleva a consultarlos por caminos distintos y a que uno se
 * olvide.
 */
struct AddressSpaceEntry {
    AddressSpace space = AddressSpace::Unknown;
    /// Por que no se supo.  Solo tiene sentido con @c Unknown.
    asa::UnknownReason reason = asa::UnknownReason::NotAsked;
    /// El caso exacto, en vocabulario estable del dominio.  Vacio = ninguno.
    const char *reason_code = "";
    /**
     * @brief La operacion que dejo el hueco, si se sabe cual.
     *
     * Un motivo sin el dato no dice donde ampliar el analisis.  @c IR_NO_VALUE
     * = no cuelga de ninguna en concreto.
     */
    ir::IrValueId reason_at = ir::IR_NO_VALUE;
};

/**
 * @name Los casos exactos por los que no se supo
 *
 * Vocabulario ESTABLE del dominio, y publico a proposito: quien consulta
 * necesita distinguirlos porque se arreglan de forma distinta -- lo que llega
 * por un parametro no lo puede saber nunca quien lo recibe, solo quien lo
 * pasa; lo que viene de memoria si se deduce mirando quien escribe ahi --.
 * Un vocabulario estable que nadie puede nombrar no sirve de nada.
 *
 * NO es texto de usuario: lo que se imprime sale del catalogo multi-idioma.
 * @{
 */
/// El valor no es una direccion, o su ranura ya no existe.
extern const char *const kAddrWhyNotAnAddress;
/// El propio intermedio dijo que no se sabe.
extern const char *const kAddrWhyIrUnknown;
/// Llega por un parametro: lo sabe quien llama.
extern const char *const kAddrWhyParam;
/// Lo devolvio otra funcion, y no se cruzan llamadas.
extern const char *const kAddrWhyCall;
/// No habia a quien preguntarle por los punteros.
extern const char *const kAddrWhyNoOracle;
/// La direccion leida no se pudo resolver a un sitio concreto.
extern const char *const kAddrWhyAddrUnresolved;
/// Nadie escribio en ese sitio de forma identificable.
extern const char *const kAddrWhyNoStore;
/// Dos escrituras al mismo sitio guardaron memorias distintas.
extern const char *const kAddrWhyStoresDiffer;
/// Solo una escritura cayo ahi, y de SU valor no se sabe la memoria.
extern const char *const kAddrWhyStoredValueUnknown;
/// Una escritura sin resolver pudo dejar otra cosa en el hueco.
extern const char *const kAddrWhyDirtyStore;
/// Una operacion mezclo direcciones de memorias distintas.
extern const char *const kAddrWhyMixedOperands;
/// Las ramas de un PHI no coinciden.
extern const char *const kAddrWhyPhiMixed;
/// Forma que este analisis no modela.
extern const char *const kAddrWhyShape;
/// Se acabaron las vueltas antes de converger.
extern const char *const kAddrWhyBudget;
/** @} */

/// El espacio de cada valor de una funcion, indexado por value-id.
struct AddressSpaces {
    std::vector<AddressSpaceEntry> by_value;

    const AddressSpaceEntry &at(ir::IrValueId v) const {
        static const AddressSpaceEntry kNada{};
        return v < by_value.size() ? by_value[v] : kNada;
    }
    /// Atajo del caso que mas se pregunta: se puede AFIRMAR que es del
    /// anfitrion.  Un "no se" contesta false, que es lo conservador.
    bool is_host(ir::IrValueId v) const {
        return at(v).space == AddressSpace::Host;
    }
};

/**
 * @brief Resuelve el espacio de cada valor de @p fn.
 *
 * Siembra con lo que el intermedio ya dice (@c is_host_ptr) y lo propaga por
 * las derivaciones y por `store`/`load`, emparejando las direcciones con
 * points-to -- dos que resuelven al mismo sitio son el mismo sitio --.
 *
 * Donde no se pueda afirmar, la entrada queda @c Unknown CON SU MOTIVO: un
 * analisis que calla al renunciar parece que funciona.
 *
 * @param fn    Funcion a resolver.
 * @param facts Def-use, para llegar de un valor a lo que lo define.
 * @param pt    A QUIEN preguntar por el points-to de @p fn.  Oraculo y no la
 *              tabla: solo se le pregunta si la funcion llega a guardar una
 *              direccion en memoria y a leerla de vuelta, que es la minoria.
 *              Sin oraculo el analisis sigue valiendo -- siembra y propaga por
 *              las derivaciones -- y lo que venga de memoria queda @c Unknown
 *              diciendo que falto a quien preguntar.
 * @return El espacio de cada valor.
 */
AddressSpaces compute_address_spaces(const ir::IrFunction &fn,
                                     const IrFacts &facts,
                                     PointsToOracle pt = {});

// ===========================================================================
//  Guardarla y recuperarla entre compilaciones
// ===========================================================================
//
// Aqui, junto al analisis, porque quien sabe QUE campos hay que escribir es el
// propio.  Lo COMUN vive en `analysis/manager/analysis_store.h`.

/// Nombre estable con el que este analisis se identifica en el almacen.
extern const char *const kAddressSpaceAnalysisName;

/// Version del FORMATO.  Propia: cambiarla no tira lo guardado de los demas.
constexpr uint32_t kAddressSpaceFormat = 1;

/// Empaqueta @p as en bytes.
std::vector<uint8_t> serialize_address_spaces(const AddressSpaces &as);

/**
 * @brief Reconstruye en @p out lo empaquetado por @ref
 * serialize_address_spaces.
 *
 * @param data     Bytes leidos del almacen.
 * @param n        Cuantos.
 * @param n_values Cuantos valores tiene la funcion HOY.  Comprobacion de
 *                 coherencia INDEPENDIENTE de la clave: es la que convierte un
 *                 choque de claves en un descarte en vez de en una tabla que
 *                 habla de otra funcion.
 * @return @c false si no cuadra; @p out queda intacto y quien pregunta computa.
 */
bool deserialize_address_spaces(const uint8_t *data, size_t n, size_t n_values,
                                AddressSpaces &out);

/// Marcador para el AnalysisManager.  Puede depender de @c PointsToAnalysis
/// -- solo si la funcion guarda direcciones en memoria --; el resultado se
/// invalida cuando la funcion muta.
struct AddressSpaceAnalysis {
    using Result = AddressSpaces;
    static char ID;
    /// Como se llama al medir.  No es decorativo: @c FactBase::memoized lo
    /// exige, asi que un analisis que se olvide de ponerlo NO COMPILA en vez
    /// de aparecer sin medir.  En INGLES, como el resto de lo que se imprime.
    static constexpr const char *kName = "address_space";
};

} // namespace analysis

#endif // ANALYSIS_MEMORY_ADDRESS_SPACE_H
