/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/borrow/borrow_ir_check.h
 * @brief La exclusividad de un prestamo, comprobada CRUZANDO LA LLAMADA.
 *
 * El comprobador de prestamos del AST aplica la exclusividad por NOMBRE y
 * dentro de una funcion.  Pasar el dueno a otra que lo vuelve a prestar es
 * invisible para el, y entonces coexisten dos prestamos exclusivos vivos de la
 * misma region sin que nadie lo vea.  Ese es el agujero que esto cierra.
 *
 * @par Por que aqui y no en el del AST
 * La pregunta "estas dos regiones son la misma?" no se puede contestar mientras
 * se baja: dentro de la funcion sus parametros son dos NOMBRES, y quien sabe
 * que le llega a cada uno es quien la llama.  Hace falta el IR terminado y el
 * modulo entero, o sea justo lo que el comprobador del AST no tiene.
 *
 * @par Esto NO produce conocimiento, lo CONSUME
 * Los dos datos ya existen y cada uno tiene su productor:
 *
 *   - que la region de un parametro no la alcanza ningun otro de la misma
 *     llamada -- la promesa, que sale del tipo `borrow_mut<T>`;
 *   - que en tal llamada dos parametros SI reciben la misma memoria -- el dato,
 *     que sale de mirar los sitios de llamada.
 *
 * Juntarlos es lo que produce el veredicto, y juntar es del consumidor.  Acunar
 * un hecho "promesa incumplida" seria meter el juicio dentro del dato y
 * duplicar lo que esas dos proposiciones ya dicen.
 *
 * @par Y solo acusa con una PRUEBA
 * Un par sobre el que no se pudo decidir no es una violacion: no poder
 * demostrar que dos regiones son disjuntas NO es demostrar que se solapan. Solo
 * el veredicto demostrado -- con la llamada que lo ensena -- sale de aqui.
 */

#ifndef VX_BORROW_IR_CHECK_H
#define VX_BORROW_IR_CHECK_H

#include "analysis/asa/fact_store.h" // la base de hechos del momento mirado
#include "ir/ssa_ir.h"
#include "vx/diagnostic.h" // donde se cuenta lo que se encuentra

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ir {
struct IrModule;
}

namespace analysis {
namespace asa {
class FactBase;
}
} // namespace analysis

namespace vx {
namespace borrow {

/**
 * @brief Una promesa de exclusividad que el programa incumple, con su prueba.
 */
struct ExclusiveViolation {
    /// Funcion cuyos parametros prometian no alcanzarse.
    std::string function;
    /// El parametro que lo prometia.
    size_t promised = 0;
    /// El otro, que alcanza su misma region.
    size_t other = 0;
    /// Linea de la LLAMADA donde se ve que son la misma memoria.  Es la prueba:
    /// sin ella el aviso seria una acusacion sin nada detras.
    uint32_t line = 0;
    /// La promesa la ESCRIBIO el programador (la direccion del parametro), en
    /// vez de derivarla el compilador del tipo.  Viaja hasta aqui porque la
    /// salida no es la misma: a quien declaro `out` no se le puede decir que
    /// termine un prestamo que en su programa no existe.
    bool declared = false;
};

/**
 * @brief Busca promesas de exclusividad incumplidas cruzando las llamadas.
 *
 * @param mod  Modulo con el IR ya terminado.
 * @param base La base de hechos: de ahi sale que le llega a cada parametro.
 * @return Las violaciones DEMOSTRADAS, cada una con la llamada que la ensena.
 */
std::vector<ExclusiveViolation>
check_exclusive_across_calls(const ir::IrModule &mod,
                             analysis::asa::FactBase &base);

/**
 * @brief Lo mismo, ya DICHO: cada violacion con su prueba y su salida.
 *
 * Separado de @ref check_exclusive_across_calls porque son dos
 * responsabilidades: una AVERIGUA y la otra CUENTA.  El texto sale del catalogo
 * multi-idioma, nunca escrito aqui.
 *
 * Vivia dentro del fichero del camino de proyecto, que es de todo menos el sitio
 * de una comprobacion de prestamos.
 *
 * @param mod   Modulo con el IR ya terminado.
 * @param diags Donde se depositan.
 * @param file  Fichero al que atribuirlas.
 * @param base  La base de hechos del momento que se esta mirando.
 * @param level El peso del veredicto, que lo decide quien llama.
 */
void report_exclusive_across_calls(const ir::IrModule &mod, Diagnostics &diags,
                                   const std::string &file,
                                   analysis::asa::FactBase &base,
                                   DiagLevel level);

/**
 * @brief La exclusividad de los prestamos ANTES de optimizar, con su base.
 *
 * Es el punto por el que pasan los DOS caminos de compilacion, y existe porque
 * escrita en cada uno dio dos respuestas para el mismo programa: como fichero
 * suelto acusaba y abortaba, como proyecto callaba, porque uno miraba la opcion
 * y el otro no.
 *
 * Va antes de optimizar por dos razones que apuntan al mismo sitio.  De
 * correccion: esa promesa es lo que autoriza al optimizador a reordenar
 * accesos, asi que comprobarla despues seria comprobar si valia lo que ya se
 * uso.  Y de posibilidad: lo que demuestra el fallo es una LLAMADA que pasa las
 * dos regiones, y al inlinar esa llamada desaparece -- medido --.
 *
 * Se comprueba SIEMPRE.  La opcion decide el PESO del veredicto, no si se mira:
 * saltarsela dejaba a `--analyze` sin nada que ensenar.
 *
 * La base es PROPIA y corta: la que se usa despues mira el codigo YA
 * optimizado, y son dos codigos distintos.
 *
 * @param mod   El modulo que se va a optimizar.
 * @param file  Fichero al que atribuir lo que se diga.
 * @param violations_are_errors Si una violacion demostrada es error o aviso.
 * @param diags Donde se depositan.
 */
void check_borrows_before_opt(const ir::IrModule &mod, const std::string &file,
                              bool violations_are_errors, Diagnostics &diags);

} // namespace borrow
} // namespace vx

#endif // VX_BORROW_IR_CHECK_H
