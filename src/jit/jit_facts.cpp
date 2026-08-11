/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/jit_facts.cpp
 * @brief Implementacion de la base de hechos del JIT (ver @c jit/jit_facts.h).
 */

#include "jit/jit_facts.h"

#include "ir/ssa_ir.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jit {

const char *const kProductorEstructura = "asa.estructura";
const char *const kProductorRangos = "asa.rangos";

JitFactBase::~JitFactBase() {
    static const bool loguear =
        std::getenv("VESTA_JIT_HECHOS_DEBUG") != nullptr;
    if (!loguear || consultas_ == 0) return;
    volcar_hechos(volcado(), stderr);
    std::fprintf(stderr,
                 "[hechos] %zu preguntas atendidas, %zu analisis ejecutados\n",
                 consultas_, computos_);
}

std::string JitFactBase::clave_de(const ir::IrFunction &fn) {
    if (!fn.name.empty()) return fn.name;
    /* Anonima: la direccion la identifica sin ambiguedad mientras viva, y una
     * base no sobrevive al modulo cuyas funciones consulta. */
    char buf[40];
    std::snprintf(buf, sizeof buf, "<anonima:%p>",
                  static_cast<const void *>(&fn));
    return std::string(buf);
}

const analysis::IrFacts &JitFactBase::estructura(const ir::IrFunction &fn) {
    ++consultas_;
    const std::string clave = clave_de(fn);
    if (!gestor_.cached<analysis::IRFactsAnalysis>(clave)) {
        ++computos_;
        /* Un recorrido, sin reticulo ni punto fijo: lo que sale de aqui esta
         * DEMOSTRADO, no inferido.  Los def-use y el CFG son lo que el IR dice,
         * no una aproximacion de lo que podria pasar. */
        analysis::asa::Sello s;
        s.certeza = analysis::asa::Certeza::Demostrada;
        s.origen.productor = kProductorEstructura;
        auto &guardado = sellos_estructura_[clave];
        guardado = s;
        /* La procedencia apunta a la CLAVE, no al nombre de la funcion: la clave
         * vive en el mapa tanto como el sello, y el nodo no se mueve al crecer.
         * Apuntar a `fn.name` dejaria un puntero colgando en cuanto la funcion
         * consultada muriera antes que la base. */
        guardado.origen.funcion = sellos_estructura_.find(clave)->first.c_str();
    }
    return gestor_.get_or_compute<analysis::IRFactsAnalysis, analysis::IrFacts>(
        clave, [&fn]() { return analysis::build_ir_facts(fn); });
}

const analysis::RangeFacts &JitFactBase::rangos(const ir::IrFunction &fn) {
    ++consultas_;
    const std::string clave = clave_de(fn);
    const bool recien = !gestor_.cached<analysis::RangeAnalysis>(clave);
    if (recien) ++computos_;
    /* La factoria pide la estructura POR LA BASE, no por su cuenta: asi el
     * gestor anota que los rangos dependen de ella y una invalidacion arrastra
     * a los dos.  Pedirla aparte dejaria rangos vivos sobre hechos muertos. */
    const analysis::RangeFacts &rf =
        gestor_.get_or_compute<analysis::RangeAnalysis, analysis::RangeFacts>(
            clave, [this, &fn]() {
                return analysis::compute_ranges(fn, estructura(fn));
            });
    if (recien) {
        /* La certeza sale del propio analisis, no de quien pregunta: llegar a
         * punto fijo es haber visto todo lo que podia contradecirlo; pararse por
         * presupuesto es "hasta aqui he llegado", que sostiene una decision con
         * red pero no permite quitar una comprobacion. */
        analysis::asa::Sello s;
        s.certeza = rf.convergio ? analysis::asa::Certeza::Demostrada
                                 : analysis::asa::Certeza::Inferida;
        s.origen.productor = kProductorRangos;
        s.apoyos.anadir(kProductorEstructura); // se dedujeron sobre ella
        auto &guardado = sellos_rangos_[clave];
        guardado = s;
        guardado.origen.funcion = sellos_rangos_.find(clave)->first.c_str();
    }
    return rf;
}

analysis::asa::Sello
JitFactBase::sello_rangos(const ir::IrFunction &fn) const {
    auto it = sellos_rangos_.find(clave_de(fn));
    /* Nadie ha preguntado todavia: no se sabe nada, que no es lo mismo que
     * saber que no hay nada. */
    if (it == sellos_rangos_.end()) return analysis::asa::Sello{};
    return it->second;
}

std::vector<HechoRegistrado> JitFactBase::volcado() const {
    std::vector<HechoRegistrado> salida;
    salida.reserve(sellos_estructura_.size() + sellos_rangos_.size());
    for (const auto &par : sellos_estructura_)
        salida.push_back({kProductorEstructura, par.first, par.second});
    for (const auto &par : sellos_rangos_)
        salida.push_back({kProductorRangos, par.first, par.second});
    /* Orden estable: dos volcados del mismo programa deben poder compararse. */
    std::sort(salida.begin(), salida.end(),
              [](const HechoRegistrado &a, const HechoRegistrado &b) {
                  if (a.funcion != b.funcion) return a.funcion < b.funcion;
                  return std::strcmp(a.dominio, b.dominio) < 0;
              });
    return salida;
}

void volcar_hechos(const std::vector<HechoRegistrado> &entradas, FILE *salida) {
    for (const HechoRegistrado &h : entradas) {
        std::fprintf(salida, "[hechos] %-16s %-32s certeza=%s", h.dominio,
                     h.funcion.c_str(),
                     analysis::asa::nombre_certeza(h.sello.certeza));
        for (int i = 0; i < analysis::asa::Dependencias::kMax; ++i)
            if (h.sello.apoyos.de[i] != nullptr)
                std::fprintf(salida, " sobre=%s", h.sello.apoyos.de[i]);
        std::fprintf(salida, "\n");
    }
}

void JitFactBase::invalidar(const ir::IrFunction &fn) {
    const std::string clave = clave_de(fn);
    /* La estructura arrastra en cascada a todo lo que se derivo de ella; los
     * rangos se descartan tambien de forma explicita por si alguien los pidio
     * antes de que existiera esa dependencia. */
    gestor_.invalidate<analysis::IRFactsAnalysis>(clave);
    gestor_.invalidate<analysis::RangeAnalysis>(clave);
    /* Y su sello con ellos: un hecho muerto que deja su procedencia atras hace
     * que el volcado afirme lo que ya no se sabe. */
    sellos_estructura_.erase(clave);
    sellos_rangos_.erase(clave);
}

bool hay_argumento_acotado(const ir::IrFunction      &fn,
                           const analysis::RangeFacts &rangos) {
    for (const auto &bb : fn.blocks)
        for (const auto &in : bb.instrs) {
            /* Los sitios donde el conocimiento se puede aprovechar: una llamada
             * con cuerpo conocido y una reserva, que es una llamada al asignador
             * escrita como instruccion. */
            const bool interesa =
                in.op == ir::IrOp::RAW_ALLOC ||
                (in.op == ir::IrOp::CALL && !in.func_name.empty());
            if (!interesa) continue;
            for (ir::IrValueId a : in.operands) {
                const analysis::ValueRange &rg = rangos.at(a);
                if (rg.acotada() && !rg.es_todo()) return true;
            }
        }
    return false;
}

} // namespace jit
