/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/memory/memory_access.h
 * @brief Vocabulario UNICO de "acceso a memoria de una instruccion IR": que lee
 *        y que escribe, y a que localizacion (AbstractLoc, via el resolvedor
 *        points-to compartido).  Es la UNA SOLA VERDAD que consumen TODOS los
 *        pases sensibles a memoria (DSE, LICM, scheduler semantico,
 *        EffectAnalysis, ...): ninguno reimplementa el switch LOAD/STORE/
 *        GETFIELD/ARRAY_x/MEMCPY ni su propio access_bytes.  Asi todos hablan
 *        el mismo idioma y una op nueva se modela en UN sitio.
 *
 * REGLA 1 del framework (base de hechos compartida): un pase CONSUME este
 * vocabulario + la tabla points-to (que RECIBE), nunca reimplementa el switch
 * ni reconstruye la base de hechos.  Un analisis nuevo aporta conocimiento
 * DISTINTO sobre la MISMA base; no reemplaza a un especializado.  Ver el bloque
 * de REGLAS RECTORAS en src/ir/ir_optimizer.cpp (ir_optimize).
 */
#ifndef ANALYSIS_MEMORY_MEMORY_ACCESS_H
#define ANALYSIS_MEMORY_MEMORY_ACCESS_H

#include "analysis/effects/effects.h" // AbstractLoc
#include "analysis/memory/points_to.h"

#include <cstdint>

namespace ir {
// El opcode, para poder preguntar por la CLASE de acceso sin arrastrar el IR
// entero a cada consumidor de esta cabecera.
enum class IrOp : uint16_t;
} // namespace ir
#include <vector>

namespace ir {
struct IrInstr;
enum class IrType : uint8_t; // fwd; el valor real viene de ssa_ir.h
} // namespace ir

namespace analysis {

/// Bytes accedidos por un acceso ESCALAR segun su IrType (1/2/4/8; F32=4,
/// F64/PTR/HANDLE... default 8).  UNA sola definicion para todo el compilador
/// (antes duplicada en DSE/LICM/effects/scheduler).  Los tipos IR son escalares
/// (max 8 B); los anchos VECTORIALES (16/32/64) NO vienen del IrType sino del
/// `imm` de los ops VEC_* -> usar @c memory_access_size_bytes para validarlos.
int32_t memory_access_size(ir::IrType t);

/// Valida un ancho de acceso en BYTES CRUDOS y lo devuelve si es un tamano de
/// acceso legitimo -- ESCALAR (1/2/4/8) o VECTORIAL (16/32/64: XMM/YMM/ZMM).
/// 0 si el ancho es desconocido/invalido -> el consumidor lo trata como rango
/// NO exacto (aliasa conservador; NUNCA sub-estimar un vector de 32 B como 8).
/// Es la verdad de bytes compartida con el nivel maquina (mem_size_bytes).
int32_t memory_access_size_bytes(int32_t raw);

/// Acceso a memoria de UNA instruccion: que lee, que escribe, y donde.  Un
/// acceso puede tener VARIAS lecturas/escrituras (una op vectorial VEC_BINOP
/// lee 2 punteros y escribe 1; VEC_FMA lee 3), por eso @c reads / @c writes son
/// LISTAS de localizaciones -- ninguna se sub-representa.
/// - LOAD/GETFIELD/ARRAY_LOAD/ARRAY_LEN -> is_load, reads=[loc].
/// - STORE/SETFIELD/ARRAY_STORE         -> is_store, writes=[loc].
/// - MEMCPY   -> reads=[src], writes=[dst] (NO opaco: footprint conocido).
/// - VEC_UNOP/BINOP/BINOP_S/FMA (data)  -> reads/writes de sus punteros con el
///   ancho VECTORIAL real (16/32/64 de imm&0xFF).  VEC_BCAST -> touches=false
///   (broadcast escalar->registro, no toca memoria).
/// - Ops que tocan memoria de forma NO localizable (VEC_ACC_* con offsets de
///   acumulador, str-ops alloc-side, gc, atomicos, raw_asm) -> opaque=true.
/// - Calls y el resto -> touches=false (no es un acceso a memoria localizable;
///   el efecto de un call lo da EffectAnalysis, no este vocabulario).
struct MemoryAccess {
    bool touches = false; ///< accede a memoria (localizable u opaca)
    bool is_load = false;
    bool is_store = false;
    bool opaque = false; ///< toca memoria pero NO se puede localizar (top)
    std::vector<effects::AbstractLoc> reads;  ///< localizaciones leidas
    std::vector<effects::AbstractLoc> writes; ///< localizaciones escritas
};

/// Calcula el @c MemoryAccess de @p ins usando la tabla points-to @p pt.  El
/// consumidor NO construye pt: lo recibe (Regla 1 -- base de hechos
/// compartida).
MemoryAccess memory_access(const ir::IrInstr &ins, const PointsTo &pt);

/**
 * @brief QUE CLASE de acceso hace una op EN EL IR, sin mirar a donde apunta.
 *
 * Es la misma clasificacion que @c memory_access, quitandole las
 * localizaciones: responde "lee", "escribe", "no toca" a partir del opcode
 * solo.  Existe porque media docena de sitios necesitaban justo eso -- si una
 * op lee o escribe memoria -- y no tenian a mano ni la instruccion completa ni
 * la tabla points-to, asi que cada uno se escribio su propia lista.  Y las
 * listas divergian: dos @c is_store_like con contenidos distintos, y un
 * @c is_pure_op que daba por PURAS ops vectoriales que este mismo vocabulario
 * dice que escriben memoria.
 *
 * EN EL IR, y eso no es una coletilla: los accesos del CODIGO EMITIDO no son
 * los de la op del IR, y cambian por objetivo.  Una @c VEC_BINOP en el
 * interprete pasa por un scratch en la pila de la VM (@c movh +
 * @c fload / @c fstore sobre el banco ancho); en el JIT y en el AOT es un
 * @c MOVUPD directo sin ese intermediario, y cual de los dos se emite depende
 * ademas del conjunto de instrucciones del objetivo.  Lo que este vocabulario
 * describe es la memoria DEL PROGRAMA -- la que el codigo fuente puede
 * observar --, no la que cada backend use para llegar ahi.  Quien razone sobre
 * el codigo final (coste, barreras del emisor, presion de registros) necesita
 * ademas la capa por backend; ver @c aplicar_backend en ir_effects.cpp.
 *
 * Quien tenga la instruccion y el points-to debe seguir usando
 * @c memory_access: esta version no dice DONDE, y no saberlo es justo lo que
 * separa una barrera conservadora de un analisis util.
 */
struct MemoryAccessKind {
    bool touches = false;
    bool is_load = false;
    bool is_store = false;
};

/// @brief Clase de acceso de @p op.  Ver @c MemoryAccessKind.
MemoryAccessKind memory_access_kind(ir::IrOp op);

} // namespace analysis

#endif // ANALYSIS_MEMORY_MEMORY_ACCESS_H
