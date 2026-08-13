/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/base_hechos.h
 * @brief La base de hechos: lo que se sabe del programa, en UN sitio, con su
 *        procedencia y su certeza, para que quien lo necesite CONSULTE en vez de
 *        redescubrirlo.
 *
 * Es la Regla 1 hecha objeto: un pase CONSUME la base, no la CONSTRUYE.  Vive en
 * @c analysis/ y no en @c jit/ porque no es del JIT: el compilador en caliente es
 * UN consumidor, y lo mismo valen el volcado (@c analysis/asa/dump.h), el nativo
 * o una herramienta.
 *
 * POR QUE IMPORTA MAS ALLA DE AHORRAR UN COMPUTO.  Un consumidor que construye
 * lo que necesita solo puede preguntar por lo que sabe construir: se queda con
 * un dominio y con la precision que ese sitio del codigo se molesto en pedir.
 * Uno que CONSULTA puede cruzar dominios sin cambiar, y sobre todo puede recibir
 * hechos que EL no sabe producir -- los que solo existen ejecutando, que es lo
 * que aportara el C2.  Anadir una fuente de conocimiento pasa a ser anadir un
 * productor, no tocar a cada consumidor.
 *
 * DE DONDE VIENE UN HECHO NO ES ASUNTO DEL CONSUMIDOR.  Analisis estatico,
 * observacion en ejecucion o perfil de corridas anteriores alimentan la MISMA
 * base.  Lo que el consumidor mira es el hecho y su certeza -- que viaja DENTRO
 * del hecho, no la pone quien pregunta.  De ahi sale la guarda: no porque el
 * especulador lo decida, sino porque un hecho observado nace sin demostrar.
 *
 * AMBITO Y VIDA.  Una base vale para UN modulo: dentro de el un nombre
 * identifica una funcion, que es lo que hace legitimo cachear por nombre.  Quien
 * la crea la mantiene viva mientras trabaja sobre ese modulo y se la pasa a cada
 * consumidor; el conocimiento se calcula una vez y se reparte.  Sin base, el
 * consumidor se monta la suya como ultimo recurso: correcto, solo sin reparto.
 *
 * MUTAR EL IR CADUCA LOS HECHOS: quien lo toque avisa con @c invalidar.
 */
#ifndef ANALYSIS_ASA_BASE_HECHOS_H
#define ANALYSIS_ASA_BASE_HECHOS_H

#include "analysis/asa/fact.h"
#include "analysis/facts/ir_facts.h"
#include "analysis/facts/loop_facts.h"
#include "analysis/facts/range_summary.h"
#include "analysis/facts/value_range.h"
#include "analysis/manager/analysis_manager.h"
#include "analysis/memory/points_to.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace ir {
struct IrFunction;
struct IrModule;
} // namespace ir

namespace analysis {
namespace asa {

/// Los dominios que hoy guarda la base.  Nombres ESTABLES: son la procedencia
/// que aparece en el volcado, no texto de usuario.
///
/// Se DECLARAN aqui y se definen una sola vez en el .cpp, y eso no es
/// cosmetico: ASA identifica al productor por la DIRECCION del literal
/// (@c Dependencias::depende_de compara punteros).  Con @c constexpr cada
/// unidad de traduccion plegaria la lectura a SU propio literal -- los literales
/// no se unifican entre ficheros objeto -- y el mismo productor dejaria de
/// reconocerse a si mismo visto desde otro fichero.
extern const char *const kProductorEstructura;
extern const char *const kProductorRangos;
extern const char *const kProductorMemoria;
extern const char *const kProductorFrontera;
extern const char *const kProductorBucles;
/// Como se COLOCA la memoria del programa: lo unico que un compilador con
/// enlazador propio sabe y uno tradicional no.
extern const char *const kProductorDisposicion;

/// Clave con la que se guarda lo que es del MODULO entero y no de una funcion.
extern const char *const kUnidadModulo;

/**
 * @brief Da de alta los nombres de arriba como CANONICOS.
 *
 * Perezoso y a peticion: hace falta antes de leer hechos de disco, porque ASA
 * compara productores por direccion y una cadena recien leida no se
 * reconoceria a si misma.  No es un inicializador estatico a proposito --
 * reservar memoria antes de @c main corre las direcciones de todo lo demas.
 */
void register_asa_canonical_names();

/**
 * @brief Una entrada de la base, tal y como se vuelca.
 *
 * Es DATO, no frase: quien quiera enseñarlo lo formatea.  Lleva el sello de ASA
 * -- certeza, procedencia y de que otros hechos se dedujo -- porque un hecho sin
 * origen no se puede explicar ni depurar, y porque de la certeza depende lo que
 * el consumidor tiene derecho a hacer con el.
 */
struct HechoRegistrado {
    const char *dominio = kProductorEstructura;
    std::string funcion;
    Sello       sello;
};

/**
 * @brief Base de hechos compartida.
 *
 * Cachea por funcion y calcula bajo demanda.  Las dependencias entre dominios
 * las anota el @c AnalysisManager solo: cuando el computo de uno pide otro por
 * la base, queda registrado, y asi invalidar el de abajo arrastra al de arriba.
 */
class BaseDeHechos {
  public:
    /// Al morir cuenta lo que repartio si se pide con @c VESTA_ASA_HECHOS_DEBUG:
    /// una base compartida que no ahorra ninguna pregunta es un computo con otro
    /// nombre, y eso se ve o no se ve.
    /**
     * @brief Da de alta los nombres canonicos del ASA.
     *
     * Aqui y no en un inicializador estatico: reservar memoria antes de
     * @c main corre las direcciones de todo lo demas, y en un programa que
     * dependa de la alineacion de lo suyo eso cambia si funciona o no.  Y aqui
     * y no dentro del fichero de hechos, que es el formato y no tiene por que
     * conocer a los productores de nadie.
     */
    BaseDeHechos();
    ~BaseDeHechos();
    BaseDeHechos(const BaseDeHechos &) = delete;
    BaseDeHechos &operator=(const BaseDeHechos &) = delete;

    /**
     * @brief Hechos estructurales de @p fn: def-use, sitios de llamada, bucles.
     * @param fn Funcion IR a consultar.
     * @return Los hechos, cacheados mientras viva la base.
     */
    const IrFacts &estructura(const ir::IrFunction &fn);

    /**
     * @brief Entre que dos numeros esta cada valor de @p fn.
     * @param fn Funcion IR a consultar.
     * @return Los rangos por valor SSA, cacheados mientras viva la base.
     */
    const RangeFacts &rangos(const ir::IrFunction &fn);

    /**
     * @brief A que memoria puede referirse cada puntero de @p fn.
     * @param fn Funcion IR a consultar.
     * @return La tabla points-to, cacheada mientras viva la base.
     */
    const PointsTo &memoria(const ir::IrFunction &fn);

    /**
     * @brief Forma del CFG de @p fn: bucles, cabeceras y profundidad.
     * @param fn Funcion IR a consultar.
     * @return Los hechos de bucle, cacheados mientras viva la base.
     */
    const LoopFacts &bucles(const ir::IrFunction &fn);

    /**
     * @brief Lo que cruza la frontera de cada funcion del modulo.
     *
     * Es conocimiento DEL MoDULO, no de una funcion: para saber que le llega a
     * un parametro hay que ver a todos los que llaman.  Por eso se cachea una
     * vez por base y no por funcion.
     *
     * @param mod Modulo completo.
     * @return Los resumenes de entrada y salida por funcion.
     */
    const RangeSummaries &frontera(const ir::IrModule &mod);

    /**
     * @brief Los hechos de @p fn han caducado porque su IR cambio.
     * @param fn Funcion IR cuyo conocimiento se descarta (en cascada).
     */
    void invalidar(const ir::IrFunction &fn);

    /**
     * @brief El sello del conocimiento de @p productor sobre @p fn.
     *
     * La certeza NO la pone quien pregunta, viaja DENTRO del hecho, y de ella se
     * sigue lo que el consumidor puede hacer: sobre un hecho @c Demostrado se
     * puede quitar una comprobacion; sobre uno @c Inferido -- el analisis paro
     * por presupuesto, o mañana: lo observado en ejecucion -- hay que dejar red,
     * o sea una guarda.
     *
     * @param productor Uno de los @c kProductor*.
     * @param fn        Funcion IR consultada.
     * @return El sello, o uno @c Desconocido si nadie ha preguntado todavia.
     */
    Sello sello(const char *productor, const ir::IrFunction &fn) const;

    /// Igual que @c sello pero para lo que es del modulo entero (la frontera).
    Sello sello_de_modulo(const char *productor) const;

    /**
     * @brief Todo lo que la base sabe, en DATOS y en orden estable.
     *
     * Un conocimiento que no se puede volcar no se puede auditar: ni explicar un
     * veredicto, ni ver por que un analisis se callo, ni comprobar que la
     * procedencia es la que se cree.  Ordenado por funcion y dominio para que
     * dos volcados se puedan comparar.
     *
     * @return Una entrada por hecho vivo.
     */
    std::vector<HechoRegistrado> volcado() const;

    /// Preguntas atendidas.  Con @c computos mide el reparto de verdad, que es
    /// lo unico que distingue una base compartida de un computo con otro nombre.
    size_t consultas() const { return consultas_; }
    /// Analisis que hubo que ejecutar de verdad (los demas salieron de la cache).
    size_t computos() const { return computos_; }

  private:
    /// Identidad de @p fn dentro del modulo.  Sin nombre no hay identidad
    /// estable, y entonces vale su direccion: es unica mientras la funcion viva,
    /// que es lo que dura la base.
    static std::string clave_de(const ir::IrFunction &fn);

    /// Anota el sello de un hecho recien producido.
    void sellar(const char *productor, const std::string &clave, Certeza c,
                const char *apoyo = nullptr);

    /// Sellos por (productor, unidad).  Se guardan aparte del resultado porque
    /// el gestor cachea el DATO del dominio; la procedencia es del hecho, y la
    /// llevan todos por igual.
    std::unordered_map<const char *, std::unordered_map<std::string, Sello>>
        sellos_;

    AnalysisManager gestor_;
    size_t          consultas_ = 0;
    size_t          computos_ = 0;
};

/**
 * @brief Vuelca @p entradas por @p salida en una linea por hecho.
 *
 * Formato de DEPURACION, no de usuario: los mensajes de usuario salen del
 * catalogo multi-idioma.
 *
 * @param entradas Lo devuelto por @c BaseDeHechos::volcado.
 * @param salida   Fichero abierto donde escribir.
 */
void volcar_hechos(const std::vector<HechoRegistrado> &entradas, FILE *salida);

} // namespace asa
} // namespace analysis

#endif // ANALYSIS_ASA_BASE_HECHOS_H
