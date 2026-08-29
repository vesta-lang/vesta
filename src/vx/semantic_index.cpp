/**
 * @file semantic_index.cpp
 * @brief Implementacion del indice semantico por-declaracion (ver
 *        semantic_index.h).
 */
#include "util/fnv.h" // la semilla y el primo, en UN sitio
#include "vx/semantic_index.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "vx/diagnostic.h"
#include "vx/module/module_resolver.h"

namespace vx {

namespace {

/// @brief FNV-1a 64 bits sobre un rango de bytes.
uint64_t fnv1a64(const char *data, size_t n) {
    uint64_t h = util::kFnvOffset;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= util::kFnvPrime;
    }
    return h;
}

/// @brief Nombre de una declaracion top-level (vacio si no aplica).
std::string decl_name(const ast::Node *d) {
    if (!d) return {};
    using K = ast::NodeKind;
    switch (d->kind) {
    case K::FunctionDecl:
        return static_cast<const ast::FunctionDecl *>(d)->name;
    case K::GlobalVarDecl:
        return static_cast<const ast::GlobalVarDecl *>(d)->name;
    case K::TypeAliasDecl:
        return static_cast<const ast::TypeAliasDecl *>(d)->name;
    case K::StructDecl: return static_cast<const ast::StructDecl *>(d)->name;
    case K::ClassDecl: return static_cast<const ast::ClassDecl *>(d)->name;
    case K::EnumDecl: return static_cast<const ast::EnumDecl *>(d)->name;
    case K::ConceptDecl: return static_cast<const ast::ConceptDecl *>(d)->name;
    case K::ExtensionDecl:
        // No introduce un simbolo nuevo: extiende un tipo.  Clave sintetica
        // para que el indice lo rastree (su hash/deps importan).
        return "extension@" +
               static_cast<const ast::ExtensionDecl *>(d)->target_type;
    case K::ImplDecl: {
        const auto *im = static_cast<const ast::ImplDecl *>(d);
        return "impl@" + im->concept_name + "@" + im->target_type;
    }
    default: return {};
    }
}

/// @brief Visibilidad publica de una decl top-level (para el completado
/// cross-module: solo los @c public son importables desde otro modulo).
/// Las decls que no llevan flag (extension/impl) se consideran publicas
/// (no ocultan simbolos).
bool decl_is_public(const ast::Node *d) {
    if (!d) return false;
    using K = ast::NodeKind;
    switch (d->kind) {
    case K::FunctionDecl:
        return static_cast<const ast::FunctionDecl *>(d)->is_public;
    case K::GlobalVarDecl:
        return static_cast<const ast::GlobalVarDecl *>(d)->is_public;
    case K::TypeAliasDecl:
        return static_cast<const ast::TypeAliasDecl *>(d)->is_public;
    case K::StructDecl:
        return static_cast<const ast::StructDecl *>(d)->is_public;
    case K::ClassDecl: return static_cast<const ast::ClassDecl *>(d)->is_public;
    case K::EnumDecl: return static_cast<const ast::EnumDecl *>(d)->is_public;
    case K::ConceptDecl:
        return static_cast<const ast::ConceptDecl *>(d)->is_public;
    default: return true;
    }
}

/// @brief Aplana recursivamente los simbolos con nombre a una lista
/// {nombre_cualificado, kind, offset}, recorriendo los NamespaceDecl.
struct FlatDecl {
    std::string qname; ///< nombre cualificado con el namespace.
    uint8_t kind;
    uint32_t offset;
    bool is_public; ///< @c public (importable desde otro modulo).
};

void flatten_decls(const std::vector<std::unique_ptr<ast::Node>> &decls,
                   const std::string &ns_prefix, std::vector<FlatDecl> &out) {
    for (const auto &d : decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            const auto *nd = static_cast<const ast::NamespaceDecl *>(d.get());
            const std::string nested =
                ns_prefix.empty() ? nd->name : ns_prefix + "." + nd->name;
            flatten_decls(nd->decls, nested, out);
            continue;
        }
        const std::string nm = decl_name(d.get());
        if (nm.empty()) continue; // ImportDecl u otros no-simbolo.
        const std::string q = ns_prefix.empty() ? nm : ns_prefix + "." + nm;
        out.push_back({q, static_cast<uint8_t>(d->kind), d->loc.offset,
                       decl_is_public(d.get())});
    }
}

/// @brief Ultimo segmento punteado de un nombre cualificado (nombre simple).
std::string simple_name(const std::string &qname) {
    const size_t p = qname.rfind('.');
    return (p == std::string::npos) ? qname : qname.substr(p + 1);
}

/// @brief true si @p c puede iniciar/continuar un identificador Vesta.
bool ident_char(char c, bool first) {
    if (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        return true;
    if (!first && c >= '0' && c <= '9') return true;
    return false;
}

/// @brief Extrae los identificadores presentes en un span de texto.  Es un
/// escaneo LiGERO (no un parser): sobre-aproxima (captura nombres en
/// comentarios/strings), lo que produce dependencias CONSERVADORAS -> se
/// recompila de mas, nunca de menos (seguro).  El refinamiento a deps
/// precisas via AST es trabajo de una fase posterior.
void scan_identifiers(const char *data, size_t n,
                      std::unordered_set<std::string> &out) {
    size_t i = 0;
    while (i < n) {
        if (ident_char(data[i], /*first=*/true)) {
            const size_t start = i;
            ++i;
            while (i < n && ident_char(data[i], /*first=*/false))
                ++i;
            out.emplace(data + start, i - start);
        } else {
            ++i;
        }
    }
}

} // namespace

const SymbolEntry *
SemanticIndex::find(const std::string &qualified_name) const {
    for (const auto &s : symbols)
        if (s.name == qualified_name) return &s;
    return nullptr;
}

SemanticIndex build_semantic_index(const ast::ModuleNode &mod,
                                   const std::string &source,
                                   const std::string &module_path) {
    SemanticIndex idx;
    idx.module_path = module_path;
    idx.module_hash = fnv1a64(source.data(), source.size());

    // 1. Aplanar todas las decls con nombre (recorriendo namespaces) y
    //    ordenarlas por offset: los spans son [offset[i], offset[i+1]).
    std::vector<FlatDecl> flat;
    flatten_decls(mod.decls, "", flat);
    std::sort(flat.begin(), flat.end(),
              [](const FlatDecl &a, const FlatDecl &b) {
                  return a.offset < b.offset;
              });

    // 2. Conjunto de nombres SIMPLES del modulo, para filtrar las deps.
    std::unordered_set<std::string> module_simple_names;
    module_simple_names.reserve(flat.size() * 2 + 1);
    for (const auto &f : flat)
        module_simple_names.insert(simple_name(f.qname));

    const uint32_t src_end = static_cast<uint32_t>(source.size());
    idx.symbols.reserve(flat.size());
    for (size_t k = 0; k < flat.size(); ++k) {
        const uint32_t beg = flat[k].offset;
        const uint32_t end =
            (k + 1 < flat.size()) ? flat[k + 1].offset : src_end;
        // Defensa: offsets fuera de rango o invertidos -> span vacio.
        const uint32_t b = beg <= src_end ? beg : src_end;
        const uint32_t e = (end >= b && end <= src_end) ? end : src_end;
        const uint32_t len = e - b;

        SymbolEntry se;
        se.name = flat[k].qname;
        se.kind = flat[k].kind;
        se.src_offset = b;
        se.src_length = len;
        se.is_public = flat[k].is_public;
        se.content_hash = fnv1a64(source.data() + b, len);

        // Deps: identificadores del span que sean nombre de OTRO simbolo del
        // modulo (excluyendo el propio nombre simple).
        std::unordered_set<std::string> ids;
        scan_identifiers(source.data() + b, len, ids);
        const std::string self_simple = simple_name(flat[k].qname);
        for (const auto &id : ids) {
            if (id == self_simple) continue;
            if (module_simple_names.count(id)) se.deps.push_back(id);
        }
        std::sort(se.deps.begin(), se.deps.end());
        idx.symbols.push_back(std::move(se));
    }
    return idx;
}

// ---------------------------------------------------------------------------
// Serializacion binaria (.vxidx).  Formato:
//   magic 'VXIX' u32 | version u16 | _pad u16 | module_hash u64 |
//   path_len u32 | path bytes | symbol_count u32 |
//   por simbolo: name_len u32 | name | kind u8 | content_hash u64 |
//                src_offset u32 | src_length u32 | dep_count u32 |
//                por dep: dep_len u32 | dep bytes
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t VXIDX_MAGIC = 0x58495856u; // 'VXIX' little-endian
// Ecosistema alpha: SIN compat de versiones.  Un sidecar con version distinta
// se rechaza y se regenera; no hay ramas de parseo legacy.
constexpr uint16_t VXIDX_VERSION = 2;

void put_u16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(v & 0xFF);
    b.push_back((v >> 8) & 0xFF);
}
void put_u32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(v & 0xFF);
    b.push_back((v >> 8) & 0xFF);
    b.push_back((v >> 16) & 0xFF);
    b.push_back((v >> 24) & 0xFF);
}
void put_u64(std::vector<uint8_t> &b, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back((v >> (i * 8)) & 0xFF);
}
void put_str(std::vector<uint8_t> &b, const std::string &s) {
    put_u32(b, static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

struct Reader {
    const uint8_t *p;
    const uint8_t *end;
    bool ok = true;
    uint16_t u16() {
        if (p + 2 > end) {
            ok = false;
            return 0;
        }
        uint16_t v = p[0] | (p[1] << 8);
        p += 2;
        return v;
    }
    uint32_t u32() {
        if (p + 4 > end) {
            ok = false;
            return 0;
        }
        uint32_t v = static_cast<uint32_t>(p[0]) |
                     (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
        p += 4;
        return v;
    }
    uint64_t u64() {
        if (p + 8 > end) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(p[i]) << (i * 8);
        p += 8;
        return v;
    }
    std::string str() {
        uint32_t n = u32();
        if (!ok || p + n > end) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char *>(p), n);
        p += n;
        return s;
    }
};
} // namespace

std::vector<uint8_t> serialize_semantic_index(const SemanticIndex &idx) {
    std::vector<uint8_t> b;
    put_u32(b, VXIDX_MAGIC);
    put_u16(b, VXIDX_VERSION);
    put_u16(b, 0);
    put_u64(b, idx.module_hash);
    put_str(b, idx.module_path);
    put_u32(b, static_cast<uint32_t>(idx.symbols.size()));
    for (const auto &s : idx.symbols) {
        put_str(b, s.name);
        b.push_back(s.kind);
        put_u64(b, s.content_hash);
        put_u32(b, s.src_offset);
        put_u32(b, s.src_length);
        b.push_back(s.is_public ? 1 : 0); // v2
        put_u32(b, static_cast<uint32_t>(s.deps.size()));
        for (const auto &d : s.deps)
            put_str(b, d);
    }
    return b;
}

bool parse_semantic_index(const std::vector<uint8_t> &bytes,
                          SemanticIndex &out) {
    Reader r{bytes.data(), bytes.data() + bytes.size()};
    if (r.u32() != VXIDX_MAGIC) return false;
    if (r.u16() != VXIDX_VERSION) return false; // sin legacy: version exacta.
    r.u16();                                    // _pad
    out = SemanticIndex{};
    out.module_hash = r.u64();
    out.module_path = r.str();
    const uint32_t count = r.u32();
    if (!r.ok || count > 10'000'000u) return false; // cota defensiva.
    out.symbols.reserve(count);
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        SymbolEntry s;
        s.name = r.str();
        if (r.p >= r.end) {
            r.ok = false;
            break;
        }
        s.kind = *r.p++;
        s.content_hash = r.u64();
        s.src_offset = r.u32();
        s.src_length = r.u32();
        if (r.p >= r.end) {
            r.ok = false;
            break;
        }
        s.is_public = (*r.p++ != 0);
        const uint32_t dc = r.u32();
        if (!r.ok || dc > 1'000'000u) return false;
        s.deps.reserve(dc);
        for (uint32_t j = 0; j < dc && r.ok; ++j)
            s.deps.push_back(r.str());
        out.symbols.push_back(std::move(s));
    }
    return r.ok;
}

std::vector<std::string> changed_symbols_closure(const SemanticIndex &old_idx,
                                                 const SemanticIndex &new_idx) {
    // 1. Mapa nombre -> hash del indice previo.
    std::unordered_map<std::string, uint64_t> old_hash;
    old_hash.reserve(old_idx.symbols.size() * 2 + 1);
    for (const auto &s : old_idx.symbols)
        old_hash[s.name] = s.content_hash;

    // 2. Cambiados de PRIMER nivel: nuevos, o con hash distinto.
    std::unordered_set<std::string> changed;        // nombres cualificados.
    std::unordered_set<std::string> changed_simple; // sus nombres simples.
    auto simple = [](const std::string &q) {
        const size_t p = q.rfind('.');
        return (p == std::string::npos) ? q : q.substr(p + 1);
    };
    for (const auto &s : new_idx.symbols) {
        auto it = old_hash.find(s.name);
        if (it == old_hash.end() || it->second != s.content_hash) {
            changed.insert(s.name);
            changed_simple.insert(simple(s.name));
        }
    }
    // Simbolos DESAPARECIDOS: cuentan como cambio (sus dependientes deben
    // revalidarse, aunque el propio simbolo ya no exista).
    {
        std::unordered_set<std::string> new_names;
        for (const auto &s : new_idx.symbols)
            new_names.insert(s.name);
        for (const auto &s : old_idx.symbols)
            if (!new_names.count(s.name)) changed_simple.insert(simple(s.name));
    }

    // 3. Cierre transitivo por DEPENDIENTES: un simbolo cuyo dep (por nombre
    //    simple) este en changed_simple, tambien cambia.  Punto fijo.
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto &s : new_idx.symbols) {
            if (changed.count(s.name)) continue;
            for (const auto &d : s.deps) {
                if (changed_simple.count(d)) {
                    changed.insert(s.name);
                    changed_simple.insert(simple(s.name));
                    grew = true;
                    break;
                }
            }
        }
    }

    std::vector<std::string> res(changed.begin(), changed.end());
    std::sort(res.begin(), res.end());
    return res;
}

std::string semantic_index_to_json(const SemanticIndex &idx) {
    auto esc = [](const std::string &s) {
        std::string o;
        o.reserve(s.size() + 2);
        for (char c : s) {
            if (c == '"' || c == '\\') o.push_back('\\');
            o.push_back(c);
        }
        return o;
    };
    std::string j = "{\"module\":\"" + esc(idx.module_path) + "\",";
    char hb[32];
    std::snprintf(hb, sizeof(hb), "%llu",
                  static_cast<unsigned long long>(idx.module_hash));
    j += "\"module_hash\":\"" + std::string(hb) + "\",\"symbols\":[";
    for (size_t i = 0; i < idx.symbols.size(); ++i) {
        const auto &s = idx.symbols[i];
        if (i) j += ",";
        std::snprintf(hb, sizeof(hb), "%llu",
                      static_cast<unsigned long long>(s.content_hash));
        j += "{\"name\":\"" + esc(s.name) +
             "\",\"kind\":" + std::to_string(static_cast<int>(s.kind)) +
             ",\"hash\":\"" + std::string(hb) +
             "\",\"offset\":" + std::to_string(s.src_offset) +
             ",\"length\":" + std::to_string(s.src_length) + ",\"deps\":[";
        for (size_t k = 0; k < s.deps.size(); ++k) {
            if (k) j += ",";
            j += "\"" + esc(s.deps[k]) + "\"";
        }
        j += "]}";
    }
    j += "]}";
    return j;
}

// -- Indices de modulos importados (CROSS-MODULE, para el LSP) ---------------

std::vector<ImportedModuleSemIndex>
build_imported_sem_indexes(const std::string &root_file,
                           const std::string &root_overlay_text,
                           const std::vector<std::string> &extra_search_paths) {
    std::vector<ImportedModuleSemIndex> out;
    // Grafo de modulos con la MISMA resolucion de paths que el compilador.
    Diagnostics diags;
    ModuleGraph graph(diags);
    // El buffer del editor como overlay del root; las deps se leen del disco.
    graph.set_source_overlay(root_file, root_overlay_text);
    for (const auto &d : extra_search_paths)
        graph.add_search_path(d);
    graph.add_vx_path_env();
    {
        std::string sd = detect_stdlib_vx_dir();
        if (!sd.empty()) graph.set_stdlib_dir(sd);
    }
    // Search path implicito: la carpeta del root (hermanos con path relativo).
    {
        std::string norm = root_file;
        for (char &c : norm)
            if (c == '\\') c = '/';
        size_t slash = norm.find_last_of('/');
        if (slash != std::string::npos)
            graph.add_search_path(norm.substr(0, slash));
    }
    const uint32_t root_id = graph.build_from_root(root_file);
    if (root_id == UINT32_MAX)
        return out; // root irresoluble: sin cross-module.

    // Un indice por cada modulo != root que resolvio y parseo.
    const size_t n = graph.module_count();
    out.reserve(n ? n - 1 : 0);
    for (uint32_t id = 0; id < n; ++id) {
        if (id == root_id) continue;
        const ResolvedModule *m = graph.module(id);
        if (!m || !m->parsed_ast) continue;
        // Leer la fuente cruda de la dependencia (esta en disco).
        std::ifstream f(m->canonical_path, std::ios::binary);
        if (!f.good()) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string src = ss.str();
        ImportedModuleSemIndex e;
        e.path = m->canonical_path;
        e.source = std::move(src);
        try {
            e.index = build_semantic_index(*m->parsed_ast, e.source,
                                           m->canonical_path);
        } catch (...) {
            continue; // modulo con AST raro: se omite, sin abortar.
        }
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace vx
