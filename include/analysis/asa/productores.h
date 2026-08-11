/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */


/*
La clave está especialmente aquí:
                                    ┌─────────────┐
                    análisis ──────►│             │
                    estático        │             │
                                    │     ASA     │────► DSE
                    C2/runtime ────►│    Fact     │────► scheduler
                                    │             │────► codegen
                    PGO ───────────►│             │────► otros
                                    └─────────────┘

estático -> certeza demostrada
C2       -> certeza posiblemente inválida
PGO      -> conocimiento observado


En un diseño convencional, es frecuente encontrar algo conceptualmente más parecido a:

range analysis ─────► optimizador ──► criterio propio
points-to ──────────► optimizador ──► criterio propio
profile ────────────► JIT ──────────► criterio propio
type analysis ──────► codegen ──────► criterio propio

Aunque existen sistemas que comparten análisis, la idea fuerte de que el 
conocimiento sea un recurso arquitectónico explícito, 
con certeza/procedencia/prueba, y que los consumidores sean 
deliberadamente agnósticos respecto al productor, es otra cosa.

"Vesta introduce una arquitectura de conocimiento unificada basada en ASA,
donde análisis estático, runtime/JIT y PGO actúan como productores de hechos 
con certeza y procedencia, mientras los consumidores consultan ese conocimiento 
sin mantener criterios paralelos."

*/

/**
 * @file analysis/asa/productores.h
 * @brief Quien convierte el resultado de un analisis en HECHOS del ASA.
 *
 * Un analisis produce una estructura de datos suya -- intervalos, tablas
 * points-to, formas de bucle --; un hecho es una afirmacion con certeza,
 * procedencia y prueba.  El paso de lo uno a lo otro es una DECISION del
 * dominio: cual de sus resultados dice algo y cual no.  Ese criterio vive aqui,
 * en el productor, y NO en quien luego mire los hechos.
 *
 * Es la separacion que hace que el sistema no se bifurque:
 *
 *      analisis  ->  PRODUCTOR  ->  FactStore  ->  consumidores
 *      (calcula)    (afirma)       (guarda)       (deciden)
 *
 * Si el criterio de "esto merece afirmarse" viviera en el consumidor, cada
 * consumidor tendria el suyo -- y volveriamos justo al problema que el ASA
 * existe para quitar.  Por eso el volcado (@c analysis/asa/dump.h) NO decide
 * nada: solo ensena lo que hay.
 *
 * AMPLIAR es anadir un productor con @c registrar_productor: aparece en el
 * almacen, en el volcado y en los filtros sin tocar el motor.  Mañana el mismo
 * mecanismo recibe lo observado en ejecucion o un perfil de corridas anteriores:
 * otro productor, mismos hechos, otra procedencia y otra certeza.
 */
#ifndef ANALYSIS_ASA_PRODUCTORES_H
#define ANALYSIS_ASA_PRODUCTORES_H

#include "analysis/asa/base_hechos.h"
#include "analysis/asa/fact_store.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ir {
struct IrModule;
struct IrFunction;
} // namespace ir

namespace analysis {
namespace asa {

/**
 * @brief Hasta donde afirmar.  Es politica de PRODUCCION, no de presentacion.
 *
 * Se separa a proposito de los filtros del volcado: no pedir un dominio ahorra
 * CALCULARLO; ocultarlo en la vista no.  Y afirmar lo desconocido es una
 * decision de produccion porque cuesta memoria proporcional al programa.
 */
struct OpcionesProduccion {
    std::vector<std::string> dominios;  ///< por nombre corto; vacio = todos.
    std::vector<std::string> funciones; ///< por nombre exacto; vacio = todas.
    /**
     * @brief Afirmar tambien lo que se miro SIN sacar nada.
     *
     * Un "de esto no se sabe nada" es un hecho con certeza @c Desconocida, no la
     * ausencia de hecho: distingue lo que un dominio miro y no supo de lo que ni
     * siquiera miro, y esa diferencia es la que dice donde hay sitio para saber
     * mas.  Cuesta memoria proporcional al programa, asi que se pide.
     */
    bool     desconocidos = false;
    uint32_t tope_por_funcion = 0; ///< 0 = sin tope.
};

/**
 * @brief POR QUE un dominio no supo algo, y cuantas veces.
 *
 * NO SABER ALGO ES UN RESULTADO, y sin el motivo no sirve para nada: "no lo se"
 * puede significar que el valor de verdad puede ser cualquiera, que el analisis
 * se quedo sin presupuesto, o que ni siquiera se miro porque se pidio un tope.
 * Son tres cosas distintas y se arreglan de tres formas distintas.  Por eso el
 * motivo se cuenta SIEMPRE, aunque no se pidan los hechos desconocidos uno a
 * uno: callarse el porque es lo unico que el ASA no puede hacer.
 */
struct MotivoIgnorancia {
    const char *codigo = "?"; ///< estable, del vocabulario del dominio.
    uint32_t    veces = 0;
};

/// Lo que se produjo por dominio.  Las "miradas" incluyen lo que no dio nada.
struct ResumenProduccion {
    const char *dominio = nullptr;
    uint32_t    hechos = 0;   ///< afirmaciones con algo que decir.
    uint32_t    miradas = 0;  ///< entidades examinadas.
    uint32_t    callados = 0; ///< de esas, las que no dieron nada.
    long        micros = 0;
    /// Desglose de @c callados por motivo.  Pocos por dominio: vector plano.
    std::vector<MotivoIgnorancia> motivos;
};

/**
 * @brief Lo que un productor recibe.
 *
 * Trae la base de hechos ya montada -- un productor tampoco reconstruye lo
 * comun (Regla 1) -- y el almacen donde deposita.
 */
struct Produccion {
    const ir::IrModule       &mod;
    BaseDeHechos             &base;
    FactStore                &almacen;
    const OpcionesProduccion &op;
    ResumenProduccion        &resumen;
    /**
     * @brief El hecho de estructura de cada funcion, si ya se produjo.
     *
     * Permite que un productor apoye SU hecho en otro CONCRETO -- no solo en el
     * nombre del productor -- y que la derivacion se pueda recorrer despues.
     */
    std::unordered_map<std::string, FactId> &estructura_de;

    /// Si la funcion entra en el filtro pedido.
    bool interesa(const ir::IrFunction &fn) const;
    /// Deposita un hecho contandolo en el resumen.
    FactId afirmar(Fact f);
    /**
     * @brief Anota que se miro @p de_quien y no se saco nada, Y POR QUE.
     *
     * El motivo se registra siempre (agregado por codigo); ademas se afirma como
     * hecho de certeza desconocida si se pidieron los desconocidos.  Un dominio
     * NUNCA se calla sin decir por que: sin el motivo, "no lo se" y "no lo mire"
     * se leen igual.
     *
     * @param de_quien De que entidad no se supo nada.
     * @param motivo   Codigo ESTABLE del vocabulario del dominio.
     * @param dominio  Quien lo dice.
     * @param detalle  Texto para una persona (lo mismo, en cristiano).
     */
    void callar(Sujeto de_quien, const char *motivo, const char *dominio,
                const char *detalle);
};

/// Un dominio que sabe convertir su analisis en hechos.
using Productor = void (*)(Produccion &);

/**
 * @brief Da de alta un productor.
 *
 * @param dominio Nombre estable (el que aparece como procedencia).
 * @param p       Funcion que afirma sus hechos.
 */
void registrar_productor(const char *dominio, Productor p);

/// Los dominios dados de alta, en orden de registro.
std::vector<const char *> productores_registrados();

/**
 * @brief Corre los productores pedidos sobre @p mod y deposita en @p almacen.
 *
 * Monta UNA base de hechos para todos: el conocimiento comun se calcula una vez
 * aunque lo usen cinco dominios.
 *
 * @param mod     Modulo IR ya optimizado (el codigo que de verdad va a existir).
 * @param almacen Donde se depositan los hechos.
 * @param op      Politica de produccion.
 * @return El resumen por dominio.
 */
std::vector<ResumenProduccion> producir(const ir::IrModule &mod,
                                        FactStore          &almacen,
                                        const OpcionesProduccion &op);

} // namespace asa
} // namespace analysis

#endif // ANALYSIS_ASA_PRODUCTORES_H
