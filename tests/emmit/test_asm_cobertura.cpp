/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/emmit/test_asm_cobertura.cpp
 * @brief INFORME de que instrucciones de la base de datos tienen efectos
 *        declarados, y cuales no.  Tambien es un test: falla si se retrocede.
 *
 * Es un informe antes que un test.  Un "OK" no dice nada util aqui: lo que hace
 * falta saber es CUALES faltan, porque cada una es trabajo concreto y porque la
 * lista es el estado real de esta parte del compilador.  Asi que se imprimen
 * TODAS, agrupadas por extension, no una muestra.
 *
 * El test hermano (@c test_asm_efectos.cpp) comprueba que los efectos sean los
 * CORRECTOS, escribiendo a mano lo que se espera.  Ese no escala a mil y ademas
 * deja que quien lo escribe elija las que mira.  Este va al reves: enumera la
 * base y pregunta por cada una.  Los dos hacen falta -- la base modela EFECTOS,
 * pero no valida que su sintaxis se reconozca, y eso solo se comprueba a mano.
 *
 * ## Por que importa que una instruccion sea DESCONOCIDA
 *
 * Cuando el analisis no la conoce tiene dos salidas y las dos cuestan.  Si se
 * pone conservador -- "puede leer y escribir cualquier cosa" --, el bloque se
 * convierte en una barrera y alrededor no se mueve nada.  Si se pone permisivo,
 * deja pasar optimizaciones que rompen: eso es lo que paso con `movdqa`, que se
 * declaraba sin tocar memoria, y con la aritmetica empaquetada, que no estaba.
 *
 * ## Que hace fallar al test
 *
 * Que la cobertura BAJE del minimo anotado.  No exige el 100 %: exigirlo hoy lo
 * dejaria rojo permanentemente, y un rojo permanente se ignora -- y entonces el
 * dia que se rompa otra cosa nadie lo mira.  Lo que impide es retroceder, y el
 * minimo se sube al cubrir mas.
 */

#include "vx/asm/asm_effects.h"
#include "vx/asm/instr_db.h"

#include "util/test_report.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

/// Cobertura minima exigida, en tanto por mil.  SUBIRLO al cubrir mas; bajarlo es
/// admitir un retroceso, y para eso hay que decir por que en el commit.
constexpr int kMinimoPorMil = 164; // medido 2026-08-13: 318 de 1930 en x86

/* El color y los veredictos son de la utilidad comun (@c tests/util/test_report.h):
 * estaban copiados aqui y en el test de efectos, que es como dos informes del
 * mismo arbol empiezan a leerse distinto. */
#define GRIS   tests::gris()
#define ROJO   tests::rojo()
#define VERDE  tests::verde()
#define AMBAR  tests::ambar()
#define AZUL   tests::azul()
#define FUERTE tests::fuerte()
#define FIN    tests::fin()

/**
 * @brief Lo que se sabe de una instruccion, para el informe.
 *
 * No solo si esta cubierta: tambien QUE declara y cuanto cuesta.  Un informe que
 * dice "cubierta" y no dice que efectos tiene no permite ver un error como el de
 * `movdqa` -- que estaba "cubierta" y declaraba que no tocaba memoria --.
 */
struct Ficha {
    std::string mnemonico;
    bool        cubierta = false;
    /// Efectos declarados, ya en forma legible ("mem flags barrera").
    std::string efectos;
    /// Latencia en la microarquitectura por defecto, en ciclos.  <=0 = sin dato.
    float       latencia = 0.0f;
    /// Cuantos uops.  0 = sin dato.
    unsigned    uops = 0;
};

struct Resultado {
    /// Por extension ("SSE2", "AVX512EVEX", "BASE"...): TODAS sus instrucciones.
    std::map<std::string, std::vector<Ficha>> por_ext;
    size_t total = 0;
    size_t cubiertas = 0;
};

/// Recorre las clases de instruccion de @p isa y pregunta por cada nombre.
Resultado medir(vx::instr_db::Isa isa, const vx::instr_db::IsaData &db,
                const char *arch) {
    Resultado r;
    std::set<std::string> vistas; // una clase tiene muchas formas
    for (unsigned i = 0; i < db.iclass_count; ++i) {
        const int32_t fid = static_cast<int32_t>(db.iclass[i].first_fid);
        const char *nombre = vx::instr_db::iclass_name(isa, fid);
        if (nombre == nullptr || nombre[0] == '\0') continue;
        std::string m;
        for (const char *p = nombre; *p != '\0'; ++p)
            m += static_cast<char>(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
        if (!vistas.insert(m).second) continue;
        ++r.total;

        Ficha f;
        f.mnemonico = m;
        const vx::AsmEffects e = vx::asm_effects_for(m, arch);
        f.cubierta = e.known;
        if (f.cubierta) ++r.cubiertas;
        /* QUE declara, no solo si declara algo.  `movdqa` estaba "cubierta" y
         * decia que no tocaba memoria: un informe que no ensena los efectos no
         * habria permitido verlo. */
        if (e.touches_mem) f.efectos += "mem ";
        if (e.touches_flags) f.efectos += "banderas ";
        if (e.barrier) f.efectos += "barrera ";
        if (e.is_call) f.efectos += "llamada ";
        if (e.operand_write_mask != 0u) f.efectos += "escribe-op ";
        if (!e.implicit_read.empty()) f.efectos += "lee-reg ";
        if (!e.implicit_write.empty()) f.efectos += "escribe-reg ";
        if (e.align_req != 0u) f.efectos += "exige-alineacion ";
        if (f.efectos.empty()) f.efectos = f.cubierta ? "(sin efectos)" : "(no se sabe)";

        /* Y cuanto cuesta.  La latencia no es un adorno: es lo que el planificador
         * usa para ordenar, asi que una instruccion cubierta y sin coste conocido
         * es una que el planificador coloca a ciegas. */
        const vx::instr_db::AsmCost coste =
            vx::instr_db::cost(isa, fid, /*ua_id=*/0);
        f.latencia = coste.latency;
        f.uops = coste.uops;

        /* Agrupadas por EXTENSION: sirve para decidir por donde seguir.  Cubrir
         * una extension entera es una tarea; cubrir cuarenta nombres sueltos
         * elegidos por orden alfabetico no es ninguna. */
        const char *ext = vx::instr_db::ext_of(isa, fid);
        r.por_ext[(ext != nullptr && ext[0] != '\0') ? ext : "(sin extension)"]
            .push_back(std::move(f));
    }
    return r;
}

int por_mil(const Resultado &r) {
    return r.total == 0 ? 0 : static_cast<int>(r.cubiertas * 1000 / r.total);
}

void informar(const char *que, const Resultado &r) {
    const int pm = por_mil(r);
    const char *tinte = pm >= 800 ? VERDE : (pm >= 400 ? AMBAR : ROJO);
    std::printf("\n%s== %s ==%s  %s%zu de %zu%s con efectos declarados  "
                "(%s%d.%d %%%s)\n",
                FUERTE, que, FIN, FUERTE, r.cubiertas, r.total, FIN, tinte,
                pm / 10, pm % 10, FIN);
    /* TODAS, cubiertas y sin cubrir, agrupadas por la extension a la que
     * pertenecen -- que es la unidad en la que se decide el trabajo: cubrir `AES`
     * entero son seis instrucciones, cubrir cuarenta nombres elegidos por orden
     * alfabetico no es ninguna tarea.
     *
     * Y cada una con lo que declara y lo que cuesta.  El detalle solo sale con
     * VESTA_COBERTURA_DETALLE=1, porque son casi dos mil lineas por ISA: util
     * cuando se trabaja en una extension, ruido cuando se mira el total. */
    const bool detalle = std::getenv("VESTA_COBERTURA_DETALLE") != nullptr;
    for (const auto &kv : r.por_ext) {
        size_t cub = 0;
        for (const Ficha &f : kv.second)
            if (f.cubierta) ++cub;
        const char *t = cub == kv.second.size() ? VERDE
                                                : (cub == 0 ? ROJO : AMBAR);
        std::printf("  %s%-18s%s %s%zu de %zu%s\n", AZUL, kv.first.c_str(), FIN, t,
                    cub, kv.second.size(), FIN);
        if (!detalle) {
            /* Sin detalle, al menos los nombres de las que faltan: son el trabajo
             * pendiente, y una cuenta sin nombres no se puede repartir. */
            std::string linea = "       ";
            for (const Ficha &f : kv.second) {
                if (f.cubierta) continue;
                if (linea.size() + f.mnemonico.size() + 1 > 78) {
                    std::printf("%s%s%s\n", GRIS, linea.c_str(), FIN);
                    linea = "       ";
                }
                linea += f.mnemonico + " ";
            }
            if (linea.size() > 7) std::printf("%s%s%s\n", GRIS, linea.c_str(), FIN);
            continue;
        }
        for (const Ficha &f : kv.second) {
            char coste[48] = "";
            if (f.latencia > 0.0f)
                std::snprintf(coste, sizeof(coste), "lat %.1f  uops %u",
                              (double)f.latencia, f.uops);
            else
                std::snprintf(coste, sizeof(coste), "%s", "sin coste conocido");
            std::printf("     %s%-3s%s %-20s %s%-34s%s %s%s%s\n",
                        f.cubierta ? VERDE : ROJO, f.cubierta ? "ok" : "--", FIN,
                        f.mnemonico.c_str(), f.cubierta ? FIN : AMBAR,
                        f.efectos.c_str(), FIN, GRIS, coste, FIN);
        }
    }
}

} // namespace

int main() {
    std::printf("%s[cobertura de efectos del asm]%s  instrucciones de la base de "
                "datos con efectos declarados\n",
                FUERTE, FIN);

    const Resultado x86 =
        medir(vx::instr_db::Isa::X86, vx::instr_db::db_x86(), "x86_64");
    const Resultado a64 =
        medir(vx::instr_db::Isa::ARM64, vx::instr_db::db_arm64(), "arm64");
    const Resultado a32 =
        medir(vx::instr_db::Isa::ARM32, vx::instr_db::db_arm32(), "arm32");
    const Resultado rv =
        medir(vx::instr_db::Isa::RISCV, vx::instr_db::db_riscv(), "riscv");

    informar("x86", x86);
    informar("arm64", a64);
    informar("arm32", a32);
    informar("riscv", rv);

    /* El criterio de fallo se mide sobre x86, que es la que el compilador usa hoy
     * para el `asm` de la stdlib.  Las otras se informan para que su hueco se vea
     * -- y se veia poco cuando no se imprimian --, pero todavia no bloquean. */
    const int pm = por_mil(x86);
    std::printf("\n%s[resumen]%s x86 %s%d por mil%s, minimo exigido %d.  "
                "arm64 %d, arm32 %d, riscv %d.\n",
                FUERTE, FIN, pm >= kMinimoPorMil ? VERDE : ROJO, pm, FIN,
                kMinimoPorMil, por_mil(a64), por_mil(a32), por_mil(rv));
    if (pm < kMinimoPorMil) {
        std::printf("%s[cobertura] FALLA%s: bajo de %d a %d por mil -- alguna "
                    "instruccion dejo de tener efectos declarados.\n",
                    ROJO, FIN, kMinimoPorMil, pm);
        return 1;
    }
    return 0;
}
