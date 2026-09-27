/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file devirt_known_target.h
 * @brief Una llamada indirecta cuyo destino se sabe al compilar se hace
 *        directa, sea cual sea su forma.
 *
 * Una indirecta solo se paga donde de verdad se pide.  Si el destino se
 * conoce -- una direccion de funcion constante, venga por SSA o por memoria --
 * la llamada deja de serlo, y asi el inliner puede entrar.  Antes esto lo
 * hacia un pase que solo miraba @c CALLIND; una @c CALLCLOSURE con el destino
 * igual de conocido se quedaba indirecta, y con ella toda lambda pasada a una
 * funcion generica.
 *
 * Que una closure lleve ENTORNO no le impide ser directa: la llamada directa
 * lo sigue entregando.  Por eso la forma directa de una closure es la propia
 * @c CALLCLOSURE con su destino escrito como @c LABEL_ADDR -- los backends la
 * bajan a un salto directo y los inliners la copian --, y solo pasa a @c CALL
 * cuando el entorno ya viaja como parametro o no se usa.
 */

#ifndef IR_PASSES_DEVIRT_KNOWN_TARGET_H
#define IR_PASSES_DEVIRT_KNOWN_TARGET_H

#include "ir/pass_result.h"
#include "ir/ssa_ir.h"
#include "util/name_pool.h"
#include "util/named_alloc.h"

#include <cstddef>
#include <unordered_map>

namespace analysis {
namespace asa {
class FactBase;
} // namespace asa
} // namespace analysis

namespace ir {

struct NakedFnAddrIndex;

namespace scratch {
struct LabelOfValue; ///< Valor -> la funcion cuya direccion es.
} // namespace scratch

/**
 * @brief La funcion cuya direccion es cada valor definido por un
 *        @c LABEL_ADDR.
 *
 * Es la forma en que una llamada DIRECTA de closure escribe su destino, y la
 * preguntan los inliners para saber a quien copian.  Se construye una vez por
 * funcion; consultar es indexar.
 */
class LabelAddrIndex {
  public:
    /**
     * @brief Indexa los @c LABEL_ADDR de @p fn.
     * @param fn Funcion a indexar.
     */
    explicit LabelAddrIndex(const IrFunction &fn);

    /**
     * @brief La funcion cuya direccion es @p v.
     * @param v Valor a consultar.
     * @return Su nombre, o el vacio si @p v no es una direccion de funcion.
     */
    util::InternedName label_of(IrValueId v) const noexcept {
        return v < by_value_.size() ? by_value_[v] : util::InternedName();
    }

  private:
    util::NamedVector<util::InternedName, scratch::LabelOfValue> by_value_;
};

/**
 * @brief A que funcion llama @p call si es una @c CALLCLOSURE con el destino
 *        escrito.
 *
 * @param call   Instruccion a mirar.
 * @param labels Los @c LABEL_ADDR de su funcion.
 * @return El destino, o el vacio si no es una closure de destino conocido.
 */
util::InternedName known_closure_target(const IrInstr &call,
                                        const LabelAddrIndex &labels);

/**
 * @brief Lo que el pase necesita saber de una funcion que puede ser destino.
 *
 * Decide la FORMA de la llamada directa: si el entorno viaja como parametro
 * (la convencion nativa), si el cuerpo lo lee del registro de la closure, o
 * si no lo usa.
 */
struct CalleeShape {
    size_t params = 0; ///< cuantos parametros declara.
    VmRegReads vm_reg_reads = VmRegReads::None; ///< registros que lee.
};

/// Las funciones del modulo por su nombre internado.
using CalleeShapes =
    std::unordered_map<util::InternedName, CalleeShape, util::InternedNameHash>;

/**
 * @brief Recoge @ref CalleeShape de cada funcion del modulo.
 *
 * Una vez por pasada: el pase corre por funcion y rehacerlo dentro seria
 * recorrer el modulo entero por cada una.
 *
 * @param mod Modulo.
 * @return La tabla.
 */
CalleeShapes ir_callee_shapes(const IrModule &mod);

/**
 * @brief Hace directa toda llamada indirecta de @p fn cuyo destino se conoce.
 *
 * El destino se busca por los mismos caminos para toda forma de llamada:
 *
 *   - `LABEL_ADDR`, propagado por `MOV`;
 *   - `CALLN vrt:naked_fnaddr(proc, <hash>)`, la direccion NATIVA de una
 *     funcion plana, con el hash deshecho por @p index;
 *   - a traves de MEMORIA: un destino guardado una vez y leido de vuelta, que
 *     empareja points-to (se le pregunta a @p base).
 *
 * Antes que nada, lo que la jerarquia de clases ya DEMOSTRO al bajar
 * (@c IrInstr::proven_callee en @c CALLVIRT, @c CALLITF, @c CALLSUPER o el
 * @c CALLIND por tabla): pasa a @c CALL sin buscar nada.
 *
 * Y la reescritura por destino conocido depende de la forma:
 *
 *   - @c CALLIND -> @c CALL.
 *   - @c CALLCLOSURE -> @c CALL con el entorno como ultimo argumento si el
 *     destino lo recibe asi; @c CALL sin el si no lo usa; y si lo lee del
 *     registro, sigue siendo @c CALLCLOSURE pero con el destino escrito como
 *     @c LABEL_ADDR: una llamada directa que entrega el entorno.
 *
 * @param fn     Funcion a transformar.
 * @param index  Las funciones del modulo por su hash, para `naked_fnaddr`.
 * @param shapes La forma de cada posible destino (@ref ir_callee_shapes).
 * @param base   A quien preguntarle por los punteros de @p fn.
 * @return Si reescribio alguna llamada.
 */
PassResult ir_pass_devirt_known_target(IrFunction &fn,
                                       const NakedFnAddrIndex &index,
                                       const CalleeShapes &shapes,
                                       analysis::asa::FactBase &base);

/**
 * @brief Corre @ref ir_pass_devirt_known_target sobre cada funcion del modulo.
 *
 * Construye UNA vez lo que el pase consulta -- los nombres por hash, la forma
 * de cada destino y una base de hechos, cuya memoizacion solo sirve mientras
 * viva -- y avanza la version de cada funcion que cambie.  Lo usan el
 * prologo del inline y cada vuelta del punto fijo.
 *
 * @param mod Modulo.
 * @return Si alguna funcion cambio.
 */
bool ir_devirt_known_targets(IrModule &mod);

} // namespace ir

#endif // IR_PASSES_DEVIRT_KNOWN_TARGET_H
