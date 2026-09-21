/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/inline_facts.cpp
 * @brief El UNICO sitio que mira el cuerpo de una funcion para decidir si se
 *        puede inlinar.  Ver la cabecera para el porque.
 */

#include "analysis/facts/inline_facts.h"

#include "ir/ssa_ir.h"
#include "util/env_flags.h" // el interruptor para medir A/B las reservas

namespace analysis {

char InlineAnalysis::ID = 0;

namespace {

/// @return true si @p name empieza por @p prefix.
bool empieza_por(const std::string &name, const char *prefix) {
    return name.rfind(prefix, 0) == 0;
}

/**
 * @brief Ops cuya semantica va ligada al MARCO o al runtime.
 *
 * Inlinarlas mueve el marco de excepcion, el guardado de registros vivos del
 * recolector o el despacho dinamico al marco del llamante, y eso cambia el
 * resultado.  En particular, crear un objeto (NEWOBJ) mas su destructor
 * (CALLVIRT) dentro de un callee con bucle rompia el `save_live_regs` del
 * recolector al inlinarse.
 */
bool es_op_de_marco(ir::IrOp op) {
    switch (op) {
    case ir::IrOp::THROW:
    case ir::IrOp::TRYENTER:
    case ir::IrOp::TRYLEAVE:
    case ir::IrOp::RETHROW:
    case ir::IrOp::FINDCLASS:
    case ir::IrOp::REFLECT_COUNT:
    case ir::IrOp::REFLECT_AT:
    case ir::IrOp::NEWOBJ:
    case ir::IrOp::NEWOBJS:
    case ir::IrOp::GC_ALLOC:
    case ir::IrOp::GC_ALLOCP:
    case ir::IrOp::CALLVIRT:
    case ir::IrOp::CALLM:
    case ir::IrOp::CALLITF: return true;
    default: return false;
    }
}

/**
 * @brief La lista de nombres del inliner de UN bloque.
 *
 * Cada familia tiene su motivo y no es el mismo:
 *  - `__module_init*`: el cargador lo invoca por su direccion de arranque, no
 *    con una llamada.  Y esta partido en tandas a proposito -- inlinarlas lo
 *    devolveria a la funcion gigante que se partio.
 *  - `__lambda_*`: se invoca por puntero desde CALLCLOSURE, con otra
 *    convencion (el entorno viaja en r14).
 *  - `__spawn_*`, `__async_*`, `__rspawn_*`: los invoca la maquinaria de
 *    procesos, no un CALL.
 *  - `__vx_str*`: los accesores de cadena de Vesta Embed.  Cada uno expande
 *    una decena de instrucciones y varios juntos reventaban el asignador.
 */
bool en_lista_de_un_bloque(const std::string &name) {
    return empieza_por(name, "__module_init") ||
           empieza_por(name, "__lambda_") || empieza_por(name, "__spawn_") ||
           empieza_por(name, "__async_") || empieza_por(name, "__rspawn_") ||
           empieza_por(name, "__vx_str") || name == "__uncaught";
}

/**
 * @brief La del inliner de VARIOS bloques, que hoy es MAS CORTA.
 *
 * No es un olvido: se recoge tal como esta.  Igualarlas es otra decision, y
 * con su medida.
 */
bool en_lista_de_varios_bloques(const std::string &name) {
    return empieza_por(name, "__module_init") || empieza_por(name, "__lambda");
}

/**
 * @brief Sale el ensamblador en linea por su cuenta?
 *
 * Busca una instruccion de retorno al principio de alguna linea del cuerpo del
 * asm, saltandose comentarios.  Inlinar una funcion que retorna por su cuenta
 * mete ese retorno en mitad del llamante.
 */
bool asm_sale_por_su_cuenta(const ir::IrFunction &fn) {
    for (const ir::IrBlock &blk : fn.blocks)
        for (const ir::IrInstr &in : blk.instrs) {
            if (in.op != ir::IrOp::INLINE_ASM) continue;
            const std::string &body = in.func_name;
            size_t p = 0;
            while (p <= body.size()) {
                const size_t nl = body.find('\n', p);
                std::string ln = body.substr(
                    p, nl == std::string::npos ? std::string::npos : nl - p);
                size_t cm = ln.find(';');
                if (cm != std::string::npos) ln.resize(cm);
                cm = ln.find("//");
                if (cm != std::string::npos) ln.resize(cm);
                const size_t a = ln.find_first_not_of(" \t");
                if (a != std::string::npos) {
                    const size_t e = ln.find_first_of(" \t", a);
                    const std::string tok = ln.substr(
                        a, e == std::string::npos ? std::string::npos : e - a);
                    if (tok == "ret" || tok == "iret" || tok == "iretq" ||
                        tok == "retf" || tok == "sysret")
                        return true;
                }
                if (nl == std::string::npos) break;
                p = nl + 1;
            }
        }
    return false;
}

} // namespace

InlineFacts compute_inline_facts(const ir::IrFunction &fn) {
    InlineFacts f;

    /* Lo que se sabe sin abrir el cuerpo. */
    f.is_native = fn.is_native;
    f.is_naked = fn.is_naked;
    f.wants_inline = fn.wants_inline;
    f.has_section = !fn.section.empty();
    f.is_new_helper = ir::is_new_helper_name(fn.name, nullptr);
    f.blacklisted_single = en_lista_de_un_bloque(fn.name);
    f.blacklisted_multi = en_lista_de_varios_bloques(fn.name);
    f.is_overlay_resolver = empieza_por(fn.name, "__ovl_resolve_");
    f.asm_returns_manually = asm_sale_por_su_cuenta(fn);

    f.block_count = static_cast<uint32_t>(fn.blocks.size());
    f.asm_bindings = static_cast<uint32_t>(fn.asm_reg_bindings.size());
    f.all_blocks_closed = !fn.blocks.empty();

    /* Y AQUI, una sola vez, lo que costaba un recorrido por pase y por
     * pasada. */
    for (size_t k = 0; k < fn.blocks.size(); ++k) {
        const ir::IrBlock &b = fn.blocks[k];
        if (b.instrs.empty()) {
            f.any_block_empty = true;
            f.all_blocks_closed = false;
            continue;
        }
        f.instr_count += static_cast<uint32_t>(b.instrs.size());
        if (k == 0)
            f.entry_instr_count = static_cast<uint32_t>(b.instrs.size());

        const ir::IrInstr &last = b.instrs.back();
        const bool cierra = last.op == ir::IrOp::BR ||
                            last.op == ir::IrOp::BR_COND ||
                            last.op == ir::IrOp::RET;
        if (!cierra) f.all_blocks_closed = false;
        if (last.op == ir::IrOp::RET) {
            f.has_ret = true;
            if (k == 0) f.entry_ends_in_ret = true;
        }

        for (const ir::IrInstr &in : b.instrs) {
            if ((in.op == ir::IrOp::CALL || in.op == ir::IrOp::TAILCALL) &&
                in.func_name == fn.name)
                f.recursive = true;
            if (in.op == ir::IrOp::RAW_ASM) f.has_raw_asm = true;
            if (in.op == ir::IrOp::INLINE_ASM) f.has_inline_asm = true;
            if (in.op == ir::IrOp::SWITCH_DENSE || !in.jump_targets.empty())
                f.has_jump_table = true;
            if (in.op == ir::IrOp::ALLOCA) f.has_alloca = true;
            if (in.op == ir::IrOp::RAW_FREE || in.op == ir::IrOp::SMARTPTR_FREE)
                f.frees_resources = true;
            if (es_op_de_marco(in.op)) f.has_frame_op = true;
            if (in.op == ir::IrOp::CALL &&
                ir::is_new_helper_name(in.func_name, nullptr))
                f.calls_new_helper = true;
            if (in.op == ir::IrOp::MAKE_CLOSURE) f.makes_closure = true;
            if (k == 0 && in.op == ir::IrOp::PHI) f.entry_has_phi = true;
        }
    }
    return f;
}

bool inlineable_single_block(const InlineFacts &f, size_t threshold,
                             bool escape_scalar_on) {
    if (f.is_native || f.is_naked || f.has_section) return false;
    if (f.asm_returns_manually) return false;
    if (f.blacklisted_single) return false;
    /* El resolvedor de overlay devuelve la DIRECCION host de un campo de una
     * vista, y la marca de "es del anfitrion" vive en el CALL del llamante: al
     * inlinar se pierde y el acceso emite `mov` en vez de `movh`, o sea lee
     * otra memoria. */
    if (f.is_overlay_resolver) return false;
    /* Con el reemplazo escalar activo, la factoria de objetos se deja como
     * llamada A PROPOSITO: el pase borra la reserva de los que no escapan, y
     * los que escapan pagan una llamada barata a un ayudante trivial. */
    if (escape_scalar_on && f.is_new_helper) return false;

    if (f.block_count != 1) return false;
    if (f.entry_instr_count == 0) return false;
    if (!f.entry_ends_in_ret) return false;

    /* Una factoria de closures se inlina con MAS holgura: al hacerlo, el
     * entorno nace en el marco del llamante y, si la closure no se escapa de
     * ahi, un pase posterior lo promueve de monton a pila -- cero reservas. */
    const size_t efectivo = f.makes_closure ? threshold * 3 + 8 : threshold;

    /* El desazucarado de `register()` son un ALLOCA y un STORE por atadura, y
     * es puro andamiaje: al inlinar se funde o desaparece.  Si contara, un
     * envoltorio minimo de asm con seis parametros pareceria "grande" y no se
     * inlinaria nunca. */
    size_t cuerpo = f.entry_instr_count;
    const size_t azucar = 2u * f.asm_bindings;
    cuerpo -= (azucar < cuerpo ? azucar : 0);
    /* `@Inline` levanta el umbral, no las reglas de correccion: las de abajo
     * -- recursiva, asm crudo -- siguen decidiendo. */
    if (!f.wants_inline && cuerpo > efectivo) return false;

    if (f.recursive) return false;
    /* RAW_ASM asume la convencion de llamada de la VM y no se puede mover.
     * INLINE_ASM SI: el copiado remapea sus ataduras de registro al llamante.
     */
    if (f.has_raw_asm) return false;
    return true;
}

bool inlineable_multi_block(const InlineFacts &f, size_t threshold) {
    if (f.is_native || f.is_naked || f.has_section) return false;
    if (f.block_count == 0) return false;
    if (f.blacklisted_multi || f.is_overlay_resolver || f.is_new_helper)
        return false;

    if (f.any_block_empty) return false;
    if (!f.all_blocks_closed) return false;
    if (f.recursive) return false;
    if (f.has_raw_asm || f.has_inline_asm) return false;
    if (f.has_jump_table) return false;
    /* Reservar memoria NO impide inlinar.  Lo impedia porque una reserva de
     * pila dejada dentro de un bucle hacia crecer la pila en cada vuelta,
     * pero eso se arregla IZANDOLA al bloque de entrada del llamante -- que
     * corre una vez --, y es lo que hace `inline_one_multiblock`.  Rechazar
     * por ello dejaba fuera a casi cualquier funcion con un struct local, que
     * es justo donde el inline multi-bloque tenia algo que dar.
     *
     * El interruptor devuelve el rechazo, solo para poder medir A/B que
     * desbloquea quitarlo. */
    if (f.has_alloca && util::flag_on(util::FlagId::NoInlineWithAlloca))
        return false;
    if (f.frees_resources) return false;
    if (f.has_frame_op) return false;
    if (f.calls_new_helper) return false;
    if (f.entry_has_phi) return false;
    if (!f.has_ret) return false;

    /* Un solo bloque de hasta doce ya lo cubre el otro inliner.  Este rellena
     * el hueco: un bloque de trece en adelante -- metodos puros que el otro
     * rechaza por tamano, como un `__add__` -- y cualquier multi-bloque que
     * quepa. */
    if (f.block_count == 1 && f.instr_count <= 12) return false;
    /* `@Inline` levanta el UMBRAL y solo el umbral: todo lo de arriba son
     * reglas de que NO SE PUEDE y siguen aplicando.  El umbral es una cuenta
     * de conveniencia para el caso general, y la anotacion dice que en esta
     * funcion ya la hizo quien la escribio. */
    if (f.wants_inline) return true;
    return f.instr_count <= threshold;
}

} // namespace analysis
