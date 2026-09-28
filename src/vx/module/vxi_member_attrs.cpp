/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxi_member_attrs.cpp
 * @brief El bloque de atributos de un miembro en el `.vxi`: escribirlo y
 *        leerlo.
 */

#include "vx/module/vxi_member_attrs.h"

#include "vx/module/vxi_io.h"

namespace vx {

using namespace vxi_io;

namespace {

/// Bytes de los datos de @ref VxiMemberAttr::Visibility.
constexpr uint16_t kVisibilityBytes = 1;
/// Bytes de los datos de @ref VxiMemberAttr::DeclaredIn.
constexpr uint16_t kDeclaredInBytes = 8;

/**
 * @brief Escribe la cabecera de un atributo.
 * @param payload Carga util.
 * @param tag     Etiqueta.
 * @param len     Bytes de sus datos.
 */
void emit_attr_header(std::vector<uint8_t> &payload, VxiMemberAttr tag,
                      uint16_t len) {
    write_u16(payload, static_cast<uint16_t>(tag));
    write_u16(payload, len);
}

} // namespace

void vxi_emit_member_attrs(std::vector<uint8_t> &payload,
                           StringPoolBuilder &pool,
                           const VxiMemberAttrs &attrs) {
    const bool has_visibility = attrs.visibility != Visibility::Unwritten;
    const bool has_declared_in = !attrs.declared_in.empty();
    write_u16(payload, static_cast<uint16_t>((has_visibility ? 1 : 0) +
                                             (has_declared_in ? 1 : 0)));
    if (has_visibility) {
        emit_attr_header(payload, VxiMemberAttr::Visibility, kVisibilityBytes);
        write_u8(payload, static_cast<uint8_t>(attrs.visibility));
    }
    if (has_declared_in) {
        const std::string &text = attrs.declared_in.str();
        emit_attr_header(payload, VxiMemberAttr::DeclaredIn, kDeclaredInBytes);
        write_u32(payload, pool.intern(text));
        write_u32(payload, static_cast<uint32_t>(text.size()));
    }
}

VxiMemberAttrsRead vxi_read_member_attrs(const uint8_t *data, size_t size,
                                         size_t &off, uint32_t pool_start,
                                         VxiMemberAttrs &out) {
    uint16_t count = 0;
    if (!read_u16(data, size, off, count)) return VxiMemberAttrsRead::Truncated;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t tag = 0;
        uint16_t len = 0;
        if (!read_u16(data, size, off, tag) || !read_u16(data, size, off, len))
            return VxiMemberAttrsRead::Truncated;
        if (off + len > size) return VxiMemberAttrsRead::Truncated;
        switch (static_cast<VxiMemberAttr>(tag)) {
        case VxiMemberAttr::Visibility: {
            if (len != kVisibilityBytes) return VxiMemberAttrsRead::BadLength;
            uint8_t v = 0;
            (void)read_u8(data, size, off, v);
            /* Un valor que no existe no se toma por uno cualquiera: leerlo
             * como "sin palabra" abriria un miembro privado a todos. */
            if (v > kVisibilityMax) return VxiMemberAttrsRead::BadVisibility;
            out.visibility = static_cast<Visibility>(v);
            break;
        }
        case VxiMemberAttr::DeclaredIn: {
            if (len != kDeclaredInBytes) return VxiMemberAttrsRead::BadLength;
            uint32_t name_off = 0;
            uint32_t name_len = 0;
            (void)read_u32(data, size, off, name_off);
            (void)read_u32(data, size, off, name_len);
            std::string text;
            if (!read_name(data, size, name_off, name_len, pool_start, text))
                return VxiMemberAttrsRead::Truncated;
            out.declared_in = util::InternedName::intern(text);
            break;
        }
        default:
            /* Una etiqueta de un escritor mas nuevo: se salta entera. */
            off += len;
            break;
        }
    }
    return VxiMemberAttrsRead::Ok;
}

const char *vxi_member_attrs_read_error(VxiMemberAttrsRead r) {
    switch (r) {
    case VxiMemberAttrsRead::Ok: return "";
    case VxiMemberAttrsRead::Truncated:
        return "member attribute block runs past the end of the file";
    case VxiMemberAttrsRead::BadLength:
        return "member attribute with a length that does not match its tag";
    case VxiMemberAttrsRead::BadVisibility:
        return "member visibility value out of range";
    }
    return "unknown member attribute read result";
}

} // namespace vx
