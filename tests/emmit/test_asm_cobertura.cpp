/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/emmit/test_asm_cobertura.cpp
 * @brief De TODAS las instrucciones que la base de datos conoce, cuales tienen
 *        efectos declarados y cuales no.
 *
 * El test hermano (@c test_asm_efectos.cpp) comprueba que los efectos de una
 * instruccion sean los CORRECTOS, y para eso hay que escribir a mano lo que se
 * espera de cada una.  Eso no escala a mil, y ademas deja que quien lo escribe
 * elija las que mira -- justo las que se le ocurren, no las que faltan.
 *
 * Este va al reves: enumera las instrucciones de la base y pregunta por cada una
 * si el analisis de efectos la conoce.  Los huecos los encuentra el test, no una
 * persona, y aparecen con nombre.
 *
 * ## Por que importa que una instruccion sea DESCONOCIDA
 *
 * Cuando el analisis no conoce una instruccion, tiene dos salidas y las dos
 * cuestan.  Si se pone en lo conservador -- "puede leer y escribir cualquier
 * cosa" --, el bloque se convierte en una barrera y alrededor no se puede mover
 * nada.  Si se pone en lo permisivo -- "no hace nada" --, deja pasar
 * optimizaciones que rompen; eso es exactamente lo que paso con `movdqa`, que se
 * declaraba sin tocar memoria, y con la aritmetica empaquetada, que no estaba en
 * absoluto.
 *
 * Asi que la cobertura no es una metrica de vanidad: cada instruccion que falta
 * es una de las dos cosas.
 *
 * ## Que hace fallar al test
 *
 * Falla si la cobertura BAJA respecto al minimo anotado.  No exige el 100 %:
 * exigirlo hoy lo dejaria rojo permanentemente, y un rojo permanente se ignora.
 * Lo que impide es retroceder -- y el minimo se sube cada vez que se cubren mas,
 * que es como esto avanza sin que nadie tenga que acordarse.
 */

#include "vx/asm/asm_effects.h"
#include "vx/asm/instr_db.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

/// Cobertura minima exigida, en tanto por mil de las instrucciones de la base.
/// SUBIR este numero al cubrir mas.  Bajarlo es admitir un retroceso, y para eso
/// hace falta decir por que en el commit.
constexpr int kMinimoPorMil = 164; // medido 2026-08-13: 318 de 1930 en x86

struct Resultado {
    std::vector<std::string> conocidas;
    std::vector<std::string> desconocidas;
};

/// Recorre las clases de instruccion de @p isa y pregunta por cada nombre.
Resultado medir(vx::instr_db::Isa isa, const vx::instr_db::IsaData &db,
                const char *arch) {
    Resultado r;
    std::set<std::string> vistas; // una clase puede tener muchas formas
    for (unsigned i = 0; i < db.iclass_count; ++i) {
        const int32_t fid = static_cast<int32_t>(db.iclass[i].first_fid);
        const char *nombre = vx::instr_db::iclass_name(isa, fid);
        if (nombre == nullptr || nombre[0] == '\0') continue;
        std::string m;
        for (const char *p = nombre; *p != '\0'; ++p)
            m += static_cast<char>(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
        if (!vistas.insert(m).second) continue;
        const vx::AsmEffects e = vx::asm_effects_for(m, arch);
        if (e.known) r.conocidas.push_back(m);
        else r.desconocidas.push_back(m);
    }
    return r;
}

void informar(const char *que, const Resultado &r, int muestra) {
    const size_t total = r.conocidas.size() + r.desconocidas.size();
    if (total == 0) {
        std::printf("[cobertura] %s: la base no dio ninguna instruccion\n", que);
        return;
    }
    const int por_mil = static_cast<int>(r.conocidas.size() * 1000 / total);
    std::printf("[cobertura] %s: %zu de %zu con efectos declarados (%d.%d %%)\n",
                que, r.conocidas.size(), total, por_mil / 10, por_mil % 10);
    if (r.desconocidas.empty()) return;
    std::printf("[cobertura] %s: sin declarar, %d de %zu:\n", que, muestra,
                r.desconocidas.size());
    int n = 0;
    std::string linea = "   ";
    for (const std::string &m : r.desconocidas) {
        if (n++ >= muestra) break;
        linea += " " + m;
        if (linea.size() > 70) {
            std::printf("%s\n", linea.c_str());
            linea = "   ";
        }
    }
    if (linea.size() > 3) std::printf("%s\n", linea.c_str());
}

} // namespace

int main() {
    const Resultado x86 =
        medir(vx::instr_db::Isa::X86, vx::instr_db::db_x86(), "x86_64");
    const Resultado a64 =
        medir(vx::instr_db::Isa::ARM64, vx::instr_db::db_arm64(), "arm64");

    informar("x86", x86, 40);
    informar("arm64", a64, 40);

    /* El criterio de fallo: no retroceder.  Se mide sobre x86, que es la que el
     * compilador usa hoy para el `asm` de la stdlib; arm64 se informa para que su
     * hueco se vea, pero todavia no bloquea. */
    const size_t total_x86 = x86.conocidas.size() + x86.desconocidas.size();
    const int por_mil =
        total_x86 == 0 ? 0
                       : static_cast<int>(x86.conocidas.size() * 1000 / total_x86);
    if (por_mil < kMinimoPorMil) {
        std::printf("[cobertura] FALLA: la cobertura de x86 bajo a %d por mil, "
                    "el minimo anotado es %d.  Alguna instruccion dejo de tener "
                    "efectos declarados.\n",
                    por_mil, kMinimoPorMil);
        return 1;
    }
    std::printf("[cobertura] minimo exigido %d por mil, medido %d -> OK\n",
                kMinimoPorMil, por_mil);
    return 0;
}
