/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analyze/asa_asm.cpp
 * @brief El dominio del ASM como productor de hechos del ASA.
 *
 * Responde lo que hay que poder preguntar de un `asm` sin abrir el fuente:
 *
 *   - QUE se elevo a IR y por tanto lo ve el optimizador como cualquier otra
 *     operacion (y lo ejecuta el interprete).
 *   - QUE se quedo como MICRO ASM: una instruccion que no tiene forma tipada en
 *     el IR -- `cpuid`, `mfence`, SIMD -- pero que NO es una caja negra: lleva su
 *     identidad en la base de datos de instrucciones, de donde salen sus efectos.
 *   - QUE bloque no se elevo en absoluto y sigue siendo opaco, con cuanto de el
 *     se entiende y que rasgos del procesador exige.
 *   - QUE EFECTOS tiene cada cosa: memoria, banderas, barreras, control.
 *
 * Vive en @c src/analyze y no en la capa de analisis porque usa el informe de
 * bloques asm (@c analyze::analizar_bloques_asm), que conoce la base de datos de
 * instrucciones.  Se da de alta con @c registrar_productor desde quien lo tenga
 * disponible: el registro existe justamente para que un dominio pueda vivir en
 * su capa y aun asi aparecer en el almacen y en el volcado.
 */

#include "analyze/asm_report.h"

#include "vx/asm/instr_db.h"

#include "analysis/asa/productores.h"
#include "ir/ssa_ir.h"

#include <sstream>
#include <unordered_map>

namespace analyze {

namespace {

using analysis::asa::Certeza;
using analysis::asa::Fact;
using analysis::asa::Produccion;
using analysis::asa::FactId;
using analysis::asa::Sujeto;

const char *const kProductorAsm = "asa.asm";

/// El mnemonico de una micro-instruccion: la primera palabra de su plantilla.
std::string mnemonico(const std::string &tmpl) {
    size_t i = 0;
    while (i < tmpl.size() && (tmpl[i] == ' ' || tmpl[i] == '\t')) ++i;
    const size_t ini = i;
    while (i < tmpl.size() && tmpl[i] != ' ' && tmpl[i] != '\t') ++i;
    return tmpl.substr(ini, i - ini);
}

/// Las tablas de la ISA de una micro-instruccion.  @c AsmMicro::isa lleva el
/// mismo numero que @c instr_db::Isa (lo pone el elevado).
const vx::instr_db::IsaData *datos_isa(uint8_t isa) {
    using vx::instr_db::Isa;
    switch (static_cast<Isa>(isa)) {
    case Isa::X86: return &vx::instr_db::db_x86();
    case Isa::ARM64: return &vx::instr_db::db_arm64();
    case Isa::ARM32: return &vx::instr_db::db_arm32();
    case Isa::RISCV: return &vx::instr_db::db_riscv();
    }
    return nullptr;
}

/// Lo que la BASE DE DATOS dice de una forma, y lo que el IR se guardo de ella.
///
/// Se ensenan LOS DOS a proposito.  El campo @c eff de la instruccion es un
/// ATAJO cacheado; la verdad esta en la DB.  Mientras coincidan, el atajo es
/// legitimo; el dia que no, hay un fallo de modelado nuestro -- y esto es lo
/// unico que lo hace visible sin abrir el generador de codigo.
struct EfectosMicro {
    std::string texto;
    bool        discrepa = false;
    std::string discrepancia;
};

EfectosMicro efectos_de(const ir::AsmMicro &m) {
    EfectosMicro r;
    std::ostringstream o;

    const vx::instr_db::IsaData *d = datos_isa(m.isa);
    const vx::instr_db::DbForm  *forma =
        (d != nullptr && m.form_id < d->form_count) ? &d->forms[m.form_id]
                                                    : nullptr;
    auto cadena = [d](uint32_t idx) -> const char * {
        return (d != nullptr && idx < d->str_count) ? d->str[idx] : "?";
    };

    if (forma != nullptr) {
        o << cadena(forma->iclass);
        const char *conjunto = cadena(forma->isa_set);
        if (conjunto[0] != '\0' && conjunto[0] != '-')
            o << " (" << conjunto << ")";
        /* Se nombra lo que se esta leyendo: la DB marca memoria y banderas, NO
         * barreras ni llamadas.  Decir "DB: sin efectos" de un `mfence` seria
         * falso -- la DB no dice que no sea barrera, dice que no toca memoria ni
         * banderas --, y ese matiz es justo el que hay que ver para saber si el
         * modelado es bueno. */
        o << " | DB(memoria/banderas):";
        const bool db_mem = (forma->memflags & 0x01) != 0;
        const bool db_wf = (forma->memflags & 0x04) != 0;
        const bool db_rf = (forma->memflags & 0x08) != 0;
        if (db_mem) o << " memoria";
        if (db_rf) o << " lee-banderas";
        if (db_wf) o << " escribe-banderas";
        if (!db_mem && !db_rf && !db_wf) o << " sin marcas";
        /* Los operandos con su papel y el conjunto de registros que la DB
         * permite: es lo que hay que mirar para saber si el elevado ato los
         * valores del programa a los sitios correctos. */
        for (uint8_t i = 0; i < forma->ops_count && d != nullptr; ++i) {
            const size_t idx = forma->ops_off + i;
            if (idx >= d->ops_count) break;
            const vx::instr_db::DbOperand &op = d->ops[idx];
            const bool lee = (op.flags & 0x01) != 0;
            const bool esc = (op.flags & 0x02) != 0;
            const bool imp = (op.flags & 0x04) != 0;
            o << " [" << (lee ? "r" : "") << (esc ? "w" : "")
              << (imp ? "!" : "") << " " << cadena(op.regset);
            if (op.width != 0) o << ":" << op.width;
            o << "]";
        }

        /* Y ahora el contraste con lo que el IR cacheo. */
        const bool ir_mem = (m.eff & 0x01) != 0;
        const bool ir_rf = (m.eff & 0x02) != 0;
        const bool ir_wf = (m.eff & 0x04) != 0;
        std::ostringstream dif;
        if (ir_mem != db_mem) dif << " memoria(IR=" << ir_mem << ",DB=" << db_mem << ")";
        if (ir_rf != db_rf) dif << " lee-banderas(IR=" << ir_rf << ",DB=" << db_rf << ")";
        if (ir_wf != db_wf) dif << " escribe-banderas(IR=" << ir_wf << ",DB=" << db_wf << ")";
        if (!dif.str().empty()) {
            r.discrepa = true;
            r.discrepancia = dif.str();
        }
    } else {
        /* Sin forma en la DB no hay verdad contra la que contrastar: lo que se
         * sepa viene solo del atajo, y eso se dice. */
        o << "forma " << m.form_id << " no esta en la base de datos";
    }

    o << " | IR:";
    if (m.eff & 0x01) o << " memoria";
    if (m.eff & 0x02) o << " lee-banderas";
    if (m.eff & 0x04) o << " escribe-banderas";
    if (m.eff & 0x08) o << " barrera";
    if (m.eff & 0x10) o << " llamada";
    uint32_t lee = 0, escribe = 0, vec = 0;
    for (const ir::AsmMicroOperand &op : m.operands) {
        if (op.reads()) ++lee;
        if (op.writes()) ++escribe;
        if (op.regclass == 2) ++vec;
    }
    o << " lee=" << lee << " escribe=" << escribe;
    if (vec != 0) o << " vectoriales=" << vec;
    r.texto = o.str();
    return r;
}

void producir_asm(Produccion &p) {
    /* 1. Lo que se quedo como MICRO ASM: sigue siendo asm, pero dentro del IR y
     *    con sus efectos consultables.  El optimizador lo reordena y el backend
     *    lo re-emite verbatim. */
    std::unordered_map<std::string, uint32_t> micros_por_funcion;
    for (const ir::IrFunction &fn : p.mod.functions) {
        if (!p.interesa(fn)) continue;
        uint32_t n = 0;
        for (const ir::IrBlock &b : fn.blocks)
            for (const ir::IrInstr &in : b.instrs) {
                if (in.op != ir::IrOp::ASM_MICRO) continue;
                const size_t idx = static_cast<size_t>(in.imm);
                if (idx >= fn.asm_micros.size()) continue;
                const ir::AsmMicro &m = fn.asm_micros[idx];
                ++n;
                const EfectosMicro ef = efectos_de(m);
                Fact f;
                f.que.dominio = kProductorAsm;
                f.que.codigo = "asm.micro";
                f.que.a = m.isa;
                f.que.b = m.form_id;
                std::ostringstream o;
                o << mnemonico(m.tmpl) << " -- " << ef.texto;
                f.que.detalle = p.almacen.internar(o.str());
                f.de_quien.clase = Sujeto::Clase::Instruccion;
                f.de_quien.funcion = p.almacen.internar(fn.name);
                f.de_quien.id = static_cast<uint32_t>(idx);
                /* La identidad esta en la base de datos y de ahi salen los
                 * efectos: no es una suposicion sobre lo que hara. */
                f.sello.certeza = Certeza::Demostrada;
                f.sello.origen.productor = kProductorAsm;
                f.sello.origen.funcion = f.de_quien.funcion;
                f.prueba.regla = "base-de-datos-de-instrucciones";
                const FactId id_micro = p.afirmar(std::move(f));

                /* Si el atajo cacheado en el IR no dice lo mismo que la DB, eso
                 * es un fallo de modelado NUESTRO, y hay que verlo: el resto del
                 * compilador razona con el atajo.  Es el ASA comprobando al
                 * propio compilador. */
                if (!ef.discrepa) continue;
                Fact g;
                g.que.dominio = kProductorAsm;
                g.que.codigo = "asm.efectos_discrepan";
                g.que.a = m.isa;
                g.que.b = m.form_id;
                g.que.detalle = p.almacen.internar(
                    "el atajo del IR no coincide con la DB:" + ef.discrepancia);
                g.de_quien = f.de_quien;
                /* Que discrepan esta VISTO, no supuesto: son dos numeros y no
                 * coinciden.  Lo que no se afirma es cual de los dos miente. */
                g.sello.certeza = Certeza::Demostrada;
                g.sello.origen.productor = kProductorAsm;
                g.sello.origen.funcion = g.de_quien.funcion;
                g.prueba.regla = "contraste-ir-contra-db";
                g.prueba.de.push_back(id_micro);
                p.afirmar(std::move(g));
            }
        micros_por_funcion[fn.name] = n;
    }

    /* 2. Los bloques que NO se elevaron: siguen siendo opacos para el IR.  De
     *    ellos se sabe lo que la base reconoce, y lo que no se sabe se dice. */
    const std::vector<AsmBlockReport> bloques = analizar_bloques_asm(p.mod);
    std::unordered_map<std::string, uint32_t> opacos_por_funcion;
    for (const AsmBlockReport &b : bloques) {
        /* El filtro por funcion vale tambien aqui: el informe mira el modulo
         * entero y sin esto se colaria lo que no se ha pedido. */
        if (micros_por_funcion.find(b.funcion) == micros_por_funcion.end())
            continue;
        ++opacos_por_funcion[b.funcion];
        Fact f;
        f.que.dominio = kProductorAsm;
        f.que.codigo = b.opacidad_pedida ? "asm.opaco_pedido" : "asm.no_elevado";
        f.que.a = b.instrucciones;
        f.que.b = b.desconocidas;
        std::ostringstream o;
        o << "linea " << b.linea << ": " << b.instrucciones << " instrucciones, "
          << b.conocidas << " entendidas";
        if (b.desconocidas != 0) o << ", " << b.desconocidas << " sin entender";
        if (b.lee_mem || b.escribe_mem)
            o << " | memoria lee=" << b.lee_mem << " escribe=" << b.escribe_mem;
        if (b.escribe_flags) o << " | banderas=" << b.escribe_flags;
        if (b.control) o << " | saltos=" << b.control;
        if (b.barrera) o << " | barrera";
        if (!b.rasgos.empty()) {
            o << " | exige";
            for (const std::string &r : b.rasgos) o << " " << r;
        }
        f.que.detalle = p.almacen.internar(o.str());
        f.de_quien.clase = Sujeto::Clase::Funcion;
        f.de_quien.funcion = p.almacen.internar(b.funcion);
        /* Si la base entiende TODAS sus instrucciones, lo que se dice de sus
         * efectos esta visto entero.  Si queda alguna sin entender, la evidencia
         * apunta pero pudo quedarse algo fuera: eso es inferido, no demostrado. */
        f.sello.certeza =
            b.desconocidas == 0 ? Certeza::Demostrada : Certeza::Inferida;
        f.sello.origen.productor = kProductorAsm;
        f.sello.origen.funcion = f.de_quien.funcion;
        f.prueba.regla = "lectura-del-bloque-asm";
        p.afirmar(std::move(f));
    }

    /* 3. El resumen por funcion, que es lo que uno mira primero. */
    for (const ir::IrFunction &fn : p.mod.functions) {
        if (!p.interesa(fn)) continue;
        const uint32_t micros = micros_por_funcion[fn.name];
        auto it = opacos_por_funcion.find(fn.name);
        const uint32_t opacos = it == opacos_por_funcion.end() ? 0u : it->second;
        if (micros == 0 && opacos == 0) {
            p.callar({Sujeto::Clase::Funcion, p.almacen.internar(fn.name), 0},
                     "asm.ninguno", kProductorAsm, "no tiene asm sin elevar");
            continue;
        }
        Fact f;
        f.que.dominio = kProductorAsm;
        f.que.codigo = "asm.resumen";
        f.que.a = micros;
        f.que.b = opacos;
        std::ostringstream o;
        o << micros << " instrucciones como micro asm, " << opacos
          << " bloques sin elevar";
        f.que.detalle = p.almacen.internar(o.str());
        f.de_quien.clase = Sujeto::Clase::Funcion;
        f.de_quien.funcion = p.almacen.internar(fn.name);
        f.sello.certeza = Certeza::Demostrada;
        f.sello.origen.productor = kProductorAsm;
        f.sello.origen.funcion = f.de_quien.funcion;
        f.prueba.regla = "recuento-sobre-el-ir";
        p.afirmar(std::move(f));

        /* Lo que NO se puede decir, dicho: cuantas instrucciones se ELEVARON a
         * ops tipadas no viaja en el IR -- una vez elevadas son operaciones
         * normales y nada las marca --, asi que se sabe en el momento del
         * elevado y aqui ya no.  Callarlo daria a entender que no hubo. */
        p.callar({Sujeto::Clase::Funcion, f.de_quien.funcion, 0},
                 "asm.elevado_sin_contar", kProductorAsm,
                 "lo elevado a ops tipadas no queda marcado en el IR");
    }
}

} // namespace

void registrar_productor_asm() {
    analysis::asa::registrar_productor(kProductorAsm, &producir_asm);
}

} // namespace analyze
