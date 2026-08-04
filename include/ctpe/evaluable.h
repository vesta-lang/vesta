/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file evaluable.h
 * @brief CTPE -- que funciones son EVALUABLES en tiempo de compilacion.
 *
 * CTPE = Compile-Time Program Execution: ejecutar PROGRAMAS ENTEROS durante la
 * compilacion con el MISMO motor JIT del runtime (el ComptimeRuntime) e inyectar
 * el resultado.  No es "constant folding": una funcion con bucles, recursion,
 * miles de instrucciones, POO y GC se ejecuta entera si sus entradas son
 * constantes (p.ej. `main()` sin params, o una llamada con args constantes).
 *
 * ============================================================================
 *  PRINCIPIO RECTOR (paradigma del CTPE):
 *
 *    NO se clasifica por "PUREZA", sino por AISLAMIENTO del universo temporal
 *    del ComptimeRuntime.
 *
 *  El modo CTPE ejecuta el programa en una VM de compilacion que se DESTRUYE al
 *  terminar.  Puede mutar globals, el heap, ejecutar el GC, crear y modificar
 *  miles de objetos/arrays/strings/caches/singletons -- da igual: ese mundo es
 *  un universo aislado que desaparece al finalizar la evaluacion.  Una funcion
 *  NO tiene que ser pura; solo tiene que quedar CONTENIDA.  Lo unico que la
 *  descalifica es que un efecto ESCAPE de ese universo (I/O externo, entorno,
 *  FFI, distribucion remota) o que cambie la ESTRUCTURA del programa
 *  (metaprogramacion).  Esto encaja con la filosofia de Vesta mucho mejor que
 *  imponer una nocion clasica de pureza funcional.
 * ============================================================================
 *
 * Cada opcode tiene una POLITICA (@c CtpePolicy).  El modo CTPE acepta las que
 * quedan contenidas (@c Always / @c Snapshot) y rechaza @c NeedsHost /
 * @c ExternalIO / @c MetaMutation.
 *
 * Modelo de ejecucion EXECUTE-AND-TRAP: el modo CTPE corre en el ComptimeRuntime
 * con capacidades DENEGADAS (sandbox) + presupuesto (tiempo/instr/heap); si la
 * ejecucion real toca una op no-contenida, aborta limpio -> fallback (o error si
 * el CTPE era requerido).  Este analisis es un PRE-FILTRO ligero (descarta antes
 * de ejecutar + da la razon para el diagnostico); el dispatch dinamico
 * (CALLVIRT/CALLIND/CALLCLOSURE) NO se comprueba aqui: su destino real lo
 * verifica el trap.  El sandbox y el presupuesto son EXCLUSIVOS del modo CTPE:
 * las `comptime`/`@Macro` del lenguaje corren sin restriccion (responsabilidad
 * del programador), reusando el mismo ComptimeRuntime.
 */
#ifndef CTPE_EVALUABLE_H
#define CTPE_EVALUABLE_H

#include "ir/ssa_ir.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ctpe {

/**
 * @brief Politica CTPE de un opcode: DONDE quedan sus efectos.
 *
 * Se clasifica por AISLAMIENTO, no por pureza.  Un op que muta estado global o
 * el heap comptime es evaluable porque ese estado vive y muere en la VM de
 * compilacion.
 */
enum class CtpePolicy {
    Always,       ///< 🟢 efectos contenidos en el ComptimeRuntime -> evaluable
                  ///< siempre (aritmetica, memoria/alloc local, GC, POO, strings,
                  ///< reflexion de CONSULTA, excepciones, atomics sobre memoria
                  ///< privada, async local dentro del scheduler CTPE...).
    Snapshot,     ///< 🟡 muta estado global del comptime (SETSTATIC); al terminar
                  ///< se DESCARTA (o, fase 2, se serializa a .rodata).  Contenido
                  ///< -> evaluable.
    NeedsHost,    ///< 🔴 depende del ENTORNO/host: getpid, getarg(c), getproc/vm/
                  ///< mgr, read_vm_reg, distribucion remota (rspawn/shared).  NO
                  ///< evaluable.
    ExternalIO,   ///< 🔴 I/O externo / FFI nativa / carga dinamica / asm opaco:
                  ///< calln, dlopen, dlsym, loadmod, raw_asm/inline_asm.  NO
                  ///< evaluable.
    MetaMutation, ///< ⚫ RESERVADA.  Cambiar la estructura del programa dentro de
                  ///< la VM comptime es CONTENIDO (el registry se descarta) ->
                  ///< defclass/defmethod/... son @c Always, no esto.  Esta
                  ///< categoria queda para el dia que CTPE emita ESTRUCTURA al
                  ///< binario (fase de serializacion): ahi si seria metaprog real.
                  ///< Hoy NINGUN op mapea aqui.
};

/// @return politica CTPE del opcode @p op.
CtpePolicy ctpe_policy(ir::IrOp op);

/// @return true si el op queda CONTENIDO (Always/Snapshot) -> aceptable en CTPE.
inline bool op_contained(ir::IrOp op) {
    CtpePolicy p = ctpe_policy(op);
    return p == CtpePolicy::Always || p == CtpePolicy::Snapshot;
}

/// Por que una funcion NO es evaluable (para el diagnostico VX3xxx).
struct BlockReason {
    ir::IrOp op = ir::IrOp::NOP;    ///< op no-contenida que la bloquea.
    CtpePolicy policy = CtpePolicy::Always; ///< su categoria (NeedsHost/...).
    ir::IrBlockId block = ir::IR_NO_BLOCK;  ///< bloque donde aparece.
    size_t instr_index = 0;        ///< indice de la instr en el bloque.
    uint32_t source_line = 0;      ///< linea fuente (si se conoce).
    std::string callee;            ///< si bloquea por llamar a una no-evaluable.
};

/// Resultado del analisis de evaluabilidad CTPE del modulo.
struct Evaluability {
    /// Funciones que PASAN el pre-filtro (candidatas a ejecutar en CTPE).
    std::unordered_set<std::string> evaluable;
    /// Para las NO evaluables: la primera razon (op no-contenida + localizacion).
    std::unordered_map<std::string, BlockReason> reason;

    bool is_evaluable(const std::string &fn) const {
        return evaluable.count(fn) != 0;
    }
};

/**
 * @brief Calcula que funciones del modulo pasan el pre-filtro de evaluabilidad.
 *
 * Punto fijo: una funcion es evaluable si (a) no es nativa, (b) todas sus ops
 * quedan CONTENIDAS (@c op_contained), y (c) toda CALL/TAILCALL DIRECTA (con
 * @c func_name resoluble) es a otra funcion evaluable.  El dispatch dinamico no
 * se comprueba (lo cubre el trap del sandbox).  Para las no evaluables se guarda
 * la primera razon.
 */
Evaluability compute_evaluability(const ir::IrModule &mod);

/// Un candidato de PRECOMPUTO: una funcion cuyas ENTRADAS son constantes y cuyo
/// resultado es un escalar inyectable como CONST.
struct Candidate {
    std::string fn;        ///< funcion a ejecutar entera en compile-time.
    ir::IrType ret_type;   ///< tipo escalar del retorno (foldable a CONST).
};

/**
 * @brief Enumera los candidatos de precomputo del modulo.
 *
 * V1: funciones EVALUABLES con CERO parametros (entradas trivialmente constantes,
 * p.ej. `main()`, generadores) y retorno ESCALAR (int/float/bool).  Ejecutar la
 * funcion entera en el ComptimeRuntime da un CONST que reemplaza su cuerpo por
 * `return CONST`.  (Futuro: `CALL(fn_evaluable, args_const)` -> fold del sitio.)
 */
std::vector<Candidate> find_candidates(const ir::IrModule &mod,
                                       const Evaluability &ev);

} // namespace ctpe

#endif // CTPE_EVALUABLE_H
