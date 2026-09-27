/**
 * @file module_checks.h
 * @brief Lo que se comprueba sobre el intermedio de ANTES de optimizar, en UN
 *        solo sitio.
 *
 * Existe porque estaba repartido y DIVERGIO en silencio.  Hay dos caminos de
 * compilacion -- el de un fichero suelto y el de un proyecto con dependencias
 * --, y las comprobaciones vivian escritas dentro de cada uno.  El de proyecto,
 * que es el que toma TODO programa real (`vx_source_needs_project` devuelve
 * cierto en cuanto la stdlib declara un auto-importable, o sea siempre), se
 * quedo sin tres de ellas.  Nada fallo: dejaron de ejecutarse.
 *
 * La que se noto fue la del desbordamiento entero, porque tenia un test
 * negativo que se puso rojo.  Las otras no tenian quien las mirara.
 *
 * De ahi la regla que este fichero hace cumplir: **una comprobacion nueva se
 * anade AQUI**, y los dos caminos la toman por construccion.  Escribirla en uno
 * de los dos es volver a la situacion que esto arregla.
 *
 * PENDIENTE, y no es un olvido: los contratos de TIPO (`@pod`/`@no_heap`/
 * `@size`) no estan aqui todavia porque su huella sale de los layouts del
 * comprobador de tipos, y en una compilacion de proyecto hay UNO POR MODULO.
 * De donde salen esos layouts al fusionar es una decision pendiente; meterla a
 * ciegas seria afirmar sobre unos layouts y callar sobre los demas.
 */
#ifndef VESTA_VX_MODULE_CHECKS_H
#define VESTA_VX_MODULE_CHECKS_H

#include "analyze/fingerprint.h"
#include "ir/ssa_ir.h"
#include "vx/ast.h"
#include "vx/diagnostic.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vx {

/// Lo que hace falta para comprobar un modulo antes de optimizarlo.
struct PreOptInput {
    /// El intermedio tal como lo dejo el bajado; fusionado, si hay varios
    /// modulos.  Se mira ESTE y no el optimizado a proposito: es lo que el
    /// usuario escribio, y asi ninguna regla depende del nivel de optimizacion
    /// -- si dependiera, el mismo programa seria valido o no segun como se
    /// compile.
    const ir::IrModule *module = nullptr;
    /// Fichero al que atribuir lo que se diga.
    const std::string *file = nullptr;
    /// Los contratos de huella ya recogidos.  Los trae quien llama porque cada
    /// camino los junta a su modo: el suelto de su unico AST, el de proyecto
    /// acumulando los de cada modulo.
    const analyze::FunctionContractMap *contracts = nullptr;
    /// Los contratos de TIPO (`@pod`/`@no_heap`/`@size`) y la huella de cada
    /// tipo.  Cada modulo los calcula al compilarse, con su comprobador, y se
    /// unen (`gather_unit_results`).  Nulos = no se comprueba ninguno.
    const analyze::TypeContractMap *type_contracts = nullptr;
    const analyze::TypeFingerprints *type_fingerprints = nullptr;
    /// Cierto en `--analyze`: ahi se MIDE y se ensena, no se rechaza.  Un
    /// incumplimiento se muestra aparte; emitir el error marcaria fallo justo
    /// en el objetivo que hay que ensenar para corregirlo.
    bool measure_only = false;
};

/**
 * @brief Corre TODAS las comprobaciones previas a optimizar.
 *
 * Deja los diagnosticos puestos con su codigo del catalogo.  Lo que comprueba,
 * en orden, y por que ese orden:
 *
 *  1. Nativas declaradas dos veces con efectos distintos.  Va primero y fuera
 *     de cualquier condicion: el choque existe aunque el programa no escriba un
 *     solo contrato, y explica por que uno puede no cumplirse -- el modulo ya
 *     se quedo con la union de lo peor de las dos.
 *  2. Los contratos de huella, por la puerta de los tres veredictos.
 *  3. Los contratos de TIPO, si quien llama pudo aportar los layouts.
 *  4. La cuenta entera que se sale de su tipo.
 *
 * @param in    Lo que hay que mirar.
 * @param diags Donde se depositan los diagnosticos.
 * @return @c false si el modulo debe RECHAZARSE.
 */
bool run_pre_opt_checks(const PreOptInput &in, Diagnostics &diags);

/* Leer del AST lo que el programa DECLARA es otra responsabilidad, y vive en
 * `vx/contracts_collect.h`.  Aqui solo se decide QUE se comprueba, en que orden
 * y que hacer con cada veredicto. */

} // namespace vx

#endif // VESTA_VX_MODULE_CHECKS_H
