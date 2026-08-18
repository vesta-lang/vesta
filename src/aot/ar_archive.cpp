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
 * @file aot/ar_archive.cpp
 * @brief Implementacion del lector de archivos estaticos ar(1) (.a).
 * @see aot/ar_archive.h
 */

#include "aot/ar_archive.h"

namespace aot {
namespace {

constexpr size_t AR_MAGIC_LEN = 8; // "!<arch>\n"
constexpr size_t AR_HDR_LEN = 60;  // cabecera de miembro
constexpr size_t AR_NAME_OFF = 0;  // nombre (16 bytes)
constexpr size_t AR_NAME_LEN = 16;
constexpr size_t AR_SIZE_OFF = 48; // tamano (10 bytes, decimal ASCII)
constexpr size_t AR_SIZE_LEN = 10;

/// Lee un campo decimal ASCII (con relleno de espacios) -> size_t.
size_t read_decimal(const uint8_t *p, size_t n) {
    size_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        if (c < '0' || c > '9') break; // para en espacio/relleno
        v = v * 10 + (size_t)(c - '0');
    }
    return v;
}

/// uint32 big-endian (formato del indice de simbolos GNU).
uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/// Recorta espacios y '/' finales de un nombre corto GNU.
std::string trim_name(const uint8_t *p, size_t n) {
    size_t end = n;
    while (end > 0 && (p[end - 1] == ' ' || p[end - 1] == '\n'))
        --end;
    // GNU termina los nombres cortos con '/'.
    if (end > 0 && p[end - 1] == '/') --end;
    return std::string(reinterpret_cast<const char *>(p), end);
}

} // namespace

namespace {
// true si @p buf empieza con el magic de un thin archive ("!<thin>\n").
bool ar_is_thin(const std::vector<uint8_t> &buf) {
    static const char kThin[AR_MAGIC_LEN] = {'!', '<', 't', 'h',
                                             'i', 'n', '>', '\n'};
    if (buf.size() < AR_MAGIC_LEN) return false;
    for (size_t i = 0; i < AR_MAGIC_LEN; ++i)
        if ((char)buf[i] != kThin[i]) return false;
    return true;
}
} // namespace

bool ar_is_archive(const std::vector<uint8_t> &buf) {
    static const char kMagic[AR_MAGIC_LEN] = {'!', '<', 'a', 'r',
                                              'c', 'h', '>', '\n'};
    if (buf.size() < AR_MAGIC_LEN) return false;
    bool reg = true;
    for (size_t i = 0; i < AR_MAGIC_LEN; ++i)
        if ((char)buf[i] != kMagic[i]) {
            reg = false;
            break;
        }
    return reg || ar_is_thin(buf); // regular o thin (ambos parseables)
}

bool ar_parse(const std::vector<uint8_t> &buf, std::vector<ArMember> &members,
              std::vector<ArSymbol> &symbols, std::string &err) {
    members.clear();
    symbols.clear();
    if (!ar_is_archive(buf)) {
        err = "no es un archivo .a (magic '!<arch>'/'!<thin>' ausente)";
        return false;
    }
    // En un thin archive los miembros no llevan datos inline: la ruta del
    // objeto (resuelta desde la tabla "//") es el nombre, y el linker lee ese
    // fichero.
    const bool thin = ar_is_thin(buf);

    // Tabla de nombres largos GNU ("//"): span dentro del buffer.
    const uint8_t *longnames = nullptr;
    size_t longnames_len = 0;

    // Indice de simbolos GNU pendiente de mapear a indices de miembro:
    // (nombre, header_offset del miembro que lo define).
    std::vector<std::pair<std::string, size_t>> pending_syms;

    size_t pos = AR_MAGIC_LEN;
    while (pos + AR_HDR_LEN <= buf.size()) {
        const uint8_t *hdr = &buf[pos];
        // Validar el terminador de cabecera "`\n".
        if (hdr[58] != 0x60 || hdr[59] != 0x0A) {
            err = "archivo .a: cabecera de miembro corrupta";
            return false;
        }
        const size_t header_offset = pos;
        const size_t hdr_size = read_decimal(hdr + AR_SIZE_OFF, AR_SIZE_LEN);
        // En un thin archive los miembros-OBJETO no llevan datos inline (su
        // size en la cabecera es el del fichero externo); los especiales
        // ("/" symtab y "//" longnames) SI son inline.
        const uint8_t *raw_pre = hdr + AR_NAME_OFF;
        const bool special =
            (raw_pre[0] == '/' &&
             (raw_pre[1] == ' ' || raw_pre[1] == '\n' || raw_pre[1] == '/'));
        const bool external_data = thin && !special;
        size_t data_size = external_data ? 0 : hdr_size;
        size_t data_offset = pos + AR_HDR_LEN;
        if (data_offset + data_size > buf.size()) {
            err = "archivo .a: miembro truncado";
            return false;
        }

        // Nombre crudo (16 bytes).
        const uint8_t *raw = hdr + AR_NAME_OFF;
        std::string name;
        bool is_symtab = false;
        bool is_longtab = false;

        if (raw[0] == '/' && (raw[1] == ' ' || raw[1] == '\n')) {
            // Indice de simbolos GNU ("/").
            is_symtab = true;
        } else if (raw[0] == '/' && raw[1] == '/') {
            // Tabla de nombres largos GNU ("//").
            is_longtab = true;
        } else if (raw[0] == '/' && raw[1] >= '0' && raw[1] <= '9') {
            // Referencia a nombre largo GNU ("/N").
            const size_t off = read_decimal(raw + 1, AR_NAME_LEN - 1);
            if (longnames && off < longnames_len) {
                size_t e = off;
                while (e < longnames_len && longnames[e] != '\n' &&
                       longnames[e] != '/')
                    ++e;
                name.assign(reinterpret_cast<const char *>(longnames + off),
                            e - off);
            }
        } else if (raw[0] == '#' && raw[1] == '1' && raw[2] == '/') {
            // Nombre largo BSD ("#1/<len>"): los primeros <len> bytes de los
            // datos son el nombre real.
            const size_t nlen = read_decimal(raw + 3, AR_NAME_LEN - 3);
            if (nlen <= data_size) {
                name.assign(reinterpret_cast<const char *>(&buf[data_offset]),
                            nlen);
                data_offset += nlen; // los datos reales empiezan tras el nombre
                data_size -= nlen;
                // Detectar el indice de simbolos BSD por nombre.
                if (name == "__.SYMDEF" || name == "__.SYMDEF SORTED")
                    is_symtab = true;
            }
        } else {
            name = trim_name(raw, AR_NAME_LEN);
        }

        if (is_longtab) {
            longnames = &buf[data_offset];
            longnames_len = data_size;
        } else if (is_symtab) {
            // Parsear el indice GNU: [u32be count][count u32be offsets]
            // [count nombres NUL-terminados].  (BSD __.SYMDEF se ignora: el
            // linker escaneara los symtab de los miembros como fallback.)
            if (raw[0] == '/' && data_size >= 4) {
                const uint8_t *d = &buf[data_offset];
                const uint32_t count = rd32be(d);
                const size_t names_off = 4 + (size_t)count * 4;
                if (names_off <= data_size) {
                    size_t np = names_off;
                    for (uint32_t i = 0; i < count; ++i) {
                        const uint32_t moff = rd32be(d + 4 + (size_t)i * 4);
                        // Nombre NUL-terminado.
                        size_t ne = np;
                        while (ne < data_size && d[ne] != 0)
                            ++ne;
                        if (ne > data_size) break;
                        pending_syms.emplace_back(
                            std::string(reinterpret_cast<const char *>(d + np),
                                        ne - np),
                            (size_t)moff);
                        np = ne + 1;
                    }
                }
            }
        } else {
            // Miembro-objeto normal (o thin: la ruta esta en el nombre).
            ArMember m;
            m.name = name.empty() ? "<anon>" : name;
            m.header_offset = header_offset;
            m.data_offset = thin ? 0 : data_offset;
            m.size = thin ? 0 : data_size;
            m.is_thin = thin;
            members.push_back(std::move(m));
        }

        // Avanzar al siguiente miembro (datos alineados a 2 bytes).  Para los
        // objetos de un thin archive no hay datos inline (consumed=0); para el
        // resto, el size ORIGINAL de la cabecera (en BSD incluye el nombre).
        size_t consumed = external_data ? 0 : hdr_size;
        pos = header_offset + AR_HDR_LEN + consumed;
        if (pos & 1) ++pos; // padding a frontera par
    }

    // Mapear el indice de simbolos a indices de miembro via header_offset.
    if (!pending_syms.empty()) {
        symbols.reserve(pending_syms.size());
        for (auto &ps : pending_syms) {
            int idx = -1;
            for (size_t i = 0; i < members.size(); ++i)
                if (members[i].header_offset == ps.second) {
                    idx = (int)i;
                    break;
                }
            ArSymbol s;
            s.name = std::move(ps.first);
            s.member_index = idx;
            symbols.push_back(std::move(s));
        }
    }
    return true;
}

} // namespace aot
