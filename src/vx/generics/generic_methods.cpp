/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file generic_methods.cpp
 * @brief Monomorphizacion de metodos genericos `R metodo<U>(...)` (#4).
 *
 * Un metodo generico se trata como una funcion con `this` explicito cuyo
 * dispatch es SIEMPRE estatico (como C++/Rust: no hay metodo template
 * virtual).  La llamada `obj.metodo<U>(args)` (con U explicito o inferido)
 * dispara la clonacion del @c ClassMethodDecl con U sustituido por el tipo
 * concreto, generando `metodo_<mangle(U)>`, que se anyade al struct/clase
 * (AST + layout) y se baja como un metodo concreto normal.  Cero artefacto
 * generico en runtime; el backend (interp/JIT/AOT) ve un metodo concreto
 * indistinguible de uno escrito a mano.
 *
 * Para evitar invalidar el iterador del bucle de metodos en
 * @c check_functions (la monomorphizacion ocurre mientras se chequea OTRO
 * body, posiblemente del mismo struct/clase), el metodo clonado se anyade
 * al LAYOUT inmediatamente (para resolver la llamada actual) pero se ENCOLA
 * para anyadirlo al AST + chequear su body en un drenado posterior
 * (@c drain_pending_method_monos), que repite hasta punto fijo.
 */

#include "vx/type_checker.h"

#include "vx/generics/generic_clone.h"

namespace vx {
using namespace vxgen;

const ast::ClassMethodDecl *TypeChecker::find_generic_method_template(
    const std::string &container, const std::string &method_name) const {
    // Busqueda directa en mod_.decls del struct/clase `container` y, dentro,
    // del metodo `method_name` con method_type_params no vacios.  Se llama
    // solo en llamadas a metodo con type-args o no resueltas (no es hot
    // path), asi que la busqueda lineal es aceptable.
    for (const auto &d : mod_.decls) {
        if (!d) continue;
        const std::vector<std::unique_ptr<ast::ClassMethodDecl>> *methods =
            nullptr;
        if (d->kind == ast::NodeKind::StructDecl) {
            auto *sd = static_cast<const ast::StructDecl *>(d.get());
            if (sd->name != container) continue;
            methods = &sd->methods;
        } else if (d->kind == ast::NodeKind::ClassDecl) {
            auto *cd = static_cast<const ast::ClassDecl *>(d.get());
            if (cd->name != container) continue;
            methods = &cd->methods;
        } else {
            continue;
        }
        for (const auto &m : *methods) {
            if (m && m->name == method_name && !m->method_type_params.empty())
                return m.get();
        }
        // Contenedor encontrado pero sin ese metodo generico: no seguir.
        return nullptr;
    }
    return nullptr;
}

std::string TypeChecker::monomorphize_method(const std::string &container,
                                             bool is_struct,
                                             const ast::ClassMethodDecl *tmpl,
                                             const std::vector<Type> &targs,
                                             const SourceLoc &loc) {
    if (tmpl->method_type_params.size() != targs.size()) {
        diags_.diag(loc, DiagLevel::ERR, "VX2093",
                    {tmpl->name,
                     std::to_string(tmpl->method_type_params.size()),
                     std::to_string(targs.size())});
        return std::string();
    }

    const std::string mangled = tmpl->name + "_" + mangle_args(targs);
    const std::string key = container + "#" + mangled;
    if (monomorphized_methods_.count(key)) return mangled; // ya generado
    monomorphized_methods_.insert(key);

    // #6: verificar las constraints del metodo (`R m<U: Concepto>()`) sobre
    // los type-args concretos (una vez por instancia; cero codigo emitido).
    check_type_bounds(tmpl->type_bounds, tmpl->method_type_params, targs, loc);

    GenSubst g{&tmpl->method_type_params, &targs};

    // Clon del ClassMethodDecl con U sustituido por el tipo concreto.
    auto cloned = std::make_unique<ast::ClassMethodDecl>();
    cloned->loc = tmpl->loc;
    cloned->name = mangled;
    cloned->access = tmpl->access;
    cloned->is_static = tmpl->is_static;
    cloned->is_final = tmpl->is_final;
    cloned->is_inline = tmpl->is_inline;
    // is_constructor / is_destructor / advice / property: un metodo
    // generico siempre es un metodo de instancia/estatico normal.
    // method_type_params queda VACIO: el clon ya es concreto.
    if (tmpl->return_type)
        cloned->return_type = clone_type_with_subst(tmpl->return_type.get(), g);
    for (const auto &p : tmpl->params) {
        auto np = std::make_unique<ast::ParamDecl>();
        np->loc = p->loc;
        np->name = p->name;
        np->is_expr_capture = p->is_expr_capture;
        np->type = clone_type_with_subst(p->type.get(), g);
        cloned->params.push_back(std::move(np));
    }
    if (tmpl->body) {
        auto cb = clone_stmt(tmpl->body.get(), g);
        if (cb && cb->kind == ast::NodeKind::BlockStmt)
            cloned->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
    }

    // Construir el ClassMethodInfo concreto y anyadirlo al layout AHORA,
    // para que la resolucion de la llamada que dispara esta
    // monomorphizacion lo encuentre (reescribiremos field_name al mangled).
    ClassMethodInfo mi;
    mi.name = mangled;
    mi.is_static = cloned->is_static;
    mi.is_final = cloned->is_final;
    mi.is_inline = cloned->is_inline;
    mi.defining_class = container;
    mi.source_file = tmpl->loc.file();
    mi.source_line = tmpl->loc.line;
    mi.return_type = cloned->return_type
                         ? type_from_node(cloned->return_type.get())
                         : Type{PrimitiveKind::VOID};
    mi.param_types.reserve(cloned->params.size());
    for (const auto &p : cloned->params)
        mi.param_types.push_back(type_from_node(p->type.get()));

    /* Entra al layout y se CIERRA el recien llegado: aparece DESPUES del cierre
     * del tipo, asi que sin esto se quedaba sin simbolo y quien lo emitia tenia
     * que armarselo -- que es justo lo que dejo de haber --.  Se cierra EL, no
     * el layout entero: instanciar un metodo generico no puede costar el
     * cuadrado de los metodos del tipo cada vez.
     *
     * Y se le apunta en QUE hueco cae, como a cualquier otro metodo: es por ahi
     * por donde quien emite el cuerpo llega a su simbolo. */
    if (is_struct) {
        auto it = struct_layouts_.find(container);
        if (it == struct_layouts_.end()) return std::string();
        // Los structs no usan vtable (dispatch estatico); vtable_index
        // queda en 0 (irrelevante).
        const size_t slot = it->second.methods.size();
        cloned->layout_slot = static_cast<uint32_t>(slot);
        it->second.methods.push_back(std::move(mi));
        close_method(it->second.methods, slot, container, /*ctor_arity=*/true);
    } else {
        auto it = class_layouts_.find(container);
        if (it == class_layouts_.end()) return std::string();
        // Metodo PROPIO nuevo al final: vtable_index = tamano actual,
        // identico a como collect asigna un metodo propio recien anyadido.
        const size_t slot = it->second.methods.size();
        mi.vtable_index = static_cast<uint32_t>(slot);
        cloned->layout_slot = mi.vtable_index;
        it->second.methods.push_back(std::move(mi));
        close_method(it->second.methods, slot, container, /*ctor_arity=*/false);
    }

    // Encolar para anyadir al AST del contenedor + chequear el body en el
    // drenado posterior (no aqui, para no invalidar el iterador del bucle
    // de metodos que pueda estar activo en check_functions).
    PendingMethodMono pm;
    pm.container = container;
    pm.is_struct = is_struct;
    pm.method = std::move(cloned);
    pending_method_monos_.push_back(std::move(pm));
    return mangled;
}

TypeChecker::GenericMethodCall TypeChecker::try_monomorphize_method_call(
    ast::CallExpr *e, ast::FieldAccessExpr *fa, const Type &bt) {
    if (bt.kind != PrimitiveKind::STRUCT && bt.kind != PrimitiveKind::CLASS)
        return GenericMethodCall::NotGeneric;
    const bool is_struct = (bt.kind == PrimitiveKind::STRUCT);
    const std::string &container = bt.struct_name;

    const ast::ClassMethodDecl *tmpl =
        find_generic_method_template(container, fa->field_name);
    if (!tmpl) return GenericMethodCall::NotGeneric; // resolucion normal

    /* Tenerlo no cierra la pregunta, igual que con uno corriente: si ademas
     * hay una libre que toma este receptor, hay dos candidatos y no se elige
     * en silencio.  Es la regla 2.2, y se declara con la MISMA funcion que la
     * declara desde las otras dos grafias.
     *
     * Faltaba justo aqui, y por eso el choque dependia de si el metodo era
     * GENERICO: con uno normal la llamada no compilaba, y con uno generico
     * ganaba el metodo callando y la libre se quedaba sin llamar nunca. */
    if (report_ufcs_clash(bt, fa->field_name, written_type_name(bt), e->loc))
        return GenericMethodCall::Failed; // ya se dijo; no se instancia nada

    // Resolver los type-args: explicitos (`obj.m<U>()`) o inferidos del
    // tipo de los argumentos (`obj.m(x)` con U == tipo del param x).
    std::vector<Type> targs;
    if (!e->type_args.empty()) {
        // EXPLICITOS: resolver tal cual.  La validacion del numero correcto
        // de type-args la hace monomorphize_method (mensaje preciso).
        targs.reserve(e->type_args.size());
        for (auto &ta : e->type_args)
            targs.push_back(resolve_type_node(ta.get()));
    } else {
        /* INFERIDOS, con la MISMA deduccion que una funcion libre generica: es
         * la misma pregunta, y tenerla escrita dos veces acabaria deduciendo
         * distinto para la misma firma segun se llame por el punto o por su
         * nombre -- con lo que dejarian de ser la misma llamada.
         *
         * La clave lleva el duenyo delante porque dos tipos pueden declarar un
         * metodo generico con el mismo nombre y otra firma. */
        const bool ok = deduce_call_type_args(e, tmpl, tmpl->method_type_params,
                                              tmpl->params, targs);
        if (!ok) {
            report_type_args_not_deduced(e->loc, fa->field_name, tmpl,
                                         tmpl->method_type_params, targs);
            return GenericMethodCall::Failed;
        }
    }

    const std::string mangled =
        monomorphize_method(container, is_struct, tmpl, targs, e->loc);
    if (mangled.empty()) return GenericMethodCall::Failed;

    // Reescribir la llamada al metodo concreto; la resolucion normal de
    // check_call (mas abajo) lo encuentra ya en el layout.
    fa->field_name = mangled;
    e->type_args.clear();
    return GenericMethodCall::Rewritten;
}

void TypeChecker::drain_pending_method_monos() {
    // Punto fijo: un metodo generico puede llamar a otro, encolando mas
    // durante el chequeo de su body.  Cota dura defensiva contra bucles.
    for (int round = 0; round < 64 && !pending_method_monos_.empty(); ++round) {
        // Tomar el lote actual y vaciar la cola: las nuevas
        // monomorphizaciones (de los bodies que chequeamos) van a la cola
        // recien vaciada y se procesan en la siguiente ronda.
        std::vector<PendingMethodMono> batch;
        batch.swap(pending_method_monos_);

        for (auto &pm : batch) {
            // Localizar el AST del contenedor y anyadirle el metodo clonado.
            ast::ClassMethodDecl *m_raw = nullptr;
            for (auto &d : mod_.decls) {
                if (!d) continue;
                if (pm.is_struct && d->kind == ast::NodeKind::StructDecl) {
                    auto *sd = static_cast<ast::StructDecl *>(d.get());
                    if (sd->name != pm.container) continue;
                    sd->methods.push_back(std::move(pm.method));
                    m_raw = sd->methods.back().get();
                    break;
                }
                if (!pm.is_struct && d->kind == ast::NodeKind::ClassDecl) {
                    auto *cd = static_cast<ast::ClassDecl *>(d.get());
                    if (cd->name != pm.container) continue;
                    cd->methods.push_back(std::move(pm.method));
                    m_raw = cd->methods.back().get();
                    break;
                }
            }
            if (!m_raw || !m_raw->body) continue;

            // Chequear el body con el contexto del contenedor (current_*
            // guia check_this/visibilidad como en check_functions).
            if (pm.is_struct) {
                auto it = struct_layouts_.find(pm.container);
                if (it == struct_layouts_.end()) continue;
                const std::string saved = current_struct_;
                current_struct_ = pm.container;
                check_struct_method(it->second, m_raw);
                current_struct_ = saved;
            } else {
                auto it = class_layouts_.find(pm.container);
                if (it == class_layouts_.end()) continue;
                const std::string saved = current_class_;
                current_class_ = pm.container;
                check_class_method(it->second, m_raw);
                current_class_ = saved;
            }
        }
    }
}

} // namespace vx
