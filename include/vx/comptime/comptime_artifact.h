/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file comptime_artifact.h
 * @brief El artefacto comptime de un modulo: su bytecode, compilado una vez y
 *        reusado por contenido.
 *
 * @par El problema
 * Hoy el bytecode que alimenta a la ComptimeVM se produce compilando el
 * PROYECTO ENTERO, lo que da un artefacto MONOLITICO -- medido: 704 KB, 182
 * macros, ~800 ms, el 43% de una compilacion en frio --.  Como es uno solo,
 * tocar UNA funcion comptime lo regenera entero, aunque las raices comptime
 * reales de ese mismo programa sean ocho funciones.
 *
 * @par La pieza que falta
 * Nada de esto necesita maquinaria nueva: el conjunto comptime ya se recolecta
 * (@ref vx::collect_comptime_unit), ya trae su texto compilable
 * (@c ComptimeUnit::unit_source) y su clave de cache
 * (@c ComptimeUnit::content_hash), compilar un fuente en memoria ya se sabe
 * hacer (@c vesta::tc::compile con @c source_overlay) y guardar un artefacto por
 * contenido tambien (@ref vx::CasStore).  Lo que faltaba era JUNTARLO, que es lo
 * unico que hay aqui.
 *
 * @par Por que por contenido y no por fichero
 * La clave es el @c content_hash del conjunto, que cambia si y solo si cambia
 * una decl comptime o una de sus dependencias.  Editar codigo de runtime -- o
 * anadir un `import` que el comptime no usa -- NO la mueve, asi que el artefacto
 * se reusa entre compilaciones y entre proyectos (el store por defecto es
 * global).  Ese es todo el ahorro.
 */

#ifndef VESTA_VX_COMPTIME_ARTIFACT_H
#define VESTA_VX_COMPTIME_ARTIFACT_H

#include "vx/comptime/comptime_collect.h"
#include "vx/incremental.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vx {

/**
 * @brief Resultado de obtener el artefacto de un conjunto comptime.
 */
struct ComptimeArtifact {
    bool ok = false;              ///< @c true si hay bytecode utilizable.
    bool from_cache = false;      ///< @c true si se reuso (no se compilo).
    std::vector<uint8_t> velb;    ///< Bytecode del conjunto.
    std::string error;            ///< Motivo, si @c ok es @c false.
};

/**
 * @brief Obtiene el bytecode del conjunto comptime @p unit, compilandolo solo
 *        si no estaba ya en @p cas.
 *
 * Compila @c unit.unit_source de forma AISLADA: el conjunto arrastra sus
 * dependencias no-comptime y los `import`, asi que compila sin el resto del
 * modulo.  Si no compilara, el conjunto no seria auto-suficiente y eso es un
 * fallo del recolector, no de aqui -- por eso el error se devuelve tal cual en
 * vez de degradarse en silencio a "sin artefacto".
 *
 * @param unit      Conjunto comptime del modulo.  Si @c unit.empty() o no tiene
 *                  texto, devuelve @c ok=false sin tocar el store: no hay nada
 *                  que compilar, que NO es lo mismo que un fallo (se distingue
 *                  por @c error, que queda vacio).
 * @param cas       Store por contenido donde buscar y guardar.
 * @param work_dir  Directorio para los intermedios de la compilacion.
 * @return El artefacto; @c from_cache dice si se ahorro la compilacion.
 */
ComptimeArtifact comptime_artifact_get(const ComptimeUnit &unit,
                                       const CasStore &cas,
                                       const std::string &work_dir);

} // namespace vx

#endif // VESTA_VX_COMPTIME_ARTIFACT_H
