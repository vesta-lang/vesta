/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/inline_facts.h
 * @brief Lo que hay que saber de una funcion para decidir si se inlina, SIN
 *        volver a mirarle el cuerpo.
 *
 * @par Por que existe
 * Los dos inliners -- el de un bloque y el de varios -- empezaban recorriendo
 * el cuerpo de TODAS las funciones del modulo solo para clasificarlas:
 *
 *     ok[i] = is_inlineable_mb(mod.functions[i], threshold);
 *
 * Sobre 73.501 funciones eso es recorrer el programa entero dos veces por
 * pasada, y ademas obliga a tener TODOS los cuerpos en memoria a la vez --
 * que es lo que sostiene el pico del compilador: medido, el pico es el
 * conjunto de trabajo de un modulo multiplicado por cuantos modulos hay.
 *
 * Lo que ese recorrido produce cabe en veinte y pico bytes: un punado de
 * banderas y cuatro cuentas.  Asi que se produce UNA vez, se guarda como
 * hecho, y la decision pasa a ser aritmetica sobre el hecho.
 *
 * @par Y por que la decision NO recibe la funcion
 * Porque un resumen que se puede puentear deja de ser la fuente de verdad en
 * cuanto alguien tenga prisa.  @ref inlineable_single_block y @ref
 * inlineable_multi_block reciben SOLO el hecho: el dia que un pase necesite
 * mirar algo que el hecho no lleva, **no compila**, y tiene que anadir su
 * propiedad aqui -- que es el unico sitio donde se decide la inlineabilidad.
 * Es el mismo remedio que @c PassResult: no se arregla acordandose, se
 * arregla haciendo que no se pueda escribir el codigo que se lo salta.
 *
 * @par Rancidez
 * El hecho describe una VERSION de la funcion.  Se cachea con la clave de
 * siempre -- `(name_key, version)` -- asi que cualquier mutacion lo invalida,
 * y `applied()` es lo unico que puede subir esa version.  Ademas, cuando el
 * cuerpo pueda desalojarse, la garantia se refuerza sola: no se puede mutar
 * lo que no se tiene, y traerlo de vuelta restablece el par cuerpo-hecho.
 */

#ifndef VESTA_ANALYSIS_FACTS_INLINE_FACTS_H
#define VESTA_ANALYSIS_FACTS_INLINE_FACTS_H

#include <cstddef>
#include <cstdint>

namespace ir {
struct IrFunction;
}

namespace analysis {

/**
 * @struct InlineFacts
 * @brief Todo lo que las dos decisiones de inline miran de una funcion.
 *
 * Las cuentas van aparte de las banderas a proposito: el UMBRAL es politica
 * del llamante -- el inliner de un bloque usa uno, el de varios otro, y el C2
 * los sube --, asi que no entra en el hecho.  El hecho dice lo que la funcion
 * ES; quien pregunta decide que hace con ello.
 */
struct InlineFacts {
    // --- Lo que se sabe sin mirar el cuerpo (nombre y marcas) -------------
    bool is_native = false;      ///< declarada nativa: no hay cuerpo que copiar.
    bool is_naked = false;       ///< @Naked: sin prologo, epilogo ni retorno.
    bool has_section = false;    ///< colocada en una seccion propia.
    /* DOS listas negras, y no es un descuido: hoy los dos inliners rechazan
     * familias de nombres DISTINTAS -- el de un bloque tambien aparta
     * `__spawn_`, `__async_`, `__rspawn_` y los ayudantes de cadena --, y
     * unificarlas aqui cambiaria el comportamiento sin que nadie lo pidiera.
     * Se recogen las dos tal cual son; juntarlas, si procede, es otra
     * decision y con su medida. */
    bool blacklisted_single = false; ///< la lista del inliner de UN bloque.
    bool blacklisted_multi = false;  ///< la del de VARIOS.
    /// `__ovl_resolve_*`: los rechazan LOS DOS, y por el mismo motivo, asi que
    /// va aparte de las listas.
    bool is_overlay_resolver = false;
    bool is_new_helper = false;  ///< `__new_<Clase>`: lo decide el reemplazo
                                 ///< escalar, que esta tras bandera, asi que
                                 ///< va SEPARADO de las listas.

    // --- Forma del cuerpo -------------------------------------------------
    uint32_t block_count = 0;       ///< cuantos bloques tiene.
    uint32_t instr_count = 0;       ///< instrucciones, sumando todos.
    uint32_t entry_instr_count = 0; ///< las del bloque de entrada.
    uint32_t asm_bindings = 0;      ///< cuantas ataduras de registro declara.

    bool any_block_empty = false;   ///< algun bloque sin instrucciones.
    bool all_blocks_closed = false; ///< todos acaban en BR/BR_COND/RET.
    bool has_ret = false;           ///< algun bloque acaba en RET.
    bool entry_ends_in_ret = false; ///< el de entrada acaba en RET.
    bool entry_has_phi = false;     ///< el de entrada lleva un PHI.

    // --- Lo que prohibe inlinar, y cada uno por su razon ------------------
    bool recursive = false;      ///< se llama a si misma.
    bool has_raw_asm = false;    ///< RAW_ASM: asume la convencion de la VM.
    bool has_inline_asm = false; ///< INLINE_ASM: sus ataduras son suyas.
    bool has_jump_table = false; ///< SWITCH_DENSE o destinos calculados.
    bool has_alloca = false;     ///< crece la pila del llamante en un bucle.
    bool frees_resources = false;  ///< RAW_FREE / SMARTPTR_FREE: duplicar la
                                   ///< limpieza cambia el resultado.
    bool has_frame_op = false;     ///< marco de excepcion, GC, reflexion o
                                   ///< despacho dinamico.
    bool calls_new_helper = false; ///< construye un objeto via `__new_X`.
    bool makes_closure = false;    ///< MAKE_CLOSURE: es una factoria, y por eso
                                   ///< el llamante le da mas holgura.
    /// El ensamblador en linea sale por su cuenta (`ret`, `iret`, `sysret`...).
    /// Inlinar eso mete un retorno en mitad del llamante.
    bool asm_returns_manually = false;
};

/**
 * @brief Marcador del analisis (identidad para el AnalysisManager).
 */
struct InlineAnalysis {
    static char ID;
    /// Como se llama al medirlo.  Lo exige la puerta del gestor.
    static constexpr const char *kName = "inline";
};

/**
 * @par Cachearlo en el gestor NO sale a cuenta, y esta medido
 * Se probo lo que esta cabecera sugiere -- pedirselo al @c AnalysisManager con
 * la clave `(name_key, version)` en vez de calcularlo -- y cuesta MAS: los dos
 * inliners clasifican de una tirada y EN SERIE, o sea ~24.000 consultas por
 * pasada a ~1 us cada una entre el cerrojo y la tabla, mientras que calcular
 * este hecho sobre una funcion de once instrucciones no cuesta casi nada.
 * Medido dentro del proceso, que es donde no hay ruido de reloj:
 * `x-mod:inline` pasaba de 30,6 a 54,0 ms y volvia a 32 al quitarlo.
 *
 * Lo que se buscaba NO era tiempo: era clasificar SIN MIRAR CUERPOS, que es lo
 * que permitiria que un cuerpo pueda no estar en memoria.  Eso sigue haciendo
 * falta, pero con el hecho viviendo AL LADO de la funcion y no detras de un
 * cerrojo.
 */

/**
 * @brief Mira @p fn UNA vez y resume lo que los inliners necesitan.
 * @param fn Funcion a resumir.
 * @return El hecho.  Describe la version de @p fn que se le paso.
 */
InlineFacts compute_inline_facts(const ir::IrFunction &fn);

/**
 * @brief Se puede inlinar con el inliner de UN bloque?
 *
 * @param f         Lo que se sabe de la funcion.
 * @param threshold Cuantas instrucciones se aceptan.  Politica del llamante.
 * @param escape_scalar_on Si el reemplazo escalar esta activo.  Cuando lo
 *        esta, un `__new_X` se deja como llamada a proposito: el pase elimina
 *        la reserva de los objetos que no escapan y los que escapan se quedan
 *        con una llamada barata a un ayudante trivial.
 * @return true si se puede.
 */
bool inlineable_single_block(const InlineFacts &f, size_t threshold,
                             bool escape_scalar_on);

/**
 * @brief Se puede inlinar con el inliner de VARIOS bloques?
 *
 * Cubre el hueco del otro: un solo bloque de 13 en adelante (metodos puros
 * que el de un bloque rechaza por tamano) y cualquier multi-bloque que quepa
 * en @p threshold.
 *
 * @param f         Lo que se sabe de la funcion.
 * @param threshold Cuantas instrucciones se aceptan, en total.
 * @return true si se puede.
 */
bool inlineable_multi_block(const InlineFacts &f, size_t threshold);

} // namespace analysis

#endif // VESTA_ANALYSIS_FACTS_INLINE_FACTS_H
