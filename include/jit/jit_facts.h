/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/jit_facts.h
 * @brief La base de hechos que consume el compilador en caliente.  Los
 *        consumidores del JIT CONSULTAN lo que se sabe del programa; no lo
 *        vuelven a descubrir cada uno por su cuenta.
 *
 * Es el mismo trato que ya rige en el optimizador (@c ir_optimize construye UNA
 * tabla points-to y LICM/DSE/planificador la reciben; ver la Regla 1 en
 * @c ir_optimizer.cpp y @c analysis/memory/memory_access.h).  Aqui se traslada
 * al JIT, que hasta ahora era el sitio donde un consumidor -- el especializador
 * de llamadas -- construia su propio conocimiento en el sitio, para una sola
 * pregunta y de un solo dominio.
 *
 * POR QUE IMPORTA MAS ALLA DE AHORRAR UN COMPUTO.  Un consumidor que construye
 * lo que necesita solo puede preguntar por lo que sabe construir: se queda con
 * un dominio y con la precision que ese sitio del codigo se molesto en pedir.
 * Uno que CONSULTA puede cruzar dominios sin cambiar, y sobre todo puede recibir
 * hechos que EL no sabe producir -- los que solo existen ejecutando, que es
 * justo lo que el C2 va a aportar.  Anadir una fuente de conocimiento pasa a ser
 * anadir un productor, no tocar a cada consumidor.
 *
 * DE DONDE VIENE UN HECHO NO ES ASUNTO DEL CONSUMIDOR.  Analisis estatico, C2 en
 * ejecucion o perfil de corridas anteriores alimentan la MISMA base.  Lo que el
 * consumidor mira es el hecho y su certeza -- que viaja DENTRO del hecho, no la
 * pone quien pregunta.  De ahi sale la guarda cuando llegue la especulacion: no
 * porque el especulador lo decida, sino porque un hecho observado en ejecucion
 * nace sin demostrar.
 *
 * AMBITO Y VIDA.  Una base vale para UN modulo IR: dentro de el un nombre
 * identifica una funcion, que es lo que hace legitimo cachear por nombre.  Quien
 * la crea la mantiene viva mientras compila ese modulo y se la pasa a cada
 * compilacion; los hechos se calculan una vez y se reparten.  Sin base -- una
 * compilacion suelta -- el consumidor se monta la suya como ultimo recurso, que
 * es correcto y solo pierde el reparto.
 *
 * MUTAR EL IR CADUCA LOS HECHOS: quien lo toque avisa con @c invalidar, igual
 * que el optimizador hace antes de cada consumidor.
 */
#ifndef VESTA_JIT_JIT_FACTS_H
#define VESTA_JIT_JIT_FACTS_H

#include "analysis/asa/fact.h"
#include "analysis/facts/ir_facts.h"
#include "analysis/facts/value_range.h"
#include "analysis/manager/analysis_manager.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace ir {
struct IrFunction;
}

namespace jit {

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

/**
 * @brief Una entrada de la base, tal y como se vuelca.
 *
 * Es DATO, no frase: quien quiera enseñarlo lo formatea.  Lleva el sello de ASA
 * -- certeza, procedencia y de que otros hechos se dedujo -- porque un hecho sin
 * origen no se puede explicar ni depurar, y porque de la certeza depende lo que
 * el consumidor tiene derecho a hacer con el.
 */
struct HechoRegistrado {
    const char          *dominio = kProductorEstructura;
    std::string          funcion;
    analysis::asa::Sello sello;
};

/**
 * @brief Base de hechos compartida por los consumidores del JIT.
 *
 * Cachea por funcion y calcula bajo demanda.  Las dependencias entre dominios
 * las registra el @c analysis::AnalysisManager solo (los rangos piden la
 * estructura, asi que invalidar la estructura arrastra a los rangos).
 */
class JitFactBase {
  public:
    JitFactBase() = default;
    /// Al morir cuenta lo que repartio si se pide con @c VESTA_JIT_HECHOS_DEBUG:
    /// una base compartida que no ahorra ninguna pregunta es un computo con otro
    /// nombre, y eso se ve o no se ve.
    ~JitFactBase();
    JitFactBase(const JitFactBase &) = delete;
    JitFactBase &operator=(const JitFactBase &) = delete;

    /**
     * @brief Hechos estructurales de @p fn: def-use, sitios de llamada, bucles.
     * @param fn Funcion IR a consultar.
     * @return Los hechos, cacheados mientras viva la base.
     */
    const analysis::IrFacts &estructura(const ir::IrFunction &fn);

    /**
     * @brief Entre que dos numeros esta cada valor de @p fn.
     * @param fn Funcion IR a consultar.
     * @return Los rangos por valor SSA, cacheados mientras viva la base.
     */
    const analysis::RangeFacts &rangos(const ir::IrFunction &fn);

    /**
     * @brief Los hechos de @p fn han caducado porque su IR cambio.
     * @param fn Funcion IR cuyo conocimiento se descarta (en cascada).
     */
    void invalidar(const ir::IrFunction &fn);

    /**
     * @brief El sello del conocimiento de rangos de @p fn: de donde salio y
     *        cuanto se puede uno fiar.
     *
     * La certeza NO la pone quien pregunta, viaja DENTRO del hecho, y de ella se
     * sigue lo que el consumidor puede hacer: sobre un hecho @c Demostrado se
     * puede quitar una comprobacion; sobre uno @c Inferido -- el analisis paro
     * por presupuesto, o mañana: lo observado en ejecucion por el C2 -- hay que
     * dejar red, o sea una guarda.
     *
     * @param fn Funcion IR consultada (debe habersele pedido ya el hecho).
     * @return El sello, o uno @c Desconocido si nadie ha preguntado todavia.
     */
    analysis::asa::Sello sello_rangos(const ir::IrFunction &fn) const;

    /**
     * @brief Todo lo que la base sabe, en DATOS y en orden estable.
     *
     * Un conocimiento que no se puede volcar no se puede auditar: ni explicar un
     * veredicto, ni ver por que un analisis se callo, ni comprobar que la
     * procedencia es la que se cree.  Ordenado por funcion y dominio para que dos
     * volcados se puedan comparar.
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

    /// Sello de cada hecho vivo, por dominio y funcion.  Se guarda aparte del
    /// resultado porque el gestor cachea el DATO del dominio; la procedencia es
    /// del hecho, no del dominio, y la comparten todos por igual.
    std::unordered_map<std::string, analysis::asa::Sello> sellos_estructura_;
    std::unordered_map<std::string, analysis::asa::Sello> sellos_rangos_;

    analysis::AnalysisManager gestor_;
    size_t                    consultas_ = 0;
    size_t                    computos_ = 0;
};

/**
 * @brief Vuelca @p entradas por @p salida en una linea por hecho.
 *
 * Formato de DEPURACION, no de usuario: los mensajes de usuario salen del
 * catalogo multi-idioma.
 *
 * @param entradas Lo devuelto por @c JitFactBase::volcado.
 * @param salida   Fichero abierto donde escribir.
 */
void volcar_hechos(const std::vector<HechoRegistrado> &entradas, FILE *salida);

/**
 * @brief Si a alguna llamada o reserva de @p fn llega un argumento del que se
 *        sepa algo.
 *
 * "Saber algo" no es "ser una constante": es estar ACOTADO y que la cota diga
 * mas que el tipo.  Un tamano del que solo se sabe que no pasa del tope del
 * bloque pequeno ya poda la rama de los bloques grandes sin ser constante.
 *
 * Es una PREGUNTA sobre el programa, no una decision: responde que se sabe, y
 * quien pregunta decide que hacer con ello.
 *
 * @param fn     Funcion IR a examinar.
 * @param rangos Rangos de @p fn, recibidos de la base de hechos.
 * @return true si algun operando de un sitio de llamada o de reserva esta
 *         acotado por debajo de todo su tipo.
 */
bool hay_argumento_acotado(const ir::IrFunction    &fn,
                           const analysis::RangeFacts &rangos);

} // namespace jit

#endif // VESTA_JIT_JIT_FACTS_H
