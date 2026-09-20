/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file vx/generics/generic_infer.cpp
 * @brief Deduccion de argumentos de tipo.  Ver @c vx/generics/generic_infer.h.
 */

#include "vx/generics/generic_infer.h"

#include "vx/type_checker.h"

namespace vx {
namespace generics {

namespace {

/**
 * @brief En que posicion esta @p name dentro de @p vars, o -1 si no esta.
 *
 * Un recorrido y no una tabla: @p vars tiene una, dos o tres entradas, y a esa
 * escala construir un hash cuesta mas que las comparaciones que ahorra.  Las
 * cadenas de distinta longitud se descartan sin mirar ningun caracter.
 *
 * @param vars Los nombres de las variables, en orden de declaracion.
 * @param name El nombre a buscar.
 * @return Su indice, o -1.
 */
inline int var_index(const std::vector<std::string> &vars,
                     const std::string &name) noexcept {
    for (size_t i = 0; i < vars.size(); ++i)
        if (vars[i] == name) return static_cast<int>(i);
    return -1;
}

/**
 * @brief Enciende en @p mask las variables que aparecen dentro de @p t.
 *
 * Recorre el tipo DECLARADO entero, no solo su raiz: es toda la diferencia
 * entre ver el `T` de `T x` y ver el de `fn(T) -> R cb`.
 *
 * @param t    El tipo declarado.
 * @param vars Los nombres que son variables.
 * @param mask [in,out] Un bit por variable.
 */
void collect_vars(const ast::TypeNode *t, const std::vector<std::string> &vars,
                  uint32_t &mask) {
    if (t == nullptr) return;
    switch (t->kind) {
    case ast::NodeKind::NamedTypeNode: {
        const auto *n = static_cast<const ast::NamedTypeNode *>(t);
        /* Un nombre CON argumentos no es una variable, es una instanciacion:
         * en `Caja<T>` la variable es la `T` de dentro, no `Caja`. */
        if (n->type_args.empty()) {
            const int i = var_index(vars, n->name);
            if (i >= 0 && i < kMaxTypeParams) mask |= (1u << i);
        }
        for (const auto &ta : n->type_args)
            collect_vars(ta.get(), vars, mask);
        break;
    }
    case ast::NodeKind::PrimitiveTypeNode: {
        /* Los punteros inteligentes y las colecciones se parsean como
         * primitivos CON argumentos (`unique<T>`, `ArrayList<T>`), asi que lo
         * que llevan dentro tambien cuenta. */
        const auto *p = static_cast<const ast::PrimitiveTypeNode *>(t);
        for (const auto &ta : p->type_args)
            collect_vars(ta.get(), vars, mask);
        break;
    }
    case ast::NodeKind::PointerTypeNode:
        collect_vars(
            static_cast<const ast::PointerTypeNode *>(t)->pointee.get(), vars,
            mask);
        break;
    case ast::NodeKind::ArrayTypeNode:
        collect_vars(
            static_cast<const ast::ArrayTypeNode *>(t)->element_type.get(),
            vars, mask);
        break;
    case ast::NodeKind::FunctionTypeNode: {
        const auto *f = static_cast<const ast::FunctionTypeNode *>(t);
        for (const auto &pt : f->param_types)
            collect_vars(pt.get(), vars, mask);
        collect_vars(f->return_type.get(), vars, mask);
        break;
    }
    default: break;
    }
}

} // namespace

DeductionPlan
build_deduction_plan(const std::vector<std::string> &type_params,
                     const std::vector<const ast::TypeNode *> &param_types) {
    DeductionPlan plan;
    plan.count = static_cast<uint16_t>(type_params.size());
    if (type_params.empty()) return plan;

    /* Un recorrido por PARAMETRO, no uno por variable: mirar su tipo una vez y
     * apuntar todas las que lleve dentro sale mas barato que recorrerlo tantas
     * veces como variables haya, y ademas encuentra de un golpe las varias que
     * un solo parametro puede ligar (`fn(T) -> R` liga dos). */
    for (size_t pi = 0; pi < param_types.size(); ++pi) {
        uint32_t here = 0;
        collect_vars(param_types[pi], type_params, here);
        if (here == 0) continue;
        plan.deducible |= here;
        plan.needed_args.push_back(static_cast<uint16_t>(pi));
    }
    /* Los indices salen ya en orden y sin repetir -- un recorrido, cada
     * parametro apuntado como mucho una vez --, asi que quien los consuma no
     * tiene que ordenar ni deduplicar nada. */
    return plan;
}

std::string type_node_text(const ast::TypeNode *t) {
    if (t == nullptr) return std::string();
    switch (t->kind) {
    case ast::NodeKind::NamedTypeNode: {
        const auto *n = static_cast<const ast::NamedTypeNode *>(t);
        if (n->type_args.empty()) return n->name;
        std::string s = n->name + "<";
        for (size_t i = 0; i < n->type_args.size(); ++i) {
            if (i != 0) s += ", ";
            s += type_node_text(n->type_args[i].get());
        }
        return s + ">";
    }
    case ast::NodeKind::PrimitiveTypeNode: {
        const auto *p = static_cast<const ast::PrimitiveTypeNode *>(t);
        std::string s = primitive_name(p->prim);
        if (p->type_args.empty()) return s;
        s += "<";
        for (size_t i = 0; i < p->type_args.size(); ++i) {
            if (i != 0) s += ", ";
            s += type_node_text(p->type_args[i].get());
        }
        return s + ">";
    }
    case ast::NodeKind::PointerTypeNode:
        return type_node_text(static_cast<const ast::PointerTypeNode *>(t)
                                  ->pointee.get()) +
               "*";
    case ast::NodeKind::ArrayTypeNode:
        return type_node_text(static_cast<const ast::ArrayTypeNode *>(t)
                                  ->element_type.get()) +
               "[]";
    case ast::NodeKind::FunctionTypeNode: {
        const auto *f = static_cast<const ast::FunctionTypeNode *>(t);
        std::string s = "fn(";
        for (size_t i = 0; i < f->param_types.size(); ++i) {
            if (i != 0) s += ", ";
            s += type_node_text(f->param_types[i].get());
        }
        s += ")";
        if (f->return_type) s += " -> " + type_node_text(f->return_type.get());
        return s;
    }
    default: return std::string();
    }
}

uint32_t shape_specificity(const ast::TypeNode *t,
                           const std::vector<std::string> &vars) {
    if (t == nullptr) return 0;
    switch (t->kind) {
    case ast::NodeKind::NamedTypeNode: {
        const auto *n = static_cast<const ast::NamedTypeNode *>(t);
        uint32_t s = 0;
        if (!n->type_args.empty() || var_index(vars, n->name) < 0) ++s;
        for (const auto &ta : n->type_args)
            s += shape_specificity(ta.get(), vars);
        return s;
    }
    case ast::NodeKind::PointerTypeNode:
        return 1 +
               shape_specificity(
                   static_cast<const ast::PointerTypeNode *>(t)->pointee.get(),
                   vars);
    case ast::NodeKind::ArrayTypeNode:
        return 1 + shape_specificity(static_cast<const ast::ArrayTypeNode *>(t)
                                         ->element_type.get(),
                                     vars);
    case ast::NodeKind::FunctionTypeNode: {
        const auto *f = static_cast<const ast::FunctionTypeNode *>(t);
        uint32_t s = 1; // ser una funcion ya es forma
        for (const auto &p : f->param_types)
            s += shape_specificity(p.get(), vars);
        s += shape_specificity(f->return_type.get(), vars);
        return s;
    }
    default: return 1; // un primitivo pide un tipo exacto
    }
}

Type unresolved_param_type(const ast::TypeNode *t,
                           const std::vector<std::string> &vars) {
    uint32_t mask = 0;
    collect_vars(t, vars, mask);
    if (mask == 0) return Type{}; // no lleva variables: es un tipo de verdad
    /* Lo que la variable no sepa NO se lleva por delante lo que si se sabe.
     *
     * `T*` es un PUNTERO apunte a lo que apunte: mide ocho bytes, viaja en un
     * registro y se desreferencia.  Colapsarlo entero a "parametro de tipo"
     * perdia eso, y con ello todo el que pregunta si algo es una direccion.
     *
     * La IDENTIDAD no se pierde: lo de dentro sigue siendo el parametro con su
     * nombre, asi que `T*` y `U*` siguen siendo distintos para quien discrimina
     * entre plantillas homonimas -- que es lo que hay que conservar --.
     *
     * Lo mismo con un array: `T[]` es un array de algo. */
    if (t->kind == ast::NodeKind::PointerTypeNode) {
        const auto *pn = static_cast<const ast::PointerTypeNode *>(t);
        return Type::make_ptr(unresolved_param_type(pn->pointee.get(), vars),
                              pn->is_virtual);
    }
    if (t->kind == ast::NodeKind::ArrayTypeNode) {
        const auto *an = static_cast<const ast::ArrayTypeNode *>(t);
        return Type::make_array(
            unresolved_param_type(an->element_type.get(), vars), 0);
    }
    Type u;
    u.kind = PrimitiveKind::TYPE_PARAM;
    u.struct_name = type_node_text(t);
    return u;
}

bool match_type_pattern(TypeChecker &tc, const ast::TypeNode *pattern,
                        const Type &concrete,
                        const std::vector<std::string> &vars, Type *out,
                        uint32_t &bound) {
    if (pattern == nullptr) return false;
    switch (pattern->kind) {
    case ast::NodeKind::NamedTypeNode: {
        const auto *n = static_cast<const ast::NamedTypeNode *>(pattern);
        /* Generico ANIDADO (`Caja<T>`): lo que llega es una instanciacion ya
         * aplanada (`Caja_i64`), y un @c Type no lleva sus argumentos.  Se
         * recuperan de la ficha que dejo la instanciacion, que es quien los
         * sabe; sacarlos del nombre seria adivinar, porque un tipo del usuario
         * puede llamarse igual que un nombre aplanado. */
        if (!n->type_args.empty()) {
            if (concrete.kind != PrimitiveKind::STRUCT &&
                concrete.kind != PrimitiveKind::CLASS)
                return false;
            const auto *mi = tc.monomorph_info(concrete.struct_name);
            if (mi == nullptr || mi->template_name != n->name) return false;
            if (mi->type_arg_types.size() != n->type_args.size()) return false;
            for (size_t i = 0; i < n->type_args.size(); ++i)
                if (!match_type_pattern(tc, n->type_args[i].get(),
                                        mi->type_arg_types[i], vars, out,
                                        bound))
                    return false;
            return true;
        }
        const int vi = var_index(vars, n->name);
        if (vi >= 0) {
            if (vi >= kMaxTypeParams) return false; // no cabe en la mascara
            const uint32_t bit = 1u << vi;
            /* Ya ligada: tiene que COINCIDIR.  Es lo que hace que `Par<T, T>`
             * no acepte dos tipos distintos, y lo que convierte un error de
             * quien llama en una respuesta en vez de una eleccion al azar. */
            if ((bound & bit) != 0) return out[vi] == concrete;
            out[vi] = concrete;
            bound |= bit;
            return true;
        }
        // Un nombre concreto no liga nada: exige ser el mismo tipo.
        return tc.resolve_type_node(pattern) == concrete;
    }
    case ast::NodeKind::PrimitiveTypeNode: {
        const auto *p = static_cast<const ast::PrimitiveTypeNode *>(pattern);
        /* Sin argumentos es un escalar y basta con que sea el mismo.  CON
         * argumentos es un puntero inteligente o una coleccion (`unique<T>`,
         * `ArrayList<T>`), y lo que hay que ligar es lo de dentro. */
        if (p->type_args.empty())
            return tc.resolve_type_node(pattern) == concrete;
        /* Estos guardan su tipo interior en el apuntado.  Si no hay -- una
         * coleccion que no lo conserva --, no se puede deducir: se dice que no
         * encaja y quien llama escribe el type-arg, que es lo honesto.  Dar por
         * bueno lo que no se sabe ligaria una variable a un tipo inventado. */
        if (p->type_args.size() != 1 || !concrete.pointee) return false;
        return match_type_pattern(tc, p->type_args[0].get(), *concrete.pointee,
                                  vars, out, bound);
    }
    case ast::NodeKind::PointerTypeNode: {
        if (concrete.kind != PrimitiveKind::PTR || !concrete.pointee)
            return false;
        const auto *p = static_cast<const ast::PointerTypeNode *>(pattern);
        return match_type_pattern(tc, p->pointee.get(), *concrete.pointee, vars,
                                  out, bound);
    }
    case ast::NodeKind::ArrayTypeNode: {
        if (concrete.kind != PrimitiveKind::ARRAY || !concrete.pointee)
            return false;
        const auto *a = static_cast<const ast::ArrayTypeNode *>(pattern);
        return match_type_pattern(tc, a->element_type.get(), *concrete.pointee,
                                  vars, out, bound);
    }
    case ast::NodeKind::FunctionTypeNode: {
        /* El caso que hace comodo el codigo generico: en
         * `map<T,R>(Lista<T> xs, fn(T) -> R f)` este es el UNICO sitio donde
         * `R` aparece, asi que sin esto `map` no se puede llamar sin escribir
         * sus type-args -- y con ellos deja de merecer la pena. */
        if (concrete.kind != PrimitiveKind::FUNCTION) return false;
        const auto *f = static_cast<const ast::FunctionTypeNode *>(pattern);
        const std::vector<Type> &cps = concrete.fn_params();
        if (f->param_types.size() != cps.size()) return false;
        for (size_t i = 0; i < cps.size(); ++i)
            if (!match_type_pattern(tc, f->param_types[i].get(), cps[i], vars,
                                    out, bound))
                return false;
        /* El retorno de un tipo funcion vive en el apuntado.  Uno sin retorno
         * declarado no liga nada por ahi, y eso no es un fallo. */
        if (!f->return_type) return true;
        if (!concrete.pointee) return false;
        return match_type_pattern(tc, f->return_type.get(), *concrete.pointee,
                                  vars, out, bound);
    }
    default: return false;
    }
}

/**
 * @brief Si el patron de un parametro es una VARIABLE de tipo a secas (`T`).
 *
 * Es la forma de preguntar si el parametro PIDE algo del argumento.  `T*`,
 * `Caja<T>` o `fn(T) -> R` piden una forma -- puntero, instanciacion, funcion
 * --; `T` a secas no pide nada y se queda con lo que el argumento sea.
 *
 * @param pattern El tipo declarado del parametro.
 * @param vars    Los nombres que son variables.
 * @return true si es exactamente una variable, sin nada alrededor.
 */
inline bool pattern_is_bare_var(const ast::TypeNode *pattern,
                                const std::vector<std::string> &vars) noexcept {
    if (pattern == nullptr || pattern->kind != ast::NodeKind::NamedTypeNode)
        return false;
    const auto *n = static_cast<const ast::NamedTypeNode *>(pattern);
    return n->type_args.empty() && var_index(vars, n->name) >= 0;
}

} // namespace generics

const generics::DeductionPlan &TypeChecker::deduction_plan(
    const void *key, const std::vector<std::string> &type_params,
    const std::vector<std::unique_ptr<ast::ParamDecl>> &params) {
    auto it = deduction_plans_.find(key);
    if (it != deduction_plans_.end()) return it->second;

    std::vector<const ast::TypeNode *> nodes;
    nodes.reserve(params.size());
    for (const auto &p : params)
        nodes.push_back(p ? p->type.get() : nullptr);

    return deduction_plans_
        .emplace(key, generics::build_deduction_plan(type_params, nodes))
        .first->second;
}

bool TypeChecker::deduce_call_type_args(
    ast::CallExpr *e, const void *key,
    const std::vector<std::string> &type_params,
    const std::vector<std::unique_ptr<ast::ParamDecl>> &params,
    std::vector<Type> &out) {
    /* Las ligaduras van en un array paralelo a las variables y "cual esta
     * ligada" en una mascara: identificarlas por POSICION cuesta un
     * desplazamiento, y por nombre costaria hashear cadenas cortas para
     * contestar lo mismo. */
    out.assign(type_params.size(), Type{PrimitiveKind::COUNT});
    if (type_params.empty()) return true;

    const generics::DeductionPlan &plan =
        deduction_plan(key, type_params, params);
    uint32_t bound = 0;

    /* Solo los argumentos que el plan pide.  Comprobar uno de mas no es gratis
     * -- es recorrer su expresion entera, y el camino normal de la llamada la
     * va a comprobar otra vez --, asi que esto es lo que impide que el trabajo
     * se duplique por nivel y se multiplique con llamadas anidadas. */
    for (const uint16_t pi : plan.needed_args) {
        if (pi >= e->args.size() || pi >= params.size()) continue;
        if (!params[pi]) continue;
        Type at = check_expr(e->args[pi].get());
        if (at.kind == PrimitiveKind::COUNT) continue; // ya se dijo que fallaba
        /* El NOMBRE de una funcion, a secas, no se tipa como funcion: se
         * convierte en valor-funcion donde se espera una, y aqui todavia no se
         * sabe que se espera -- es lo que se esta deduciendo --.
         *
         * Lo dice el PATRON, que para eso esta: si el parametro se declaro como
         * una funcion, el nombre de una es lo que hay que leer, y su firma ya
         * la tiene el compilador.  Sin esto, `aplica(5, mas_uno)` no deduce
         * nada y hay que escribir `&mas_uno` o los type-args -- que es pedir
         * ceremonia justo donde el codigo generico deberia ser comodo. */
        if (at.kind == PrimitiveKind::VOID &&
            params[pi]->type->kind == ast::NodeKind::FunctionTypeNode &&
            e->args[pi]->kind == ast::NodeKind::IdentExpr) {
            const auto *aid =
                static_cast<const ast::IdentExpr *>(e->args[pi].get());
            if (const FunctionSig *fs = function_sig_by_name(aid->name))
                at = Type::make_function(fs->param_types, fs->return_type);
        }
        /* Y lo mismo con un LITERAL de cadena, por la misma razon.  Se MODELA
         * como puntero a los bytes de la seccion estatica -- es lo que pide la
         * frontera FFI -- y el CONTEXTO lo refina donde se pide otra cosa
         * (`string s = "hola"`, un parametro declarado `string`, `println`).
         *
         * Aqui el patron es la variable a secas, o sea que NADIE esta pidiendo
         * un puntero, y el tipo de una cadena es `string`: su tipo, no su
         * representacion.  A `char*` se baja pidiendolo (`.cstr()`), igual que
         * desde cualquier otra cadena.  Es la MISMA regla que ya aplica `auto a
         * = "hola"` (ver la inferencia local en @c check_var_decl), que hasta
         * ahora la deduccion generica no compartia: `f("hola")` ligaba
         * `T = void*` y `f(s)` con `string s` ligaba `string`, o sea dos tipos
         * para el mismo argumento segun como se hubiera escrito -- y con
         * llamada uniforme eso parte en dos `"hola".f()` y `f("hola")`.
         *
         * Donde el patron SI pide una forma (`T*`, `T[]`, `Caja<T>`) no se
         * toca: ahi hay contexto, y el contexto manda. */
        if (at.kind == PrimitiveKind::PTR &&
            e->args[pi]->kind == ast::NodeKind::StringLitExpr &&
            generics::pattern_is_bare_var(params[pi]->type.get(), type_params))
            at = Type{PrimitiveKind::STRING};
        /* Que un parametro no encaje NO es un fallo aqui: puede haber otro que
         * ligue la misma variable, y quien dice si la llamada vale es la
         * comprobacion de argumentos de siempre.  Aqui solo se recoge lo que se
         * pueda deducir. */
        (void)generics::match_type_pattern(*this, params[pi]->type.get(), at,
                                           type_params, out.data(), bound);
    }

    return bound == generics::full_type_param_mask(
                        static_cast<uint16_t>(type_params.size()));
}

namespace {

/**
 * @brief Si esta plantilla tiene TODAS las ranuras que la llamada nombra, y
 *        cada una una sola vez.
 *
 * Es la misma regla que @c overload::select aplica a las sobrecargas normales,
 * pero aqui no se puede delegar en ella: sus candidatas se comparan por TIPOS,
 * y los de una plantilla no existen hasta instanciarla.  Lo que si se puede
 * comparar antes de instanciar nada son los nombres, y eso es justo lo que
 * separa a dos plantillas que solo se distinguen por como llaman a sus
 * parametros.
 *
 * @param t     La plantilla.
 * @param names Con que nombre se escribio cada argumento; vacio donde fue
 *              posicional.
 */
bool template_has_named_slots(const ast::FunctionDecl *t,
                              const ParamNames &names) {
    const size_t np = t->params.size();
    // En pila: una firma de hasta ocho parametros no toca el monton.
    util::SmallVector<uint8_t, 8> taken;
    taken.resize(np, 0);
    for (const auto &want : names) {
        if (want.empty()) continue;
        size_t at = np;
        for (size_t j = 0; j < np; ++j)
            if (t->params[j] && t->params[j]->name == want.str()) {
                at = j;
                break;
            }
        if (at == np || taken[at] != 0) return false;
        taken[at] = 1;
    }
    return true;
}

} // namespace

bool TypeChecker::pick_generic_fn_template(ast::CallExpr *e,
                                           const std::string &name,
                                           size_t &out) {
    auto it = generic_fn_templates_.find(name);
    if (it == generic_fn_templates_.end()) {
        // Puede venir por su nombre PUBLICO, si es una plantilla importada.
        auto pn = generic_fn_public_names_.find(name);
        if (pn != generic_fn_public_names_.end())
            it = generic_fn_templates_.find(pn->second);
    }
    if (it == generic_fn_templates_.end() || it->second.empty()) return false;

    /* Con una sola no hay nada que decidir, y probarla costaria comprobar
     * argumentos en TODA llamada a una generica -- que son casi todas. */
    if (it->second.size() == 1) {
        out = it->second[0];
        return true;
    }

    /* Varias homonimas: lo que las separa es la FORMA de sus parametros o como
     * se llaman sus ranuras, y eso no se ve comparando tipos -- los suyos no
     * existen hasta instanciarlas --.  Se ve intentando DEDUCIR cada una: solo
     * liga sus variables la que de verdad encaja.
     *
     * Ni lista de las que encajan ni nada que reservar: un contador y el indice
     * de la primera bastan para las tres respuestas posibles. */
    size_t picked = 0;
    unsigned fits = 0;       // cuantas empatan en lo mas especifico
    uint32_t best = 0;       // cuanta forma pide la mejor hasta ahora
    std::vector<Type> targs; // reusado entre candidatas
    for (const size_t idx : it->second) {
        if (idx >= mod_.decls.size() || !mod_.decls[idx]) continue;
        const auto *t =
            static_cast<const ast::FunctionDecl *>(mod_.decls[idx].get());
        // La aridad las separa sin tocar un solo argumento.
        if (t->params.size() != e->args.size()) continue;
        /* Si la llamada NOMBRA alguna ranura, una candidata que no la tenga no
         * es viable aunque sus tipos cuadraran: es la misma regla que ya separa
         * dos sobrecargas normales, y es lo unico que distingue a dos
         * plantillas iguales salvo por como llaman a sus parametros. */
        if (!e->arg_names.empty() && !template_has_named_slots(t, e->arg_names))
            continue;
        if (!e->type_args.empty()) {
            // Con los type-args escritos, lo que separa es cuantos pide.
            if (t->type_params.size() != e->type_args.size()) continue;
        } else if (!deduce_call_type_args(e, t, t->type_params, t->params,
                                          targs)) {
            continue;
        }
        /* Encaja.  Entre las que encajan gana la que mas forma pide: la que
         * pide menos las habria cogido todas, asi que quedarse con ella
         * volveria inutil a la especifica.  Ver generics::shape_specificity. */
        uint32_t spec = 0;
        for (const auto &p : t->params)
            if (p && p->type)
                spec +=
                    generics::shape_specificity(p->type.get(), t->type_params);
        if (fits == 0 || spec > best) {
            best = spec;
            picked = idx;
            fits = 1;
        } else if (spec == best) {
            ++fits;
        }
    }

    if (fits == 1) {
        out = picked;
        return true;
    }
    /* Ninguna o varias: en los dos casos se DICE, porque elegir en silencio
     * entre dos plantillas es decidir por el programador y ademas por un
     * criterio que no esta escrito en ningun sitio -- el orden en que se
     * declararon --.
     *
     * Salvo que de ese nombre ya se dijera que esta definido dos veces: las dos
     * plantillas SON esa colision, la llamada es ambigua por ella y no por otra
     * cosa, y repetirlo en cada sitio de llamada entierra el error de verdad --
     * el que senyala las dos definiciones -- bajo decenas de lineas iguales. */
    if (fits > 1 && already_redefined(name)) return false;
    diags_.diag(e->loc, DiagLevel::ERR, fits == 0 ? "VX2090" : "VX2091",
                {written_name(name)});
    return false;
}

void TypeChecker::report_type_args_not_deduced(
    const SourceLoc &loc, const std::string &what, const void *key,
    const std::vector<std::string> &type_params,
    const std::vector<Type> &deduced) {
    /* Se nombra la variable que falta, no "no se pudieron inferir": con dos o
     * tres declaradas, decir cual es la diferencia entre mirar la firma y
     * adivinarla.  Y se distingue de QUIEN es el problema -- de la firma o de
     * esta llamada --, porque la respuesta manda a sitios distintos. */
    auto it = deduction_plans_.find(key);
    const uint32_t deducible =
        (it != deduction_plans_.end()) ? it->second.deducible : 0u;
    for (size_t i = 0; i < type_params.size(); ++i) {
        if (i < deduced.size() && deduced[i].kind != PrimitiveKind::COUNT)
            continue;
        const bool from_signature =
            (i >= generics::kMaxTypeParams) || ((deducible & (1u << i)) == 0);
        diags_.diag(loc, DiagLevel::ERR, from_signature ? "VX2088" : "VX2089",
                    {what, type_params[i]});
        return; // la primera basta: arreglarla suele destapar el resto
    }
}

} // namespace vx
