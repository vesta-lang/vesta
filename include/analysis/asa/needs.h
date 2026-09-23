/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file analysis/asa/needs.h
 * @brief Lo que un consumidor necesita del ASA: se DECLARA, no se monta.
 *
 * Eran TRES campos paralelos en las opciones del compilador -- que dominios,
 * en que momentos, y una marca de "todos" -- y cada uno de los seis sitios que
 * piden hechos los montaba a mano.  Habian divergido en cuatro combinaciones
 * distintas, y la peor era muda: las dos vistas del editor pedian el primer y
 * el ultimo momento, asi que se perdian el de EN MEDIO -- que es donde vive por
 * que NO disparo una optimizacion -- y no fallaban; sencillamente esos hechos
 * no aparecian.
 *
 * Aqui van juntos porque son UNA peticion, y con ellos van las preguntas que
 * antes cada lector se respondia por su cuenta: si hay algo que producir
 * (@ref anything), si quiere un momento (@ref wants_stage) y que lista de
 * dominios toca (@ref domain_list).  Las dos primeras estaban escritas DOS
 * veces -- una lambda en el camino de fichero suelto y una funcion en el de
 * proyecto --, que es como el mismo programa acaba analizandose distinto segun
 * por donde entre.
 *
 * Quien quiera un subconjunto a proposito lo dice y explica por que en
 * @ref asked_by; lo que no puede pasar es que "todo" signifique cosas
 * distintas segun el sitio.
 */
#ifndef ANALYSIS_ASA_NEEDS_H
#define ANALYSIS_ASA_NEEDS_H

#include <cstring>
#include <vector>

namespace analysis {
namespace asa {

/**
 * @brief La peticion de UN consumidor al ASA.
 *
 * Vacio por defecto quiere decir NO PRODUCIR NADA, y eso es la mitad del
 * diseno: producir conocimiento que nadie pide no es prevision, es tiempo
 * tirado.  Medido en `199_cfn_vs_lambda`, producirlo todo llevaba la
 * compilacion de 200 ms a 1,5 s -- siete veces -- sin un solo consumidor
 * esperandolo.
 */
struct AsaNeeds {
    /**
     * @brief Dominios que alguien va a consultar.  Vacio = ninguno.
     *
     * Se DERIVAN siempre que se pueda (el linter los saca de sus familias
     * registradas): una lista escrita se queda vieja en cuanto entra una
     * familia que consulte otro dominio, y se queda vieja EN SILENCIO.
     */
    std::vector<const char *> domains;

    /**
     * @brief En que momentos.  Vacio = ninguno.  @see kStage*
     *
     * Sin esto el conocimiento no se podia usar: quien pide dominios dice QUE
     * quiere saber pero no DE QUE CoDIGO -- el que el programa dice o el que va
     * a ejecutarse --, y son cosas distintas.  Un hecho lleva su momento
     * sellado, asi que producir en uno y preguntar por otro no ve NADA: no
     * falla, no dice nada, y encima lo recalcula.
     */
    std::vector<const char *> stages;

    /**
     * @brief TODOS los dominios, sin listarlos.  Lo pide quien VUELCA.
     *
     * Explicito y no un centinela sobre @ref domains: "vacio = ninguno" y
     * "vacio = todos" en el mismo campo es la ambiguedad que acaba produciendo
     * 544 us por modulo sin que nadie sepa por que.
     */
    bool all_domains = false;

    /**
     * @brief QUIeN pregunta, y por tanto por que pide esto.
     *
     * Un subconjunto sin explicacion no se distingue de un olvido -- que es
     * exactamente como se colo que el editor no pidiera el momento de en medio
     * --.  Viaja con la peticion para que un volcado pueda decir de quien es.
     */
    const char *asked_by = "";

    /// Hay algo que producir?  Si no, ni se monta la base de hechos.
    bool anything() const { return !domains.empty() || all_domains; }

    /// Se quiere @p stage?  La pregunta que dos caminos distintos se
    /// respondian cada uno por su cuenta.
    bool wants_stage(const char *stage) const {
        for (const char *s : stages)
            if (std::strcmp(s, stage) == 0) return true;
        return false;
    }

    /**
     * @brief La lista que se le pasa al productor.
     *
     * Vacia significa "todos" AHi ABAJO, al reves que en @ref domains.  La
     * conversion estaba escrita dos veces como un ternario suelto; dicha aqui
     * una vez, los dos caminos no pueden interpretarla distinto.
     */
    std::vector<const char *> domain_list() const {
        return all_domains ? std::vector<const char *>{} : domains;
    }
};

/**
 * @brief Todo lo que se sabe, en los tres momentos.
 *
 * Es lo que pide quien ENSENA el conocimiento -- el volcado de la linea de
 * ordenes y las vistas del editor --, porque su trabajo es precisamente no
 * dejarse nada: una lista escrita a mano se quedaria corta en cuanto entrara
 * un dominio o un momento nuevo, calladamente.
 *
 * @param asked_by Quien pregunta.
 */
AsaNeeds needs_all(const char *asked_by);

} // namespace asa
} // namespace analysis

#endif // ANALYSIS_ASA_NEEDS_H
