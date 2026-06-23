/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file aot/linker.cpp
 * @brief Phase AOT.5 -- linker propio: fusiona objetos ELF64 .o en un EXEC.
 *
 * Parsea cada ELF64 @c ET_REL a mano (lectores little-endian propios -- NO
 * incluye LibPEparse para respetar la frontera C/C++ del proyecto; el shim .c
 * sigue siendo el unico que toca la lib).  Fusiona las secciones ALLOC por
 * nombre, construye una tabla de simbolos global, resuelve cada reloc del .o
 * a una @c aot::AbsReloc y emite el ejecutable via @c aot::ObjectWriter (que
 * reutiliza su motor de relocs tras el layout).
 */

#include "aot/linker.h"

#include "aot/aot_native.h" // aot_make_start_stub, AotArch

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace aot {
namespace {

// ---- Lectores little-endian sobre el buffer del .o ----
inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
inline uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// ---- Constantes ELF64 (subset que usamos) ----
constexpr uint16_t ET_REL = 1;
constexpr uint32_t SHT_SYMTAB = 2;
constexpr uint32_t SHT_RELA = 4;
constexpr uint32_t SHT_NOBITS = 8;
constexpr uint64_t SHF_WRITE = 0x1;
constexpr uint64_t SHF_ALLOC = 0x2;
constexpr uint64_t SHF_EXECINSTR = 0x4;
constexpr uint16_t SHN_UNDEF = 0;
constexpr uint8_t STB_GLOBAL = 1;
constexpr uint8_t STT_SECTION = 3;
// Tipos de reloc x86-64.
constexpr uint32_t R_X86_64_64 = 1;
constexpr uint32_t R_X86_64_PC32 = 2;
constexpr uint32_t R_X86_64_PLT32 = 4;
constexpr uint32_t R_X86_64_32 = 10;
constexpr uint32_t R_X86_64_32S = 11;

struct ObjSec {
    std::string name;
    uint32_t sh_type = 0;
    uint64_t sh_flags = 0;
    uint64_t sh_size = 0;
    uint64_t sh_offset = 0;
    uint64_t sh_addralign = 0;
    uint32_t sh_info = 0; // SHT_RELA: seccion a la que aplica
};
struct ObjSym {
    std::string name;
    uint8_t type = 0;
    uint8_t bind = 0;
    uint16_t shndx = 0;
    uint64_t value = 0;
};
struct ObjRel {
    uint32_t applies_sh = 0;
    uint64_t off = 0;
    uint32_t sym = 0;
    uint32_t type = 0;
    int64_t addend = 0;
};
struct ParsedObj {
    std::string path;
    std::vector<uint8_t> bytes;
    std::vector<ObjSec> secs; // indexado por shndx ELF
    std::vector<ObjSym> syms; // indexado por indice de symtab
    std::vector<ObjRel> rels;
};

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out.resize((size_t)n);
    if (n > 0)
        f.read(reinterpret_cast<char *>(out.data()), n);
    return (bool)f;
}

bool parse_obj(const std::string &path, ParsedObj &po, std::string &err) {
    po.path = path;
    if (!read_file(path, po.bytes)) {
        err = "no se puede abrir el objeto: " + path;
        return false;
    }
    const std::vector<uint8_t> &b = po.bytes;
    if (b.size() < 64 || b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' ||
        b[3] != 'F') {
        err = path + ": no es un ELF";
        return false;
    }
    if (b[4] != 2) { // ELFCLASS64
        err = path + ": no es ELF64 (slice 1 solo enlaza ELF64)";
        return false;
    }
    const uint16_t e_type = rd16(&b[16]);
    if (e_type != ET_REL) {
        err = path + ": no es un objeto relocatable (ET_REL)";
        return false;
    }
    const uint64_t e_shoff = rd64(&b[40]);
    const uint16_t e_shentsize = rd16(&b[58]);
    const uint16_t e_shnum = rd16(&b[60]);
    const uint16_t e_shstrndx = rd16(&b[62]);
    if (e_shoff == 0 || e_shnum == 0 ||
        e_shoff + (uint64_t)e_shnum * e_shentsize > b.size()) {
        err = path + ": tabla de secciones invalida";
        return false;
    }
    // Cabeceras de seccion (sin nombre todavia).
    po.secs.resize(e_shnum);
    for (uint16_t i = 0; i < e_shnum; ++i) {
        const uint8_t *sh = &b[e_shoff + (uint64_t)i * e_shentsize];
        ObjSec &s = po.secs[i];
        s.sh_type = rd32(sh + 4);
        s.sh_flags = rd64(sh + 8);
        s.sh_offset = rd64(sh + 24);
        s.sh_size = rd64(sh + 32);
        s.sh_info = rd32(sh + 44);
        s.sh_addralign = rd64(sh + 48);
        // sh_name lo resolvemos tras conocer la shstrtab.
        s.name.assign(reinterpret_cast<const char *>(sh)); // placeholder
        s.name.clear();
        s.name = std::to_string(rd32(sh)); // guarda sh_name como string temp
    }
    // Resolver nombres via shstrtab.
    if (e_shstrndx >= e_shnum) {
        err = path + ": shstrndx invalido";
        return false;
    }
    const ObjSec &shstr = po.secs[e_shstrndx];
    auto str_at = [&](uint64_t base, uint32_t off) -> std::string {
        uint64_t p = base + off;
        std::string r;
        while (p < b.size() && b[p] != 0)
            r.push_back((char)b[p++]);
        return r;
    };
    for (uint16_t i = 0; i < e_shnum; ++i) {
        const uint8_t *sh = &b[e_shoff + (uint64_t)i * e_shentsize];
        po.secs[i].name = str_at(shstr.sh_offset, rd32(sh));
    }
    // Symtab + strtab.
    int symtab_idx = -1;
    for (uint16_t i = 0; i < e_shnum; ++i)
        if (po.secs[i].sh_type == SHT_SYMTAB) { symtab_idx = i; break; }
    if (symtab_idx >= 0) {
        const uint8_t *shp = &b[e_shoff + (uint64_t)symtab_idx * e_shentsize];
        const uint32_t strtab_idx = rd32(shp + 40); // sh_link
        const ObjSec &st = po.secs[symtab_idx];
        const ObjSec &strs =
            (strtab_idx < e_shnum) ? po.secs[strtab_idx] : po.secs[0];
        const uint64_t nsyms = st.sh_size / 24;
        po.syms.resize(nsyms);
        for (uint64_t k = 0; k < nsyms; ++k) {
            const uint8_t *sy = &b[st.sh_offset + k * 24];
            ObjSym &os = po.syms[k];
            os.name = str_at(strs.sh_offset, rd32(sy));
            const uint8_t info = sy[4];
            os.type = info & 0xf;
            os.bind = info >> 4;
            os.shndx = rd16(sy + 6);
            os.value = rd64(sy + 8);
        }
    }
    // Relocs (SHT_RELA).
    for (uint16_t i = 0; i < e_shnum; ++i) {
        const ObjSec &rs = po.secs[i];
        if (rs.sh_type != SHT_RELA) continue;
        const uint64_t nrel = rs.sh_size / 24;
        for (uint64_t k = 0; k < nrel; ++k) {
            const uint8_t *re = &b[rs.sh_offset + k * 24];
            ObjRel r;
            r.applies_sh = rs.sh_info;
            r.off = rd64(re);
            const uint64_t info = rd64(re + 8);
            r.sym = (uint32_t)(info >> 32);
            r.type = (uint32_t)(info & 0xffffffff);
            r.addend = (int64_t)rd64(re + 16);
            po.rels.push_back(r);
        }
    }
    return true;
}

// Seccion fusionada de salida.
struct MergedSec {
    std::string name;
    uint32_t perms = 0;        // r/w/x compuesto en flags del writer
    std::vector<uint8_t> data; // bytes (PROGBITS)
    uint64_t bss_size = 0;     // cola NOBITS
    uint64_t align = 8;
};

inline uint64_t align_up(uint64_t v, uint64_t a) {
    if (a < 1) a = 1;
    return (v + a - 1) / a * a;
}

} // namespace

bool aot_link(const std::vector<std::string> &inputs,
              const std::string &out_path, const LinkOptions &opts,
              std::string &err) {
    if (opts.fmt != ObjFormat::ELF) {
        err = "linker: slice 1 solo soporta --format elf";
        return false;
    }
    if (inputs.empty()) {
        err = "linker: sin objetos de entrada";
        return false;
    }
    // 1. Parsear todos los objetos.
    std::vector<ParsedObj> objs(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
        if (!parse_obj(inputs[i], objs[i], err)) return false;

    // 2. Fusionar secciones ALLOC por nombre.  map[(obj, shndx)] ->
    //    (indice de seccion fusionada, offset base dentro de ella).
    std::vector<MergedSec> merged;
    std::unordered_map<std::string, int> merged_by_name;
    struct SecMap {
        int mindex = -1;
        uint64_t base = 0;
    };
    std::vector<std::vector<SecMap>> secmap(objs.size());

    for (size_t oi = 0; oi < objs.size(); ++oi) {
        ParsedObj &o = objs[oi];
        secmap[oi].resize(o.secs.size());
        for (size_t si = 0; si < o.secs.size(); ++si) {
            ObjSec &s = o.secs[si];
            if (!(s.sh_flags & SHF_ALLOC)) continue;
            // perms por flags.
            uint32_t perms = SecFlag::READ;
            const bool is_exec = (s.sh_flags & SHF_EXECINSTR) != 0;
            const bool is_write = (s.sh_flags & SHF_WRITE) != 0;
            if (is_exec)
                perms |= SecFlag::EXEC | SecFlag::CODE;
            else if (is_write)
                perms |= SecFlag::WRITE | SecFlag::DATA;
            else
                perms |= SecFlag::DATA;
            const bool is_bss = (s.sh_type == SHT_NOBITS);
            if (is_bss) perms |= SecFlag::BSS;
            int mi;
            auto it = merged_by_name.find(s.name);
            if (it == merged_by_name.end()) {
                mi = (int)merged.size();
                merged_by_name[s.name] = mi;
                MergedSec m;
                m.name = s.name;
                m.perms = perms;
                m.align = is_exec ? 16 : 8;
                merged.push_back(std::move(m));
            } else {
                mi = it->second;
                merged[mi].perms |= perms; // union conservadora
            }
            MergedSec &m = merged[mi];
            uint64_t a = s.sh_addralign ? s.sh_addralign : 1;
            if (a > m.align) m.align = a;
            // Posicion base = tras data + bss actuales, alineada.
            uint64_t cur = m.data.size() + m.bss_size;
            uint64_t base = align_up(cur, a);
            // Rellenar el hueco de alineamiento con ceros en data (si no hay
            // bss intermedio) -- mantiene data contigua antes del bss.
            if (m.bss_size == 0 && base > m.data.size())
                m.data.resize(base, 0);
            secmap[oi][si].mindex = mi;
            secmap[oi][si].base = base;
            if (is_bss) {
                m.bss_size = base + s.sh_size - m.data.size();
            } else {
                // Si ya habia bss, no podemos intercalar data limpia; para el
                // slice 1 asumimos PROGBITS antes que NOBITS por nombre.
                if (base < m.data.size()) base = m.data.size();
                m.data.resize(base, 0);
                const uint8_t *src = &o.bytes[s.sh_offset];
                m.data.insert(m.data.end(), src, src + s.sh_size);
            }
        }
    }

    // 3. Indices de seccion en el ObjectWriter.  Hosted (sin --entry) reserva
    //    el indice 0 para el stub _start.
    const bool hosted = opts.entry.empty();
    const int sec_base = hosted ? 1 : 0;

    // 4. Tabla de simbolos global: nombre -> (writer-sec, offset).
    struct GDef {
        int wsec = -1;
        uint64_t off = 0;
        bool defined = false;
    };
    std::unordered_map<std::string, GDef> globals;
    for (size_t oi = 0; oi < objs.size(); ++oi) {
        ParsedObj &o = objs[oi];
        for (ObjSym &sy : o.syms) {
            if (sy.bind != STB_GLOBAL || sy.shndx == SHN_UNDEF ||
                sy.name.empty() || sy.type == STT_SECTION)
                continue;
            if (sy.shndx >= secmap[oi].size()) continue;
            const SecMap &sm = secmap[oi][sy.shndx];
            if (sm.mindex < 0) continue;
            GDef d;
            d.wsec = sec_base + sm.mindex;
            d.off = sm.base + sy.value;
            d.defined = true;
            auto it = globals.find(sy.name);
            if (it != globals.end() && it->second.defined) {
                err = "linker: definicion multiple del simbolo '" + sy.name + "'";
                return false;
            }
            globals[sy.name] = d;
        }
    }

    // 5. Crear el ObjectWriter y anyadir las secciones (stub primero si hosted).
    ObjectWriter w(opts.fmt);
    LayoutConfig cfg = opts.layout;
    if (opts.image_base) cfg.image_base = opts.image_base;
    w.set_config(cfg);

    StartStub stub;
    if (hosted) {
        stub = aot_make_start_stub(AotArch::X86_64, opts.fmt);
        if (!stub.ok) {
            err = "linker: no se pudo sintetizar _start: " + stub.err;
            return false;
        }
        WriterSection ss;
        ss.name = ".text._start";
        ss.flags = SecFlag::READ | SecFlag::EXEC | SecFlag::CODE;
        ss.data = stub.bytes;
        ss.align = 16;
        w.add_section(std::move(ss)); // indice 0
    }
    for (MergedSec &m : merged) {
        WriterSection ws;
        ws.name = m.name;
        ws.flags = m.perms;
        ws.data = std::move(m.data);
        ws.bss_size = (uint32_t)m.bss_size;
        ws.align = m.align;
        w.add_section(std::move(ws));
    }

    // 6. Resolver relocs -> AbsReloc del writer.
    std::vector<std::string> unresolved;
    for (size_t oi = 0; oi < objs.size(); ++oi) {
        ParsedObj &o = objs[oi];
        for (ObjRel &r : o.rels) {
            if (r.applies_sh >= secmap[oi].size()) continue;
            const SecMap &site = secmap[oi][r.applies_sh];
            if (site.mindex < 0) continue; // reloc en seccion no-ALLOC: ignorar
            const int site_sec = sec_base + site.mindex;
            const uint64_t site_off = site.base + r.off;
            if (r.sym >= o.syms.size()) continue;
            ObjSym &sy = o.syms[r.sym];

            RelocTarget tgt;
            bool ok = false;
            if (sy.type == STT_SECTION) {
                if (sy.shndx < secmap[oi].size() &&
                    secmap[oi][sy.shndx].mindex >= 0) {
                    const SecMap &tm = secmap[oi][sy.shndx];
                    tgt = RelocTarget::addr(sec_base + tm.mindex, tm.base);
                    ok = true;
                }
            } else if (sy.shndx != SHN_UNDEF) {
                if (sy.shndx < secmap[oi].size() &&
                    secmap[oi][sy.shndx].mindex >= 0) {
                    const SecMap &tm = secmap[oi][sy.shndx];
                    tgt = RelocTarget::addr(sec_base + tm.mindex,
                                            tm.base + sy.value);
                    ok = true;
                }
            } else { // UNDEF -> resolver por nombre global
                auto it = globals.find(sy.name);
                if (it != globals.end() && it->second.defined) {
                    tgt = RelocTarget::addr(it->second.wsec, it->second.off);
                    ok = true;
                } else {
                    unresolved.push_back(sy.name);
                    continue;
                }
            }
            if (!ok) continue;

            RelocKind kind;
            int64_t addend = r.addend;
            switch (r.type) {
            case R_X86_64_PC32:
            case R_X86_64_PLT32:
                kind = RelocKind::REL32;
                // writer REL32 = sec_va+off+addend-(site+4); ELF PC32 = S+A-P.
                // -> addend += 4 para igualar.
                addend += 4;
                break;
            case R_X86_64_64:
                kind = RelocKind::ABS64;
                break;
            case R_X86_64_32:
            case R_X86_64_32S:
                kind = RelocKind::IMM32;
                break;
            default:
                err = "linker: tipo de reloc no soportado (" +
                      std::to_string(r.type) + ") en " + o.path;
                return false;
            }
            w.add_reloc(site_sec, site_off, tgt, kind, addend);
        }
    }

    // 7. Punto de entrada.
    if (hosted) {
        auto it = globals.find("main");
        if (it == globals.end() || !it->second.defined) {
            err = "linker: 'main' no esta definido (usa --entry <sym> para un "
                  "binario sin main, p.ej. un kernel)";
            return false;
        }
        w.set_entry(0, 0); // stub en indice 0, offset 0
        w.add_reloc(0, stub.main_call_off,
                    RelocTarget::addr(it->second.wsec, it->second.off),
                    RelocKind::REL32);
        if (stub.has_import_call)
            w.add_import_call(ImportCall{stub.import_dll, stub.import_func, 0,
                                         stub.import_call_off});
    } else {
        auto it = globals.find(opts.entry);
        if (it == globals.end() || !it->second.defined) {
            err = "linker: simbolo de entrada '" + opts.entry +
                  "' no esta definido";
            return false;
        }
        w.set_entry(it->second.wsec, it->second.off);
    }

    if (!unresolved.empty()) {
        err = "linker: simbolos no resueltos:";
        for (const std::string &u : unresolved)
            err += " " + u;
        return false;
    }

    return w.write(out_path, err);
}

} // namespace aot
