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
 * @file analysis/asa/dump.h
 * @brief La VISTA del conocimiento: ensenar lo que hay en el almacen.
 *
 * Aqui NO se decide que es un hecho ni si algo merece afirmarse -- eso es del
 * productor de cada dominio (@c analysis/asa/productores.h).  Esto solo ordena,
 * filtra y escribe.  La separacion importa: si la vista decidiera, tendriamos
 * otra vez un criterio propio en un consumidor, que es exactamente lo que el ASA
 * existe para quitar.
 *
 * Un conocimiento que no se puede mirar no se puede ni auditar ni ampliar: no
 * hay forma de saber si un consumidor renuncia porque no hay nada que saber o
 * porque nadie se lo ha contado.  Por eso esta vista ensena tambien los hechos
 * con certeza DESCONOCIDA -- lo que se miro sin sacar nada --, que es donde hay
 * sitio para saber mas.
 */
#ifndef ANALYSIS_ASA_DUMP_H
#define ANALYSIS_ASA_DUMP_H

#include "analysis/asa/fact_store.h"
#include "analysis/asa/productores.h"

#include <cstdio>
#include <string>
#include <vector>

namespace analysis {
namespace asa {

/// Que ensenar.  Son filtros de PRESENTACION: no ahorran calcular nada (para
/// eso estan las opciones de produccion), solo deciden que se ve.
struct OpcionesVista {
    /// Ensenar tambien los hechos de certeza desconocida (los silencios).  Solo
    /// tiene efecto si se pidieron al producir: lo que no se afirmo no existe.
    bool desconocidos = true;
    /// Ensenar la derivacion de cada hecho (de que otros hechos se sigue).
    bool pruebas = false;
};

/// Salida legible, en orden estable para poder comparar dos volcados.
void imprimir_volcado(const FactStore                      &almacen,
                      const std::vector<ResumenProduccion> &resumenes,
                      const OpcionesVista &vista, FILE *salida);

/// Salida para herramientas.  Mismo contenido, sin adornos.
std::string volcado_json(const FactStore                      &almacen,
                         const std::vector<ResumenProduccion> &resumenes,
                         const OpcionesVista                  &vista);

} // namespace asa
} // namespace analysis

#endif // ANALYSIS_ASA_DUMP_H
