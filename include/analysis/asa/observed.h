/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/observed.h
 * @brief Lo que un PASE observa al transformar, dicho en el vocabulario del
 * ASA.
 *
 * Un pase del optimizador sabe cosas que no estan ni antes ni despues de el:
 * el desenrollador SABE cuantas vueltas da un bucle justo antes de deshacerlo,
 * y la devirtualizacion SABE el tipo justo antes de fijarlo.  Ese conocimiento
 * moria con la transformacion que lo producia.
 *
 * Aqui NO hay un vocabulario nuevo: hay UNA funcion por hecho, que construye
 * el mismo `Fact` que construye el productor del dominio.  Es la diferencia
 * entre "una base, varias fuentes" y dos formas de decir lo mismo -- con dos,
 * basta que una se quede atras para que el mismo bucle se cuente distinto
 * segun quien lo mire.
 *
 * Quien lo produce (el productor del dominio o el pase) cambia la PROCEDENCIA
 * y el MOMENTO del hecho, no lo que dice.  Los dos viajan sellados dentro.
 */

#ifndef ANALYSIS_ASA_OBSERVED_H
#define ANALYSIS_ASA_OBSERVED_H

#include "analysis/asa/fact.h"
#include "analysis/asa/fact_base.h" // los NOMBRES de dominio, que son vocabulario
#include "analysis/asa/fact_store.h"
#include "analysis/facts/bulk_memory.h"
#include "analysis/facts/loop_trip_count.h"
#include "ir/ssa_ir.h"

namespace ir {
/// Un sitio de llamada indirecta ya clasificado.  Adelantado: quien incluya
/// esto solo lo pasa por referencia, y traerse el optimizador entero haria que
/// compilar cualquier productor dependiera de el.
struct CallTargetSite;
/// Por que no se supo de quien es el codigo.  Adelantado con su tipo
/// subyacente, que es lo que permite declararlo sin la definicion.
enum class CallTargetUnknown : uint8_t;
} // namespace ir

namespace analysis {
namespace asa {

/**
 * @brief El hecho "este bucle da N vueltas" (o "como mucho N"), sin depositar.
 *
 * Devuelve el @c Fact en vez de guardarlo porque los dos que lo usan lo
 * depositan de forma distinta: el productor del dominio lo cuenta en su
 * resumen, y un pase lo mete directo en el almacen.  Lo que NO puede diferir
 * es lo que el hecho dice, y por eso se arma aqui.
 *
 * "Da N vueltas" y "da como mucho N" salen con CODIGOS distintos, no como el
 * mismo con menos confianza: con el primero se puede quitar una comprobacion,
 * con el segundo solo elegir.  Y la certeza la trae @p trip, no la pone quien
 * publica.
 *
 * @param store  Para internar el nombre de la funcion (el sujeto lo
 * referencia).
 * @param fn     Funcion a la que pertenece el bucle.
 * @param header Bloque cabecera del bucle: es el sujeto del hecho.
 * @param trip   Lo que el analisis averiguo, con su certeza dentro.
 * @param stage  En que MOMENTO vale (@see kStage*).
 * @param source De donde sale: estatico, observado al transformar, medido...
 * @param out    Donde se deja el hecho.  Solo se toca si se devuelve true.
 * @return true si habia algo que afirmar.
 *
 * Devuelve un BOOL y no un hecho "vacio" a proposito: los campos de @c Fact
 * traen `"?"` de fabrica -- ni nulos ni cadena vacia --, asi que cualquier
 * centinela que se mirara despues daria "hay hecho" siempre, y el almacen
 * acabaria lleno de vacios que ademas se cuentan.  Con un bool no hay nada
 * que acordarse de comprobar.
 */
bool loop_trip_fact(FactStore &store, const ir::IrFunction &fn,
                    ir::IrBlockId header, const LoopTripInfo &trip,
                    const char *stage, Source source, Fact &out);

/**
 * @brief El hecho "este bucle es en realidad una operacion de bloque".
 *
 * Lo afirman DOS sitios y por eso se arma aqui: el dominio, mirando el codigo,
 * y el pase que lo reduce, que lo dice justo antes de que el bucle deje de
 * existir -- despues no hay bucle que reconocer, y ese conocimiento se tiraba.
 *
 * CUANTOS elementos no siempre se sabe: la cota de un bucle suele ser un valor
 * del programa.  Cuando lo es, sale el numero; cuando no, sale un codigo
 * distinto que dice que el tramo se dimensiona al ejecutar.  Publicar el
 * identificador del valor como si fuera la cuenta -- que es lo que pasaba --
 * imprimia "1 elementos" para un tramo de longitud desconocida: no un error,
 * un numero equivocado.
 *
 * @param store  Para internar el nombre de la funcion.
 * @param fn     Funcion a la que pertenece el bucle.
 * @param b      Lo que el analisis reconocio.
 * @param stage  En que MOMENTO vale (@see kStage*).
 * @param source De donde sale.
 * @param out    Donde se deja el hecho.
 * @return true siempre que @p b describa un bucle con cabecera valida.
 */
bool bulk_memory_fact(FactStore &store, const ir::IrFunction &fn,
                      const BulkMemoryFact &b, const char *stage, Source source,
                      Fact &out);

/**
 * @brief El mismo hecho, cuando el movimiento de bloque estaba escrito EN
 *        RECTA en vez de como un bucle.
 *
 * Produce el MISMO vocabulario (@c bulk.fill y companeros) a proposito: lo que
 * el codigo hace es lo mismo, y un consumidor no deberia tener que preguntar
 * dos veces segun como estuviera escrito.  Lo que cambia es el SUJETO -- ahi
 * una cabecera de bucle, aqui un bloque -- y la longitud, que aqui siempre se
 * sabe porque las direcciones son constantes.
 *
 * @param store  Almacen, para internar los nombres.
 * @param fn     Funcion a la que pertenece el grupo.
 * @param b      El grupo reconocido.
 * @param stage  Fase en la que se dice.
 * @param source De donde sale el conocimiento.
 * @param out    Sale el hecho armado.
 * @return false si el grupo no da para un hecho.
 */
bool straight_line_bulk_fact(FactStore &store, const ir::IrFunction &fn,
                             const StraightLineBulkFact &b, const char *stage,
                             Source source, Fact &out);

/**
 * @brief DE QUIEN es el codigo al que salta una llamada indirecta.
 *
 * @note El sitio lo clasifica @c ir::ir_callind_target_memory.  Se declara
 *       adelantado y no se incluye el optimizador entero: esta cabecera la
 *       lee todo productor, y arrastrarlo aqui haria que compilar cualquiera
 *       de ellos dependiera de el.
 *
 * Armado aqui, como los demas, porque el hecho lo puede afirmar mas de un
 * sitio -- el dominio mirando el codigo, y el pase que elige como emitir la
 * llamada justo antes de emitirla -- y con dos constructores bastaria que uno
 * se quedara atras para que el mismo salto se describiera distinto segun quien
 * lo mirara.
 *
 * @param store  Almacen, para internar los nombres.
 * @param fn     Funcion donde esta la llamada.
 * @param site   Lo que el clasificador contesto de ese sitio.
 * @param stage  Momento en el que se dice.
 * @param source De donde sale el conocimiento.
 * @param out    Sale el hecho armado.
 * @return false si de ese sitio no se pudo afirmar nada -- entonces lo que
 *         corresponde es decir POR QUE, no callarse.
 */
bool code_origin_fact(FactStore &store, const ir::IrFunction &fn,
                      const ir::CallTargetSite &site, const char *stage,
                      Source source, Fact &out);

/**
 * @brief El caso exacto de un no-saber de este dominio, en vocabulario
 *        estable.
 *
 * Junto al constructor del hecho y no dentro del productor: es la MISMA
 * vocabulario -- lo que se afirma y lo que se renuncia a afirmar -- y partirlo
 * en dos ficheros es como acaban dos mitades diciendo cosas distintas.
 */
const char *code_origin_unknown_code(ir::CallTargetUnknown why);

/**
 * @brief De QUE CLASE es ese no-saber, en el vocabulario COMuN del ASA.
 *
 * Los casos NO son el mismo, y meterlos en un cajon es lo que ese vocabulario
 * existe para impedir: es lo que decide la ACCIoN, y las de estos son
 * opuestas entre si -- lo que llega por un parametro se declara, lo que sale
 * de memoria se deduce mirando quien escribe, y lo que se corto por
 * presupuesto no es culpa del programa --.
 */
UnknownReason code_origin_unknown_kind(ir::CallTargetUnknown why);

} // namespace asa
} // namespace analysis

#endif // ANALYSIS_ASA_OBSERVED_H
