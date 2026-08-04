/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file backend_meta.h
 * @brief Metadatos del codigo generado: que es, con que se corresponde y donde
 *        quedo.
 *
 * Capa mas baja del subsistema.
 *
 * **No sabe que es una clase.**  Ni un trait, ni un espacio de nombres, ni una
 * plantilla.  Solo conoce cuerpos de codigo y de que instruccion del intermedio
 * proviene cada tramo.  Es deliberado: es lo que permite que otro lenguaje
 * compile a este backend generando unicamente su capa de fuente y reutilice
 * esta tal cual.  El dia que aqui aparezca la palabra "herencia", el backend
 * habra dejado de ser de todos.
 *
 * Tres cosas distintas, tres nodos distintos:
 *
 *  - @ref CodeNode -- QUE es ese codigo: de que intermedio sale, quien lo
 *    genero, con que optimizacion.  Nada mas.
 *  - @ref CodeDebug -- COMO se corresponde con el intermedio.  Va aparte porque
 *    el codigo puede existir sin el, y porque regenerar la correspondencia no
 *    deberia obligar a regenerar el codigo.
 *  - @ref PlacementMap -- DONDE quedo al ejecutarse.  Aparte tambien: mover el
 *    codigo mil veces no invalida su descripcion, que es lo que hace falta para
 *    que el cache sirva de algo.
 *
 * Un mismo intermedio tiene varios de los dos primeros a la vez -- el
 * interpretado, el compilado al vuelo, el compilado por adelantado -- y todos
 * apuntan a las MISMAS instrucciones intermedias.  Poner un punto de parada en
 * una sentencia los alcanza todos sin duplicar nada.
 */

#ifndef VXDBG_BACKEND_META_H
#define VXDBG_BACKEND_META_H

#include "vxdbg/ids.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vxdbg {

/// Version del esquema de los nodos.  Sube al cambiar el significado de un
/// campo; va en cada nodo porque dentro de diez anos habra nodos de varias
/// versiones conviviendo en el mismo almacen.
static constexpr uint32_t BACKEND_SCHEMA_VERSION = 1;

/**
 * @brief Quien genero el codigo.
 *
 * Habla del PRODUCTOR, no del formato: manana habra WASM, SPIR-V o lo que
 * venga, y cada uno querra decir quien es.  Lo que no este en la lista se
 * declara @ref Other con su nombre al lado, en vez de forzar a nadie a mentir
 * eligiendo el valor menos malo.
 */
enum class BackendKind : uint16_t {
    Interpreter = 0, ///< se ejecuta interpretado, sin generar codigo
    Velb = 1,        ///< bytecode de la maquina
    X86_64 = 2,
    X86_32 = 3,
    AArch64 = 4,
    Wasm = 5,
    Other = 0xFFFF, ///< cualquier otro; ver @ref CodeNode::backend_name
};

/**
 * @brief Un cuerpo de codigo generado: que es.
 *
 * Deliberadamente pequeno.  Lleva DOS identidades porque son dos
 * REPRESENTACIONES del mismo programa: la del intermedio del que sale y la del
 * codigo que se genero.  Ninguna de las dos es "la fisica": donde acabo
 * colocado no esta aqui, esta en el mapa de colocacion, y esa es justamente la
 * separacion que permite mover el codigo sin invalidar su descripcion.
 */
struct CodeNode {
    uint32_t schema_version = BACKEND_SCHEMA_VERSION;

    IrFunctionId ir_function; ///< que intermedio se compilo
    BackendKind backend = BackendKind::Velb;
    std::string backend_name; ///< si @ref BackendKind::Other, cual
    std::string optimization; ///< como se compilo ("O0", "O2", "c2"...)
    uint32_t byte_size = 0;

    /// De que intermedio sale.  No es "la huella logica": es la identidad de
    /// OTRA representacion, la de arriba.
    ContentHash semantic_hash;
    /// La del codigo generado en si.  Antes se llamaba fisica, y era mentira:
    /// lo fisico -- donde acabo colocado -- vive fuera del nodo, en el mapa de
    /// colocacion.  Las dos huellas son dos representaciones del mismo
    /// programa, no una descripcion y una direccion.
    ContentHash backend_hash;

    /// De que compilacion salio.  Dos versiones del compilador producen codigo
    /// distinto del mismo intermedio, y sin esto se pareceria lo bastante como
    /// para confundirlas.
    UnitId unit;

    /// Todo lo demas de lo que depende el codigo generado y que no esta en el
    /// intermedio: la convencion de llamada, las capacidades de la maquina
    /// concreta, el perfil de uso que guio al optimizador, los ajustes del
    /// backend.  Si cambia cualquiera, el codigo puede cambiar, y entonces su
    /// identidad tiene que cambiar tambien: eso es lo que evita reutilizar del
    /// cache algo generado bajo otras condiciones.
    std::vector<ContentHash> dependencies;

    /// @return La huella del nodo, derivada de todo lo anterior.
    ContentHash compute_hash() const;
};

/**
 * @brief Que le paso a un tramo de codigo.
 *
 * Que una instruccion intermedia no tenga codigo NO es informacion que falte:
 * es informacion.  Sin poder decirlo, un punto de parada que nunca se alcanza
 * parece un fallo de la herramienta cuando en realidad el optimizador borro
 * ese codigo, y saberlo es justo lo que el programador necesita.
 */
enum class RangeKind : uint8_t {
    Generated = 0,     ///< el intermedio produjo este codigo
    OptimizedAway = 1, ///< el intermedio existio y no produjo ninguno
    Synthetic = 2,     ///< codigo sin origen en el intermedio (prologos, saltos)
};

/**
 * @brief Un tramo de codigo y las instrucciones intermedias que lo generaron.
 *
 * La relacion es de MUCHOS A MUCHOS, no de una a varias: una instruccion
 * intermedia se convierte en varias de maquina, pero tambien dos intermedias
 * -- una comparacion y un salto -- pueden fundirse en un solo tramo.  Guardar
 * una sola referencia habria obligado a elegir cual de las dos mentir.
 *
 * El rango es RELATIVO al inicio de su cuerpo, nunca absoluto: la direccion
 * depende de donde se coloque, y guardarla obligaria a reescribir los
 * metadatos cada vez que el codigo se mueve.
 */
struct CodeRange {
    uint32_t begin = 0;
    uint32_t end = 0;
    RangeKind kind = RangeKind::Generated;
    std::vector<IrInstrId> ir_instrs; ///< de cuales salio
};

/**
 * @brief La correspondencia de un cuerpo de codigo con el intermedio.
 *
 * Separado de @ref CodeNode porque el codigo puede existir sin esto -- se
 * distribuye un binario sin sus metadatos igual que se hace en cualquier otro
 * sistema -- y porque rehacer la correspondencia no deberia obligar a rehacer
 * el codigo.
 */
struct CodeDebug {
    uint32_t schema_version = BACKEND_SCHEMA_VERSION;

    CodeId code; ///< a que cuerpo describe

    /// Ordenados por @c begin, para resolver por busqueda binaria sin construir
    /// ningun indice al cargar.
    std::vector<CodeRange> ranges;

    /**
     * @brief Instrucciones intermedias que ocupan un desplazamiento.
     * @param offset Desplazamiento relativo al inicio del cuerpo.
     * @return Las instrucciones; vacio si no cae en ningun tramo.
     */
    std::vector<IrInstrId> ir_at(uint32_t offset) const;

    ContentHash compute_hash() const;
};

/* El indice inverso (de una instruccion intermedia a los tramos que genero)
 * NO vive aqui.  Es cache derivada: se reconstruye entera a partir de los
 * tramos, no aporta informacion y guardarla habria hecho que un nodo dejara de
 * ser un dato para pasar a ser una estructura con caches dentro.  Quien la
 * necesite -- resolver un punto de parada sin recorrer todo -- se la construye
 * al vuelo. */

/**
 * @brief Donde quedo un cuerpo de codigo en una ejecucion.
 */
struct CodePlacement {
    CodeId code;
    uint64_t base = 0;
    uint32_t size = 0;
    uint32_t generation = 0; ///< ver @ref SessionMap
};

/**
 * @brief De una direccion al cuerpo que la contiene.
 *
 * Es lo unico de esta capa que depende de la ejecucion en curso, y por eso vive
 * separado del resto: los metadatos describen el codigo, esto dice donde esta.
 * Es la diferencia con los formatos clasicos, que mezclan ambas cosas y obligan
 * a reescribir la descripcion cada vez que algo se mueve.
 */
class PlacementMap {
  public:
    void add(CodePlacement p);

    /**
     * @brief Cuerpo que contiene una direccion.
     * @param address Direccion de ejecucion.
     * @param out_offset Recibe el desplazamiento dentro del cuerpo.
     * @return El identificador; vacio si ninguna colocacion la cubre.
     */
    CodeId find(uint64_t address, uint32_t &out_offset) const;

    /// @return Cuantas colocaciones hay.
    size_t size() const { return placements_.size(); }

  private:
    mutable std::vector<CodePlacement> placements_;
    mutable bool sorted_ = false;
};

/**
 * @brief Las sucesivas versiones del codigo de una ejecucion.
 *
 * El compilador al vuelo recompila: la misma funcion pasa por varias versiones
 * segun se va conociendo como se usa.  Tirar la anterior al sustituirla dejaria
 * sin explicar cualquier marco de pila que siga dentro de ella -- justo los que
 * mas interesa mirar cuando algo falla.  Por eso cada version es una
 * GENERACION y todas siguen consultables.
 */
class SessionMap {
  public:
    /**
     * @brief Abre una generacion nueva.
     * @return Su numero.
     */
    uint32_t new_generation();

    /// Colocaciones de una generacion, para anadir o consultar.
    PlacementMap &generation(uint32_t gen);

    /**
     * @brief Busca una direccion en TODAS las generaciones, de la mas reciente
     *        a la mas antigua.
     * @param address Direccion.
     * @param out_offset Recibe el desplazamiento.
     * @param out_generation Recibe en que generacion se encontro.
     * @return El cuerpo; vacio si no esta en ninguna.
     */
    CodeId find(uint64_t address, uint32_t &out_offset,
                uint32_t &out_generation) const;

    /// @return Cuantas generaciones hay.
    size_t generation_count() const { return generations_.size(); }

  private:
    std::vector<PlacementMap> generations_;
};

// -------------------------------------------------------------------------
//  Donde vive una variable
//
//  Esto es del BACKEND y no del lenguaje: donde acaba una variable lo decide
//  el asignador de registros, asi que la misma variable tiene una ubicacion
//  distinta por cada forma de compilarla mientras el nodo que la describe -- su
//  nombre, su tipo, su ambito -- se comparte.  Tenerlo en la capa semantica
//  habria obligado a duplicar la variable por cada backend.
// -------------------------------------------------------------------------
/// Donde vive un valor durante un tramo.
enum class LocationKind : uint8_t {
    Register = 0,     ///< en un registro de la maquina
    StackSlot = 1,    ///< a un desplazamiento del marco
    Global = 2,       ///< en una direccion fija
    Constant = 3,     ///< el valor es conocido, no esta en ningun sitio
    OptimizedOut = 4, ///< el optimizador lo elimino; decirlo es mejor que callarlo
};

/**
 * @brief Donde esta un valor entre dos puntos del intermedio.
 *
 * Los limites se expresan en instrucciones INTERMEDIAS, no en direcciones: asi
 * la misma lista vale para el interpretado, el compilado al vuelo y el
 * compilado por adelantado, que comparten intermedio pero no direcciones.
 */
struct LocationRange {
    IrInstrId from; ///< desde esta instruccion (incluida)
    IrInstrId to;   ///< hasta esta (excluida)
    LocationKind kind = LocationKind::OptimizedOut;
    int64_t value = 0; ///< registro, desplazamiento, direccion o constante
};

/**
 * @brief Donde vive una variable a lo largo del codigo.
 *
 * Separado de @ref VariableNode igual que la correspondencia del codigo esta
 * separada del codigo: una variable no esta siempre en el mismo sitio -- el
 * asignador la mueve, y a veces la elimina -- y eso depende del backend, no del
 * lenguaje.  La misma variable tiene un mapa distinto por cada forma de
 * compilarla, y el nodo se comparte.
 */
struct VariableMap {
    uint32_t schema_version = BACKEND_SCHEMA_VERSION;
    VariableId variable;
    std::vector<LocationRange> locations;

    /**
     * @brief Donde esta la variable en un punto del intermedio.
     * @param at Instruccion intermedia.
     * @return El tramo que la cubre, o uno con @ref LocationKind::OptimizedOut
     *         si en ese punto no esta en ningun sitio.
     */
    LocationRange at(IrInstrId at) const;

    ContentHash compute_hash() const;
};

} // namespace vxdbg

#endif // VXDBG_BACKEND_META_H
