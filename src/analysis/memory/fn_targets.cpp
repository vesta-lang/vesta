/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file fn_targets.cpp
 * @brief Implementacion del resolvedor de punteros a funcion (ver
 * fn_targets.h).
 */
#include "analysis/memory/fn_targets.h"

#include "analysis/memory/points_to.h"
#include "ir/ssa_ir.h"

namespace analysis {

namespace {

using ir::IrOp;

/// Profundidad maxima al seguir copias.  Una cadena mas larga que esto es un
/// patron que no se ha previsto, y ahi se para en vez de adivinar.
constexpr int kMaxSaltos = 16;

std::string seguir(const ir::IrFunction &fn, const IrFacts &facts,
                   ir::IrValueId v, int saltos) {
    if (saltos > kMaxSaltos) return {};
    const ir::IrInstr *d = facts.def(v);
    if (d == nullptr) return {};
    switch (d->op) {
    case IrOp::LABEL_ADDR:
        // Aqui se toma la direccion: es el unico sitio donde nace un destino.
        return d->func_name;
    case IrOp::MOV:
    case IrOp::BITCAST:
        // Copias y reinterpretaciones no cambian a donde apunta.
        return d->operands.empty()
                   ? std::string{}
                   : seguir(fn, facts, d->operands[0], saltos + 1);
    case IrOp::LOAD: {
        /* Guardado y releido.  Solo vale si el hueco se escribe UNA vez: con
         * dos escrituras el contenido depende del camino, y entonces el destino
         * tambien. */
        if (d->operands.empty()) return {};
        const ir::IrValueId guardado =
            valor_unico_del_hueco(fn, d->operands[0]);
        if (guardado == ir::IR_NO_VALUE) return {};
        return seguir(fn, facts, guardado, saltos + 1);
    }
    case IrOp::PHI: {
        // Varios caminos: solo se afirma si todos llevan al mismo sitio.
        std::string comun;
        for (const ir::IrPhiArg &pa : d->phi_args) {
            const std::string n = seguir(fn, facts, pa.value, saltos + 1);
            if (n.empty()) return {};
            if (comun.empty())
                comun = n;
            else if (comun != n)
                return {};
        }
        return comun;
    }
    default: return {};
    }
}

/// Todos los usos de @p v dentro de @p fn son el puntero de una llamada
/// indirecta.  Cualquier otro uso -- guardarlo, pasarlo, devolverlo, meterlo en
/// una PHI -- lo saca de la vista.
bool usos_solo_en_llamadas(const ir::IrFunction &fn, ir::IrValueId v,
                           std::vector<SitioIndirecto> &sitios) {
    bool alguno = false;
    for (const ir::IrBlock &b : fn.blocks) {
        for (const ir::IrInstr &in : b.instrs) {
            if (in.op == IrOp::CALLIND && in.func_ptr == v) {
                sitios.push_back({&fn, &in});
                alguno = true;
                // El puntero puede aparecer TAMBIEN entre los argumentos: eso
                // seria pasarselo a otro, y entonces ya no se ve.
                for (ir::IrValueId a : in.operands)
                    if (a == v) return false;
                continue;
            }
            for (ir::IrValueId a : in.operands)
                if (a == v) return false;
            if (in.func_ptr == v)
                return false; // otro tipo de llamada indirecta
            for (const ir::IrPhiArg &pa : in.phi_args)
                if (pa.value == v) return false;
        }
    }
    return alguno;
}

} // namespace

std::string funcion_apuntada(const ir::IrFunction &fn, const IrFacts &facts,
                             ir::IrValueId v) {
    if (v == ir::IR_NO_VALUE) return {};
    return seguir(fn, facts, v, 0);
}

DireccionTomada seguir_direccion(const ir::IrModule &mod,
                                 const std::string &nombre) {
    DireccionTomada out;
    if (nombre.empty()) return out;
    bool todas = true;
    for (const ir::IrFunction &fn : mod.functions) {
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &in : b.instrs) {
                /* Un bloque de ensamblador puede saltar a un simbolo por su
                 * nombre sin que aparezca ninguna instruccion de llamada.  No
                 * se intenta interpretarlo: basta con que lo nombre. */
                if (in.op == IrOp::RAW_ASM || in.op == IrOp::INLINE_ASM) {
                    if (in.func_name.find(nombre) != std::string::npos) {
                        out.tomada = true;
                        todas = false;
                    }
                    continue;
                }
                if (in.func_name != nombre) continue;
                if (in.op == IrOp::CALL || in.op == IrOp::TAILCALL)
                    continue; // llamada directa: se ve sin seguir nada
                out.tomada = true;
                if (in.op != IrOp::LABEL_ADDR || in.dst == ir::IR_NO_VALUE) {
                    // Registrada como metodo, usada de deleter, nombrada por
                    // una nativa...: son puertas de entrada que no se pueden
                    // censar.
                    todas = false;
                    continue;
                }
                if (!usos_solo_en_llamadas(fn, in.dst, out.indirectas))
                    todas = false;
            }
        }
    }
    out.todas_se_ven = out.tomada && todas;
    return out;
}

} // namespace analysis
