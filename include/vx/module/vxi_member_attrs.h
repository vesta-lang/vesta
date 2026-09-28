/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxi_member_attrs.h
 * @brief Lo que un campo o un metodo exportado dice de si mismo en el `.vxi`,
 *        mas alla de su forma: quien lo ve y que tipo lo escribio.
 *
 * Sin esto un miembro `private` de un tipo importado llegaba al otro modulo
 * como uno cualquiera y se podia usar.
 *
 * En disco es un BLOQUE de atributos con etiqueta tras cada miembro: una
 * cuenta y, por atributo, `{u16 etiqueta, u16 longitud, datos}`.  Un lector
 * salta las etiquetas que no conoce, asi que anadir un atributo no obliga a
 * cambiar la forma de las ranuras.  Un atributo en su valor por defecto no se
 * escribe: la mayoria de miembros no lleva ninguno.
 */

#ifndef VX_MODULE_VXI_MEMBER_ATTRS_H
#define VX_MODULE_VXI_MEMBER_ATTRS_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/name_pool.h" // util::InternedName
#include "vx/visibility.h"

namespace vx {

namespace vxi_io {
class StringPoolBuilder;
}

/**
 * @enum VxiMemberAttr
 * @brief La etiqueta de un atributo de miembro en disco.  Valores ESTABLES:
 *        se persisten, no se reordenan ni se reutilizan.
 */
enum class VxiMemberAttr : uint16_t {
    Visibility = 1, ///< u8: el valor de @ref vx::Visibility
    DeclaredIn = 2, ///< u32 desplazamiento + u32 longitud en el pozo
};

/**
 * @struct VxiMemberAttrs
 * @brief Los atributos de un miembro exportado, ya leidos.
 */
struct VxiMemberAttrs {
    /// Quien lo ve.
    Visibility visibility = Visibility::Unwritten;
    /// El tipo que lo ESCRIBIO, con el nombre con que el modulo nombra sus
    /// tipos en las firmas.  Vacio = el propio tipo que lo exporta.
    util::InternedName declared_in;
};

/**
 * @enum VxiMemberAttrsRead
 * @brief Como acabo la lectura de un bloque de atributos.
 */
enum class VxiMemberAttrsRead : uint8_t {
    Ok,              ///< leido
    Truncated,       ///< el bloque se sale del fichero
    BadLength,       ///< una etiqueta conocida con una longitud que no es suya
    BadVisibility,   ///< un valor de visibilidad que no existe
};

/**
 * @brief Escribe el bloque de atributos de un miembro.
 * @param payload Carga util del simbolo.
 * @param pool    Pozo de cadenas del fichero.
 * @param attrs   Los atributos.
 */
void vxi_emit_member_attrs(std::vector<uint8_t> &payload,
                           vxi_io::StringPoolBuilder &pool,
                           const VxiMemberAttrs &attrs);

/**
 * @brief Lee el bloque de atributos de un miembro.
 * @param data       El fichero.
 * @param size       Su tamano.
 * @param off        Posicion; avanza lo leido.
 * @param pool_start Donde empieza el pozo de cadenas.
 * @param out        Destino.
 * @return Como acabo.
 */
VxiMemberAttrsRead vxi_read_member_attrs(const uint8_t *data, size_t size,
                                         size_t &off, uint32_t pool_start,
                                         VxiMemberAttrs &out);

/**
 * @brief El motivo de una lectura fallida, para el mensaje con que se rechaza
 *        el `.vxi`.
 * @param r El resultado.
 * @return Texto en ingles; vacio si @p r es Ok.
 */
const char *vxi_member_attrs_read_error(VxiMemberAttrsRead r);

} // namespace vx

#endif // VX_MODULE_VXI_MEMBER_ATTRS_H
