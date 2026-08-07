/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/fact.h
 * @brief NUCLEO de ASA: que es un hecho, de donde viene y cuanto se puede uno
 *        fiar de el.  Vocabulario UNICO para todos los dominios.
 *
 * ASA no es un analisis: es la base de conocimiento del compilador.  Un dominio
 * (rangos, regiones, efectos, agregados, prestamos, bucles) DESCUBRE hechos; los
 * consumidores (comprobaciones de seguridad, optimizador, diagnosticos,
 * herramientas) deciden que hacer con ellos.  Este fichero define lo unico que
 * todos comparten, y existe por una razon concreta:
 *
 *   SI CADA DOMINIO INVENTA SU PROPIA PALABRA PARA "SEGURO", "DE DONDE SALE" Y
 *   "HECHO", CRUZARLOS DEJA DE SER POSIBLE.  No se puede combinar lo que no se
 *   mide igual, y lo que hace util una base de conocimiento es justo cruzarla.
 *
 * Tres piezas, y ninguna es decorativa:
 *
 *   CERTEZA      cuanto te puedes fiar.  Va DENTRO del hecho, no en el
 *                consumidor: si cada consumidor decide por su cuenta si algo es
 *                fiable, el mismo hecho justifica cosas contradictorias.
 *   PROCEDENCIA  quien lo descubrio y mirando que.  Sin esto no se puede
 *                explicar un veredicto ni depurar un analisis que miente.
 *   DEPENDENCIAS de que otros hechos se dedujo.  Un hecho derivado no puede ser
 *                mas fuerte que el peor de los que lo sostienen, y sin
 *                declararlas no hay forma de invalidarlo cuando uno cambia.
 */
#ifndef ANALYSIS_ASA_FACT_H
#define ANALYSIS_ASA_FACT_H

#include <cstdint>

namespace analysis {
namespace asa {

/**
 * @brief Confianza de un hecho.  El orden importa: a mayor valor, mas fuerte.
 *
 * La frontera que decide es la de arriba:
 *
 *   Desconocida   no hay evidencia suficiente.  NO es "falso": es que no se ha
 *                 mirado, o lo mirado no dice nada.  Distinguirlo de "falso" es
 *                 lo que impide que un analisis ciego parezca uno concluyente.
 *   Inferida      la evidencia apunta ahi, pero pudo quedar algo sin ver.
 *                 Sirve para OPTIMIZAR CON RED: especular con un guard, elegir
 *                 una version rapida con camino de vuelta.
 *   Demostrada    se ha visto TODO lo que podria contradecirlo.  Es lo unico
 *                 sobre lo que se puede rechazar un programa o quitar una
 *                 comprobacion sin dejar red.
 *
 * NO confundir con el VEREDICTO sobre una propiedad ("esto es seguro / no lo
 * es"): son ejes distintos.  Un hecho Demostrado puede afirmar que algo NO es
 * seguro.  Aqui se mide cuanto te fias de la afirmacion, no lo que dice.
 */
enum class Certeza : uint8_t {
    Desconocida = 0,
    Inferida = 1,
    Demostrada = 2,
};

/// La confianza de algo deducido de varias cosas es la MAS DEBIL de todas: una
/// cadena no es mas fuerte que su eslabon peor.
inline Certeza combinar(Certeza a, Certeza b) { return a < b ? a : b; }

/// Nombre estable para volcados y depuracion.  NO es texto de usuario: los
/// mensajes salen del catalogo i18n, nunca de aqui.
inline const char *nombre_certeza(Certeza c) {
    switch (c) {
    case Certeza::Demostrada: return "demostrada";
    case Certeza::Inferida: return "inferida";
    default: return "desconocida";
    }
}

/**
 * @brief Quien descubrio un hecho y mirando que.
 *
 * Un hecho sin procedencia no se puede explicar ni depurar: cuando un veredicto
 * sorprende, la primera pregunta es siempre "¿de donde ha salido esto?".  El
 * @c productor es un literal estatico (el nombre del analisis), no una cadena
 * construida: identifica al modulo, no al caso.
 */
struct Procedencia {
    const char *productor = "?"; ///< analisis que lo emitio.
    const char *funcion = "";    ///< funcion mirada (vacio si es de modulo).
    uint32_t    sitio = 0;       ///< value-id, bloque o linea, segun el dominio.
};

/**
 * @brief Que otros hechos sostienen a este.
 *
 * Se guarda por PRODUCTOR, no por instancia: basta para saber a quien invalidar
 * cuando algo cambia, y no obliga a que cada hecho arrastre punteros a otros.
 * Un maximo pequeno y fijo evita que un hecho crezca sin control; si a un
 * dominio le hicieran falta mas dependencias, es senal de que ese hecho hace
 * demasiadas cosas.
 */
struct Dependencias {
    static constexpr int kMax = 4;
    const char *de[kMax] = {nullptr, nullptr, nullptr, nullptr};

    void anadir(const char *productor) {
        for (int i = 0; i < kMax; ++i) {
            if (de[i] == nullptr) { de[i] = productor; return; }
            if (de[i] == productor) return;
        }
    }
    bool depende_de(const char *productor) const {
        for (int i = 0; i < kMax; ++i)
            if (de[i] == productor) return true;
        return false;
    }
};

/**
 * @brief Lo que TODO hecho de ASA lleva encima, sea cual sea su dominio.
 *
 * Se embebe por composicion en cada hecho concreto en vez de heredarse: los
 * hechos se copian en bucles calientes y viven en vectores, y una jerarquia con
 * funciones virtuales pagaria indireccion en el sitio equivocado.
 */
struct Sello {
    Certeza      certeza = Certeza::Desconocida;
    Procedencia  origen;
    Dependencias apoyos;
};

} // namespace asa
} // namespace analysis

#endif // ANALYSIS_ASA_FACT_H
