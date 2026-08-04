/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file lowering_map.h
 * @brief Correspondencia entre lo que se escribio y el codigo intermedio.
 *
 * La pieza que ata la capa semantica con la intermedia, y la que faltaba para
 * que el camino de la depuracion quedara completo:
 *
 *     entidad -> sentencia -> [bajada] -> intermedio -> [codigo] -> maquina
 *
 * Va aparte de las dos capas que une, igual que la correspondencia del codigo
 * va aparte del codigo.  Y es INDEPENDIENTE DEL BACKEND: un `for` se convierte
 * en inicializacion, comparacion, salto, cuerpo e incremento del mismo modo se
 * interprete, se compile al vuelo o se compile por adelantado, porque eso lo
 * decide el frontend al bajar y no quien genera el codigo.  Por eso hay UNA
 * bajada y varios mapas de codigo, y no una copia de todo por cada forma de
 * ejecutar.
 *
 * La relacion es de muchos a muchos: una sentencia produce varias instrucciones
 * y una instruccion puede servir a varias sentencias (el optimizador funde
 * cosas).  Guardar una sola referencia habria obligado a elegir cual mentir.
 */

#ifndef VXDBG_LOWERING_MAP_H
#define VXDBG_LOWERING_MAP_H

#include "vxdbg/ids.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vxdbg {

/// Version del esquema de esta capa.
static constexpr uint32_t LOWERING_SCHEMA_VERSION = 1;

/// Que le paso a una sentencia al bajarla.
enum class LoweringKind : uint8_t {
    Lowered = 0,    ///< produjo instrucciones intermedias
    Folded = 1,     ///< se resolvio al compilar (`1 + 2` -> `3`)
    Eliminated = 2, ///< el optimizador la borro (`if (false)`)
    Inlined = 3,    ///< su cuerpo se incorporo dentro de otra funcion
    /// Codigo que NO viene de ninguna sentencia escrita.  Un `defer` deja un
    /// bloque de limpieza, un `await` una maquina de estados, un `match` una
    /// tabla de salto: nadie escribio esas instrucciones y presentarlas como
    /// si vinieran de la linea de al lado despista mas que ayuda.
    Generated = 4,
};

/**
 * @brief Quien expandio la sentencia, cuando no la escribio nadie tal cual.
 *
 * `a += b` puede llegar al intermedio como una suma y una asignacion, y esa
 * transformacion la hizo el analizador, no la bajada.  Distinguirlo es lo que
 * permitira depurar macros, plantillas o azucar sintactico: sin ello, todo
 * parece haber salido del mismo sitio.
 */
enum class OriginKind : uint8_t {
    Written = 0,  ///< esta escrito asi en el fuente
    Parser = 1,   ///< lo expandio el analizador (azucar, operadores compuestos)
    Macro = 2,    ///< salio de una macro
    Template = 3, ///< salio de instanciar algo generico
    Lowering = 4, ///< lo genero la bajada al intermedio
};

/**
 * @brief Una sentencia y en que se convirtio.
 *
 * Distinguir "no produjo codigo" de "no se sabe" importa: un punto de parada
 * que nunca se alcanza porque la sentencia se resolvio al compilar no es un
 * fallo de la herramienta, y decirlo ahorra buscar donde no hay nada.
 */
struct LoweringEntry {
    StatementId statement;
    LoweringKind kind = LoweringKind::Lowered;
    std::vector<IrInstrId> ir_instrs; ///< en que se convirtio
    /// Si @ref LoweringKind::Inlined, dentro de que FUNCION intermedia acabo.
    /// El inline ocurre sobre funciones y en el intermedio, no sobre
    /// sentencias del fuente.
    IrFunctionId inlined_into;

    /// De donde salio la sentencia, si no la escribio nadie tal cual.
    OriginKind origin = OriginKind::Written;
};

/**
 * @brief La bajada de una unidad: de sus sentencias a su intermedio.
 *
 * Sirve en los dos sentidos, que son las dos preguntas que se hacen al depurar:
 * "esta instruccion, de que linea salio" al mirar un fallo, y "esta linea, en
 * que instrucciones quedo" al poner un punto de parada.
 */
struct LoweringMap {
    uint32_t schema_version = LOWERING_SCHEMA_VERSION;
    std::vector<LoweringEntry> entries;

    /**
     * @brief En que se convirtio una sentencia.
     * @param stmt Sentencia.
     * @return Su entrada, o @c nullptr si no consta.
     */
    const LoweringEntry *of_statement(StatementId stmt) const;

    /**
     * @brief De que sentencias salio una instruccion intermedia.
     * @param instr Instruccion.
     * @return Las sentencias; vacio si no consta ninguna.
     */
    std::vector<StatementId> statements_of(IrInstrId instr) const;

    /// Prepara los indices de consulta.  Se llama tras rellenar @c entries.
    void build_index();

    ContentHash compute_hash() const;

  private:
    /// Indice inverso, construido aparte y no guardado: es reconstruible, y
    /// meterlo en el dato habria hecho que el nodo dejara de ser un dato.
    std::unordered_map<IrInstrId, std::vector<StatementId>> by_instr_;
};

} // namespace vxdbg

#endif // VXDBG_LOWERING_MAP_H
