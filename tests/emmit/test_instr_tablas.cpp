/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/emmit/test_instr_tablas.cpp
 * @brief Que las tres listas de instrucciones digan lo mismo.
 *
 * El mnemonico de una instruccion vive en tres sitios: la lista unica
 * (@c emmit/instr_list.h), la tabla del PARSER (que decide que se puede
 * escribir en un `.vel`) y la del CODIFICADOR (que decide que se puede
 * convertir a bytes).  Las tres tienen que coincidir, y cuando no coinciden el
 * fallo no se parece a su causa:
 *
 *   - algo que el codificador tiene y el parser no, es INALCANZABLE: parece
 *     disponible y no hay forma de escribirlo.  No da error en ningun sitio.
 *   - algo que el parser acepta y el codificador no tiene, se lee bien y
 *     revienta al emitir, lejos de donde se escribio.
 *
 * Ya paso una vez -- por eso existe la lista unica --, asi que esto lo mide en
 * vez de confiar.
 */

#include "emmit/mnemonic.h"
#include "emmit/parser_to_bytecode.h"
#include "parser/parser.h"

#include <cstdio>
#include <set>
#include <string>

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("FALLO [%s:%d]: %s\n", __FILE__, __LINE__, msg);       \
        }                                                                      \
    } while (0)

/// Los mnemonicos de la lista unica.
static std::set<std::string> de_la_lista() {
    std::set<std::string> s;
#define VX_INSTR(id, text, cat) s.insert(text);
#include "emmit/instr_list.h"
#undef VX_INSTR
    return s;
}

/**
 * @brief Las que el parser sabe leer.
 *
 * DIRECTIVAS del ensamblador que el parser trata como si fueran instrucciones:
 * no tienen -- ni deben tener -- entrada en el codificador, porque no producen
 * bytes.  Se descuentan aqui para que la comparacion hable solo de
 * instrucciones.
 */
static bool es_directiva(const std::string &m) {
    return m == "align" || m == "org" || m == "resbp" || m == "import";
}

/**
 * @brief Instrucciones que el codificador tiene y el parser NO reconoce.
 *
 * Son LEGACY y estan asi a proposito, no por descuido.  Se quedan fuera de la
 * comparacion para que este test hable de divergencias NUEVAS; si alguna
 * volviera a ser interesante, se quita de aqui y el test dira que hay que
 * anadirla al parser.
 *
 * Estado de cada una, para cuando haya que decidir:
 *
 *   edm, edmw4, edmw6  direccionamiento NONE y sin funcion de emision: solo
 *                      codifican su opcode, asi que funcionarian tal cual.
 *   loop, callnr       sin funcion de emision: hoy imprimirian "no esta
 *                      implementada" aunque el parser las aceptara.
 *   jmpr               la UNICA con valor real, y esta ENTERA: tabla de
 *                      decodificacion, `exec_instr_jmpr` que escribe RIP desde
 *                      un registro, y emision (emit_pop_push).  Le falta solo
 *                      el parser.  No tiene reemplazo: `callvmr` empuja
 *                      direccion de retorno, `tailcall` salta por registro pero
 *                      se come el marco, y `jumptable` necesita tabla e indice
 *                      acotado.  Un "salta a este registro conservando el
 *                      marco" no existe hoy.
 */
static bool es_legacy(const std::string &m) {
    return m == "callnr" || m == "edm" || m == "edmw4" || m == "edmw6" ||
           m == "jmpr" || m == "loop";
}

int main() {
    const std::set<std::string> lista = de_la_lista();

    std::set<std::string> parser;
    for (const auto &e : vm::instruction_set_names())
        if (!es_directiva(e)) parser.insert(e);

    std::set<std::string> codificador;
    for (const auto &e : Assembly::Bytecode::instr_table_names())
        if (!es_legacy(e)) codificador.insert(e);

    std::printf("lista=%zu  parser=%zu  codificador=%zu\n", lista.size(),
                parser.size(), codificador.size());

    // 1) Nada que se pueda codificar debe ser imposible de escribir.
    for (const auto &m : codificador) {
        const bool escribible = parser.count(m) != 0;
        if (!escribible)
            std::printf("  INALCANZABLE: '%s' esta en el codificador y el "
                        "parser no la conoce\n",
                        m.c_str());
        CHECK(escribible,
              "toda instruccion codificable tiene que ser escribible");
    }

    // 2) Nada que se pueda escribir debe morir al codificar.
    for (const auto &m : parser) {
        const bool codificable = codificador.count(m) != 0;
        if (!codificable)
            std::printf("  SIN CODIFICAR: el parser acepta '%s' y el "
                        "codificador no la tiene\n",
                        m.c_str());
        CHECK(codificable, "toda instruccion escribible tiene que codificarse");
    }

    // 3) La lista unica es la fuente: tiene que cubrir a las dos.
    for (const auto &m : codificador)
        CHECK(lista.count(m) != 0,
              "el codificador usa algo que no esta en la lista");
    for (const auto &m : parser)
        CHECK(lista.count(m) != 0,
              "el parser acepta algo que no esta en la lista");

    std::printf("=== tablas de instrucciones: %d checks, %d fallos ===\n",
                g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
