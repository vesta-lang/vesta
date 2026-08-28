/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/comptime.cpp
 * @brief Lo que ya se sabe al compilar: bajar su RESULTADO, no su calculo.
 *
 * Cuando algo se resuelve en tiempo de compilacion, lo que llega al programa no
 * es el codigo que lo calcula sino lo calculado: un valor, una estructura ya
 * rellena, unos bytes.  Aqui se materializa eso -- se deja el resultado en los
 * datos del modulo y se emite una referencia -- y se recorre el bucle
 * `comptime`, que no genera un bucle: genera sus vueltas.
 *
 * Va con la INTROSPECCION porque es lo mismo visto del otro lado: lo que el
 * programa puede preguntar de un tipo -- sus campos, sus nombres, sus tamanos
 * -- tambien se sabe al compilar, y por eso viaja como datos ya escritos en el
 * binario en vez de como algo que se averigua al ejecutar.  Ahi esta la
 * promesa de que la reflexion no cuesta aparte.
 */
#include "util/env_flags.h"
#include "vx/lowering.h"
#include "util/thread_slot.h" // el estado por hilo NO va en thread_local
#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType
#include "loader/oop_types.h" // ADVICE_*: el orden de la cadena
#include <algorithm>
#include <chrono>
#include <iostream>
#include "ffi/virtual_lib_registry.h" // lookup_virtual_fn (bug 161: MC.23)
#include "vx/asm/asm_effects.h"       // inferencia de clobbers ( AS inc.4)
#include "vx/asm/asm_diag.h"      // diagnosticos estructurales del asm (ASA.2)
#include "vx/asm/asm_lift_emit.h" // lift de patrones atomicos a IR tipado (ASA.3)
#include "vx/asm/asm_lift_general.h" // lift general straight-line entero a IR real
#include "vx/asm/asm_lift_micro.h"
#include "vx/asm/asm_lift_registro.h"
#include "vx/asm/asm_phys_reg.h" // asm_body_subst_greedy // lift de asm opaco sin operandos -> ASM_MICRO
#include "vx/asm/instr_db.h"    // reschedule_asm (reoptimizador de asm, ASA)
#include "vx/asm/asm_backend.h" // validacion de sintaxis via Keystone (inc.4b)
#include "vx/collection_intrinsics.h"        // tabla de tipos coleccion
#include "vx/comptime/comptime_introspect.h" // helpers compartidos rama A
#include "vx/generics/concepts.h"      // conceptos como predicado -> CONST bool
#include "vx/generics/generic_clone.h" // clone_expr (custom print to_string)
#include "vx/lexer.h"                  // parse de fragments para @Macro
#include "vx/parser.h"                 // parse_one_expr para @Macro
#include "ir/ir_optimizer.h"           // register_pure_new_helper
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include "lowering_internal.h" // la cocina compartida del lowering

namespace vx {

// ---------------------------------------------------------------------
// Sprint 4 (A.37.s4): IntrospectInfo POD chunks.
//
// Para cada layout marcado @Introspect emitimos UN chunk en
// static_data con este layout self-contained (todas las direcciones
// son offsets relativos al inicio del chunk, asi no hace falta
// relocation cross-chunk):
//
// HEADER (24 bytes):
//   +0   u32 kind         (0=Prim, 1=Class, 2=Struct, 3=Enum)
//   +4   u32 size_bytes
//   +8   u32 align_bytes
//   +12  u32 field_count
//   +16  u32 name_off     -- offset interno a los bytes del nombre
//   +20  u32 name_len
//
// FIELDS array (16 bytes cada uno):  ofset 24 + i*16
//   +0   u32 offset       -- offset del field DENTRO de la instancia
//   +4   u32 size_bytes
//   +8   u32 name_off     -- offset interno
//   +12  u32 name_len
//
// NAMES area: empieza tras los FIELDS.  Bytes raw, sin NUL
// terminator (la longitud va en name_len; el accesor type_info_name
// construye un StringObject via STRMAKE con name_addr + name_len).
// ---------------------------------------------------------------------
void Lowering::emit_introspect_info_chunks() {
    auto build_chunk =
        [this](
            const std::string &name, uint32_t kind, uint32_t size_bytes,
            uint32_t align_bytes,
            const std::vector<
                std::pair<std::string, std::pair<uint32_t, uint32_t>>> &fields)
        -> std::vector<uint8_t> {
        const uint32_t field_count = static_cast<uint32_t>(fields.size());
        const size_t header_sz = 24;
        const size_t fields_sz = field_count * 16;
        /* Calculamos los offsets de los nombres tras la tabla de fields. */
        uint32_t name_off = static_cast<uint32_t>(header_sz + fields_sz);
        uint32_t name_len = static_cast<uint32_t>(name.size());
        std::vector<std::pair<uint32_t, uint32_t>> field_name_ranges;
        field_name_ranges.reserve(fields.size());
        uint32_t cur = name_off + name_len;
        for (auto &f : fields) {
            const uint32_t flen = static_cast<uint32_t>(f.first.size());
            field_name_ranges.push_back({cur, flen});
            cur += flen;
        }
        /* Reservamos el buffer con tamano exacto y rellenamos. */
        std::vector<uint8_t> buf;
        buf.resize(cur, 0);
        auto put_u32 = [&buf](size_t at, uint32_t v) {
            buf[at + 0] = static_cast<uint8_t>(v & 0xFF);
            buf[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            buf[at + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            buf[at + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        };
        put_u32(0, kind);
        put_u32(4, size_bytes);
        put_u32(8, align_bytes);
        put_u32(12, field_count);
        put_u32(16, name_off);
        put_u32(20, name_len);
        for (size_t i = 0; i < fields.size(); ++i) {
            const size_t base = header_sz + i * 16;
            put_u32(base + 0, fields[i].second.first);       /* offset */
            put_u32(base + 4, fields[i].second.second);      /* size */
            put_u32(base + 8, field_name_ranges[i].first);   /* name_off */
            put_u32(base + 12, field_name_ranges[i].second); /* name_len */
        }
        /* Copiar bytes del nombre del tipo. */
        for (size_t i = 0; i < name.size(); ++i)
            buf[name_off + i] = static_cast<uint8_t>(name[i]);
        /* Copiar bytes de los nombres de fields. */
        for (size_t i = 0; i < fields.size(); ++i) {
            const auto &nm = fields[i].first;
            const uint32_t pos = field_name_ranges[i].first;
            for (size_t j = 0; j < nm.size(); ++j) {
                buf[pos + j] = static_cast<uint8_t>(nm[j]);
            }
        }
        return buf;
    };
    /* El nombre almacenado en el chunk (lo que devuelve type_info_name)
     * debe ser el nombre PUBLICO del tipo -- el ultimo segmento tras el
     * separador de namespace "__".  La clave del indice sigue siendo el
     * nombre mangled (lo que find_type resuelve en compile-time). */
    auto public_seg = [](const std::string &mangled) -> std::string {
        const size_t p = mangled.rfind("__");
        return (p == std::string::npos) ? mangled : mangled.substr(p + 2);
    };

    /* Structs marcados @Introspect. */
    for (const auto &kv : tc_.struct_layouts()) {
        const auto &lay = kv.second;
        if (!lay.is_introspect) continue;
        std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> fs;
        fs.reserve(lay.fields.size());
        for (const auto &f : lay.fields) {
            fs.push_back({f.name, {f.offset, f.size}});
        }
        std::vector<uint8_t> chunk =
            build_chunk(public_seg(lay.name), /*Struct=*/2, lay.size_bytes,
                        lay.align_bytes, fs);
        const uint64_t idx = out_mod_->intern_static_data(std::move(chunk));
        introspect_idx_by_name_[lay.name] = idx;
    }
    /* Clases marcadas @Introspect.  No emitimos metodos por ahora
     * (Sprint 4 MVP cubre solo fields; Sprint 5 añade methods). */
    for (const auto &kv : tc_.class_layouts()) {
        const auto &lay = kv.second;
        if (!lay.is_introspect) continue;
        std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> fs;
        fs.reserve(lay.fields.size());
        for (const auto &f : lay.fields) {
            fs.push_back({f.name, {f.offset, f.size}});
        }
        std::vector<uint8_t> chunk = build_chunk(
            public_seg(lay.name), /*Class=*/1, lay.size_bytes, /*align=*/8, fs);
        const uint64_t idx = out_mod_->intern_static_data(std::move(chunk));
        introspect_idx_by_name_[lay.name] = idx;
    }
    /* Enums marcados @Introspect.  Listamos variantes como "fields"
     * sinteticos con offset=tag, size=0 -- conveccion para el MVP;
     * el usuario sabe que el campo offset en realidad es el tag. */
    for (const auto &kv : tc_.enum_layouts()) {
        const auto &lay = kv.second;
        if (!lay.is_introspect) continue;
        std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> fs;
        fs.reserve(lay.variants.size());
        for (const auto &v : lay.variants) {
            fs.push_back({v.name, {v.tag, 0}});
        }
        std::vector<uint8_t> chunk = build_chunk(
            public_seg(lay.name), /*Enum=*/3, lay.size_bytes, /*align=*/8, fs);
        const uint64_t idx = out_mod_->intern_static_data(std::move(chunk));
        introspect_idx_by_name_[lay.name] = idx;
    }
}

ir::IrValueId Lowering::materialize_comptime_struct(const ComptimeEvalResult &r,
                                                    const StructLayout &lay,
                                                    uint32_t line) {
    // Alocar el buffer del struct en memoria host (es un value-type).
    const uint64_t buf_bytes =
        (static_cast<uint64_t>(lay.size_bytes) + 7ULL) & ~7ULL;
    const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
    ir::IrInstr al{};
    al.op = ir::IrOp::ALLOCA;
    al.type = ir::IrType::I8;
    al.imm = buf_bytes;
    al.dst = v_buf;
    al.host_alloca = true;
    al.source_line = line;
    emit(current_block_, std::move(al));
    fn_->values[v_buf].is_host_ptr = true;
    fill_comptime_struct_into(v_buf, r, lay, line);
    return v_buf;
}

void Lowering::fill_comptime_struct_into(ir::IrValueId base_addr,
                                         const ComptimeEvalResult &r,
                                         const StructLayout &lay,
                                         uint32_t line) {
    for (const auto &fi : lay.fields) {
        auto it = r.struct_fields.find(fi.name);
        if (it == r.struct_fields.end() || !it->second) continue;
        const ComptimeValue &cv = *it->second;
        // Direccion del campo (base + offset), heredando la naturaleza host/VM.
        ir::IrValueId v_addr = base_addr;
        if (fi.offset != 0) {
            const ir::IrValueId v_off =
                emit_const(ir::IrType::I64, (uint64_t)fi.offset, line);
            v_addr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_addr].is_host_ptr =
                fn_->values[base_addr].is_host_ptr;
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_addr;
            ad.operands = {base_addr, v_off};
            ad.source_line = line;
            emit(current_block_, std::move(ad));
        }
        if (fi.type.kind == PrimitiveKind::STRUCT && cv.is_struct) {
            // Campo struct anidado: rellenar recursivamente en su direccion.
            auto its = tc_.struct_layouts().find(fi.type.struct_name);
            if (its != tc_.struct_layouts().end()) {
                const ComptimeEvalResult sub = result_from_value(cv);
                fill_comptime_struct_into(v_addr, sub, its->second, line);
            }
            continue;
        }
        // Campo escalar: constante + STORE en la direccion del campo.
        const ir::IrType ir_ft = ir_type_from_primitive(fi.type.kind);
        const ir::IrValueId v_val = emit_const(ir_ft, (uint64_t)cv.value, line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir_ft;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_val, v_addr};
        st.source_line = line;
        emit(current_block_, std::move(st));
    }
}

void Lowering::lower_static_local(ast::VarDeclStmt *vd, const Type &sem_type) {
    // Nombre unico por funcion: dos wrappers con `static ctx` no colisionan.
    const std::string fn_name = fn_ ? fn_->name : std::string("?");
    const std::string mangled = fn_name + "$static$" + vd->name;
    const bool aggregate = (sem_type.kind == PrimitiveKind::STRUCT ||
                            sem_type.kind == PrimitiveKind::ARRAY);
    uint64_t nbytes = static_cast<uint64_t>(size_of_type(sem_type));
    if (nbytes < 8) nbytes = 8; // minimo un qword
    const uint64_t slot = get_or_create_runtime_global_slot(mangled, nbytes);
    const ir::IrType ld =
        aggregate ? ir::IrType::PTR : ir_type_from_primitive(sem_type.kind);
    static_local_slots_[vd->name] = {slot, ld, aggregate};

    // Sin init: el slot ya es zero-init (get_or_create_runtime_global_slot).
    if (!vd->init) return;

    // Init cero constante: el slot ya vale 0 -> nada que emitir.
    if (!aggregate && vd->init->kind == ast::NodeKind::IntLitExpr &&
        static_cast<const ast::IntLitExpr *>(vd->init.get())->value == 0) {
        return;
    }

    // Agregado (struct): el init-once emite los valores por defecto de los
    // campos en el slot.  Un array o un struct sin layout queda zero-init.
    const StructLayout *agg_lay = nullptr;
    if (aggregate) {
        if (sem_type.kind != PrimitiveKind::STRUCT)
            return; // array -> zero-init
        auto it_l = tc_.struct_layouts().find(sem_type.struct_name);
        if (it_l == tc_.struct_layouts().end()) return;
        agg_lay = &it_l->second;
    }

    // Init-once: un booleano global guarda si ya se corrio el init.  La
    // PRIMERA ejecucion de la funcion baja el init y marca done=1; las
    // siguientes lo saltan -> estado persistente entre llamadas.
    const uint64_t done_slot =
        get_or_create_runtime_global_slot(mangled + "$done", 8);
    const int ln = vd->loc.line;

    auto emit_addr = [&](uint64_t s) -> ir::IrValueId {
        ir::IrValueId a = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr is{};
        is.op = ir::IrOp::STR_LIT_ADDR;
        is.type = ir::IrType::PTR;
        is.dst = a;
        is.imm = s;
        is.source_line = ln;
        emit(current_block_, std::move(is));
        fn_->values[a].is_host_ptr = true; // gdata vive en memoria host
        return a;
    };

    // done_val = LOAD i64 [done_slot]
    const ir::IrValueId addr_done = emit_addr(done_slot);
    const ir::IrValueId done_val = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr l{};
        l.op = ir::IrOp::LOAD;
        l.type = ir::IrType::I64;
        l.dst = done_val;
        l.operands = {addr_done};
        l.source_line = ln;
        emit(current_block_, std::move(l));
    }
    // cond = (done_val == 0)
    const ir::IrValueId zero = emit_const(ir::IrType::I64, 0, ln);
    const ir::IrValueId cond = fn_->new_value(ir::IrType::BOOL);
    {
        ir::IrInstr c{};
        c.op = ir::IrOp::CMP_EQ;
        c.type = ir::IrType::BOOL;
        c.dst = cond;
        c.operands = {done_val, zero};
        c.source_line = ln;
        emit(current_block_, std::move(c));
    }
    const ir::IrBlockId init_bb = fn_->new_block("static_init");
    const ir::IrBlockId cont_bb = fn_->new_block("static_cont");
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands.push_back(cond);
        br.target_block = init_bb;
        br.false_block = cont_bb;
        br.source_line = ln;
        emit(current_block_, std::move(br));
    }
    fn_->blocks[current_block_].succs.push_back(init_bb);
    fn_->blocks[current_block_].succs.push_back(cont_bb);
    fn_->blocks[init_bb].preds.push_back(current_block_);
    fn_->blocks[cont_bb].preds.push_back(current_block_);

    // init_bb: correr el init -> STORE al slot + STORE done=1 -> BR cont.
    current_block_ = init_bb;
    block_terminated_ = false;
    if (aggregate) {
        // Struct: zero-fill + init.
        const ir::IrValueId var_addr = emit_addr(slot);
        emit_zero_fill(var_addr, static_cast<uint64_t>(agg_lay->size_bytes),
                       ln);
        // El inicializador (p.ej. un ctor comptime `T(...)`) se DESCARTABA:
        // el slot solo recibia los defaults, asi que el ctor no se aplicaba
        // nunca a un `static`.  Ahora se baja y se copia su imagen; despues se
        // emiten SOLO los defaults que esa imagen no puede llevar (una
        // referencia a funcion necesita una direccion resuelta al enlazar).
        // `T()` sobre un struct SIN ningun constructor que case es
        // value-init, no una llamada: bajarlo emitiria una CALL a un simbolo
        // inexistente (`code.T`).  En ese caso basta con los defaults.
        bool init_is_bare_value_init = false;
        if (vd->init->kind == ast::NodeKind::CallExpr) {
            auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
            if (ce->callee && ce->callee->kind == ast::NodeKind::IdentExpr &&
                static_cast<ast::IdentExpr *>(ce->callee.get())->name ==
                    agg_lay->name) {
                bool tiene_ctor = false;
                for (const auto &m : agg_lay->methods)
                    if (m.is_constructor &&
                        m.param_types.size() == ce->args.size()) {
                        tiene_ctor = true;
                        break;
                    }
                init_is_bare_value_init = !tiene_ctor;
            }
        }
        const ir::IrValueId v_src = init_is_bare_value_init
                                        ? ir::IR_NO_VALUE
                                        : lower_expr(vd->init.get());
        if (v_src != ir::IR_NO_VALUE) {
            const bool src_is_host = fn_->values[v_src].is_host_ptr;
            const uint64_t qwords =
                (static_cast<uint64_t>(agg_lay->size_bytes) + 7) / 8;
            for (uint64_t qi = 0; qi < qwords; ++qi) {
                const ir::IrValueId v_off = emit_const(
                    ir::IrType::I64, static_cast<int64_t>(qi * 8), ln);
                const ir::IrValueId v_s = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_s].is_host_ptr = src_is_host;
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = v_s;
                    ad.operands = {v_src, v_off};
                    ad.source_line = ln;
                    emit(current_block_, std::move(ad));
                }
                const ir::IrValueId v_w = fn_->new_value(ir::IrType::I64);
                {
                    ir::IrInstr l2{};
                    l2.op = ir::IrOp::LOAD;
                    l2.type = ir::IrType::I64;
                    l2.dst = v_w;
                    l2.operands = {v_s};
                    l2.source_line = ln;
                    emit(current_block_, std::move(l2));
                }
                const ir::IrValueId v_d = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_d].is_host_ptr = true; // gdata = memoria host
                {
                    ir::IrInstr ad{};
                    ad.op = ir::IrOp::ADD;
                    ad.type = ir::IrType::I64;
                    ad.dst = v_d;
                    ad.operands = {var_addr, v_off};
                    ad.source_line = ln;
                    emit(current_block_, std::move(ad));
                }
                {
                    ir::IrInstr st2{};
                    st2.op = ir::IrOp::STORE;
                    st2.type = ir::IrType::I64;
                    st2.operands = {v_w, v_d};
                    st2.source_line = ln;
                    emit(current_block_, std::move(st2));
                }
            }
            emit_struct_field_defaults(var_addr, *agg_lay, ln,
                                       /*only_non_comptime=*/true);
        } else {
            emit_struct_field_defaults(var_addr, *agg_lay, ln);
        }
        if (agg_lay->is_polymorphic)
            emit_struct_vptr_init(var_addr, *agg_lay, ln);
    } else {
        const ir::IrValueId iv = lower_expr(vd->init.get());
        if (iv != ir::IR_NO_VALUE) {
            const ir::IrValueId var_addr = emit_addr(slot);
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ld;
            st.operands = {iv, var_addr};
            st.source_line = ln;
            emit(current_block_, std::move(st));
        }
    }
    {
        const ir::IrValueId one = emit_const(ir::IrType::I64, 1, ln);
        const ir::IrValueId addr_done2 = emit_addr(done_slot);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {one, addr_done2};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    }
    {
        ir::IrInstr br{};
        br.op = ir::IrOp::BR;
        br.target_block = cont_bb;
        br.source_line = ln;
        emit(current_block_, std::move(br));
    }
    fn_->blocks[init_bb].succs.push_back(cont_bb);
    fn_->blocks[cont_bb].preds.push_back(init_bb);

    current_block_ = cont_bb;
    block_terminated_ = false;
}

ir::IrValueId Lowering::lower_enum_constructor(
    const std::string &enum_name, const std::string &variant_name,
    const std::vector<std::unique_ptr<ast::Expr>> &args, const SourceLoc &loc) {
    // Localizar el layout del enum y la variante.
    const auto &elays = tc_.enum_layouts();
    auto it = elays.find(enum_name);
    if (it == elays.end()) {
        // `typedef Color Tinta new;` -> el layout (variantes, tags, payloads)
        // es el del enum de debajo; el newtype solo anade la identidad, que ya
        // viaja en el Type.
        if (const std::string real = tc_.underlying_layout_name(enum_name);
            !real.empty())
            it = elays.find(real);
    }
    if (it == elays.end()) {
        error_at(loc, "lowering: enum desconocido '" + enum_name + "'");
        return ir::IR_NO_VALUE;
    }
    const EnumLayout &elay = it->second;
    const EnumVariantInfo *var = nullptr;
    for (const auto &v : elay.variants) {
        if (v.name == variant_name) {
            var = &v;
            break;
        }
    }
    if (!var) {
        error_at(loc, "lowering: variante desconocida '" + variant_name +
                          "' en enum '" + enum_name + "'");
        return ir::IR_NO_VALUE;
    }

    // Un enum CON VALOR no es un agregado: es su entero base, y la variante ES
    // ese numero.  Construirle un buffer con tag y campos lo convierte en una
    // direccion, y a partir de ahi comparar dos variantes compara direcciones
    // -- dos `Less` distintos dejan de ser iguales.
    //
    // La comprobacion va AQUI, por donde pasan todas las formas de nombrar una
    // variante, y no en cada llamante: con el enum importado alguno de ellos
    // no lo detectaba y caia al camino de agregado.
    if (elay.is_valued && elay.backing_type_name.empty()) {
        const ir::IrType t = ir_type_from_primitive(elay.backing);
        const ir::IrValueId c = fn_->new_value(t);
        fn_->values[c].is_const = true;
        fn_->values[c].const_val = static_cast<uint64_t>(var->int_value);
        ir::IrInstr ci{};
        ci.op = ir::IrOp::CONST;
        ci.type = t;
        ci.dst = c;
        ci.imm = static_cast<uint64_t>(var->int_value);
        ci.source_line = loc.line;
        emit(current_block_, std::move(ci));
        return c;
    }

    // marker: MAKE_VARIANT identifica la construccion completa de
    // un valor ADT.  Emitido ANTES de la secuencia ALLOCA + STOREs para
    // que el C2 JIT ( D.8) pueda reconocer el patron y aplicar
    // escape analysis (promocion del slot a regs si no escapa) +
    // case-splitting eficiente del match downstream.  No produce SSA
    // value; el emitter actual lo trata como no-op.
    //
    // Lower de los args ANTES del marker para que sus SSA values
    // esten disponibles como operandos.  Cada payload se promueve a
    // un slot de 8 bytes (i64).  Para floats (F32/F64) usamos BITCAST
    // (preserva los bits IEEE) en lugar de FTOI/F2I (que truncaria
    // el valor).  El bug se manifiesta como `Circle(5.0)` con payload
    // 0 porque FTOI(5.0) -> 5, pero luego al destructurar como f64
    // se interpreta 5 como bits IEEE de un denormal cerca de cero.
    std::vector<ir::IrValueId> payload_vals;
    payload_vals.reserve(args.size());
    for (size_t i = 0; i < args.size() && i < var->field_types.size(); ++i) {
        // Auto-promotion literal -> StringObject cuando el payload
        // tipo es STRING.  Sin esto, `Token.Word("hello")` almacena
        // el raw ptr del literal en lugar del GcHandle, y la
        // extraccion `case Word(s) => s` da un ptr invalido al
        // intentar usarlo como string.
        ir::IrValueId v;
        const ast::Expr *ae = args[i].get();
        if (var->field_types[i].kind == PrimitiveKind::STRING && ae &&
            ae->kind == ast::NodeKind::StringLitExpr) {
            auto *slit = const_cast<ast::StringLitExpr *>(
                static_cast<const ast::StringLitExpr *>(ae));
            v = lower_string_literal_to_string_object(slit);
        } else {
            v = lower_expr(args[i].get());
        }
        if (v == ir::IR_NO_VALUE) {
            v = emit_const(ir::IrType::I64, 0, loc.line);
        }
        ir::IrType vt = fn_->values[v].type;
        if (vt == ir::IrType::F64) {
            // bitcast f64 -> i64 (mismo ancho, preserva bits IEEE).
            ir::IrValueId v2 = fn_->new_value(ir::IrType::I64);
            ir::IrInstr bc{};
            bc.op = ir::IrOp::BITCAST;
            bc.type = ir::IrType::I64;
            bc.dst = v2;
            bc.operands = {v};
            bc.source_line = loc.line;
            emit(current_block_, std::move(bc));
            v = v2;
        } else if (vt == ir::IrType::F32) {
            // f32: primero ampliar a f64 (preserva el valor), luego
            // bitcast a i64 (preserva los bits IEEE).
            ir::IrValueId vw = fn_->new_value(ir::IrType::F64);
            {
                ir::IrInstr ext{};
                ext.op = ir::IrOp::F32TOF64;
                ext.type = ir::IrType::F64;
                ext.dst = vw;
                ext.operands = {v};
                ext.source_line = loc.line;
                emit(current_block_, std::move(ext));
            }
            ir::IrValueId v2 = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr bc{};
                bc.op = ir::IrOp::BITCAST;
                bc.type = ir::IrType::I64;
                bc.dst = v2;
                bc.operands = {vw};
                bc.source_line = loc.line;
                emit(current_block_, std::move(bc));
            }
            v = v2;
        } else if (vt != ir::IrType::I64 && vt != ir::IrType::PTR) {
            // Tipos enteros mas estrechos: promocion normal a i64
            // (sign/zero-extend segun signedness).
            v = cast_if_needed(v, vt, ir::IrType::I64, loc.line);
        }
        payload_vals.push_back(v);
    }
    {
        ir::IrInstr mv{};
        mv.op = ir::IrOp::MAKE_VARIANT;
        mv.type = ir::IrType::VOID;
        mv.dst = ir::IR_NO_VALUE;
        mv.operands = payload_vals;
        mv.func_name = enum_name + "." + variant_name;
        mv.imm = static_cast<uint64_t>(var->tag);
        mv.source_line = loc.line;
        emit(current_block_, std::move(mv));
    }

    // 1. ALLOCA slot del enum (size_bytes = 8 + 8*max_payload_fields).
    const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = addr;
        al.imm = static_cast<uint64_t>(elay.size_bytes);
        // Host, como todo agregado (ver lower_var_decl).
        al.host_alloca = true;
        al.source_line = loc.line;
        emit(current_block_, std::move(al));
        fn_->values[addr].is_host_ptr = true;
    }

    // 2. STORE i64 tag en offset 0 (= addr).
    {
        ir::IrValueId tag_v = emit_const(
            ir::IrType::I64, static_cast<uint64_t>(var->tag), loc.line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {tag_v, addr};
        st.source_line = loc.line;
        emit(current_block_, std::move(st));
    }

    // 3. STORE de cada payload arg en offset 8 + 8*i (promovido a i64).
    // Reusa los payload_vals ya lowereados arriba (para el marker
    // MAKE_VARIANT): evita doble-lowering de los args.
    for (size_t i = 0; i < payload_vals.size(); ++i) {
        ir::IrValueId v = payload_vals[i];
        if (v == ir::IR_NO_VALUE) continue;

        // Calcular addr_i = addr + (8 + 8*i).
        const uint64_t off = 8ULL + 8ULL * static_cast<uint64_t>(i);
        ir::IrValueId addr_i = fn_->new_value(ir::IrType::PTR);
        ir::IrValueId off_v = emit_const(ir::IrType::I64, off, loc.line);
        ir::IrInstr ad{};
        ad.op = ir::IrOp::ADD;
        ad.type = ir::IrType::I64;
        ad.dst = addr_i;
        ad.operands = {addr, off_v};
        ad.source_line = loc.line;
        emit(current_block_, std::move(ad));
        // Propagar is_host_ptr del buffer al puntero buf+off (patron
        // is_host_ptr-en-add): el STORE del payload usa la naturaleza del
        // buffer.  No-op hoy (el buffer del constructor es VM stack) pero
        // unifica el patron con Some/Ok/value/error/unwrap.
        fn_->values[addr_i].is_host_ptr = fn_->values[addr].is_host_ptr;

        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.operands = {v, addr_i};
        st.source_line = loc.line;
        emit(current_block_, std::move(st));
    }

    // El SSA value de la expresion es la direccion del slot.
    return addr;
}

void Lowering::lower_comptime_for(ast::ComptimeForStmt *s) {
    if (!s || !s->lo_expr || !s->hi_expr || !s->body) return;
    const ComptimeEvalResult lo = comptime_eval_expr(tc_, s->lo_expr.get());
    const ComptimeEvalResult hi = comptime_eval_expr(tc_, s->hi_expr.get());
    if (!lo.ok || !hi.ok || lo.is_str || hi.is_str) {
        error_at(s->loc, "comptime for: rango no evaluable (lo/hi deben ser "
                         "enteros comptime)");
        return;
    }
    /* Limite defensivo para evitar explosion de codigo. */
    const int64_t lo_v = lo.value;
    int64_t hi_v = hi.value;
    if (s->inclusive) hi_v += 1;
    if (hi_v - lo_v > 4096) {
        error_at(s->loc, "comptime for: rango excede 4096 iteraciones; usar un "
                         "loop runtime en su lugar");
        return;
    }
    /* A.39: el bind del index lo hacemos en DOS lugares:
     *   1. `lowering_comptime_scopes_` para que @c lower_ident lo
     *      inline como CONST en el codigo runtime emitido.
     *   2. `tc.comptime_const_locals_` para que @c comptime_eval_expr
     *      pueda resolverlo cuando aparezca como arg de un comptime fn
     *      o builtin comptime.  Sin esto, `fact(k)` desde el body
     *      del for fallaria con "no comptime-evaluable" porque k
     *      no estaria en tc's stack. */
    auto &mut_tc = const_cast<TypeChecker &>(tc_);
    for (int64_t i = lo_v; i < hi_v; ++i) {
        /* Push lowering scope. */
        std::unordered_map<std::string, ComptimeLocalEntry> scope;
        ComptimeLocalEntry ent;
        ent.value = i;
        ent.ir_t = ir::IrType::I64;
        scope[s->var_name] = ent;
        lowering_comptime_scopes_.push_back(std::move(scope));
        /* Push tc scope. */
        mut_tc.push_comptime_scope();
        TypeChecker::ComptimeConst c;
        c.type = Type{PrimitiveKind::I64};
        c.value = i;
        mut_tc.register_comptime_local(s->var_name, std::move(c));
        /* Lower body. */
        lower_stmt(s->body.get());
        /* Pop. */
        mut_tc.pop_comptime_scope();
        lowering_comptime_scopes_.pop_back();
    }
}

bool Lowering::materialize_comptime_bytes(const std::vector<uint8_t> &bytes,
                                          const StructLayout &layout,
                                          ir::IrValueId v_dst,
                                          uint32_t source_line) {
    if (v_dst == ir::IR_NO_VALUE || bytes.empty()) return false;

    // El valor ES el bloque de memoria que dejo la ejecucion, asi que se copia
    // entero, por palabras.  No se recorren los campos a proposito: mirar la
    // estructura obliga a resolver uniones (varias vistas de los mismos
    // bytes), anidamiento y relleno, y nada de eso cambia lo que hay que
    // copiar.  Un `u256` son cuatro palabras seguidas, se llame como se llame
    // cada trozo por dentro.
    //
    // Lo que si descalifica al tipo es que contenga una direccion: un puntero
    // calculado al compilar apunta a memoria del compilador, que no existe
    // cuando el programa corre.  Eso no se puede trasladar y se dice, en vez
    // de dejar una direccion invalida en el binario.
    for (const auto &f : layout.fields)
        if (f.type.kind == PrimitiveKind::PTR) return false;

    const size_t n = bytes.size();
    for (size_t off = 0; off < n; off += 8) {
        const size_t chunk = (n - off >= 8) ? 8 : (n - off);
        uint64_t raw = 0;
        std::memcpy(&raw, bytes.data() + off, chunk);

        const ir::IrType wt =
            (chunk == 8)
                ? ir::IrType::I64
                : (chunk >= 4
                       ? ir::IrType::I32
                       : (chunk >= 2 ? ir::IrType::I16 : ir::IrType::I8));
        const ir::IrValueId v_val = emit_const(wt, raw, source_line);
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, off, source_line);
        const ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr add{};
        add.op = ir::IrOp::ADD;
        add.type = ir::IrType::PTR;
        add.dst = v_addr;
        add.operands = {v_dst, v_off};
        add.source_line = source_line;
        emit(current_block_, std::move(add));
        fn_->values[v_addr].is_host_ptr = fn_->values[v_dst].is_host_ptr;

        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = wt;
        st.operands = {v_val, v_addr};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    }
    return true;
}

uint64_t Lowering::get_or_create_comptime_global_slot(const std::string &name) {
    auto it = comptime_global_slots_.find(name);
    if (it != comptime_global_slots_.end()) return it->second;
    const auto &cgv = tc_.comptime_const_values();
    auto cit = cgv.find(name);
    if (cit == cgv.end()) return UINT64_MAX;
    /* Solo int en v1 -- strings serializados requieren STRMAKE en
     * __module_init que no esta integrado todavia. */
    if (cit->second.is_str) return UINT64_MAX;
    /* Empaquetar el valor inicial como 8 bytes little-endian. */
    const uint64_t init_val = static_cast<uint64_t>(cit->second.value);
    std::vector<uint8_t> bytes(8);
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((init_val >> (i * 8)) & 0xFFu);
    }
    const uint64_t idx = out_mod_->intern_static_data(std::move(bytes));
    comptime_global_slots_[name] = idx;
    return idx;
}


} // namespace vx
