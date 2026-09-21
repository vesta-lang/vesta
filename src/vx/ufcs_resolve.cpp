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
 * @file ufcs_resolve.cpp
 * @brief La llamada uniforme vista desde el comprobador: RESOLVER y REESCRIBIR.
 *
 * @par Que hace cada mitad
 * @c vx/ufcs.cpp es el INDICE -- que candidatas hay para una cabeza de tipo y
 * un nombre --, y no sabe nada del arbol ni del comprobador.  Aqui esta la otra
 * mitad: consultarlo, elegir con @c overload::select y CONVERTIR el nodo a la
 * otra grafia para que lo compruebe el camino de siempre.
 *
 * @par Las DOS direcciones, en un solo sitio
 * UFCS es bidireccional, y las dos mitades viven juntas a proposito:
 *
 *   - `x.f(a)` -> `f(x, a)`: @c try_ufcs_call, con el hueco `_` y el
 *     cualificador `$`.
 *   - `f(x, a)` -> `x.f(a)`: @c try_ufcs_reverse.
 *
 * Con la regla de choque (2.2) COMPARTIDA: @c report_ufcs_clash la declara una
 * vez y las dos direcciones la llaman.  Tenerla escrita dos veces seria tener
 * dos criterios esperando a divergir, y el sintoma es el peor posible -- que el
 * mismo programa compile o no segun como se escriba la llamada.
 *
 * @par Por que un fichero y no unas lineas en el comprobador
 * Lo mismo que se dijo del indice en @c vx/ufcs.h: es una REGLA del lenguaje,
 * no un caso mas de la resolucion de metodos.  Repartida por el comprobador --
 * unas lineas donde falla un metodo de struct, otras donde falla el de una
 * clase, otras donde no hay funcion libre -- el criterio queda en varios
 * sitios.
 */

#include "util/alloc/small_vector.h" // las candidatas, sin pasar por el heap
#include "vx/type_checker.h"

#include "vx/diag/diag_catalog.h"
#include "vx/parser.h" // la lista de builtins que llevan type-args
#include "vx/ufcs.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vx {

bool TypeChecker::base_denotes_type(const ast::Expr *base) const {
    if (base == nullptr) return false;
    /* Un nombre a secas: `Punto.f()`.  Que no sea ademas una VARIABLE es lo
     * que separa el tipo del valor -- `Punto` puede ser las dos cosas si
     * alguien llamo asi a una local, y entonces gana la variable. */
    if (base->kind == ast::NodeKind::IdentExpr) {
        const auto *id = static_cast<const ast::IdentExpr *>(base);
        return lookup(id->name) == nullptr;
    }
    /* O cualificado: `geo.Punto.f()`.  La base de la base tiene que ser un
     * NAMESPACE, no un valor con un campo que se llame como un tipo -- si
     * `obj.Punto` es un campo, `obj.Punto.f()` habla del campo. */
    if (base->kind == ast::NodeKind::FieldAccessExpr) {
        const auto *fa = static_cast<const ast::FieldAccessExpr *>(base);
        if (!fa->base || fa->base->kind != ast::NodeKind::IdentExpr)
            return false;
        const auto *ns = static_cast<const ast::IdentExpr *>(fa->base.get());
        const Symbol *s = lookup(ns->name);
        if (s != nullptr) return s->kind == SymbolKind::Namespace;
        // NS short-form: el ultimo segmento de un namespace importado.
        return ns_idx_by_local_name_.find(ns->name) !=
               ns_idx_by_local_name_.end();
    }
    return false;
}

bool TypeChecker::try_ufcs_type_receiver(ast::CallExpr *e,
                                         ast::FieldAccessExpr *fa,
                                         const std::string &type_name) {
    if (e == nullptr || fa == nullptr) return false;

    /* CUAL es la plantilla.  El nombre se escribe corto (`medida`) y la
     * declaracion vive aplanada (`app__medida`), asi que hay que probar las dos
     * formas -- la del fichero primero, que es la que gana si hay homonimas en
     * otro namespace importado --.  Es el mismo desdoble que hace la resolucion
     * de una llamada libre; sin el, una generica del propio namespace no se
     * encontraba y `Punto.medida()` seguia de largo hasta morir tipando
     * `Punto` como si fuera un valor. */
    std::string plantilla;
    if (!current_ns_prefix_.empty() &&
        is_generic_fn_template(current_ns_prefix_ + fa->field_name)) {
        plantilla = current_ns_prefix_ + fa->field_name;
    } else if (is_generic_fn_template(fa->field_name)) {
        plantilla = fa->field_name;
    } else if (is_comptime_builtin_name(fa->field_name)) {
        /* Y los BUILTIN que llevan argumentos de tipo: es de donde sale que
         * `u64.sizeof()` y `Punto.field_count()` funcionen sin escribir nada
         * para ellos -- son `sizeof<T>()` y `field_count<T>()` con el receptor
         * de argumento --.  La lista es la del parser, no una copia. */
        plantilla = fa->field_name;
    } else {
        const std::string &resuelto = resolve_generic_fn_name(fa->field_name);
        if (!is_generic_fn_template(resuelto)) return false;
        plantilla = resuelto;
    }

    /* El tipo receptor ES el argumento de tipo, asi que no hay nada que
     * deducir: `Punto.medida()` es `medida<Punto>()`.  Si la plantilla declara
     * mas variables queda alguna sin fijar, y de eso ya habla la deduccion con
     * su propio diagnostico -- que dice CUAL falta --, no este. */
    /* El nodo del tipo receptor.  Un PRIMITIVO no es un nombre que resolver:
     * si se pasa como tal, no resuelve a nada y `sizeof<u64>()` contesta CERO
     * en vez de ocho -- un resultado equivocado, no un error. */
    std::unique_ptr<ast::TypeNode> tn;
    const PrimitiveKind prim = primitive_from_name(type_name);
    if (prim != PrimitiveKind::COUNT) {
        auto pn = std::make_unique<ast::PrimitiveTypeNode>();
        pn->loc = fa->loc;
        pn->prim = prim;
        tn = std::move(pn);
    } else {
        auto nn = std::make_unique<ast::NamedTypeNode>();
        nn->loc = fa->loc;
        nn->name = type_name;
        tn = std::move(nn);
    }

    auto id = std::make_unique<ast::IdentExpr>();
    id->loc = fa->loc;
    id->name = plantilla;

    /* Se REESCRIBE, no se resuelve aqui: a partir de este punto es una llamada
     * generica corriente, asi que la eleccion entre homonimas, los argumentos
     * por nombre y el bajado son literalmente el mismo codigo.  Es lo mismo que
     * hace el receptor de valor, y por eso las dos clases de base no pueden
     * divergir. */
    e->callee = std::move(id);
    e->type_args.clear();
    e->type_args.push_back(std::move(tn));
    e->result_type = check_call(e);
    return true;
}

Type TypeChecker::ufcs_key_type(const Type &t) const {
    if (t.kind != PrimitiveKind::STRUCT && t.kind != PrimitiveKind::CLASS)
        return t;
    const auto *mi = monomorph_info(t.struct_name);
    if (mi == nullptr || mi->template_name.empty()) return t;
    Type key = t;
    key.struct_name = mi->template_name;
    return key;
}

bool TypeChecker::report_ufcs_clash(const Type &recv, const std::string &name,
                                    const std::string &owner,
                                    const SourceLoc &loc) {
    /* Solo el INDICE: la pregunta es si ALGUIEN declaro una libre con ese
     * nombre para esta cabeza de tipo, no cual de ellas ganaria.  Es una sonda
     * en una tabla, asi que el metodo cuyo nombre no comparte ninguna libre --
     * que son casi todos -- no paga por esta regla. */
    if (ufcs_.find(ufcs_key_type(recv), name, current_ns_prefix_) == nullptr)
        return false;
    diags_.diag(loc, DiagLevel::ERR, "VX2068", {name, owner});
    return true;
}

bool TypeChecker::report_ufcs_cast_hint(const Type &recv,
                                        const std::string &name,
                                        const SourceLoc &loc) {
    // Solo entre escalares: es donde un cast arregla algo de verdad.
    if (!is_numeric(recv.kind)) return false;

    const ufcs::Candidates *slots = ufcs_.all_named(name);
    if (slots == nullptr) return false;

    /* De las que se llaman asi, las que piden un escalar DISTINTO: esas son
     * las que un cast alcanza, y las que hay que nombrar.  Se quedan las
     * CANDIDATAS, no sus nombres: el nombre se compone una vez al final, y
     * repetido se descarta comparando el TIPO -- que es su identidad -- y no
     * como se escribe, que es solo como se ve. */
    ufcs::Candidates picked;
    for (uint32_t s : *slots) {
        if (s >= function_sigs_.size()) continue;
        const FunctionSig &sig = function_sigs_[s];
        if (sig.param_types.empty()) continue;
        const Type &p = sig.param_types[0];
        if (!is_numeric(p.kind)) continue;
        /* Y que pida OTRO tipo.  Una que ya toma este receptor no se alcanza
         * con un cast -- convertirlo a lo que ya es no dice nada --: si esta y
         * no se encontro, el problema es que no esta en AMBITO, y de eso habla
         * otro aviso. */
        if (p == recv) continue;
        bool seen = false;
        for (uint32_t q : picked)
            if (function_sigs_[q].param_types[0] == p) {
                seen = true;
                break;
            }
        if (!seen) picked.push_back(s);
    }
    if (picked.empty()) return false;

    /* Por orden de DECLARACION, que es el de la ranura: el indice es una tabla
     * hash, asi que sin esto el mismo programa daria el mismo error con los
     * tipos en otro orden segun donde se compile. */
    std::sort(picked.begin(), picked.end());

    std::string list =
        written_type_name(function_sigs_[picked[0]].param_types[0]);
    const std::string first = list;
    for (size_t i = 1; i < picked.size(); ++i)
        list +=
            ", " + written_type_name(function_sigs_[picked[i]].param_types[0]);
    diags_.diag(loc, DiagLevel::ERR, "VX2071",
                {written_type_name(recv), name, list, first});
    return true;
}

bool TypeChecker::report_ufcs_lend_hint(const Type &recv,
                                        const std::string &name,
                                        const SourceLoc &loc) {
    /* Solo si el receptor es un DUENYO.  Es lo unico desde donde se puede
     * prestar, y lo que hace que la salida sea `lend` y no un cast. */
    if (recv.kind != PrimitiveKind::UNIQUE_PTR &&
        recv.kind != PrimitiveKind::SHARED_PTR)
        return false;
    if (!recv.pointee) return false;

    const ufcs::Candidates *slots = ufcs_.all_named(name);
    if (slots == nullptr) return false;

    /* De las que se llaman asi, la que pide un prestamo DE LO MISMO.  Si pide
     * otra cosa, prestar no acerca nada y de eso no hay que hablar aqui. */
    for (uint32_t s : *slots) {
        if (s >= function_sigs_.size()) continue;
        const FunctionSig &sig = function_sigs_[s];
        if (sig.param_types.empty()) continue;
        const Type &p = sig.param_types[0];
        const bool mut = p.kind == PrimitiveKind::BORROW_MUT;
        if (!mut && p.kind != PrimitiveKind::BORROW) continue;
        if (!p.pointee || !(*p.pointee == *recv.pointee)) continue;
        diags_.diag(loc, DiagLevel::ERR, "VX2127",
                    {written_type_name(recv), name, written_type_name(p),
                     mut ? "lend_mut" : "lend"});
        return true;
    }
    return false;
}

bool TypeChecker::report_ufcs_ns_hint(const Type &recv, const ast::Expr *base,
                                      const std::string &name,
                                      const SourceLoc &loc) {
    const std::string *declared = nullptr;
    const ufcs::Candidates *slots = ufcs_.all_named(name, &declared);
    if (slots == nullptr || declared == nullptr) return false;
    /* Solo si se declaro con OTRO nombre del que se escribio: eso es que vive
     * en un namespace, y que la busqueda no lo alcanzo es que no es el de
     * aqui.  Si coincide, el nombre si estaba en ambito y lo que fallo fue el
     * receptor, que es lo que cuenta el otro aviso. */
    if (*declared == name) return false;
    const auto it = declared_ns_symbols_.find(*declared);
    if (it == declared_ns_symbols_.end()) return false;

    for (uint32_t s : *slots) {
        if (s >= function_sigs_.size()) continue;
        const FunctionSig &sig = function_sigs_[s];
        if (sig.param_types.empty()) continue;
        const Type &p = sig.param_types[0];
        const bool promoted = p.kind == PrimitiveKind::STRING &&
                              ufcs_promotes_to_string(recv, base);
        if (!(p == recv) && !promoted) continue;
        diags_.diag(
            loc, DiagLevel::ERR, "VX2080",
            {written_type_name(promoted ? p : recv), name, it->second.first});
        return true;
    }
    return false;
}

bool TypeChecker::ufcs_promotes_to_string(const Type &recv,
                                          const ast::Expr *base) {
    return recv.kind == PrimitiveKind::PTR && base != nullptr &&
           base->kind == ast::NodeKind::StringLitExpr;
}

bool TypeChecker::report_ufcs_import_hint(const Type &recv,
                                          const ast::Expr *base,
                                          const std::string &name,
                                          const SourceLoc &loc) {
    /* Que namespaces traen este nombre se PREGUNTA, no se busca: lo apunto
     * `register_namespace_symbol` al meterlos, que es donde el dato estaba en
     * la mano.  Antes se recorrian todos preguntando uno por uno. */
    const auto brought = ns_by_fn_name_.find(util::intern_name(name));
    if (brought == ns_by_fn_name_.end()) return false;

    for (uint32_t ni : brought->second) {
        const ImportedNamespace &ns = imported_namespaces_[ni];
        const auto it = ns.by_name.find(name);
        if (it == ns.by_name.end()) continue;
        /* Y que ALGUNA de las que traen ese nombre tome de verdad este
         * receptor.  Sin comprobarlo, el mensaje mandaria a importar algo que
         * tampoco sirve, que es peor que no decir nada.  La cadena son las
         * sobrecargas de ESE nombre en ESE namespace: una, casi siempre. */
        for (uint32_t s = it->second; s != ImportedNamespace::kNoHomonym;
             s = ns.symbols[s].next_homonym) {
            const ImportedNamespace::Sym &sym = ns.symbols[s];
            if (sym.kind != 0 || sym.sig.param_types.empty()) continue;
            const Type &p = sym.sig.param_types[0];
            /* Con la MISMA promocion que hace la llamada de verdad: un literal
             * de cadena es un `ptr` que solo se vuelve `string` donde hace
             * falta, y sin contarla el aviso no saltaba justo con las que
             * toman una cadena -- que son la mitad de las candidatas. */
            const bool promoted = p.kind == PrimitiveKind::STRING &&
                                  ufcs_promotes_to_string(recv, base);
            if (!(p == recv) && !promoted) continue;
            /* El receptor se NOMBRA como lo que el usuario escribio: si lo que
             * encajo fue la cadena, decir `ptr` manda a mirar un tipo que el
             * no puso en ningun sitio.  Y el namespace, tal como lo escribio
             * en su `import` -- `std.fileio`, no `fileio` --, que es lo unico
             * que al teclearlo resuelve. */
            diags_.diag(
                loc, DiagLevel::ERR, "VX2079",
                {written_type_name(promoted ? p : recv), name,
                 ns.local_name.empty() ? ns.module_name : ns.local_name});
            return true;
        }
    }
    return false;
}

size_t TypeChecker::ufcs_receiver_hole(ast::CallExpr *e) {
    // El parser ya dijo si hay alguno, asi que una llamada corriente -- que
    // son casi todas -- no recorre sus argumentos para descubrir que no.
    if (!e->has_receiver_hole) return kUfcsNoHole;
    size_t found = kUfcsNoHole;
    for (size_t i = 0; i < e->args.size(); ++i) {
        const ast::Expr *a = e->args[i].get();
        if (a == nullptr || a->kind != ast::NodeKind::IdentExpr) continue;
        if (static_cast<const ast::IdentExpr *>(a)->name != "_") continue;
        if (found != kUfcsNoHole) {
            diags_.diag(a->loc, DiagLevel::ERR, "VX2072", {});
            return kUfcsHoleBad;
        }
        found = i;
    }
    return found;
}

bool TypeChecker::try_ufcs_qualified(ast::CallExpr *e,
                                     ast::FieldAccessExpr *fa) {
    if (e == nullptr || fa == nullptr || !fa->base) return false;
    /* Que lo escrito tras el `$` SEA un namespace se comprueba aqui y no al
     * reescribir: el camino de la llamada cualificada no sabe que hubo un `$`,
     * asi que dice "nombre no declarado: 'geo'" -- senyalando el primer
     * segmento y sin nombrar la calificacion, que es lo unico que el usuario
     * escribio de mas. */
    if (ns_idx_by_local_name_.find(fa->ns_qualifier) ==
        ns_idx_by_local_name_.end()) {
        diags_.diag(fa->loc, DiagLevel::ERR, "VX2081",
                    {fa->ns_qualifier, fa->field_name});
        for (auto &a : e->args)
            (void)check_expr(a.get());
        e->result_type = Type{PrimitiveKind::COUNT};
        return true;
    }
    const size_t hole = ufcs_receiver_hole(e);
    if (hole == kUfcsHoleBad) {
        // Ya se dijo por que; la llamada se da por contestada.
        e->result_type = Type{PrimitiveKind::COUNT};
        return true;
    }

    /* La calificacion se convierte en la llamada cualificada de SIEMPRE:
     * `6.doble$geo.metrico()` pasa a ser `geo.metrico.doble(6)`, que es el
     * nodo que el parser habria armado para esa otra grafia.
     *
     * Reescribir en vez de resolver aqui es lo que hace que las dos no puedan
     * divergir: quien resuelve el namespace, elige entre sobrecargas y
     * comprueba los argumentos es literalmente el mismo codigo, y al bajado no
     * le llega nada nuevo. */
    std::unique_ptr<ast::Expr> callee;
    size_t start = 0;
    while (start <= fa->ns_qualifier.size()) {
        const size_t dot = fa->ns_qualifier.find('.', start);
        const size_t end =
            (dot == std::string::npos) ? fa->ns_qualifier.size() : dot;
        std::string segment = fa->ns_qualifier.substr(start, end - start);
        if (!callee) {
            auto id = std::make_unique<ast::IdentExpr>();
            id->loc = fa->loc;
            id->name = std::move(segment);
            callee = std::move(id);
        } else {
            auto step = std::make_unique<ast::FieldAccessExpr>();
            step->loc = fa->loc;
            step->base = std::move(callee);
            step->field_name = std::move(segment);
            callee = std::move(step);
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    auto target = std::make_unique<ast::FieldAccessExpr>();
    target->loc = fa->loc;
    target->base = std::move(callee);
    target->field_name = fa->field_name;

    std::unique_ptr<ast::Expr> receiver = std::move(fa->base);
    e->callee = std::move(target);
    /* Y el receptor a su sitio, con la misma regla que sin calificar: delante
     * por defecto, y en el hueco si se escribio uno. */
    if (hole == kUfcsNoHole) {
        e->args.insert(e->args.begin(), std::move(receiver));
        if (!e->arg_names.empty()) e->arg_names.insert_at(0, PooledName());
    } else {
        e->args[hole] = std::move(receiver);
    }
    e->result_type = check_call(e);
    return true;
}

bool TypeChecker::try_ufcs_call(ast::CallExpr *e, ast::FieldAccessExpr *fa,
                                const Type &recv) {
    if (e == nullptr || fa == nullptr || !fa->base) return false;
    /* QUE candidatas hay lo dice el indice de UFCS, que las correlaciono al
     * DECLARARLAS por la cabeza del tipo de su primer parametro.  Aqui no se
     * recorre nada: dos punteros ya internados y una sonda. */
    const size_t hole_pre = ufcs_receiver_hole(e);
    const std::string *chosen = nullptr; // con QUE nombre se declaro
    /* Con hueco el receptor no cae en el primer parametro, asi que la pregunta
     * es otra: "cual tiene ALGUN parametro que lo admita".  Sin hueco sigue
     * siendo "cual lo toma de primero", que es lo que la regla 2.2 mira. */
    const ufcs::Candidates *cand_slots =
        (hole_pre != kUfcsNoHole && hole_pre != kUfcsHoleBad)
            ? ufcs_.find_any(ufcs_key_type(recv), fa->field_name,
                             current_ns_prefix_, &chosen)
            : ufcs_.find(ufcs_key_type(recv), fa->field_name,
                         current_ns_prefix_, &chosen);
    /* Un literal de cadena es un `ptr` a datos estaticos y solo se PROMUEVE a
     * `string` donde hace falta -- por eso `grita("hola")` compila --, asi que
     * si no hay nada para el puntero se pregunta tambien por la cadena.  Sin
     * esto `"hola".grita()` no encontraria lo que `grita("hola")` si encuentra.
     * Se prueba en este orden porque el puntero es lo que el literal ES y la
     * cadena lo que puede llegar a ser. */
    if (cand_slots == nullptr && ufcs_promotes_to_string(recv, fa->base.get()))
        cand_slots =
            (hole_pre != kUfcsNoHole && hole_pre != kUfcsHoleBad)
                ? ufcs_.find_any(Type{PrimitiveKind::STRING}, fa->field_name,
                                 current_ns_prefix_, &chosen)
                : ufcs_.find(Type{PrimitiveKind::STRING}, fa->field_name,
                             current_ns_prefix_, &chosen);

    /* Y si tampoco, por el PUNTERO.  Un primer parametro se puede declarar por
     * valor o por puntero, y el receptor tiene que valer donde una llamada
     * libre valdria (6-bis.5): `f(r)` con `r : P*` compila, asi que `r.f()`
     * tiene que compilar tambien, o las dos grafias dejan de ser la misma
     * llamada.
     *
     * Aqui llega ya deshecho un nivel -- el punto auto-desreferencia para
     * despachar sobre lo apuntado, que es lo que hace que `r.x` funcione --,
     * asi que buscar solo por lo apuntado no encontraba NINGUNA libre que
     * pidiera puntero.
     *
     * Las dos situaciones se resuelven con la misma sonda y se distinguen por
     * lo que hay que hacerle a la base:
     *
     *   r : P*  ->  la base YA es el puntero: se pasa tal cual.
     *   q : P   ->  hay que tomarle la direccion, `&q`.
     *
     * Y tomarla sola no es una comodidad que se anyade: es lo que el MIEMBRO ya
     * hace -- `q.metodo()` de un struct recibe `P*` y modifica `q` --, asi que
     * exigir sintaxis solo cuando la funcion esta declarada fuera haria que el
     * mismo programa se escriba distinto segun donde este. */
    Type recv_efectivo = recv;
    bool tomar_direccion = false;
    if (cand_slots == nullptr || cand_slots->empty()) {
        const Type &real = fa->base->result_type;
        const bool base_es_ptr = real.kind == PrimitiveKind::PTR;
        Type como_ptr = base_es_ptr ? real : Type::make_ptr(recv);
        const ufcs::Candidates *por_ptr =
            (hole_pre != kUfcsNoHole && hole_pre != kUfcsHoleBad)
                ? ufcs_.find_any(ufcs_key_type(como_ptr), fa->field_name,
                                 current_ns_prefix_, &chosen)
                : ufcs_.find(ufcs_key_type(como_ptr), fa->field_name,
                             current_ns_prefix_, &chosen);
        if (por_ptr != nullptr && !por_ptr->empty()) {
            cand_slots = por_ptr;
            recv_efectivo = std::move(como_ptr);
            tomar_direccion = !base_es_ptr;
        }
    }
    /* Un BUILTIN del lenguaje tras el punto tambien es una llamada uniforme.
     *
     * La mayoria estan en el indice porque se registran con una firma
     * (`free`, `sqrt`, `malloc`...), pero los POLIMORFICOS no: `unwrap` vale
     * sobre cualquier `Optional<T>` y `ptr_of` sobre cualquier `unique<T>`,
     * asi que no tienen un primer parametro de tipo concreto que indexar y el
     * comprobador los resuelve por su cuenta.  Sin esto, `o.unwrap()` decia
     * que no hay ninguna funcion que tome un `Optional<i64>` -- cuando
     * `unwrap(o)`, que es la MISMA llamada, compila --.
     *
     * No hace falta enumerarlos: si el nombre ES un builtin, se reescribe a la
     * grafia libre y decide la comprobacion de siempre.  Si el receptor no le
     * vale, el error habla de tipos, que es lo que pasa de verdad, en vez de
     * negar que el nombre exista. */
    if ((cand_slots == nullptr || cand_slots->empty()) &&
        builtin_from_name(fa->field_name) != Builtin::Unknown) {
        auto callee = std::make_unique<ast::IdentExpr>();
        callee->loc = fa->loc;
        callee->name = fa->field_name;
        std::vector<std::unique_ptr<ast::Expr>> args;
        args.reserve(e->args.size() + 1);
        args.push_back(std::move(fa->base));
        for (auto &a : e->args)
            args.push_back(std::move(a));
        e->callee = std::move(callee);
        e->args = std::move(args);
        /* Y se comprueba como la llamada libre que ahora ES: sin esto el nodo
         * quedaba reescrito pero sin tipo, y el error pasaba a hablar de un
         * `void` que nadie escribio. */
        e->result_type = check_call(e);
        return true;
    }
    /* Sin candidata en el indice, pero CON hueco: la llamada se reescribe y
     * decide el camino de siempre.
     *
     * El indice guarda funciones LIBRES.  Un miembro no esta ahi, y sin
     * embargo es alcanzable en forma libre -- `mezcla(c, 2, 3)` encuentra el
     * metodo de `c`, que es la otra direccion de `2.1` --, asi que con el
     * receptor en medio (`2.mezcla(c, _, 3)`) no habia quien contestara: el
     * punto buscaba un metodo de `i64` o una libre que tomara un `i64`, y
     * ninguna de las dos es la pregunta.
     *
     * Reescribir es la DEFINICION del punto, asi que aqui no se decide nada
     * nuevo: se deja el nodo en su otra grafia y lo resuelve `check_call`, que
     * ya sabe encontrar tanto una libre como un miembro por el tipo de su
     * primer argumento.
     *
     * Va al final a proposito -- despues de todas las sondas del indice --
     * para que una libre con hueco siga pasando por el camino que le toca, con
     * su auto-`&`, su regla del `out` y su choque con el miembro homonimo. */
    if (cand_slots == nullptr || cand_slots->empty()) {
        if (hole_pre == kUfcsNoHole || hole_pre == kUfcsHoleBad) return false;
        const std::string name = fa->field_name;
        /* Si el RECEPTOR declara un miembro con ese nombre, se mira ANTES de
         * reescribir -- despues el nodo ya no tiene receptor --.  Es el caso
         * de `c.mezcla(2, _)`: la forma libre del miembro tiene una ranura mas
         * (el receptor) y la llamada solo da las otras, asi que el error que
         * saldra abajo sera cierto y se callara lo util -- que el nombre SI
         * existe, y donde --.  UNA consulta: la lista y la busqueda salen de
         * la misma llamada. */
        std::string owner;
        const std::vector<ClassMethodInfo> *ms =
            receiver_methods(recv, nullptr, &owner, nullptr);
        const bool era_miembro =
            ms != nullptr && find_instance_method(*ms, name) != nullptr;

        auto id = std::make_unique<ast::IdentExpr>();
        id->loc = fa->loc;
        id->name = name;
        /* El receptor se toma ANTES de tocar el callee: `fa` vive dentro de
         * el, asi que sustituirlo primero se lleva por delante su base. */
        std::unique_ptr<ast::Expr> receiver = std::move(fa->base);
        e->callee = std::move(id);
        e->args[hole_pre] = std::move(receiver); // el hueco ERA su sitio
        /* Y deja de haberlo: en su sitio esta el receptor.  Sin borrar la
         * marca, la llamada reescrita sigue diciendo que lleva hueco, y quien
         * la resuelve despues -- que puede acabar en un metodo del tipo del
         * primer argumento -- se niega a tomar el miembro por esa misma
         * regla.  La marca describe el nodo, asi que se actualiza con el. */
        e->has_receiver_hole = false;

        const size_t errors_before = diags_.error_count();
        e->result_type = check_call(e);
        if (era_miembro && diags_.error_count() > errors_before)
            diags_.diag(e->loc, DiagLevel::NOTE, "VX2120", {name, owner});
        return true;
    }

    /* DONDE cae el receptor.  Por defecto delante -- `x.f(a)` es `f(x, a)` --,
     * y en el hueco si se escribio uno: `x.f(a, _)` es `f(a, x)`.  Eso es lo
     * que permite llamar por el punto a una firma cuyo primer parametro no es
     * el sujeto (`memcpy(dst, src, n)`) sin retorcer la firma, que era el
     * precio que la seccion 5 del plan daba por inevitable.
     *
     * Es una REORDENACION, y ahi acaba: a partir de aqui todo es posicional
     * como siempre, asi que la seleccion de sobrecarga no se entera.  Tocarla
     * puede cambiar en silencio a que cuerpo va un programa ya escrito. */
    const size_t hole = hole_pre;
    if (hole == kUfcsHoleBad) {
        /* Ya se dijo por que, asi que la llamada se da por CONTESTADA: dejarla
         * seguir la manda al camino de "no existe tal metodo", que sugiere un
         * cast para un problema que no es ese. */
        e->result_type = Type{PrimitiveKind::COUNT};
        return true;
    }
    const size_t at = (hole == kUfcsNoHole) ? 0 : hole;

    std::vector<Type> arg_types;
    arg_types.reserve(e->args.size() + 1);
    for (size_t i = 0; i < e->args.size(); ++i) {
        if (i == at) arg_types.push_back(recv_efectivo);
        if (i == hole) continue; // el hueco NO es un argumento suyo
        arg_types.push_back(check_expr(e->args[i].get()));
    }
    if (at >= e->args.size()) arg_types.push_back(recv_efectivo);

    /* Los nombres, alineados con esa lista.  Con hueco ya lo estan -- el
     * receptor ocupa el sitio del `_`, y `.b = _` dice que va a `b` --; sin
     * hueco el receptor se mete DELANTE, asi que delante va tambien su nombre
     * vacio.  Descuadrar los dos vectores manda cada nombre a la ranura de al
     * lado, que es un error silencioso de los caros. */
    ParamNames names_for_select;
    if (!e->arg_names.empty()) {
        if (hole == kUfcsNoHole) names_for_select.push_back(PooledName());
        for (const PooledName &nm : e->arg_names)
            names_for_select.push_back(nm);
    }

    /* Y CUAL de ellas se elige con la MISMA regla que una llamada libre --
     * exacta antes que compatible --, no con una propia: si aqui se decidiera
     * de otra manera, `x.f(a)` y `f(x, a)` dejarian de ser la misma llamada,
     * que es toda la propuesta. */
    util::SmallVector<overload::Candidate, 4> cands;
    for (uint32_t idx : *cand_slots) {
        if (idx >= function_sigs_.size()) continue;
        const FunctionSig &sig = function_sigs_[idx];
        overload::Candidate c;
        c.params = &sig.param_types;
        c.param_names = &sig.param_names;
        c.needs_names = sig.overload_needs_names;
        c.slot = idx;
        c.by_ref_mask = sig.param_by_ref_mask;
        if (sig.is_raw_variadic)
            c.raw_variadic = true;
        else if (sig.is_variadic)
            c.variadic_elem = &sig.variadic_elem;
        cands.push_back(c);
    }
    const uint32_t pick =
        overload::select(cands.data(), cands.size(), arg_types,
                         &overload_accepts, this, &names_for_select);
    /* Que NINGUNA encaje por tipos no quiere decir que la llamada no exista.
     *
     * Lo que un argumento admite es mas de lo que sabe la regla de seleccion:
     * un literal sin sufijo se re-tipa si cabe, una constante entra en un
     * newtype, un puntero se convierte.  Por eso la llamada LIBRE, en este
     * mismo punto, se queda con la primera candidata y deja que el error --
     * si lo hay -- lo de la comprobacion de argumentos hablando de TIPOS; los
     * metodos hacen lo mismo.
     *
     * Aqui se hacia lo contrario -- darse por no encontrada --, y entonces
     * `memcpy_c(dst, src, 8)` compilaba mientras `dst.memcpy_c(src, 8)` decia
     * que no hay ninguna funcion libre que tome un `u8*`, con un consejo que
     * mandaba a importar lo que ya estaba importado.  El tercer parametro es
     * `usize` y el literal es `i64`: eso lo arregla la conversion, no la
     * seleccion.  Dos grafias de la misma llamada no pueden contestar cosas
     * distintas.
     *
     * Una PLANTILLA nunca encaja aqui, y por la misma razon con otro nombre:
     * sus parametros no resuelven a nada hasta instanciarla.  Quien decide si
     * vale es la DEDUCCION, al comprobar la llamada ya reescrita.
     *
     * Y con VARIAS candidatas se sigue cediendo el paso.  No es lo mismo: con
     * una no hay nada que elegir -- la llamada libre tampoco elige --, pero con
     * varias, quedarse con la primera seria decidir por el programador con un
     * criterio que no esta escrito en el fuente.  Ahi la respuesta la tiene que
     * dar quien todavia puede mirar otras cosas: un metodo del receptor, un
     * campo que sea una funcion, un builtin. */
    if (pick == overload::kNoPick && cand_slots->size() != 1 &&
        (chosen == nullptr || !is_generic_fn_template(*chosen)))
        return false;

    /* Elegida, pero por el punto NO se llama si la ranura del receptor es de
     * SALIDA.  Un `out` dice "esto es un hueco donde escribo", y el receptor de
     * un punto se lee como el SUJETO de la llamada: `s.f()` dejaria `s`
     * modificada sin que nada en la escritura lo aparente.
     *
     * La linea no es "por referencia si o no" -- `inout` va igual de por
     * referencia y SI vale --, es de donde sale el valor: `inout` lee el
     * receptor y lo devuelve modificado, o sea que el receptor es el sujeto;
     * `out` no lo lee siquiera.
     *
     * Se dice POR QUE, no "no existe `s.f`": el nombre existe y la funcion
     * tambien, y mandar a buscarla es mandar donde no es. */
    if (pick != overload::kNoPick && pick < function_sigs_.size()) {
        const FunctionSig &elegida = function_sigs_[pick];
        if (at < elegida.param_dirs.size() &&
            elegida.param_dirs[at] == ParamDir::Out) {
            diags_.diag(
                e->loc, DiagLevel::ERR, "VX2098",
                {written_name(*chosen), at < elegida.param_names.size()
                                            ? elegida.param_names[at].str()
                                            : std::string()});
            e->result_type = Type{PrimitiveKind::COUNT};
            return true; // contestada: ya se dijo por que no
        }
    }

    /* Encontrada: el nodo se convierte en la OTRA grafia y lo comprueba el
     * camino de siempre.  Reescribir en vez de resolver aqui es lo que hace que
     * las dos formas no puedan divergir -- comprobacion de argumentos,
     * prestamos y la firma elegida son literalmente el mismo codigo -- y que al
     * bajado, al JIT y al nativo no les llegue nada nuevo.  Es la operacion
     * inversa de la que ya hace `__call__`, unas lineas mas abajo. */
    auto id = std::make_unique<ast::IdentExpr>();
    id->loc = fa->loc;
    id->name = *chosen;
    std::unique_ptr<ast::Expr> receiver = std::move(fa->base);
    /* La candidata pide un puntero y la base es un valor: se le toma la
     * direccion AQUI, en el nodo, para que a partir de este punto la llamada
     * sea una libre corriente y la comprobacion de argumentos, los prestamos y
     * la bajada sean literalmente el mismo codigo de siempre. */
    if (tomar_direccion) {
        auto addr = std::make_unique<ast::UnaryExpr>();
        addr->loc = receiver->loc;
        addr->op = ast::UnOp::AddrOf;
        addr->operand = std::move(receiver);
        receiver = std::move(addr);
    }
    e->callee = std::move(id);
    if (hole == kUfcsNoHole) {
        e->args.insert(e->args.begin(), std::move(receiver));
        // Y su nombre vacio con el, para que los dos sigan cuadrando.
        if (!e->arg_names.empty()) e->arg_names.insert_at(0, PooledName());
    } else {
        e->args[hole] = std::move(receiver); // el hueco ERA su sitio
    }
    // Consumido: en su sitio esta el receptor.  Ver la nota de mas arriba.
    e->has_receiver_hole = false;
    e->result_type = check_call(e);
    return true;
}

const std::vector<ClassMethodInfo> *
TypeChecker::receiver_methods(const Type &recv, Type *norm, std::string *owner,
                              const char **keyword) {
    Type t = recv;
    /* El punto ya auto-desreferencia un `Struct*` para despachar sobre lo
     * apuntado, asi que aqui tambien: si las dos grafias no miraran lo mismo,
     * `f(p, a)` y `p.f(a)` dejarian de ser la misma llamada justo donde mas se
     * usa, que es el receptor por puntero. */
    if (t.kind == PrimitiveKind::PTR && t.pointee &&
        t.pointee->kind == PrimitiveKind::STRUCT)
        t = *t.pointee;
    const std::vector<ClassMethodInfo> *ms = nullptr;
    const char *kw = nullptr;
    if (t.kind == PrimitiveKind::STRUCT) {
        auto it = struct_layouts_.find(t.struct_name);
        if (it == struct_layouts_.end()) return nullptr;
        ms = &it->second.methods;
        kw = "struct";
    } else if (t.kind == PrimitiveKind::CLASS) {
        auto it = class_layouts_.find(t.struct_name);
        if (it == class_layouts_.end()) return nullptr;
        ms = &it->second.methods;
        kw = "class";
    } else {
        return nullptr;
    }
    if (norm != nullptr) *norm = t;
    /* El nombre como se ESCRIBE, no el que le puso el aplanado: citar
     * `ejemplos__ufcs__Punto` manda a buscar un tipo que no esta en el
     * fichero. */
    if (owner != nullptr) *owner = written_type_name(t);
    if (keyword != nullptr) *keyword = kw;
    return ms;
}

/**
 * @brief El receptor de la grafia libre, si lo hay: el primer argumento.
 *
 * Tiene que ser POSICIONAL.  `this` no es una ranura con nombre, asi que
 * nombrar el primer argumento es decir que ese argumento no es el receptor.
 *
 * @param e La llamada.
 * @return La expresion del receptor, o nullptr si no la hay.
 */
static ast::Expr *ufcs_reverse_receiver(ast::CallExpr *e) {
    if (e == nullptr || e->args.empty() || !e->args[0]) return nullptr;
    if (!e->arg_names.empty() && !e->arg_names[0].empty()) return nullptr;
    return e->args[0].get();
}

/**
 * @struct QuietDiags
 * @brief Calla los diagnosticos mientras vive.
 *
 * Las sondas de esta direccion PREGUNTAN por el tipo del receptor, y preguntar
 * no es comprobar: si la respuesta no sirve, la llamada sigue su camino y sus
 * argumentos se comprueban como siempre.  Sin esto el receptor se comprueba dos
 * veces y sus quejas salen DOS veces -- un `nombre no declarado` duplicado por
 * haberlo mirado --, que es ruido que el usuario no puede explicarse.
 */
struct QuietDiags {
    explicit QuietDiags(Diagnostics &d)
        : d_(d), prev_(d.set_suppressed(true)) {}
    ~QuietDiags() { d_.set_suppressed(prev_); }
    QuietDiags(const QuietDiags &) = delete;
    QuietDiags &operator=(const QuietDiags &) = delete;

  private:
    Diagnostics &d_;
    bool prev_;
};

bool TypeChecker::try_ufcs_reverse(ast::CallExpr *e, const ast::IdentExpr *id) {
    if (id == nullptr) return false;
    ast::Expr *recv_expr = ufcs_reverse_receiver(e);
    if (recv_expr == nullptr) return false;
    // Preguntar el tipo, sin comprobar: ver `QuietDiags`.
    Type recv;
    {
        QuietDiags quiet(diags_);
        recv = check_expr(recv_expr);
    }
    if (recv.kind == PrimitiveKind::COUNT) return false;
    Type norm;
    const std::vector<ClassMethodInfo> *ms =
        receiver_methods(recv, &norm, nullptr, nullptr);
    if (ms == nullptr) return false;
    /* El nombre tal y como se ESCRIBIO.  El aplanado renombra las llamadas al
     * namespace desde el que se hacen -- `crece(r, 2)` llega aqui como
     * `app__crece` --, y un metodo se llama por su nombre corto: sin esto la
     * direccion inversa deja de funcionar en cuanto el fichero declara un
     * namespace, que es casi siempre. */
    const std::string member = written_name(id->name);
    /* Por NOMBRE y ahi acaba la pregunta.  Cual de sus sobrecargas, si son
     * varias, lo decide el camino del punto -- que es el mismo selector que
     * usa una llamada libre --; adelantarlo aqui seria un segundo criterio
     * esperando a divergir.
     *
     * Y un metodo GENERICO no esta en el layout con ese nombre: ahi solo hay
     * instancias ya concretas (`mete_i64`), porque la plantilla se clona al
     * llamarla.  Preguntar solo por el layout hacia decir que el tipo no tiene
     * ese miembro, que es FALSO: lo tiene, es una plantilla. */
    if (find_instance_method(*ms, member) == nullptr &&
        find_generic_method_template(norm.struct_name, member) == nullptr)
        return false;

    /* Encontrado: el nodo se convierte en la OTRA grafia.  A partir de aqui no
     * hay nada propio de esta direccion: los argumentos por nombre, la
     * seleccion entre sobrecargas y la visibilidad los comprueba el codigo que
     * ya atiende a `x.f(a)`. */
    auto fa = std::make_unique<ast::FieldAccessExpr>();
    fa->loc = id->loc;
    fa->field_name = member;
    fa->base = std::move(e->args[0]);
    e->args.erase(e->args.begin());
    if (!e->arg_names.empty()) {
        // El nombre del receptor se va con el, para que los dos sigan
        // cuadrando.
        ParamNames rest;
        for (size_t i = 1; i < e->arg_names.size(); ++i)
            rest.push_back(e->arg_names[i]);
        e->arg_names = std::move(rest);
    }
    e->callee = std::move(fa);
    e->result_type = check_call(e);
    return true;
}

bool TypeChecker::report_ufcs_reverse_missing(ast::CallExpr *e,
                                              const ast::IdentExpr *id) {
    if (id == nullptr) return false;
    ast::Expr *recv_expr = ufcs_reverse_receiver(e);
    if (recv_expr == nullptr) return false;
    /* El tipo ya se conoce: lo dejo anotado la sonda de `try_ufcs_reverse`,
     * que corre justo antes.  Volver a comprobar la expresion aqui repetiria
     * sus quejas. */
    std::string owner;
    const char *kw = nullptr;
    const std::vector<ClassMethodInfo> *ms =
        receiver_methods(recv_expr->result_type, nullptr, &owner, &kw);
    if (ms == nullptr) return false;
    // Y se cita con el nombre que se ESCRIBIO, no con el que le puso el
    // aplanado: mandar a buscar `app__crece` es mandar a buscar lo que no esta.
    diags_.diag(e->loc, DiagLevel::ERR, "VX2086",
                {written_name(id->name), kw, owner});
    return true;
}

bool TypeChecker::report_ufcs_reverse_clash(ast::CallExpr *e,
                                            const ast::IdentExpr *id,
                                            const Symbol *s) {
    if (id == nullptr || s == nullptr) return false;
    /* La rama que hace que esto no cueste: si lo que la libre pide de primer
     * parametro no es un struct ni una clase, no hay receptor con miembros que
     * pueda chocar con ella y no se mira nada mas. */
    if (s->sig_index >= function_sigs_.size()) return false;
    const std::vector<Type> &ps = function_sigs_[s->sig_index].param_types;
    if (ps.empty()) return false;
    const Type &p0 = ps[0];
    const bool p0_receptor = p0.kind == PrimitiveKind::STRUCT ||
                             p0.kind == PrimitiveKind::CLASS ||
                             (p0.kind == PrimitiveKind::PTR && p0.pointee &&
                              p0.pointee->kind == PrimitiveKind::STRUCT);
    if (!p0_receptor) return false;
    ast::Expr *recv_expr = ufcs_reverse_receiver(e);
    if (recv_expr == nullptr) return false;
    // Igual que en `try_ufcs_reverse`: se pregunta, no se comprueba.
    Type recv;
    {
        QuietDiags quiet(diags_);
        recv = check_expr(recv_expr);
    }
    Type norm;
    std::string owner;
    const std::vector<ClassMethodInfo> *ms =
        receiver_methods(recv, &norm, &owner, nullptr);
    if (ms == nullptr) return false;
    // Por el nombre ESCRITO, igual que la reescritura: ver `try_ufcs_reverse`.
    const std::string member = written_name(id->name);
    /* Y por la MISMA pareja de preguntas que hace la reescritura: un metodo
     * GENERICO no esta en el layout con su nombre -- ahi solo hay instancias ya
     * concretas --, asi que preguntar solo por el layout decia que no hay
     * miembro cuando lo hay.
     *
     * Sin esto, el choque dependia de si el metodo era generico: con uno
     * normal la llamada no compilaba, y con uno generico se elegia en silencio
     * -- que es justo lo que esta regla existe para no hacer --. */
    if (find_instance_method(*ms, member) == nullptr &&
        find_generic_method_template(norm.struct_name, member) == nullptr)
        return false;
    /* Y el choque lo declara la MISMA funcion que lo declara desde el punto,
     * con el mismo mensaje: es una regla, no dos. */
    return report_ufcs_clash(norm, member, owner, e->loc);
}

bool TypeChecker::try_ufcs_reverse_over_free(ast::CallExpr *e,
                                             const ast::IdentExpr *id,
                                             const FunctionSig &sig) {
    /* Una variadica acepta lo que le echen, asi que no hay "no sirve" que
     * detectar: su rama se comprueba aparte y no llega aqui. */
    if (sig.is_variadic || sig.is_raw_variadic) return false;
    /* Si la ARIDAD no cuadra, la libre no sirve y da igual lo que cuesten sus
     * argumentos: ese camino ya acababa en un error.  Cuando SI cuadra hay que
     * ser mucho mas cuidadoso, porque la llamada puede ser perfectamente
     * valida y entonces esto se cruza en el camino bueno. */
    if (e->args.size() == sig.param_types.size()) {
        /* Un receptor con miembros es un struct, una clase o un puntero a
         * struct; un primer parametro de esa familia PUEDE recibirlo, asi que
         * se deja contestar a la libre. */
        if (!sig.param_types.empty()) {
            const PrimitiveKind k = sig.param_types[0].kind;
            if (k == PrimitiveKind::STRUCT || k == PrimitiveKind::CLASS ||
                k == PrimitiveKind::PTR)
                return false;
        }
        /* Y aqui la guarda que de verdad importa: preguntar el tipo del
         * receptor tiene que ser BARATO.  Un identificador es una consulta a la
         * tabla de simbolos; una LLAMADA hay que comprobarla entera, y el
         * camino normal la comprobara OTRA VEZ -- asi que con llamadas
         * anidadas el trabajo se duplica por nivel.
         *
         * No es teorico: un macro que emite `mas_uno(mas_uno(...))` con cien
         * niveles dejo de compilar en absoluto.  Cien niveles duplicados son
         * dos elevado a cien.  Cualquier trabajo repetido por nivel se
         * convierte en exponencial en cuanto alguien anida, y los macros
         * anidan por definicion. */
        const ast::Expr *recv = ufcs_reverse_receiver(e);
        if (recv == nullptr || recv->kind != ast::NodeKind::IdentExpr)
            return false;
    }
    return try_ufcs_reverse(e, id);
}

} // namespace vx
