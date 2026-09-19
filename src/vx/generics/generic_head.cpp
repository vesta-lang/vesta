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
 * @file generic_head.cpp
 * @brief Reparte los `<...>` de cada declaracion en variables o argumentos.
 *
 * La regla y el porque, en @c vx/generics/generic_head.h.
 */

#include "vx/generics/generic_head.h"

#include "vx/type_checker.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vx {
namespace generics {
namespace {

/// Los nombres que YA son un tipo.  Una por modulo, no por declaracion.
///
/// Guarda vistas y no cadenas: los nombres viven en el AST y en las tablas del
/// comprobador, que no se tocan mientras esto existe, asi que copiarlos seria
/// reservar una vez por tipo del programa para no ganar nada.
class TypeNameSet {
  public:
    void reserve(size_t n) { names_.reserve(n); }
    void add(const std::string &n) {
        if (!n.empty()) names_.insert(std::string_view(n));
    }
    bool has(const std::string &n) const noexcept {
        return names_.find(std::string_view(n)) != names_.end();
    }

  private:
    std::unordered_set<std::string_view> names_;
};

/// @brief Un identificador desnudo: ni puntero, ni array, ni instanciado.
///
/// Es la unica forma que puede ser las dos cosas.  Todo lo demas -- `i64`,
/// `T*`, `Inner<T>` -- es un argumento por su notacion.
const ast::NamedTypeNode *bare_name(const ast::TypeNode *t) noexcept {
    if (t == nullptr || t->kind != ast::NodeKind::NamedTypeNode) return nullptr;
    const auto *n = static_cast<const ast::NamedTypeNode *>(t);
    return n->type_args.empty() ? n : nullptr;
}

/// @brief Los identificadores del patron que NO son tipos: son variables.
///
/// En una especializacion parcial (`Caja<T*>`, `Caja<Inner<T>>`) las variables
/// no van en la cabeza sino DENTRO del patron, y hay que sacarlas de ahi.  Un
/// nombre repetido se apunta una sola vez -- `Par<T, T>` declara una `T` --, y
/// la lista es de uno o dos elementos, asi que recorrerla gana a cualquier
/// tabla.
void collect_free_names(const ast::TypeNode *t, const TypeNameSet &known,
                        std::vector<std::string> &out) {
    if (t == nullptr) return;
    switch (t->kind) {
    case ast::NodeKind::NamedTypeNode: {
        const auto *n = static_cast<const ast::NamedTypeNode *>(t);
        if (n->type_args.empty() && !known.has(n->name)) {
            for (const auto &e : out)
                if (e == n->name) return;
            out.push_back(n->name);
            return;
        }
        for (const auto &ta : n->type_args)
            collect_free_names(ta.get(), known, out);
        return;
    }
    case ast::NodeKind::PointerTypeNode:
        collect_free_names(
            static_cast<const ast::PointerTypeNode *>(t)->pointee.get(), known,
            out);
        return;
    case ast::NodeKind::ArrayTypeNode:
        collect_free_names(
            static_cast<const ast::ArrayTypeNode *>(t)->element_type.get(),
            known, out);
        return;
    default: return; // primitivos, punteros a funcion: nada que declarar
    }
}

/**
 * @brief Reparte UNA cabeza.
 *
 * @param pattern  [in,out] Lo escrito entre `<...>`.  Se VACIA si resulta ser
 *                 una declaracion de variables: entonces no hay patron.
 * @param tparams  [out] Los nombres que son variables.
 * @param is_spec  [out] Si lo escrito son argumentos.
 * @param has_bounds Si se escribio alguna cota, que solo cabe declarando.
 * @param known    Los nombres que ya son tipos.
 */
void classify_one(std::vector<std::unique_ptr<ast::TypeNode>> &pattern,
                  std::vector<std::string> &tparams, bool &is_spec,
                  bool has_bounds, const TypeNameSet &known, SourceLoc loc,
                  TypeChecker *tc, std::string *clash) {
    if (pattern.empty()) return;

    // Declara variables cuando TODO lo escrito es un identificador desnudo que
    // no nombra ningun tipo.  Una cota lo zanja sin mirar: nadie le pone una
    // cota a un argumento -- y entonces el choque con un tipo se DICE, que es
    // el unico sitio donde se puede decir sin ambiguedad.
    bool declares = true;
    for (const auto &node : pattern) {
        const ast::NamedTypeNode *n = bare_name(node.get());
        if (n == nullptr) {
            declares = false;
            break;
        }
        if (!known.has(n->name)) continue;
        if (has_bounds) {
            if (tc != nullptr)
                tc->diagnostics().diag(loc, DiagLevel::ERR, "VX2095",
                                       {tc->written_name(n->name)});
            continue;
        }
        declares = false;
        // Solo esto es AMBIGUO: escrito, se lee igual que una declaracion, y
        // lo que lo decanta es que ese nombre sea un tipo.  Se apunta para
        // exigir despues que la primaria exista; lo demas (`<i64>`, `<T*>`) no
        // se confunde con una declaracion ni escribiendolo mal.
        if (clash != nullptr) *clash = n->name;
        break;
    }

    if (declares) {
        tparams.clear();
        tparams.reserve(pattern.size());
        for (const auto &node : pattern)
            tparams.push_back(bare_name(node.get())->name);
        pattern.clear(); // no es un patron: no hay nada contra lo que casar
        is_spec = false;
        return;
    }

    is_spec = true;
    tparams.clear();
    for (const auto &node : pattern)
        collect_free_names(node.get(), known, tparams);
}

/// @brief Si esta declaracion trae algo generico de lo que ocuparse: unos
///        `<...>` por repartir, o variables ya declaradas que comprobar.
/// @param d La declaracion, que puede ser nula.
bool has_generics(const ast::Node *d) noexcept {
    if (d == nullptr) return false;
    switch (d->kind) {
    case ast::NodeKind::StructDecl: {
        const auto *sd = static_cast<const ast::StructDecl *>(d);
        if (sd->generic_head_unresolved) return true;
        for (const auto &m : sd->methods)
            if (m && !m->method_type_params.empty()) return true;
        return false;
    }
    case ast::NodeKind::ClassDecl: {
        const auto *cd = static_cast<const ast::ClassDecl *>(d);
        if (cd->generic_head_unresolved) return true;
        for (const auto &m : cd->methods)
            if (m && !m->method_type_params.empty()) return true;
        return false;
    }
    case ast::NodeKind::FunctionDecl:
        return static_cast<const ast::FunctionDecl *>(d)
            ->generic_head_unresolved;
    case ast::NodeKind::EnumDecl:
        return !static_cast<const ast::EnumDecl *>(d)->type_params.empty();
    default: return false;
    }
}

/**
 * @brief El reparto, con o sin comprobador.
 *
 * Lo que cambia entre los dos no es la REGLA sino cuanto se sabe: sin
 * comprobador faltan los tipos que llegan importados, asi que se reparte con
 * lo que hay y no se da ningun veredicto.  Escribirlo dos veces seria tener
 * dos criterios esperando a divergir -- y el que divergiera seria el del
 * editor, donde nadie lo mira.
 *
 * @param tc  El comprobador, o nulo si solo se ha parseado.
 * @param mod El modulo.
 */
void classify_all(TypeChecker *tc, ast::ModuleNode &mod) {
    // Y solo si hay algo que repartir: un modulo sin genericos no paga la
    // tabla de nombres, que es lo unico que aqui cuesta algo.
    bool any = false;
    for (const auto &d : mod.decls) {
        if (has_generics(d.get())) {
            any = true;
            break;
        }
    }
    if (!any) return;

    TypeNameSet known;
    known.reserve(mod.decls.size() + (tc != nullptr
                                          ? tc->struct_layouts().size() +
                                                tc->class_layouts().size() +
                                                tc->enum_layouts().size()
                                          : 0));
    // Los declarados en este modulo.
    for (const auto &d : mod.decls) {
        if (!d) continue;
        switch (d->kind) {
        case ast::NodeKind::StructDecl:
            known.add(static_cast<const ast::StructDecl *>(d.get())->name);
            break;
        case ast::NodeKind::ClassDecl:
            known.add(static_cast<const ast::ClassDecl *>(d.get())->name);
            break;
        case ast::NodeKind::EnumDecl:
            known.add(static_cast<const ast::EnumDecl *>(d.get())->name);
            break;
        case ast::NodeKind::TypeAliasDecl:
            known.add(static_cast<const ast::TypeAliasDecl *>(d.get())->name);
            break;
        default: break;
        }
    }
    /* Y los que llegan de fuera: un `Caja<Punto>` con el `Punto` importado es
     * una especializacion igual que si estuviera escrito al lado.  Quien solo
     * ha parseado no los tiene, y entonces esos casos se quedan sin repartir
     * bien -- que para resaltar en el editor es suficiente, y para compilar no,
     * por eso el comprobador vuelve a pasar con la lista entera. */
    if (tc != nullptr) {
        for (const auto &e : tc->struct_layouts())
            known.add(e.first);
        for (const auto &e : tc->class_layouts())
            known.add(e.first);
        for (const auto &e : tc->enum_layouts())
            known.add(e.first);
        for (const auto &e : tc->type_aliases())
            known.add(e.first);
    }

    // Lo que se leyo como especializacion SOLO porque un identificador desnudo
    // resulto ser un tipo: hay que comprobar que su primaria existe, y no se
    // puede hasta haberlas clasificado todas -- la primaria puede estar mas
    // abajo en el fichero.
    struct Ambiguous {
        const std::string *name;
        std::string clash;
        SourceLoc loc;
    };
    std::vector<Ambiguous> ambiguous;
    TypeNameSet templates;

    for (const auto &d : mod.decls) {
        if (!d) continue;
        std::string clash;
        const std::string *name = nullptr;
        bool is_template = false;
        SourceLoc loc{};
        switch (d->kind) {
        case ast::NodeKind::StructDecl: {
            auto *sd = static_cast<ast::StructDecl *>(d.get());
            if (!sd->generic_head_unresolved) break;
            classify_one(sd->spec_pattern, sd->type_params,
                         sd->is_specialization, !sd->type_bounds.empty(), known,
                         sd->loc, tc, &clash);
            sd->generic_head_unresolved = false;
            name = &sd->name;
            loc = sd->loc;
            is_template = !sd->is_specialization;
            break;
        }
        case ast::NodeKind::ClassDecl: {
            auto *cd = static_cast<ast::ClassDecl *>(d.get());
            if (!cd->generic_head_unresolved) break;
            classify_one(cd->spec_pattern, cd->type_params,
                         cd->is_specialization, !cd->type_bounds.empty(), known,
                         cd->loc, tc, &clash);
            cd->generic_head_unresolved = false;
            name = &cd->name;
            loc = cd->loc;
            is_template = !cd->is_specialization;
            break;
        }
        case ast::NodeKind::FunctionDecl: {
            auto *fd = static_cast<ast::FunctionDecl *>(d.get());
            if (!fd->generic_head_unresolved) break;
            classify_one(fd->spec_pattern, fd->type_params,
                         fd->is_specialization, !fd->type_bounds.empty(), known,
                         fd->loc, tc, &clash);
            fd->generic_head_unresolved = false;
            name = &fd->name;
            loc = fd->loc;
            is_template = !fd->is_specialization;
            break;
        }
        default: break;
        }
        if (name == nullptr) continue;
        if (is_template)
            templates.add(*name);
        else if (!clash.empty())
            ambiguous.push_back({name, std::move(clash), loc});
    }

    /* Un veredicto solo se da con la lista COMPLETA de tipos: con la parcial,
     * "no hay primaria" puede ser "no la veo todavia". */
    if (tc == nullptr) return;

    /* La otra mitad de la regla, donde no hay nada que repartir porque los
     * `<...>` DECLARAN siempre: un metodo generico y un enum generico no tienen
     * especializaciones, asi que ahi no cabe la duda -- pero si cabe el choque,
     * y es el mismo: una variable que se llama como un tipo lo TAPA alli donde
     * la plantilla lo use.  Sin esto, la regla valdria segun donde estuviera
     * escrito el generico, que es justo lo que se quito. */
    for (const auto &d : mod.decls) {
        if (!d) continue;
        const std::vector<std::unique_ptr<ast::ClassMethodDecl>> *ms = nullptr;
        switch (d->kind) {
        case ast::NodeKind::StructDecl:
            ms = &static_cast<const ast::StructDecl *>(d.get())->methods;
            break;
        case ast::NodeKind::ClassDecl:
            ms = &static_cast<const ast::ClassDecl *>(d.get())->methods;
            break;
        case ast::NodeKind::EnumDecl: {
            const auto *en = static_cast<const ast::EnumDecl *>(d.get());
            for (const auto &tp : en->type_params)
                if (known.has(tp))
                    tc->diagnostics().diag(en->loc, DiagLevel::ERR, "VX2095",
                                           {tc->written_name(tp)});
            break;
        }
        default: break;
        }
        if (ms == nullptr) continue;
        for (const auto &m : *ms) {
            if (!m) continue;
            for (const auto &tp : m->method_type_params)
                if (known.has(tp))
                    tc->diagnostics().diag(m->loc, DiagLevel::ERR, "VX2095",
                                           {tc->written_name(tp)});
        }
    }

    for (const auto &a : ambiguous) {
        if (templates.has(*a.name)) continue;
        tc->diagnostics().diag(
            a.loc, DiagLevel::ERR, "VX2094",
            {tc->written_name(*a.name), tc->written_name(a.clash)});
    }
}

} // namespace

void classify_generic_heads(TypeChecker &tc, ast::ModuleNode &mod) {
    classify_all(&tc, mod);
}

void classify_generic_heads(ast::ModuleNode &mod) {
    classify_all(nullptr, mod);
}

} // namespace generics
} // namespace vx
