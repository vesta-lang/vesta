/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/analysis/test_address_space.cpp
 * @brief Que la memoria de una direccion SOBREVIVA al viaje por memoria.
 *
 * El intermedio ya dice de que memoria es cada valor, pero guardarlo y leerlo
 * de vuelta lo dejaba sin marca, y un valor sin marca que se llama acaba en
 * una llamada indirecta DE LA MAQUINA: llamar asi a una direccion del proceso
 * no da un error, devuelve CERO.
 *
 * El intermedio se construye A MANO y no compilando un `.vx` a proposito: asi
 * lo que se comprueba es el analisis, no como se escribio el programa.  Si un
 * dia el bajado marca las cosas de otra forma, estos casos siguen diciendo lo
 * mismo.
 *
 * Lo que se afirma aqui, y por que cada uno:
 *
 *  - lo que se guarda en un hueco y se lee de vuelta CONSERVA su memoria
 *    (es el eslabon que no existia);
 *  - el nulo no contradice a quien guardo una direccion: es la AUSENCIA de
 *    una, no otra distinta, que es el patron de la inicializacion perezosa;
 *  - dos escrituras que de verdad no coinciden dan "no se", no la primera;
 *  - una escritura que no se sabe donde cae ENSUCIA lo que iba a afirmarse,
 *    pero solo si lo que guardo era de otra memoria;
 *  - sin a quien preguntar por los punteros se contesta "no se" DICIENDOLO,
 *    en vez de contestar de memoria;
 *  - `NotHost` del intermedio NO se toma por "es de la maquina": eso vale
 *    tanto para lo probado como para lo que nadie dijo, y confundirlos es
 *    justo lo que este analisis viene a arreglar;
 *  - no se pregunta por los punteros si la funcion no guarda y lee (pereza);
 *  - y todo ello sobrevive a guardarse y recuperarse.
 */
#include "analysis/facts/ir_facts.h"
#include "analysis/memory/address_space.h"
#include "analysis/memory/points_to.h"
#include "ir/ssa_ir.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool cond, const std::string &what) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

// ---------------------------------------------------------------------------
// Construccion del intermedio
// ---------------------------------------------------------------------------

/// Reserva un hueco local.  @p host dice en que memoria vive, que es lo que el
/// compilador decide al emitirla.
ir::IrValueId make_slot(ir::IrFunction &fn, ir::IrBlockId blk, bool host,
                        int64_t bytes = 8) {
    const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
    ir::IrInstr in{};
    in.op = ir::IrOp::ALLOCA;
    in.type = ir::IrType::I8;
    in.imm = static_cast<uint64_t>(bytes);
    in.dst = v;
    in.host_alloca = host;
    fn.append(blk, std::move(in));
    return v;
}

/// Un valor que el intermedio ya marca como direccion del anfitrion.
ir::IrValueId make_host_address(ir::IrFunction &fn, ir::IrBlockId blk) {
    const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
    ir::IrInstr in{};
    in.op = ir::IrOp::RAW_ALLOC;
    in.type = ir::IrType::PTR;
    in.dst = v;
    fn.append(blk, std::move(in));
    fn.values[v].set_host_by_construction(true);
    return v;
}

/// Una constante entera.  Con @p as_null queda marcada como constante, que es
/// lo que permite reconocer el nulo al emparejar escrituras.
ir::IrValueId make_const(ir::IrFunction &fn, ir::IrBlockId blk, uint64_t imm,
                         bool as_null) {
    const ir::IrValueId v = fn.new_value(ir::IrType::I64);
    ir::IrInstr in{};
    in.op = ir::IrOp::CONST;
    in.type = ir::IrType::I64;
    in.dst = v;
    in.imm = imm;
    fn.append(blk, std::move(in));
    if (as_null) {
        fn.values[v].is_const = true;
        fn.values[v].const_val = imm;
    }
    return v;
}

void emit_store(ir::IrFunction &fn, ir::IrBlockId blk, ir::IrValueId val,
                ir::IrValueId addr) {
    ir::IrInstr in{};
    in.op = ir::IrOp::STORE;
    in.type = ir::IrType::I64;
    in.dst = ir::IR_NO_VALUE;
    in.operands = {val, addr};
    fn.append(blk, std::move(in));
}

ir::IrValueId emit_load(ir::IrFunction &fn, ir::IrBlockId blk,
                        ir::IrValueId addr) {
    const ir::IrValueId v = fn.new_value(ir::IrType::I64);
    ir::IrInstr in{};
    in.op = ir::IrOp::LOAD;
    in.type = ir::IrType::I64;
    in.dst = v;
    in.operands = {addr};
    fn.append(blk, std::move(in));
    return v;
}

void emit_return(ir::IrFunction &fn, ir::IrBlockId blk) {
    ir::IrInstr in{};
    in.op = ir::IrOp::RET;
    in.type = ir::IrType::VOID;
    in.dst = ir::IR_NO_VALUE;
    fn.append(blk, std::move(in));
}

// ---------------------------------------------------------------------------
// A quien se le pregunta por los punteros
// ---------------------------------------------------------------------------

/**
 * @brief Oraculo de points-to que ademas CUENTA cuantas veces se le pregunta.
 *
 * La cuenta no es un adorno: la pereza del analisis es parte de su contrato
 * -- no pedir la tabla cuando la funcion no guarda direcciones --, y sin
 * medirla "es perezoso" no pasa de ser una intencion escrita en un comentario.
 *
 * Funcion con NOMBRE y contexto, no una lambda: es el mismo patron que usan
 * los demas oraculos del proyecto.
 */
struct CountingOracle {
    analysis::PointsTo table;
    int asked = 0;
};

const analysis::PointsTo &answer_points_to(void *ctx,
                                           const ir::IrFunction &fn) {
    (void)fn;
    CountingOracle *o = static_cast<CountingOracle *>(ctx);
    ++o->asked;
    return o->table;
}

analysis::PointsToOracle oracle_of(CountingOracle &o) {
    analysis::PointsToOracle ora;
    ora.ask = &answer_points_to;
    ora.ctx = &o;
    return ora;
}

/// Resuelve de verdad los punteros de @p fn y deja el oraculo listo.
void prepare(CountingOracle &o, const ir::IrFunction &fn,
             const analysis::IrFacts &facts) {
    o.table = analysis::compute_points_to(fn, facts);
    o.asked = 0;
}

// ---------------------------------------------------------------------------
// Casos
// ---------------------------------------------------------------------------

/// Lo que se guarda en un hueco y se lee de vuelta sigue siendo del anfitrion.
/// ES el caso que motiva todo el analisis.
void case_survives_the_round_trip() {
    ir::IrFunction fn;
    fn.name = "round_trip";
    const ir::IrBlockId b = fn.new_block("entry");

    const ir::IrValueId slot = make_slot(fn, b, /*host=*/false);
    const ir::IrValueId addr = make_host_address(fn, b);
    emit_store(fn, b, addr, slot);
    const ir::IrValueId loaded = emit_load(fn, b, slot);
    emit_return(fn, b);

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    CountingOracle o;
    prepare(o, fn, facts);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts, oracle_of(o));

    check(as.is_host(addr), "the stored address is host memory");
    check(as.is_host(loaded), "and reading it back still says host memory");
    check(o.asked == 1, "points-to was asked exactly once");
}

/// Un hueco de la maquina es de la maquina, y eso se PRUEBA desde la reserva.
void case_machine_slot() {
    ir::IrFunction fn;
    fn.name = "machine_slot";
    const ir::IrBlockId b = fn.new_block("entry");

    const ir::IrValueId vm = make_slot(fn, b, /*host=*/false);
    const ir::IrValueId host = make_slot(fn, b, /*host=*/true);
    emit_return(fn, b);

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts);

    check(as.at(vm).space == analysis::AddressSpace::Machine,
          "an allocation that is not host leaves a machine slot");
    check(as.is_host(host), "and one that is, a host slot");
}

/// La direccion de una funcion NUESTRA es codigo de la maquina.  Importa tanto
/// como el otro lado: llamarla por la via nativa salta a codigo que el
/// procesador no entiende, que es el fallo simetrico.
void case_address_of_our_function() {
    ir::IrFunction fn;
    fn.name = "function_address";
    const ir::IrBlockId b = fn.new_block("entry");

    const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
    ir::IrInstr in{};
    in.op = ir::IrOp::LABEL_ADDR;
    in.type = ir::IrType::PTR;
    in.dst = v;
    in.func_name = "one_of_ours";
    fn.append(b, std::move(in));
    emit_return(fn, b);

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts);

    check(as.at(v).space == analysis::AddressSpace::Machine,
          "the address of a label is machine code");
    check(!as.is_host(v), "and not host, which would call it the wrong way");
}

/// Inicializacion perezosa: el hueco se pone a CERO y se rellena la primera
/// vez.  El nulo no es una direccion de ninguna memoria, asi que no contradice
/// a quien guardo una -- es la ausencia de una, no otra distinta --.  Es la
/// misma regla que el bajado ya aplica al fusionar un PHI, y es el patron de
/// los once envoltorios de `std.syscall.windows`.
void case_null_does_not_contradict() {
    ir::IrFunction fn;
    fn.name = "lazy_init";
    const ir::IrBlockId b = fn.new_block("entry");

    const ir::IrValueId slot = make_slot(fn, b, /*host=*/false);
    const ir::IrValueId null = make_const(fn, b, 0, /*as_null=*/true);
    emit_store(fn, b, null, slot); // arranca vacio

    const ir::IrValueId addr = make_host_address(fn, b);
    emit_store(fn, b, addr, slot); // y se rellena

    const ir::IrValueId loaded = emit_load(fn, b, slot);
    emit_return(fn, b);

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    CountingOracle o;
    prepare(o, fn, facts);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts, oracle_of(o));

    check(as.is_host(loaded),
          "an initial null does not stop the load from being host memory");
}

/// Dos escrituras que de verdad no coinciden: lo que hay depende del camino, y
/// eso NO se resuelve quedandose con una.
void case_two_stores_disagree() {
    ir::IrFunction fn;
    fn.name = "disagree";
    const ir::IrBlockId b = fn.new_block("entry");

    const ir::IrValueId slot = make_slot(fn, b, /*host=*/false);
    const ir::IrValueId from_host = make_host_address(fn, b);
    const ir::IrValueId from_machine = make_slot(fn, b, /*host=*/false);
    emit_store(fn, b, from_host, slot);
    emit_store(fn, b, from_machine, slot);
    const ir::IrValueId loaded = emit_load(fn, b, slot);
    emit_return(fn, b);

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    CountingOracle o;
    prepare(o, fn, facts);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts, oracle_of(o));

    check(!as.is_host(loaded), "two different stores assert nothing");
    check(as.at(loaded).space == analysis::AddressSpace::Unknown,
          "it stays unknown, which is not the same as machine memory");
    check(as.at(loaded).reason ==
              analysis::asa::UnknownReason::RuntimeDependent,
          "and the reason says it depends on the path taken");
}

/// Sin a quien preguntar por los punteros no se adivina: se dice que falto.
void case_no_oracle() {
    ir::IrFunction fn;
    fn.name = "no_oracle";
    const ir::IrBlockId b = fn.new_block("entry");

    const ir::IrValueId slot = make_slot(fn, b, /*host=*/false);
    const ir::IrValueId addr = make_host_address(fn, b);
    emit_store(fn, b, addr, slot);
    const ir::IrValueId loaded = emit_load(fn, b, slot);
    emit_return(fn, b);

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts);

    check(as.at(loaded).space == analysis::AddressSpace::Unknown,
          "without an oracle the load is not resolved");
    check(as.at(loaded).reason ==
              analysis::asa::UnknownReason::MissingDependency,
          "and the reason says a dependency was missing");
    check(std::string(as.at(loaded).reason_code) == analysis::kAddrWhyNoOracle,
          "with the exact case, not a generic shrug");
}

/// Si la funcion no guarda nada en memoria, NO se pide la tabla de punteros.
/// Pedir lo vacio ya cuesta cerrojo, busqueda y reservas.
void case_does_not_ask_when_pointless() {
    ir::IrFunction fn;
    fn.name = "no_memory";
    const ir::IrBlockId b = fn.new_block("entry");

    make_host_address(fn, b);
    emit_return(fn, b);

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    CountingOracle o;
    prepare(o, fn, facts);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts, oracle_of(o));
    (void)as;

    check(o.asked == 0,
          "a function that stores no address does not pay for the table");
}

/// Lo que llega por un parametro lo sabe QUIEN LLAMA, y cruzar hasta el es lo
/// que este analisis no hace -- asi que lo dice.
void case_parameter() {
    ir::IrFunction fn;
    fn.name = "with_parameter";
    const ir::IrBlockId b = fn.new_block("entry");
    const ir::IrValueId p = fn.new_value(ir::IrType::PTR);
    fn.values[p].is_param = true;
    fn.params.push_back(p);
    emit_return(fn, b);

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts);

    check(as.at(p).space == analysis::AddressSpace::Unknown,
          "an unmarked parameter asserts no memory");
    check(as.at(p).reason == analysis::asa::UnknownReason::OpaqueBoundary,
          "and the reason points at the boundary, where the answer lives");
}

/// Desplazar una direccion no la saca de su memoria.
void case_pointer_arithmetic() {
    ir::IrFunction fn;
    fn.name = "offset";
    const ir::IrBlockId b = fn.new_block("entry");

    const ir::IrValueId base = make_host_address(fn, b);
    const ir::IrValueId eight = make_const(fn, b, 8, /*as_null=*/false);

    const ir::IrValueId inside = fn.new_value(ir::IrType::PTR);
    ir::IrInstr add{};
    add.op = ir::IrOp::ADD;
    add.type = ir::IrType::I64;
    add.dst = inside;
    add.operands = {base, eight};
    fn.append(b, std::move(add));
    emit_return(fn, b);

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts);

    check(as.is_host(inside),
          "offsetting a host address still gives host memory");
}

/// Un `NotHost` del intermedio sobre un puntero NO prueba que sea de la
/// maquina: ese valor vale igual para lo probado y para lo que nadie dijo.
void case_unmarked_is_not_machine() {
    ir::IrFunction fn;
    fn.name = "unmarked";
    const ir::IrBlockId b = fn.new_block("entry");

    /* Un valor de una operacion que el analisis no modela, con la memoria por
     * defecto del intermedio. */
    const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
    ir::IrInstr in{};
    in.op = ir::IrOp::CALL;
    in.type = ir::IrType::PTR;
    in.dst = v;
    in.func_name = "somewhere_else";
    fn.append(b, std::move(in));
    emit_return(fn, b);

    check(fn.values[v].memory == ir::MemorySpace::NotHost,
          "the IR brings it unmarked");

    const analysis::IrFacts facts = analysis::build_ir_facts(fn);
    const analysis::AddressSpaces as =
        analysis::compute_address_spaces(fn, facts);

    check(as.at(v).space != analysis::AddressSpace::Machine,
          "unmarked is not taken to mean machine memory");
    check(as.at(v).reason == analysis::asa::UnknownReason::OpaqueBoundary,
          "what another function returns is answered at the boundary");
}

/// Guardarlo y recuperarlo devuelve lo mismo, motivo incluido.
void case_store_round_trip() {
    analysis::AddressSpaces as;
    as.by_value.resize(3);
    as.by_value[0].space = analysis::AddressSpace::Host;
    as.by_value[1].space = analysis::AddressSpace::Machine;
    as.by_value[2].space = analysis::AddressSpace::Unknown;
    as.by_value[2].reason = analysis::asa::UnknownReason::OpaqueBoundary;
    as.by_value[2].reason_code = analysis::kAddrWhyCall;
    as.by_value[2].reason_at = static_cast<ir::IrValueId>(7);

    const std::vector<uint8_t> bytes = analysis::serialize_address_spaces(as);
    analysis::AddressSpaces back;
    check(analysis::deserialize_address_spaces(bytes.data(), bytes.size(), 3,
                                               back),
          "what was stored can be read again");
    check(back.at(static_cast<ir::IrValueId>(0)).space ==
              analysis::AddressSpace::Host,
          "host comes back the same");
    check(back.at(static_cast<ir::IrValueId>(1)).space ==
              analysis::AddressSpace::Machine,
          "so does machine");
    check(back.at(static_cast<ir::IrValueId>(2)).reason ==
              analysis::asa::UnknownReason::OpaqueBoundary,
          "and the REASON travels with the answer, not apart from it");
    check(std::string(back.at(static_cast<ir::IrValueId>(2)).reason_code) ==
              analysis::kAddrWhyCall,
          "with its exact case");
    check(back.at(static_cast<ir::IrValueId>(2)).reason_at ==
              static_cast<ir::IrValueId>(7),
          "and the operation that left the gap");

    /* Una tabla que habla de OTRA funcion se descarta: servirla no daria un
     * error, daria respuestas sobre huecos que no existen. */
    analysis::AddressSpaces other;
    check(!analysis::deserialize_address_spaces(bytes.data(), bytes.size(), 99,
                                                other),
          "a table for a function with a different value count is refused");
}

} // namespace

int main() {
    std::printf("== which memory an address belongs to, after memory ==\n");
    case_survives_the_round_trip();
    case_machine_slot();
    case_address_of_our_function();
    case_null_does_not_contradict();
    case_two_stores_disagree();
    case_no_oracle();
    case_does_not_ask_when_pointless();
    case_parameter();
    case_pointer_arithmetic();
    case_unmarked_is_not_machine();
    case_store_round_trip();
    std::printf("%d of %d OK\n", g_checks - g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
