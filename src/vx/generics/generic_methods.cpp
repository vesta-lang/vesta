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
#include "vx/generics/member_clone.h" // la unica copia de un metodo

namespace vx {
using namespace vxgen;

const std::vector<std::unique_ptr<ast::ClassMethodDecl>> *
TypeChecker::declared_methods_of(const std::string &container) {
    // Indexar lo que se haya anyadido desde la ultima consulta.
    for (; aggregate_decls_indexed_ < mod_.decls.size();
         ++aggregate_decls_indexed_) {
        const auto &d = mod_.decls[aggregate_decls_indexed_];
        if (!d) continue;
        const std::string *name = nullptr;
        if (d->kind == ast::NodeKind::StructDecl)
            name = &static_cast<const ast::StructDecl *>(d.get())->name;
        else if (d->kind == ast::NodeKind::ClassDecl)
            name = &static_cast<const ast::ClassDecl *>(d.get())->name;
        if (name)
            aggregate_decl_of_.emplace(
                util::InternedName::intern(*name),
                static_cast<uint32_t>(aggregate_decls_indexed_));
    }
    const auto it =
        aggregate_decl_of_.find(util::InternedName::intern(container));
    if (it == aggregate_decl_of_.end()) return nullptr;
    const ast::Node *d = mod_.decls[it->second].get();
    return d->kind == ast::NodeKind::StructDecl
               ? &static_cast<const ast::StructDecl *>(d)->methods
               : &static_cast<const ast::ClassDecl *>(d)->methods;
}

const ast::ClassMethodDecl *
TypeChecker::find_generic_method_template(const std::string &container,
                                          const std::string &method_name) {
    const auto *methods = declared_methods_of(container);
    if (methods == nullptr) return nullptr;
    for (const auto &m : *methods)
        if (m && m->name == method_name && !m->method_type_params.empty())
            return m.get();
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

    /* Con homonimas genericas en el tipo, el nombre lleva ademas lo que las
     * separa, igual que una funcion libre: sin eso `f<T>(u64)` y
     * `f<T>(u64, u64)` daban las dos `f_i64` y la segunda se tomaba por ya
     * generada. */
    std::string mangled = generic_instance_name(tmpl->name, targs);
    {
        size_t homonyms = 0;
        if (const auto *methods = declared_methods_of(container))
            for (const auto &m : *methods)
                if (m && m->name == tmpl->name &&
                    !m->method_type_params.empty())
                    ++homonyms;
        if (homonyms > 1)
            mangled += "_" + homonym_discriminator(
                                 tmpl->params, tmpl->method_type_params, targs);
    }
    const std::string key = container + "#" + mangled;
    if (monomorphized_methods_.count(key)) return mangled; // ya generado
    monomorphized_methods_.insert(key);

    // #6: verificar las constraints del metodo (`R m<U: Concepto>()`) sobre
    // los type-args concretos (una vez por instancia; cero codigo emitido).
    check_type_bounds(tmpl->type_bounds, tmpl->method_type_params, targs, loc);

    GenSubst g{&tmpl->method_type_params, &targs};

    // Clon del ClassMethodDecl con U sustituido por el tipo concreto: la
    // misma copia que cualquier otro metodo.  Ya es concreto, asi que sin
    // parametros de tipo ni cotas propias (se acaban de comprobar).
    auto cloned =
        vxgen::clone_method_with_subst(*tmpl, g, vxgen::MethodBodyCopy::Clone);
    cloned->name = mangled;
    cloned->method_type_params.clear();
    cloned->type_bounds.clear();
    // Los @complexity/huella cuyo `when:` habla de U: aqui U ya es concreto.
    resolve_pending_complexity_(*cloned, *tmpl, g, loc);

    // Al layout AHORA, para que la resolucion de la llamada que dispara esta
    // monomorphizacion lo encuentre (reescribiremos field_name al mangled).
    if (add_method_late(container,
                        is_struct ? PrimitiveKind::STRUCT : PrimitiveKind::CLASS,
                        std::move(cloned)) == nullptr)
        return std::string();
    return mangled;
}

const ClassMethodInfo *
TypeChecker::add_method_late(const std::string &container, PrimitiveKind kind,
                             std::unique_ptr<ast::ClassMethodDecl> method) {
    const bool is_struct = kind == PrimitiveKind::STRUCT;
    ClassMethodInfo mi = make_method_info(*method, container);
    std::vector<ClassMethodInfo> *methods = nullptr;

    /* Entra al layout y se CIERRA el recien llegado: aparece DESPUES del cierre
     * del tipo, asi que sin esto se quedaba sin simbolo y quien lo emitia tenia
     * que armarselo -- que es justo lo que dejo de haber --.  Se cierra EL, no
     * el layout entero: instanciar un metodo generico no puede costar el
     * cuadrado de los metodos del tipo cada vez.
     *
     * Y se le apunta en QUE hueco cae, como a cualquier otro metodo: es por ahi
     * por donde quien emite el cuerpo llega a su simbolo. */
    size_t slot = 0;
    if (is_struct) {
        auto it = struct_layouts_.find(container);
        if (it == struct_layouts_.end()) return nullptr;
        // Los structs no usan vtable (dispatch estatico); vtable_index
        // queda en 0 (irrelevante).
        methods = &it->second.methods;
        slot = methods->size();
    } else {
        auto it = class_layouts_.find(container);
        if (it == class_layouts_.end()) return nullptr;
        // Metodo PROPIO nuevo al final: vtable_index = tamano actual,
        // identico a como collect asigna un metodo propio recien anyadido.
        methods = &it->second.methods;
        slot = methods->size();
        mi.vtable_index = static_cast<uint32_t>(slot);
    }
    method->layout_slot = static_cast<uint32_t>(slot);
    methods->push_back(std::move(mi));
    close_method(*methods, slot, container, /*ctor_arity=*/is_struct);

    // Encolar para anyadir al AST del contenedor + chequear el body en el
    // drenado posterior (no aqui, para no invalidar el iterador del bucle
    // de metodos que pueda estar activo en check_functions).
    PendingMethodMono pm;
    pm.container = container;
    pm.is_struct = is_struct;
    pm.method = std::move(method);
    pending_method_monos_.push_back(std::move(pm));
    return &(*methods)[slot];
}

TypeChecker::GenericMethodCall TypeChecker::try_monomorphize_method_call(
    ast::CallExpr *e, ast::FieldAccessExpr *fa, const Type &bt) {
    if (bt.kind != PrimitiveKind::STRUCT && bt.kind != PrimitiveKind::CLASS)
        return GenericMethodCall::NotGeneric;
    const bool is_struct = (bt.kind == PrimitiveKind::STRUCT);
    const std::string &container = bt.struct_name;

    /* CUAL de las homonimas: las genericas del tipo con ese nombre, elegidas
     * por el MISMO nucleo que elige entre funciones libres homonimas.  Antes
     * se cogia la primera, con su aridad y no con la que se pedia. */
    const auto *methods = declared_methods_of(container);
    if (methods == nullptr) return GenericMethodCall::NotGeneric;
    util::SmallVector<GenericCandidate, 4> cands;
    util::SmallVector<const ast::ClassMethodDecl *, 4> decl_of;
    for (const auto &m : *methods)
        if (m && m->name == fa->field_name && !m->method_type_params.empty()) {
            cands.push_back({m.get(), &m->method_type_params, &m->params});
            decl_of.push_back(m.get());
        }
    if (cands.size() == 0) return GenericMethodCall::NotGeneric;
    size_t picked = 0;
    if (cands.size() > 1) {
        const GenericPick how =
            pick_generic_candidate(e, cands.data(), cands.size(), picked);
        if (how != GenericPick::Picked) {
            diags_.diag(e->loc, DiagLevel::ERR,
                        how == GenericPick::NoneFits ? "VX2090" : "VX2091",
                        {fa->field_name});
            return GenericMethodCall::Failed;
        }
    }
    const ast::ClassMethodDecl *tmpl = decl_of[picked];

    /* Tenerlo no cierra la pregunta, igual que con uno corriente: si ademas
     * hay una libre que toma este receptor, hay dos candidatos y no se elige
     * en silencio.  Es la regla 2.2, y se declara con la MISMA funcion que la
     * declara desde las otras dos grafias.
     *
     * Faltaba justo aqui, y por eso el choque dependia de si el metodo era
     * GENERICO: con uno normal la llamada no compilaba, y con uno generico
     * ganaba el metodo callando y la libre se quedaba sin llamar nunca. */
    if (report_ufcs_clash(bt, fa->field_name, written_type_name(bt), e->loc,
                          e->args.size() + 1))
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
            /* Un tipo IMPORTADO no tiene arbol en este modulo: el metodo se
             * aloja en un `impl` sintetico sobre el, que es como se emiten los
             * metodos que este modulo anyade a un tipo de otro.  Antes se
             * tiraba aqui en silencio, y el simbolo faltaba al enlazar. */
            if (m_raw == nullptr && pm.method) {
                auto host = std::make_unique<ast::ImplDecl>();
                host->loc = pm.method->loc;
                host->target_type = pm.container;
                host->methods.push_back(std::move(pm.method));
                m_raw = host->methods.back().get();
                mod_.decls.push_back(std::move(host));
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
