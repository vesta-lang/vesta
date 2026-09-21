/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/escape/fn_addr_escape.cpp
 * @brief Que direcciones de funcion nuestra tienen que ser REALES.
 *
 * El contrato y el porque estan en la cabecera.  Aqui solo esta como se
 * averigua: seguir hacia ADELANTE, desde donde se toma la direccion hasta
 * donde se usa, y quedarse con los usos que la sacan de Vesta.
 */

#include "analysis/escape/fn_addr_escape.h"

#include "util/name_pool.h"

#include <algorithm>

namespace analysis {

const char *fn_addr_crossing_name(FnAddrCrossing c) {
    switch (c) {
    case FnAddrCrossing::None: return "none";
    case FnAddrCrossing::NativeCall: return "native-call";
    case FnAddrCrossing::InlineAsm: return "inline-asm";
    case FnAddrCrossing::EscapingStore: return "escaping-store";
    case FnAddrCrossing::Returned: return "returned";
    case FnAddrCrossing::ViaCallee: return "via-callee";
    }
    return "none";
}

const FnAddrEscape *FnAddrEscapeModule::find(const std::string *name) const {
    const auto it =
        std::lower_bound(by_function.begin(), by_function.end(), name,
                         [](const FnAddrEscapeOfFunction &e,
                            const std::string *n) { return e.function < n; });
    if (it == by_function.end() || it->function != name) return nullptr;
    return &it->escape;
}

namespace {

/**
 * @brief Las ranuras LOCALES de una funcion que codigo nativo puede alcanzar.
 *
 * Vector ORDENADO y no un conjunto disperso: son pocas por funcion, se
 * consultan por biseccion y viven en memoria contigua.  Y con nombre, para que
 * un `uint32_t` suelto no haya que ir a averiguar de que es indice.
 */
struct LocalRootsHandedOut {
    std::vector<uint32_t> roots; ///< value-id de cada reserva, ordenados.
    bool contains(uint32_t root) const {
        return std::binary_search(roots.begin(), roots.end(), root);
    }
};

/// Lo que se va sabiendo mientras se recorre una funcion.
struct Walk {
    const ir::IrFunction *fn = nullptr;
    const EscapeInfo *esc = nullptr;
    const PointsTo *pt = nullptr;
    const NativeReach *reach = nullptr;
    /// Las ranuras locales que codigo nativo puede alcanzar.
    LocalRootsHandedOut handed_out;
    /// Indexado por value-id: true si el valor ES la direccion de una funcion
    /// nuestra.  Se siembra en `LABEL_ADDR` y se propaga por las copias.
    std::vector<uint8_t> is_fn_addr;
    /// Indexado por value-id: por donde cruza, o `None`.
    std::vector<FnAddrCrossing> crossing;
    /// Indices de parametro por los que cruza algo.
    std::vector<int32_t> params;
    /// Cuantas direcciones de funcion se han visto, crucen o no.
    size_t fn_addr_count = 0;
};

/// Marca que @p v cruza por @p c, si no lo tenia ya.  El PRIMER motivo gana:
/// dos son igual de validos y quedarse con el primero hace el resultado
/// estable, que es lo que permite compararlo entre compilaciones.
void mark(Walk &w, ir::IrValueId v, FnAddrCrossing c) {
    if (v == ir::IR_NO_VALUE || v >= w.crossing.size()) return;
    if (w.crossing[v] != FnAddrCrossing::None) return;
    w.crossing[v] = c;
}

/// Si el almacen de @p ptr puede leerlo codigo de fuera.
///
/// Guardar una direccion en una ranura local que NO escapa no la saca de
/// Vesta -- nadie de fuera puede leerla --.  En una que si escapa, o en un
/// sitio cuya raiz no se resuelve, hay que suponer que si: lo que no se ve,
/// cruza.
bool store_is_observable(const Walk &w, ir::IrValueId ptr) {
    if (w.pt == nullptr || w.esc == nullptr || w.reach == nullptr) return true;
    const PointsToEntry &e = w.pt->at(ptr);
    switch (e.kind) {
    case effects::AbstractLoc::Kind::Stack:
        /* Una ranura local se alcanza desde fuera solo si SU direccion se le
         * entrega a codigo nativo.
         *
         * Y eso NO es lo mismo que "escapa", que es lo que contesta el escape:
         * alli sale del valor SSA en cuanto se la pasas a cualquiera, y
         * `defmethod`, `findclass` o `newobj` son instrucciones DE LA MAQUINA
         * -- la direccion sale del valor pero no sale de Vesta --.  Usar la
         * respuesta ancha contaba como cruce la ficha de cada clase: medido,
         * 354 de 364. */
        return w.handed_out.contains(e.root);
    case effects::AbstractLoc::Kind::Global:
        /* Y un GLOBAL, solo si se lo hemos entregado a una nativa.  Ser global
         * no basta: el runtime de la maquina lee memoria nuestra todo el rato
         * -- los parametros de `defmethod` llevan la direccion de cada metodo
         * -- y luego salta a ella como bytecode, asi que no necesita que sea
         * real.  Suponer que si era lo que contaba como cruce cada destructor
         * y cada tabla de metodos del programa. */
        if (e.root == effects::LOC_GENERIC) return true; // "algun global"
        return w.reach->reaches_global(e.root);
    default:
        /* Monton, derivado de un parametro, o sin resolver: no se puede
         * afirmar que quede dentro.  Lo que no se ve, cruza. */
        return true;
    }
}

} // namespace

ModuleFunctionNames collect_module_function_names(const ir::IrModule &mod) {
    ModuleFunctionNames out;
    out.sorted.reserve(mod.functions.size());
    for (const ir::IrFunction &fn : mod.functions) {
        if (fn.name.empty()) continue;
        out.sorted.push_back(util::intern_name(fn.name));
    }
    std::sort(out.sorted.begin(), out.sorted.end());
    out.sorted.erase(std::unique(out.sorted.begin(), out.sorted.end()),
                     out.sorted.end());
    return out;
}

NativeReach compute_native_reach(const ir::IrModule &mod,
                                 const FnAddrInputs &in) {
    NativeReach out;
    if (!in.valid()) {
        /* Sin de donde sacar points-to no se puede afirmar nada, y aqui no
         * saber significa que cualquier global es alcanzable. */
        out.unknown_handed = true;
        return out;
    }
    for (const ir::IrFunction &fn : mod.functions) {
        if (fn.is_native || fn.blocks.empty()) continue;
        const PointsTo &pt = in.points_to(in.ctx, fn);
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &ins : b.instrs) {
                if (ins.op != ir::IrOp::CALLN) continue;
                /* Lo que le entregamos a una nativa es lo unico que codigo
                 * nativo puede alcanzar.  De cada argumento se mira a que
                 * sitio apunta; los que no son direcciones resuelven a nada y
                 * no aportan. */
                for (const ir::IrValueId a : ins.operands) {
                    const PointsToEntry &e = pt.at(a);
                    if (e.kind != effects::AbstractLoc::Kind::Global) continue;
                    if (e.root == effects::LOC_GENERIC) {
                        out.unknown_handed = true;
                        continue;
                    }
                    out.globals.push_back(e.root);
                }
            }
        }
    }
    std::sort(out.globals.begin(), out.globals.end());
    out.globals.erase(std::unique(out.globals.begin(), out.globals.end()),
                      out.globals.end());
    return out;
}

FnAddrEscape compute_fn_addr_escape(const ir::IrFunction &fn,
                                    const IrFacts &facts, const EscapeInfo &esc,
                                    const PointsTo &pt,
                                    const NativeReach &reach,
                                    const ModuleFunctionNames &fns,
                                    const CalleeCrossesParam &callee) {
    (void)facts; // el recorrido es hacia adelante: basta el orden del bloque
    FnAddrEscape out;
    if (fn.is_native || fn.blocks.empty()) return out;

    Walk w;
    w.fn = &fn;
    w.esc = &esc;
    w.pt = &pt;
    w.reach = &reach;
    /* QUE RANURAS LOCALES puede alcanzar codigo nativo.  Antes de nada,
     * porque la decision de cada almacen la consulta.
     *
     * La pregunta es estrecha a proposito -- "¿se le ENTREGA a una nativa?" --
     * y no la ancha del escape -- "¿sale del valor?" --: `defmethod`,
     * `findclass` y `newobj` se llevan la direccion de una ranura y son
     * instrucciones DE LA MAQUINA, asi que lo que hay dentro no tiene que ser
     * real.  Ademas de a una nativa, sale por un RET: ahi ya no se ve quien la
     * recibe, y lo que no se ve, cruza. */
    for (const ir::IrBlock &b : fn.blocks) {
        for (const ir::IrInstr &ins : b.instrs) {
            const bool hands_out =
                ins.op == ir::IrOp::CALLN || ins.op == ir::IrOp::RET;
            if (!hands_out) continue;
            for (const ir::IrValueId a : ins.operands) {
                const PointsToEntry &e = pt.at(a);
                if (e.kind != effects::AbstractLoc::Kind::Stack) continue;
                w.handed_out.roots.push_back(e.root);
            }
        }
    }
    std::sort(w.handed_out.roots.begin(), w.handed_out.roots.end());
    w.handed_out.roots.erase(
        std::unique(w.handed_out.roots.begin(), w.handed_out.roots.end()),
        w.handed_out.roots.end());
    w.is_fn_addr.assign(fn.values.size(), 0u);
    w.crossing.assign(fn.values.size(), FnAddrCrossing::None);

    /* DOS vueltas, no una.  En forma SSA una definicion va antes que sus usos
     * DENTRO de un bloque, pero un PHI puede referirse a un valor de un bloque
     * posterior en el texto.  Con una sola vuelta se perderia esa propagacion
     * -- y perderla aqui no es imprecision: es marcar como interna una
     * direccion que sale, o sea el fallo silencioso que esto viene a cerrar. */
    for (int pass = 0; pass < 2; ++pass) {
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &ins : b.instrs) {
                // --- sembrar y propagar "esto ES una direccion de funcion" ---
                if (ins.dst != ir::IR_NO_VALUE &&
                    ins.dst < w.is_fn_addr.size()) {
                    if (ins.op == ir::IrOp::LABEL_ADDR) {
                        /* Solo si el nombre ES una funcion nuestra.  Un
                         * `LABEL_ADDR` es la direccion de una ETIQUETA, y la
                         * mayoria son de DATOS -- el nombre de una clase para
                         * `findclass`, un literal --.  Tratarlas todas como
                         * funciones contaba como cruce cada cadena del
                         * programa. */
                        if (!ins.func_name.empty() && !w.is_fn_addr[ins.dst] &&
                            fns.contains(util::intern_name(ins.func_name))) {
                            w.is_fn_addr[ins.dst] = 1u;
                            ++w.fn_addr_count;
                        }
                    } else if ((ins.op == ir::IrOp::MOV ||
                                ins.op == ir::IrOp::BITCAST) &&
                               ins.operands.size() == 1) {
                        const ir::IrValueId s = ins.operands[0];
                        if (s < w.is_fn_addr.size() && w.is_fn_addr[s])
                            w.is_fn_addr[ins.dst] = 1u;
                    } else if (ins.op == ir::IrOp::PHI) {
                        for (const ir::IrPhiArg &a : ins.phi_args) {
                            if (a.value < w.is_fn_addr.size() &&
                                w.is_fn_addr[a.value]) {
                                w.is_fn_addr[ins.dst] = 1u;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    /* Y ahora los USOS.  Una direccion que no sea de funcion no se mira: lo
     * que decide es a donde va la direccion, no que instruccion es. */
    for (const ir::IrBlock &b : fn.blocks) {
        for (const ir::IrInstr &ins : b.instrs) {
            switch (ins.op) {
            case ir::IrOp::CALLN: {
                /* Al otro lado corre codigo nativo, que no puede saltar a
                 * bytecode.  Es el caso de `qsort` con nuestro comparador. */
                for (const ir::IrValueId a : ins.operands)
                    if (a < w.is_fn_addr.size() && w.is_fn_addr[a])
                        mark(w, a, FnAddrCrossing::NativeCall);
                break;
            }
            case ir::IrOp::CALL:
            case ir::IrOp::TAILCALL: {
                /* Una llamada nuestra: cruza solo si el callee la deja
                 * cruzar.  Eso lo cierra el punto fijo del modulo. */
                for (size_t i = 0; i < ins.operands.size(); ++i) {
                    const ir::IrValueId a = ins.operands[i];
                    if (a >= w.is_fn_addr.size() || !w.is_fn_addr[a]) continue;
                    /* Sin oraculo se supone que cruza: lo que no se ve,
                     * cruza.  Es la respuesta correcta sin su cuerpo, no una
                     * renuncia. */
                    const bool crosses =
                        !callee.valid() || callee.ask(callee.ctx, ins.func_name,
                                                      static_cast<int32_t>(i));
                    if (crosses) mark(w, a, FnAddrCrossing::ViaCallee);
                }
                break;
            }
            case ir::IrOp::STORE: {
                if (ins.operands.size() < 2) break;
                const ir::IrValueId val = ins.operands[0];
                if (val >= w.is_fn_addr.size() || !w.is_fn_addr[val]) break;
                if (store_is_observable(w, ins.operands[1]))
                    mark(w, val, FnAddrCrossing::EscapingStore);
                break;
            }
            case ir::IrOp::RET: {
                /* Se va: donde acaba no se ve desde aqui.  Un callgraph
                 * completo lo afinaria; hasta entonces, la cara. */
                for (const ir::IrValueId a : ins.operands)
                    if (a < w.is_fn_addr.size() && w.is_fn_addr[a])
                        mark(w, a, FnAddrCrossing::Returned);
                break;
            }
            default: break;
            }
        }
    }

    /* Y por ultimo el ensamblador en linea, que lee ranuras atadas a un
     * registro: lo que se guarde ahi lo toca codigo REAL.  Se mira por las
     * ligaduras -- donde el compilador guarda que variable entra y sale del
     * bloque --, no tratando el asm como una caja negra, que es la regla de
     * la casa.
     *
     * La ligadura nombra la RANURA (@c alloca_value), no el valor, asi que lo
     * que hay que buscar es el almacen que va a ella. */
    if (!fn.asm_reg_bindings.empty()) {
        /* Las ranurasos atadas, ORDENADAS una vez.  Con la busqueda dentro del
         * bucle de almacenes esto seria el producto de los dos, y un producto
         * es lo que no puede haber aqui. */
        std::vector<uint32_t> bound;
        bound.reserve(fn.asm_reg_bindings.size());
        for (const ir::AsmRegBinding &bind : fn.asm_reg_bindings)
            bound.push_back(static_cast<uint32_t>(bind.alloca_value));
        std::sort(bound.begin(), bound.end());
        bound.erase(std::unique(bound.begin(), bound.end()), bound.end());
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &ins : b.instrs) {
                if (ins.op != ir::IrOp::STORE || ins.operands.size() < 2)
                    continue;
                const ir::IrValueId val = ins.operands[0];
                if (val >= w.is_fn_addr.size() || !w.is_fn_addr[val]) continue;
                const uint32_t root = pt.at(ins.operands[1]).root;
                if (!std::binary_search(bound.begin(), bound.end(), root))
                    continue;
                /* El motivo del asm PISA al que hubiera: es mas concreto que
                 * "se guardo en un sitio observable", y el motivo existe para
                 * mandar a quien lo lea al sitio correcto. */
                if (val < w.crossing.size())
                    w.crossing[val] = FnAddrCrossing::InlineAsm;
            }
        }
    }

    /* Y por que parametros deja cruzar ESTA funcion: si lo que cruza vino de
     * un parametro, quien la llame tiene que saberlo. */
    for (size_t i = 0; i < w.crossing.size(); ++i) {
        if (w.crossing[i] == FnAddrCrossing::None) continue;
        FnAddrSite s;
        s.value = static_cast<ir::IrValueId>(i);
        s.crossing = w.crossing[i];
        /* Sale YA ORDENADO: el recorrido va por value-id ascendente, asi que
         * no hay que ordenarlo despues para poder buscarlo por biseccion. */
        out.sites.push_back(s);
        const int32_t pidx =
            (i < facts.param_of.size()) ? facts.param_of[i] : -1;
        if (pidx >= 0) out.crossing_params.push_back(pidx);
    }
    out.fn_addr_count = w.fn_addr_count;
    std::sort(out.crossing_params.begin(), out.crossing_params.end());
    out.crossing_params.erase(
        std::unique(out.crossing_params.begin(), out.crossing_params.end()),
        out.crossing_params.end());
    return out;
}

namespace {

/// Lo que el punto fijo lleva consigo para poder contestar el oraculo.
struct FixpointCtx {
    FnAddrEscapeModule *acc = nullptr;
};

/// Contesta si @p callee deja cruzar su parametro @p idx, mirando lo que se
/// lleva sabido.  Con NOMBRE: quien contesta sale en el perfil.
bool answer_callee_crosses(void *ctx, const std::string &callee, int32_t idx) {
    auto *c = static_cast<FixpointCtx *>(ctx);
    /* Un callee que no esta en el modulo es DESCONOCIDO, y lo desconocido
     * cruza. */
    const FnAddrEscape *e = c->acc->find(util::intern_name(callee));
    if (e == nullptr) return true;
    return e->param_crosses(idx);
}

} // namespace

FnAddrEscapeModule compute_fn_addr_escape_module(const ir::IrModule &mod,
                                                 FnAddrInputs in) {
    FnAddrEscapeModule acc;
    if (!in.valid()) return acc;
    acc.by_function.reserve(mod.functions.size());

    /* Punto fijo: un parametro deja cruzar si lo que le pasan acaba cruzando,
     * y eso puede necesitar saber lo de OTRA funcion que aun no se ha mirado.
     * Se itera hasta que nadie cambie.
     *
     * Arranca con "ningun parametro cruza" y solo se AÑADE: asi es monotono y
     * termina.  El arranque optimista es seguro porque lo que decide de verdad
     * -- una llamada nativa, un asm, un almacen observable -- no depende del
     * oraculo; el oraculo solo propaga lo ya sabido. */
    for (const ir::IrFunction &f : mod.functions) {
        FnAddrEscapeOfFunction e;
        e.function = util::intern_name(f.name);
        acc.by_function.push_back(std::move(e));
    }
    std::sort(
        acc.by_function.begin(), acc.by_function.end(),
        [](const FnAddrEscapeOfFunction &a, const FnAddrEscapeOfFunction &b) {
            return a.function < b.function;
        });

    /* Los nombres se internan UNA vez, antes del punto fijo, y DoNDE esta la
     * ranura de cada funcion tambien.  Buscarla dentro -- recorriendo las
     * ranuras hasta dar con la del nombre -- es el producto de las funciones
     * por si mismas, y con decenas de miles en un modulo fusionado eso no
     * termina. */
    std::vector<size_t> slot_of(mod.functions.size(), 0u);
    for (size_t i = 0; i < mod.functions.size(); ++i) {
        const std::string *key = util::intern_name(mod.functions[i].name);
        const auto it = std::lower_bound(
            acc.by_function.begin(), acc.by_function.end(), key,
            [](const FnAddrEscapeOfFunction &e, const std::string *n) {
                return e.function < n;
            });
        slot_of[i] = static_cast<size_t>(it - acc.by_function.begin());
    }

    /* Que memoria entrega el programa a codigo nativo: UNA vez, fuera del
     * punto fijo.  No depende de lo que el punto fijo va aprendiendo -- un
     * global se entrega o no se entrega --, asi que recalcularlo en cada
     * vuelta seria recorrer el modulo entero por nada. */
    const NativeReach reach = compute_native_reach(mod, in);
    /* Y quienes son funciones del modulo, tambien una vez: no cambia. */
    const ModuleFunctionNames fns = collect_module_function_names(mod);

    FixpointCtx fctx;
    fctx.acc = &acc;
    CalleeCrossesParam oracle;
    oracle.ask = &answer_callee_crosses;
    oracle.ctx = &fctx;

    bool changed = true;
    int rounds = 0;
    while (changed && rounds < 8) {
        changed = false;
        ++rounds;
        for (size_t i = 0; i < mod.functions.size(); ++i) {
            const ir::IrFunction &f = mod.functions[i];
            FnAddrEscape fresh = compute_fn_addr_escape(
                f, in.facts(in.ctx, f), in.escape(in.ctx, f),
                in.points_to(in.ctx, f), reach, fns, oracle);
            FnAddrEscapeOfFunction &slot = acc.by_function[slot_of[i]];
            if (slot.escape.crossing_params != fresh.crossing_params)
                changed = true;
            slot.escape = std::move(fresh);
        }
    }
    return acc;
}

} // namespace analysis
