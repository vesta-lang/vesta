/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/fact_store.h
 * @brief Donde viven los hechos: se depositan y se consultan.  UNA representacion
 *        para todo el conocimiento del programa.
 *
 * Es la frontera del dibujo:
 *
 *      estatico        observacion en ejecucion        perfil
 *          \                    |                       /
 *           +--------- producen hechos ----------------+
 *                              |
 *                        [ FactStore ]
 *                              |
 *           +------------------+------------------+
 *           |                  |                  |
 *        volcado           optimizador          codegen
 *
 * Lo que hace que esto NO sea otro sistema de conocimiento paralelo es que el
 * consumidor no sabe -- ni le importa -- de que rama viene un hecho: lee la
 * @c Proposicion y la @c Certeza, que viajan dentro.  Un hecho observado nace
 * @c Inferido, y de ahi sale la guarda; uno demostrado deja quitar la
 * comprobacion.  Ninguna de esas dos decisiones se toma consultando el origen.
 *
 * QUE NO ES: no es una cache de analisis.  Los resultados crudos de cada dominio
 * (rangos, points-to, ...) se cachean en @c BaseDeHechos; aqui vive el
 * CONOCIMIENTO ya afirmado, con su certeza y su prueba.  Son dos capas y hacen
 * falta las dos: una evita recomputar, la otra permite razonar y explicar.
 */
#ifndef ANALYSIS_ASA_FACT_STORE_H
#define ANALYSIS_ASA_FACT_STORE_H

#include "analysis/asa/fact.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace analysis {
namespace asa {

/**
 * @brief Da de alta un nombre CANONICO (un literal estable y unico).
 *
 * ASA identifica al productor por la DIRECCION del literal
 * (@c Dependencias::depende_de compara punteros), y eso funciona mientras los
 * hechos nacen en memoria.  En cuanto uno vuelve de disco su nombre es una
 * cadena recien leida: apuntaria a otro sitio y el mismo productor dejaria de
 * reconocerse a si mismo.  Por eso al leer se devuelven los nombres conocidos a
 * su literal, y quien tenga uno lo da de alta aqui.
 *
 * @param nombre Literal estable (vive lo que el programa).
 */
void register_canonical_name(const char *nombre);

/// El literal canonico de @p s, o @c nullptr si nadie lo ha dado de alta.
const char *canonical_name(const std::string &s);

/**
 * @brief Almacen de hechos: se anaden y se consultan.
 *
 * Solo CRECE mientras se produce.  Un hecho no se corrige: si algo cambia, se
 * afirma otro con su procedencia y su certeza, y quien consulte vera los dos --
 * que es justo lo que hace falta cuando el analisis estatico dice una cosa y la
 * ejecucion otra.
 */
class FactStore {
  public:
    /// Deposita @p f y devuelve su identidad.
    FactId anadir(Fact f);

    const Fact &at(FactId id) const { return hechos_[id]; }
    size_t      size() const { return hechos_.size(); }
    const std::vector<Fact> &todos() const { return hechos_; }

    /**
     * @brief Guarda una cadena y devuelve un puntero ESTABLE a ella.
     *
     * Los sujetos apuntan a nombres; si cada productor guardara los suyos, el
     * almacen quedaria lleno de punteros a memoria ajena que puede morir antes.
     * Aqui se internan una vez y viven lo que el almacen.
     */
    const char *internar(const std::string &s);

    /**
     * @brief Avisa de cuantos hechos se esperan.
     *
     * Un programa grande produce cientos de miles, y hacerlos crecer de uno en
     * uno significa copiar el vector entero unas veinte veces.  El llamante lo
     * sabe estimar (valores del modulo x dominios) y decirlo sale gratis.
     *
     * @param n Hechos previstos.
     */
    void reservar(size_t n) { hechos_.reserve(n); }

    /// Hechos que hablan de @p funcion (cualquier clase de sujeto dentro).
    const std::vector<FactId> &de_funcion(const std::string &funcion) const;
    /// Hechos que produjo @p dominio.
    const std::vector<FactId> &de_dominio(const char *dominio) const;

    /**
     * @brief La derivacion de @p id: el hecho y, detras, aquellos de los que se
     *        dedujo, en anchura y sin repetir.
     *
     * Es "todo veredicto lleva su prueba" hecho consulta: con esto un
     * diagnostico, una herramienta o una persona pueden preguntar POR QUE.
     *
     * @param id Hecho a explicar.
     * @return Los identificadores, empezando por @p id.
     */
    std::vector<FactId> explicar(FactId id) const;

    /// Cuantos hechos hay por certeza (para saber de que se fia uno).
    struct Recuento {
        uint32_t demostradas = 0;
        uint32_t inferidas = 0;
        uint32_t desconocidas = 0;
    };
    Recuento recuento() const;

  private:
    std::vector<Fact>       hechos_;
    std::deque<std::string> nombres_; ///< arena: no invalida punteros al crecer.
    std::unordered_map<std::string, const char *> internados_;
    std::unordered_map<std::string, std::vector<FactId>> por_funcion_;
    std::unordered_map<const char *, std::vector<FactId>> por_dominio_;
};

} // namespace asa
} // namespace analysis

#endif // ANALYSIS_ASA_FACT_STORE_H
