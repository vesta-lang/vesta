/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/fact_file.h
 * @brief Los hechos, en disco: lo que solo se sabe al BAJAR el codigo sobrevive
 *        al primer acierto de cache.
 *
 * EL PROBLEMA QUE RESUELVE.  Hay conocimiento que no se puede recalcular mirando
 * el modulo ya compilado porque nacio ANTES, mientras se bajaba: que exige del
 * procesador un bloque de asm, por que no se pudo demostrar algo, que ligaduras
 * habia.  Sin esto, la primera vez el compilador avisa y la segunda -- con la
 * cache caliente -- se calla, y el aviso depende de si alguien borro un
 * directorio.  Un diagnostico que va y viene solo no es un diagnostico.
 *
 * QUE SE GUARDA.  El criterio NO es "recomputable si o no", es el COSTE:
 *
 *      no recomputable          siempre (si no se guarda, se perdio)
 *      recomputable y caro      se guarda (cuesta mas rehacerlo que leerlo)
 *      recomputable y barato    no se guarda
 *
 * Y como el coste depende del TAMAÑO del modulo, no puede ser una regla fija en
 * el codigo: cada dominio dice CUANTO le costo producir lo suyo
 * (@c DomainCost) y con eso se decide.  Un modulo pequeño no engorda su cache;
 * uno grande si, que es justo donde compensa.
 *
 * EL FORMATO, y por que es asi.  Registros AUTO-DESCRIPTIVOS por dominio, cada
 * uno con su LONGITUD delante:
 *
 *      cabecera   magia + version + identidad del modulo + cuantos hechos
 *      registro   [dominio][version][huella][cuantos][longitud][cuerpo]
 *      cola       indice dominio -> posicion, para leer solo lo que se pida
 *
 * La longitud por delante es lo importante: un lector que no conoce un dominio
 * LO SALTA en vez de fallar.  Añadir un productor no invalida las caches
 * escritas antes ni obliga a subir una version global, y cada dominio versiona
 * LO SUYO -- cambiar su contenido descarta sus registros, no el fichero entero.
 * Es el mismo reparto que hacen ELF o PNG, y por la misma razon.
 *
 * Y la HUELLA VA POR REGISTRO, no solo en la cabecera: cada dominio se valida
 * por su cuenta contra lo que hoy dependeria, asi que tocar algo que solo le
 * afecta a el descarta su registro y deja en pie los demas.  Cuanto menos haya
 * que invalidar, menos hay que rehacer -- y con una unica huella global,
 * cambiar una linea obligaria a recalcularlo todo.
 *
 * DONDE VIVE.  En un fichero propio al lado del `.vxir`, con la huella del
 * modulo dentro: si no cuadra, se descarta y se recalcula.  Aparte del IR
 * porque el IR lo lee CADA compilacion y esto casi nadie, y porque ampliar lo
 * que el compilador sabe no debe versionar lo que se ejecuta.
 */
#ifndef ANALYSIS_ASA_FACT_FILE_H
#define ANALYSIS_ASA_FACT_FILE_H

#include "analysis/asa/fact_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace analysis {
namespace asa {

/// Version del CONTENEDOR (cabecera, troceado en registros, indice).  Se sube
/// solo si cambia el sobre; el contenido de cada dominio versiona aparte.
constexpr uint16_t kContainerVersion = 1;

/// Version del layout de un HECHO.  Va en cada registro: cambiarla descarta los
/// registros viejos de todos los dominios, pero no rompe el fichero.
constexpr uint16_t kFactVersion = 1;

/**
 * @brief Cuanto se guarda.  Ajustable con @c VESTA_ASA_CACHE.
 *
 * Dos reglas lo hacen sano, y no son opcionales:
 *
 *  - El nivel NO entra en la identidad de lo cacheado.  Escribir con @c Todo y
 *    compilar despues con @c Minimo no invalida nada: lo que hay en disco sigue
 *    valiendo, solo se deja de producir mas.  Los niveles son MONOTONOS (cada
 *    uno contiene al anterior), asi que "esta esto guardado?" no depende del
 *    nivel con que se escribio.  Si el nivel se mezclara con la huella, cambiar
 *    de nivel tiraria caches validas -- justo lo que se quiere evitar.
 *  - El nivel NO puede afectar a la CORRECCION.  En los cuatro, el codigo
 *    generado es identico; lo unico que cambia es cuanto hay que rehacer.  Si un
 *    nivel cambiara la salida dejaria de ser una cache, y volveriamos a que el
 *    programa dependa de si habia cache.
 */
enum class CacheLevel : uint8_t {
    Off = 0,    ///< no guardar nada.  Es la herramienta para contestar a "esto
                 ///< es la cache o es el codigo?" sin borrar ficheros a mano.
    Minimum = 1,  ///< solo lo que no se puede recalcular.
    ByCost = 2, ///< DEFECTO: lo caro tambien.  Se ajusta solo a la maquina.
    All = 3,    ///< todo lo que se sepa guardar.
};

/// El nivel en vigor, leido una vez de @c VESTA_ASA_CACHE (0..3).
CacheLevel cache_level();

/// Nombre estable del nivel, para volcados y depuracion.
const char *level_name(CacheLevel n);

/**
 * @brief Lo que un dominio cuenta de si mismo para decidir si se guarda.
 *
 * Es el productor quien sabe estas dos cosas; el fichero solo aplica el
 * criterio.  Sin @c micros el nivel @c PorCoste no puede decidir, y sin
 * @c recomputable el nivel @c Minimo no sabe que es imprescindible.
 */
struct DomainCost {
    const char *domain = "?";
    long        micros = 0;          ///< lo que costo producirlo.
    bool        recomputable = true; ///< false = se perdio si no se guarda.
    /**
     * @brief Huella de LO QUE ESTE DOMINIO MIRO, no del modulo entero.
     *
     * Es lo que hace la cache GRANULAR: cada registro se valida por su cuenta,
     * asi que tocar algo que solo afecta a un dominio descarta ese registro y
     * deja en pie los demas.  Con una unica huella global, cambiar una linea
     * obligaria a rehacerlo todo -- y cuanto menos haya que invalidar, menos
     * hay que rehacer.
     *
     * Cero = este dominio no sabe decir de que depende; entonces no se puede
     * comprobar y se acepta lo que haya (que es lo mismo que hoy, no peor).
     */
    uint64_t fingerprint = 0;
};

/**
 * @brief Si @p nivel manda guardar un dominio con ese coste.
 *
 * @param nivel Nivel en vigor.
 * @param c     Lo que el dominio dice de si mismo.
 * @return true si merece ir a disco.
 */
bool should_store(CacheLevel nivel, const DomainCost &c);

/**
 * @brief Empaqueta @p almacen en bytes.
 *
 * @param almacen Hechos a guardar.
 * @param huella  Identidad del modulo; al leer, si no cuadra se descarta todo.
 * @param nivel   Cuanto guardar.
 * @param costes  Lo que cada dominio dice de si mismo.  Un dominio que no
 *                aparezca se trata como recomputable y de coste cero, o sea que
 *                solo entra en el nivel @c Todo.
 * @return Los bytes, o vacio si el nivel es @c Nada o no quedo nada que guardar.
 */
std::vector<uint8_t> serialize(const FactStore                 &almacen,
                                uint64_t                         huella,
                                CacheLevel                       nivel,
                                const std::vector<DomainCost> &costes);

/**
 * @brief POR QUE no se pudo leer.  Es un DATO, no una frase.
 *
 * Quien lo muestre saca el texto del catalogo con @ref diag_code, que es lo
 * que hace que el mismo motivo se lea en el idioma de quien compila.  Un modulo
 * de analisis no escribe mensajes.
 */
enum class ReadReason : uint8_t {
    Ok = 0,      ///< se leyo bien.
    NoFile,     ///< primera compilacion, o se limpio la cache.
    Empty,            ///< existe pero no tiene nada.
    NotAFactFile,      ///< la magia no cuadra: no es esto.
    OtherVersion,  ///< contenedor de otra version.
    OtherModule,   ///< hechos de otro modulo.
    Truncated,         ///< se corto a mitad de escribirlo.
    ReadFailed,     ///< el disco fallo al leerlo.
};

/// El codigo del catalogo con el que se cuenta @p m en el idioma de quien
/// compila.  Vacio para @c Ninguno.
const char *diag_code(ReadReason m);

/// Que paso al leer.  Un fichero que no vale NO es un error: es que hay que
/// recalcular, y el motivo se dice para poder distinguirlo de un fallo real.
struct ReadResult {
    bool          ok = false;
    ReadReason reason = ReadReason::Ok;
    uint32_t      facts = 0;         ///< depositados en el almacen.
    uint32_t      domains = 0;       ///< registros leidos.
    uint32_t      skipped = 0;       ///< registros de dominios/versiones ajenas.
    uint32_t      stale = 0;        ///< registros cuya huella ya no vale.
    uint32_t      lost_proofs = 0; ///< apoyos en hechos que no se cargaron.
};

/**
 * @brief Deposita en @p destino los hechos de @p datos.
 *
 * AÑADE: el almacen puede traer hechos ya puestos y los identificadores de las
 * pruebas se recolocan sobre el.  Un registro cuya version no se reconozca se
 * SALTA -- para eso lleva la longitud delante --, y lo que se apoyaba en el
 * pierde ese apoyo en vez de arrastrar una referencia a un hecho que no existe.
 *
 * @param datos   Bytes del fichero.
 * @param n       Cuantos.
 * @param huella  La que debe llevar dentro; si no cuadra no se lee nada.
 * @param destino Donde se depositan.
 * @return Que se leyo, o por que no.
 */
ReadResult read_facts(const uint8_t *datos, size_t n, uint64_t huella,
                      FactStore &destino,
                      const std::vector<DomainCost> &vigentes = {});

/// Lee @p ruta y deposita en @p destino.  Si no existe, @c motivo lo dice.
ReadResult read_facts_file(const std::string &ruta, uint64_t huella,
                           FactStore &destino,
                           const std::vector<DomainCost> &vigentes = {});

} // namespace asa
} // namespace analysis

#endif // ANALYSIS_ASA_FACT_FILE_H
