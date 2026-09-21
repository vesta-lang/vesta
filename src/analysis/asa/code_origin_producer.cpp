/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/code_origin_producer.cpp
 * @brief El dominio `asa.code_origin`: de QUIEN es el codigo al que salta una
 *        llamada -- nuestro o de fuera.
 *
 * QUE GANA CON ESTAR AQUI, que es la razon de que sea un dominio y no la
 * respuesta privada del pase que elige como emitir una llamada:
 *
 *   - **El perfilado** puede atribuir tiempo a codigo nuestro o ajeno sin
 *     deducirlo por rangos de direccion en ejecucion -- que es adivinar, y
 *     ademas cuesta en el camino caliente.
 *   - **La depuracion** puede decir de quien es un marco de la traza.
 *   - **El optimizador** sabe a quien NO puede mirarle el cuerpo: a codigo
 *     ajeno no se le inlina ni se le derivan efectos, y hoy eso lo
 *     redescubre cada consumidor por su cuenta.
 *
 * Y ES BARATO Y EXACTO porque Vesta es un mundo SEMI-CERRADO: todos los
 * modulos son nuestros y lo ajeno entra por un unico sitio, la frontera FFI.
 * En otros lenguajes habria que suponer; aqui la lista de lo nuestro existe y
 * es completa.
 *
 * SELLADO `isa = velb`, y no por prudencia: en nativo la distincion NO
 * SIGNIFICA NADA.  Alli todo es codigo real y las dos formas de llamar caen en
 * la misma instruccion -- lo hace ya @c aot_lower, que convierte la via nativa
 * de vuelta en la indirecta --.  Publicarlo sin sello seria afirmar en un
 * objetivo algo que solo es cierto en otro.
 *
 * LO QUE NO ES.  No dice "esta direccion tiene que ser real": eso es la
 * CONCLUSIoN de un consumidor, que sale de cruzar esto con el escape.  El
 * hecho es la procedencia, que es lo que sirve a los tres.
 */

#include "analysis/asa/observed.h" // el hecho, armado en UN sitio
#include "analysis/asa/producers.h"
#include "ir/ir_optimizer.h"
#include "ir/ssa_ir.h"

namespace analysis {
namespace asa {

namespace {

/// El sujeto de una renuncia: el mismo VALOR del que se habria hablado.  Que
/// coincida no es cosmetico -- es lo que permite preguntar por ese valor y
/// recibir el "no se" en vez de nada.
Subject target_subject(Production &p, const ir::IrFunction &fn,
                       ir::IrValueId target) {
    Subject s;
    s.kind = Subject::Kind::Value;
    s.function = p.store.intern(fn.name);
    s.id = target;
    return s;
}

/// Solo vale donde la distincion existe: la maquina.  En nativo hay una sola
/// clase de codigo y las dos formas de llamar caen en la misma instruccion.
Scope vm_only(const char *stage) {
    Scope sc;
    sc.isa = kIsaVelb;
    sc.stage = stage;
    return sc;
}

void produce_code_origin(Production &p) {
    for (const ir::IrFunction &fn : p.mod.functions) {
        if (!p.is_interesting(fn)) continue;
        /* Por la BASE: de que memoria es el destino ya tiene productor y
         * cache, asi que aqui se pregunta en vez de rehacerlo. */
        for (const ir::CallTargetSite &site :
             ir::ir_callind_target_memory(fn, p.base)) {
            Fact f;
            if (code_origin_fact(p.store, fn, site, p.stage, Source::Static,
                                 f)) {
                p.assert_fact(std::move(f));
                continue;
            }
            /* NUNCA callado, y con el motivo: "aqui no hay ninguna llamada
             * indirecta" y "hay una y no se de quien es" no se pueden leer
             * igual.  Y el motivo separa casos que se arreglan de forma
             * OPUESTA -- lo que llega por un parametro lo sabe quien lo pasa,
             * lo que sale de memoria lo sabe quien escribio ahi --. */
            p.say_unknown(target_subject(p, fn, site.target),
                          code_origin_unknown_kind(site.why),
                          code_origin_unknown_code(site.why),
                          kProducerCodeOrigin, "", vm_only(p.stage));
        }
    }
}

} // namespace

void register_code_origin_producer() {
    /* Lee el CoDIGO de cada funcion, y nada mas: de donde viene una direccion
     * se sigue por las definiciones dentro de la propia funcion.  Lo que no
     * alcanza asi se contesta "no se" con su motivo, en vez de mirar el grafo
     * de llamadas -- que prohibiria la clave por funcion y haria caducar esto
     * cada vez que cambiara cualquier otra. */
    register_producer(kProducerCodeOrigin, &produce_code_origin,
                      DomainInput::FunctionCode);
}

} // namespace asa
} // namespace analysis
