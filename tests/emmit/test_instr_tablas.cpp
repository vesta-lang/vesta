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
#include <map>
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
 * @brief DIRECTIVAS que el parser trata como si fueran instrucciones.
 *
 * No tienen -- ni deben tener -- entrada en el codificador, porque no se
 * convierten en un opcode.  Eso las exime de las comprobaciones que hablan del
 * CODIFICADOR, y de nada mas.
 *
 * Estaban exentas tambien de la tercera -- la que exige que la lista unica
 * cubra todo lo que el parser acepta --, y ahi si tenian que entrar: la lista
 * es de donde sale el indice con el que el parser reconoce un nombre, asi que
 * un nombre que no este en ella no se reconoce, sea directiva o no.  Las cuatro
 * faltaban, y cada `align 16` de cada fichero se iba por la ruta de
 * recuperacion de erratas avisando de una "instruccion desconocida" que existia
 * -- una ruta que ademas REESCRIBE el nombre por el mas parecido --.  La
 * exencion de mas es lo que hizo que este test no lo viera.
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
 *
 * `jmpr` estuvo aqui y YA NO: era la unica con valor real -- salta a la
 * direccion de un registro CONSERVANDO el marco, que no lo hace ninguna otra --
 * y estaba entera salvo la entrada del parser.  Se anadio.
 */
static bool es_legacy(const std::string &m) {
    return m == "callnr" || m == "edm" || m == "edmw4" || m == "edmw6" ||
           m == "loop";
}

int main() {
    const std::set<std::string> lista = de_la_lista();

    // Todo lo que el parser acepta, directivas incluidas: se descuentan mas
    // abajo y solo donde toca.
    std::set<std::string> parser;
    for (const auto &e : vm::instruction_set_names())
        parser.insert(e);

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

    // 2) Nada que se pueda escribir debe morir al codificar.  Una directiva no
    // llega al codificador por definicion, asi que no entra aqui.
    for (const auto &m : parser) {
        if (es_directiva(m)) continue;
        const bool codificable = codificador.count(m) != 0;
        if (!codificable)
            std::printf("  SIN CODIFICAR: el parser acepta '%s' y el "
                        "codificador no la tiene\n",
                        m.c_str());
        CHECK(codificable, "toda instruccion escribible tiene que codificarse");
    }

    // 3) La lista unica es la fuente: tiene que cubrir a las dos.  Aqui SI
    // entran las directivas: la lista es de donde sale el indice que reconoce
    // los nombres, y lo que no este en ella no se reconoce.
    for (const auto &m : codificador)
        CHECK(lista.count(m) != 0,
              "el codificador usa algo que no esta en la lista");
    for (const auto &m : parser) {
        if (lista.count(m) == 0)
            std::printf("  FUERA DE LA LISTA: el parser acepta '%s' y la lista "
                        "unica no lo tiene\n",
                        m.c_str());
        CHECK(lista.count(m) != 0,
              "el parser acepta algo que no esta en la lista");
    }

    /* 4) Y dicho de la forma en que se rompio: el indice que el parser usa para
     * reconocer un nombre se construye desde la lista, y sabe decir que nombres
     * de la tabla no encontro.  Preguntarselo es la comprobacion directa; las
     * de arriba lo deducen. */
    {
        // El indice se construye sobre los NOMBRES, que es de lo que habla la
        // comprobacion; el valor asociado da igual.
        std::map<std::string, int> tabla;
        for (const auto &e : vm::instruction_set_names())
            tabla.emplace(e, 0);
        const emmit::MnemonicIndex<int> idx(tabla);
        for (const auto &n : idx.unknown_names())
            std::printf("  SIN INDICE: '%s' esta en la tabla del parser y el "
                        "indice no lo resuelve\n",
                        n.c_str());
        CHECK(idx.unknown_names().empty(),
              "el indice del parser resuelve todos los nombres de su tabla");
    }

    std::printf("=== tablas de instrucciones: %d checks, %d fallos ===\n",
                g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
