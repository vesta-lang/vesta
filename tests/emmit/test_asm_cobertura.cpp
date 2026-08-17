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

/* Relativo a proposito: estos tests se compilan de dos maneras -- por CMake y a
 * mano con un `g++` y unos objetos -- y una ruta que dependa de `-I` funciona en
 * una y falla en la otra.  Con la relativa no hay nada que configurar. */
#include "../util/test_report.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

/// Cobertura minima exigida, en tanto por mil.  SUBIRLO al cubrir mas; bajarlo es
/// admitir un retroceso, y para eso hay que decir por que en el commit.
constexpr int kMinCoveragePerMille = 178; // medido 2026-08-17: 344 de 1930 x86

/* El color y los veredictos son de la utilidad comun (@c tests/util/test_report.h):
 * estaban copiados aqui y en el test de efectos, que es como dos informes del
 * mismo arbol empiezan a leerse distinto. */
#define DIM   tests::dim()
#define RED   tests::red()
#define GREEN tests::green()
#define AMBER tests::amber()
#define CYAN  tests::cyan()
#define BOLD  tests::bold()
#define RESET tests::reset()

/**
 * @brief Lo que se sabe de una instruccion, para el informe.
 *
 * No solo si esta cubierta: tambien QUE declara y cuanto cuesta.  Un informe que
 * dice "cubierta" y no dice que efectos tiene no permite ver un error como el de
 * `movdqa` -- que estaba "cubierta" y declaraba que no tocaba memoria --.
 */
struct Row {
    std::string mnemonic;
    bool        covered = false;

    /// Donde esta un operando.  Un inmediato no es un registro, y meterlos en el
    /// mismo saco es perder justo lo que se estaba intentando ver: `add rax, 8`
    /// no lee ningun registro ademas de su destino.
    enum Place { PLACE_REG = 0, PLACE_MEM, PLACE_IMM, PLACE_COUNT };

    /**
     * @brief El efecto SEPARADO por sentido y por sitio.
     *
     * Una lista plana de banderas -- "mem flags barrera" -- no distingue un
     * `add r0, r1, r2` de un `store [r0], r1`: pueden llevar las mismas y no se
     * parecen en nada.  Lo que los separa es QUE entra, QUE sale y DE DoNDE, asi
     * que se guarda asi:
     *
     *     read   reg     op0, op2
     *     write  reg     op1
     *     read   memory  op1
     *     write  flags
     *
     * Y eso es lo que hace visible un error como el de `movdqa`: un apartado
     * `memory` vacio en una instruccion que guarda en memoria salta a la vista,
     * mientras que en una lista de banderas era una linea mas que no estaba.
     */
    std::string reads[PLACE_COUNT], writes[PLACE_COUNT];
    bool reads_mem = false, writes_mem = false;
    bool reads_flags = false, writes_flags = false;
    bool barrier = false, call = false;
    /**
     * Cuantas FORMAS tiene el mnemonico en la base.
     *
     * Sale impreso porque cambia lo que significa el resto de la ficha: una
     * entrada de la tabla a mano responde por el mnemonico ENTERO, asi que
     * responde por sus doscientas formas a la vez.  Sin este numero, una linea
     * que declara un solo efecto parece precisa cuando puede estar cubriendo
     * formas que hacen cosas distintas.
     */
    unsigned forms = 0;
    unsigned align_req = 0;             ///< bytes de alineacion exigidos
    /// Latencia en la microarquitectura por defecto, en ciclos.  <=0 = sin dato.
    float       latency = 0.0f;
    /// Cuantos uops.  0 = sin dato.
    unsigned    uops = 0;

    /// Si no declara NADA: es lo que distingue "no hace nada" de "no se sabe".
    bool no_effects() const {
        for (int p = 0; p < PLACE_COUNT; ++p)
            if (!reads[p].empty() || !writes[p].empty()) return false;
        return !reads_mem && !writes_mem && !reads_flags && !writes_flags &&
               !barrier && !call;
    }
};

/// Nombre de cada sitio, para imprimirlo.
const char *const kPlaceName[Row::PLACE_COUNT] = {"reg", "memory", "imm"};

struct Coverage {
    /// Por extension ("SSE2", "AVX512EVEX", "BASE"...): TODAS sus instrucciones.
    std::map<std::string, std::vector<Row>> by_ext;
    size_t total = 0;
    size_t known = 0;
};

/// Recorre las clases de instruccion de @p isa y pregunta por cada nombre.
Coverage measure(vx::instr_db::Isa isa, const vx::instr_db::IsaData &db,
                 const char *arch) {
    Coverage r;
    std::set<std::string> seen; // una clase tiene muchas formas
    for (unsigned i = 0; i < db.iclass_count; ++i) {
        const int32_t fid = static_cast<int32_t>(db.iclass[i].first_fid);
        const char *name = vx::instr_db::iclass_name(isa, fid);
        if (name == nullptr || name[0] == '\0') continue;
        std::string m;
        for (const char *p = name; *p != '\0'; ++p)
            m += static_cast<char>(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
        if (!seen.insert(m).second) continue;
        ++r.total;

        Row f;
        f.mnemonic = m;
        const vx::AsmEffects e = vx::asm_effects_for(m, arch);
        f.covered = e.known;
        if (f.covered) ++r.known;
        /* QUE declara, no solo si declara algo.  `movdqa` estaba "cubierta" y
         * decia que no tocaba memoria: un informe que no ensena los efectos no
         * habria permitido verlo.
         *
         * Los dos sentidos de memoria salen de la FORMA, no del mnemonico.  Un
         * mnemonico no los puede separar -- la misma `movdqa` lee con
         * `movdqa xmm0, [rdi]` y escribe con `movdqa [rdi], xmm0` --, asi que
         * preguntarselo daba la misma respuesta para los dos y aqui se doblaba
         * una sola bandera.  La forma si lo sabe: dice cual de sus operandos es
         * el de memoria y en que rol. */
        /* Las banderas, TAMBIEN por sentido, y de la misma fuente que la memoria:
         * la forma.  Salian las dos del mismo bit, asi que un `add` -- que las
         * ESCRIBE -- se imprimia como que las lee, que es justo al reves. */
        vx::instr_db::flags_of(isa, fid, f.reads_flags, f.writes_flags);
        f.reads_flags = f.reads_flags || e.reads_flags;
        f.writes_flags = f.writes_flags || e.writes_flags;
        f.barrier = e.barrier;
        f.call = e.is_call;
        f.align_req = e.align_req;
        /* El rol de cada operando EXPLICITO lo dice la base, por indice.  Es lo
         * que permite decir "escribe el operando 0 y lee el 1" en vez de solo
         * "escribe algo": dos instrucciones que escriben operandos distintos no se
         * estorban, y con una bandera suelta eso no se puede saber. */
        auto append = [](std::string &dst, const std::string &what) {
            /* Sin repetir: la union recorre muchas formas y casi todas coinciden
             * en el rol de sus operandos, asi que sin esto `op0` saldria
             * doscientas veces en la misma linea. */
            if (dst == what || dst.compare(0, what.size() + 2, what + ", ") == 0 ||
                dst.find(", " + what) != std::string::npos)
                return;
            if (!dst.empty()) dst += ", ";
            dst += what;
        };
        /* TODAS las formas del mnemonico, no solo la primera.
         *
         * Una entrada de la tabla a mano responde por el mnemonico ENTERO, asi que
         * lo que hay que ensenar a su lado es lo que ese mnemonico puede hacer:
         * `movdqa` carga con `movdqa xmm0, [rdi]` y guarda con
         * `movdqa [rdi], xmm0`, y mirando solo la primera forma se veria una de
         * las dos -- la que la base tenga antes, que es un orden estructural y no
         * dice nada --.  Cada sentido sigue atribuido a SU operando, asi que la
         * union no vuelve a colapsar lo que se acaba de separar. */
        f.forms = db.iclass[i].count;
        for (unsigned nf = 0; nf < f.forms; ++nf) {
            const int32_t ffid = fid + static_cast<int32_t>(nf);
            bool form_reads_mem = false, form_writes_mem = false;
            vx::instr_db::memory_of(isa, ffid, form_reads_mem, form_writes_mem);
            f.reads_mem = f.reads_mem || form_reads_mem;
            f.writes_mem = f.writes_mem || form_writes_mem;
            for (size_t k = 0; k < 8; ++k) {
                bool op_reads = false, op_writes = false;
                vx::instr_db::DbOpKind kind = vx::instr_db::OP_REG;
                if (!vx::instr_db::explicit_operand(isa, ffid, k, op_reads,
                                                    op_writes, kind))
                    break;
                /* El SITIO del operando, no solo su rol.  Sin la clase, un
                 * `add rax, 8` sale leyendo dos registros y uno de ellos es un
                 * inmediato: el informe existe para que eso se vea, no para
                 * repetirlo. */
                Row::Place place = Row::PLACE_REG;
                if (kind == vx::instr_db::OP_MEM) place = Row::PLACE_MEM;
                else if (kind == vx::instr_db::OP_IMM) place = Row::PLACE_IMM;
                char label[8];
                std::snprintf(label, sizeof(label), "op%zu", k);
                if (op_reads) append(f.reads[place], label);
                if (op_writes) append(f.writes[place], label);
            }
        }
        /* Y los registros IMPLICITOS, con su nombre: `rep stosb` no nombra `rcx`
         * en el texto y aun asi lo lee, y quien consuma el efecto necesita
         * saberlo. */
        for (const std::string &rr : e.implicit_read)
            append(f.reads[Row::PLACE_REG], rr);
        for (const std::string &w : e.implicit_write)
            append(f.writes[Row::PLACE_REG], w);
        /* Y por donde accede a memoria cuando no lo dice en el texto: una `movsb`
         * lee por `rsi` y escribe por `rdi` porque lo fija la arquitectura.  Se
         * nombra el registro; decir solo "toca memoria" seria rendirse teniendo
         * el dato. */
        for (const std::string &rr : e.implicit_mem_read) {
            append(f.reads[Row::PLACE_MEM], "por " + rr);
            f.reads_mem = true;
        }
        for (const std::string &w : e.implicit_mem_write) {
            append(f.writes[Row::PLACE_MEM], "por " + w);
            f.writes_mem = true;
        }

        /* Y cuanto cuesta.  La latencia no es un adorno: es lo que el planificador
         * usa para ordenar, asi que una instruccion cubierta y sin coste conocido
         * es una que el planificador coloca a ciegas. */
        const vx::instr_db::AsmCost cost =
            vx::instr_db::cost(isa, fid, /*ua_id=*/0);
        f.latency = cost.latency;
        f.uops = cost.uops;

        /* Agrupadas por EXTENSION: sirve para decidir por donde seguir.  Cubrir
         * una extension entera es una tarea; cubrir cuarenta nombres sueltos
         * elegidos por orden alfabetico no es ninguna. */
        const char *ext = vx::instr_db::ext_of(isa, fid);
        r.by_ext[(ext != nullptr && ext[0] != '\0') ? ext : "(sin extension)"]
            .push_back(std::move(f));
    }
    return r;
}

int per_mille(const Coverage &r) {
    return r.total == 0 ? 0 : static_cast<int>(r.known * 1000 / r.total);
}

/// Imprime un sentido de un sitio, si tiene algo que decir.
///
/// @param sense  @c "read" o @c "write".
/// @param place  Nombre del sitio.
/// @param who    Operandos o registros; vacio = se sabe que lo toca pero no
///               por donde (memoria implicita de `push`/`pop`).
/// @param present Si el efecto esta, aunque @p who venga vacio.
void print_effect(const char *sense, const char *place, const std::string &who,
                  bool present) {
    if (!present && who.empty()) return;
    /* Vacio pero presente NO se calla: es el caso de `push`, que toca memoria sin
     * nombrarla, y decirlo es la diferencia entre "no toca" y "no se sabe por
     * donde".  Lo segundo es un dato; lo primero seria mentira. */
    std::printf("         %-6s %-7s %s%s%s\n", sense, place, DIM,
                who.empty() ? "(sin nombrar)" : who.c_str(), RESET);
}

void report(const char *what, const Coverage &r) {
    const int pm = per_mille(r);
    const char *tint = pm >= 800 ? GREEN : (pm >= 400 ? AMBER : RED);
    std::printf("\n%s== %s ==%s  %s%zu de %zu%s con efectos declarados  "
                "(%s%d.%d %%%s)\n",
                BOLD, what, RESET, BOLD, r.known, r.total, RESET, tint,
                pm / 10, pm % 10, RESET);
    /* TODAS, cubiertas y sin cubrir, agrupadas por la extension a la que
     * pertenecen -- que es la unidad en la que se decide el trabajo: cubrir `AES`
     * entero son seis instrucciones, cubrir cuarenta nombres elegidos por orden
     * alfabetico no es ninguna tarea.
     *
     * Y cada una con lo que declara y lo que cuesta.  El detalle solo sale con
     * VESTA_COBERTURA_DETALLE=1, porque son casi dos mil lineas por ISA: util
     * cuando se trabaja en una extension, ruido cuando se mira el total. */
    const bool detail = std::getenv("VESTA_COBERTURA_DETALLE") != nullptr;
    for (const auto &kv : r.by_ext) {
        size_t cov = 0;
        for (const Row &f : kv.second)
            if (f.covered) ++cov;
        const char *t = cov == kv.second.size() ? GREEN
                                                : (cov == 0 ? RED : AMBER);
        std::printf("  %s%-18s%s %s%zu de %zu%s\n", CYAN, kv.first.c_str(), RESET,
                    t, cov, kv.second.size(), RESET);
        if (!detail) {
            /* Sin detalle, al menos los nombres de las que faltan: son el trabajo
             * pendiente, y una cuenta sin nombres no se puede repartir. */
            std::string line = "       ";
            for (const Row &f : kv.second) {
                if (f.covered) continue;
                if (line.size() + f.mnemonic.size() + 1 > 78) {
                    std::printf("%s%s%s\n", DIM, line.c_str(), RESET);
                    line = "       ";
                }
                line += f.mnemonic + " ";
            }
            if (line.size() > 7) std::printf("%s%s%s\n", DIM, line.c_str(), RESET);
            continue;
        }
        for (const Row &f : kv.second) {
            /* El numero de formas va en la cabecera de la instruccion porque
             * califica todo lo que viene debajo: una linea de la tabla a mano
             * responde por las 46 formas de `vmovdqa` a la vez. */
            std::printf("     %s%-3s%s %s%-18s%s %s%u formas%s\n",
                        f.covered ? GREEN : RED, f.covered ? "ok" : "--", RESET,
                        BOLD, f.mnemonic.c_str(), RESET, DIM, f.forms, RESET);
            /* Un apartado por sentido y por sitio, y solo los que tienen algo.
             * Un `memory none` en cada instruccion que no toca memoria seria
             * ruido; el que hace falta ver es el apartado que FALTA donde deberia
             * haberlo, y para eso basta con que aparezca cuando lo hay. */
            for (int p = 0; p < Row::PLACE_COUNT; ++p) {
                const bool mem = p == Row::PLACE_MEM;
                print_effect("read", kPlaceName[p], f.reads[p],
                             mem ? f.reads_mem : !f.reads[p].empty());
                print_effect("write", kPlaceName[p], f.writes[p],
                             mem ? f.writes_mem : !f.writes[p].empty());
            }
            if (f.reads_flags) std::printf("         read   flags\n");
            if (f.writes_flags) std::printf("         write  flags\n");
            if (f.barrier)
                std::printf("         barrier        %snada la cruza%s\n", AMBER,
                            RESET);
            if (f.call)
                std::printf("         call           %sel control sale%s\n",
                            AMBER, RESET);
            if (f.align_req != 0u) {
                /* El centinela no es un numero de bytes: dice "tanto como mida su
                 * operando", que es lo correcto para las formas alineadas -- una
                 * `movdqa` exige 16 y una `vmovdqa64` exige 64, y la instruccion
                 * es la misma familia.  Imprimirlo como 65535 bytes lo hacia
                 * ilegible y de paso parecia un error. */
                if (f.align_req == vx::kAlignAnchoOperando)
                    std::printf("         align          %sel ancho de su "
                                "operando%s\n",
                                AMBER, RESET);
                else
                    std::printf("         align          %s%u bytes%s\n", AMBER,
                                f.align_req, RESET);
            }
            if (f.no_effects())
                std::printf("         %s%s%s\n", f.covered ? DIM : RED,
                            f.covered ? "(sin efectos observables)"
                                      : "(no se sabe: sin modelar)",
                            RESET);
            if (f.latency > 0.0f)
                std::printf("         cost           %slat %.1f  uops %u%s\n",
                            DIM, (double)f.latency, f.uops, RESET);
            else
                std::printf("         cost           %ssin dato%s\n", DIM, RESET);
        }
    }
}

} // namespace

int main() {
    std::printf("%s[cobertura de efectos del asm]%s  instrucciones de la base de "
                "datos con efectos declarados\n",
                BOLD, RESET);

    const Coverage x86 =
        measure(vx::instr_db::Isa::X86, vx::instr_db::db_x86(), "x86_64");
    const Coverage a64 =
        measure(vx::instr_db::Isa::ARM64, vx::instr_db::db_arm64(), "arm64");
    const Coverage a32 =
        measure(vx::instr_db::Isa::ARM32, vx::instr_db::db_arm32(), "arm32");
    const Coverage rv =
        measure(vx::instr_db::Isa::RISCV, vx::instr_db::db_riscv(), "riscv");

    report("x86", x86);
    report("arm64", a64);
    report("arm32", a32);
    report("riscv", rv);

    /* El criterio de fallo se mide sobre x86, que es la que el compilador usa hoy
     * para el `asm` de la stdlib.  Las otras se informan para que su hueco se vea
     * -- y se veia poco cuando no se imprimian --, pero todavia no bloquean. */
    const int pm = per_mille(x86);
    std::printf("\n%s[resumen]%s x86 %s%d por mil%s, minimo exigido %d.  "
                "arm64 %d, arm32 %d, riscv %d.\n",
                BOLD, RESET, pm >= kMinCoveragePerMille ? GREEN : RED, pm, RESET,
                kMinCoveragePerMille, per_mille(a64), per_mille(a32),
                per_mille(rv));
    if (pm < kMinCoveragePerMille) {
        std::printf("%s[cobertura] FALLA%s: bajo de %d a %d por mil -- alguna "
                    "instruccion dejo de tener efectos declarados.\n",
                    RED, RESET, kMinCoveragePerMille, pm);
        return 1;
    }
    return 0;
}
