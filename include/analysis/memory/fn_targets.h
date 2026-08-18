/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/memory/fn_targets.h
 * @brief A que funcion apunta un puntero a funcion, y donde acaba su direccion.
 *
 * "No se a quien llama esto" es una respuesta demasiado facil.  Muchas veces la
 * informacion ESTA en el modulo y solo hay que seguirla: la direccion se toma
 * en un sitio, viaja por un par de copias, y se llama en otro.  Rendirse ahi
 * obliga a cualquier consumidor a suponer lo peor sobre codigo que se puede
 * leer entero.
 *
 * Dos preguntas, que son las dos caras de lo mismo:
 *
 *   DESDE LA LLAMADA   `¿a que funcion apunta este valor?`  La usa quien esta
 * en un `call` indirecto y quiere el destino concreto. DESDE LA FUNCION   `¿se
 * ven TODOS los sitios que pueden llamarme?`  La usa quien quiere afirmar algo
 * sobre los parametros, que solo es licito conociendo a todos los llamantes.
 *
 * El limite esta declarado, no escondido: se sigue por copias,
 * reinterpretaciones y huecos escritos UNA sola vez.  En cuanto la direccion se
 * guarda en un sitio que se escribe mas de una vez, se pasa como argumento o se
 * devuelve, se dice que no se sabe.  Es preferible quedarse corto a afirmar de
 * mas: sobre esto se apoyan comprobaciones que pueden rechazar un programa.
 */
#ifndef ANALYSIS_MEMORY_FN_TARGETS_H
#define ANALYSIS_MEMORY_FN_TARGETS_H

#include "analysis/facts/ir_facts.h"

#include <string>
#include <utility>
#include <vector>

namespace ir {
struct IrFunction;
struct IrInstr;
struct IrModule;
} // namespace ir

namespace analysis {

/**
 * @brief Nombre de la funcion a la que apunta @p v, o cadena vacia.
 *
 * Vacio significa "no se puede afirmar", nunca "no apunta a nada".
 */
std::string funcion_apuntada(const ir::IrFunction &fn, const IrFacts &facts,
                             ir::IrValueId v);

/// Un sitio desde el que se llama, cuando la llamada es indirecta.
struct SitioIndirecto {
    const ir::IrFunction *fn = nullptr;
    const ir::IrInstr *instr = nullptr;
};

/// Que pasa con la direccion de una funcion en todo el modulo.
struct DireccionTomada {
    /// Si su nombre aparece en algun sitio que no sea un `call` directo.
    bool tomada = false;
    /// Si TODOS esos sitios acaban en llamadas indirectas que se ven.  Cuando
    /// es cierto, la lista de abajo completa el censo de llamantes.
    bool todas_se_ven = false;
    std::vector<SitioIndirecto> indirectas;
};

/**
 * @brief Sigue la direccion de @p nombre por todo @p mod.
 *
 * Responde si se pueden enumerar todos sus llamantes, y cuales son los
 * indirectos.  Los directos los ve cualquiera buscando `call`.
 */
DireccionTomada seguir_direccion(const ir::IrModule &mod,
                                 const std::string &nombre);

} // namespace analysis

#endif // ANALYSIS_MEMORY_FN_TARGETS_H
