/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxi_members.h
 * @brief Un campo o un metodo de un tipo importado: de su ranura del `.vxi` a
 *        la ficha del comprobador.
 *
 * Habia cuatro copias -- struct y clase, por la ruta de `import ... only` y
 * por la de espacio de nombres -- y ya no decian lo mismo: una de ellas no
 * copiaba el simbolo del metodo, y el comprobador lo rearmaba con la clave
 * local.  Aqui hay UNA traduccion; cada ruta solo pone su resolvedor de tipos.
 */

#ifndef VX_MODULE_VXI_MEMBERS_H
#define VX_MODULE_VXI_MEMBERS_H

#include <string>

#include "vx/module/vxi_format.h"
#include "vx/type_checker.h" // StructFieldInfo, ClassMethodInfo

namespace vx {

/**
 * @brief Los atributos con que se exporta un miembro.
 * @param v           Su visibilidad.
 * @param declared_in El tipo que lo escribio.
 * @param type_key    El tipo que lo exporta: si es el mismo, no se escribe.
 * @return Los atributos.
 */
VxiMemberAttrs vxi_member_attrs_of(Visibility v, const std::string &declared_in,
                                   const std::string &type_key);

/**
 * @brief El tipo que escribio un miembro importado, con el nombre que le da
 *        QUIEN IMPORTA.
 *
 * El `.vxi` lo guarda como el modulo nombra sus tipos en las firmas, y se
 * resuelve con el mismo resolvedor que los tipos de los campos.  Si no se
 * resuelve -- una base que su modulo no exporta --, el miembro se mide contra
 * el tipo importado: quien deriva de el deriva tambien de esa base, y nadie de
 * este modulo escribe su cuerpo.
 *
 * @param written  Lo que dice el `.vxi`; vacio = el propio tipo.
 * @param resolved @p written ya resuelto (sin uso si @p written es vacio).
 * @param type_key La clave del tipo importado.
 * @return La clave del tipo que lo escribio.
 */
std::string imported_declared_in(util::InternedName written,
                                 const Type &resolved,
                                 const std::string &type_key);

/**
 * @brief Apunta en la ficha de un metodo importado que tipo lo escribio.
 *
 * Un metodo dice quien lo escribio por su procedencia: si no es el propio
 * tipo, es heredado de @p declared_in.
 *
 * @param cmi         La ficha.
 * @param declared_in El tipo que lo escribio.
 * @param type_key    El tipo importado.
 */
void stamp_imported_method_owner(ClassMethodInfo &cmi,
                                 const std::string &declared_in,
                                 const std::string &type_key);

/**
 * @brief Resuelve el tipo que escribio un miembro, si el `.vxi` lo nombra.
 * @tparam Resolve Resolvedor de tipos de la ruta de importacion.
 * @param attrs   Los atributos del miembro.
 * @param resolve El resolvedor.
 * @return El tipo; vacio si el `.vxi` no lo nombra.
 */
template <class Resolve>
Type resolve_declared_in(const VxiMemberAttrs &attrs, const Resolve &resolve) {
    if (attrs.declared_in.empty()) return Type{};
    return resolve(attrs.declared_in.str());
}

/**
 * @brief La ficha de un campo importado.
 * @tparam Resolve Resolvedor de tipos de la ruta de importacion.
 * @param fi       La ranura del `.vxi`.
 * @param resolve  El resolvedor.
 * @param type_key La clave del tipo importado.
 * @return La ficha.
 */
template <class Resolve>
StructFieldInfo field_from_vxi(const VxiSymbol::FieldInfo &fi,
                               const Resolve &resolve,
                               const std::string &type_key) {
    StructFieldInfo out;
    out.name = fi.name;
    out.type = resolve(fi.type_str);
    out.offset = fi.offset;
    out.size = fi.size;
    out.bit_offset = fi.bit_offset;
    out.bit_width = fi.bit_width;
    out.visibility = fi.attrs.visibility;
    out.declared_in = imported_declared_in(
        fi.attrs.declared_in, resolve_declared_in(fi.attrs, resolve), type_key);
    return out;
}

/**
 * @brief La ficha de un metodo importado.
 * @tparam Resolve Resolvedor de tipos de la ruta de importacion.
 * @param mi       La ranura del `.vxi`.
 * @param resolve  El resolvedor.
 * @param type_key La clave del tipo importado.
 * @return La ficha.
 */
template <class Resolve>
ClassMethodInfo method_from_vxi(const VxiSymbol::MethodInfo &mi,
                                const Resolve &resolve,
                                const std::string &type_key) {
    ClassMethodInfo cmi;
    cmi.name = mi.name;
    cmi.return_type = resolve(mi.return_type);
    cmi.vtable_index = mi.vtable_index;
    cmi.is_static = (mi.flags & VxiSymbol::kMethodStatic) != 0;
    cmi.is_constructor = (mi.flags & VxiSymbol::kMethodConstructor) != 0;
    cmi.is_comptime = (mi.flags & VxiSymbol::kMethodComptime) != 0;
    cmi.defining_class = type_key;
    /* El simbolo lo trae hecho el modulo que lo emitio: se COPIA, no se arma
     * con la clave local. */
    cmi.link_name = mi.mangled_label;
    cmi.param_types.reserve(mi.param_types.size());
    for (const std::string &pt : mi.param_types)
        cmi.param_types.push_back(resolve(pt));
    cmi.visibility = mi.attrs.visibility;
    stamp_imported_method_owner(
        cmi,
        imported_declared_in(mi.attrs.declared_in,
                             resolve_declared_in(mi.attrs, resolve), type_key),
        type_key);
    return cmi;
}

/**
 * @struct TypeStringResolver
 * @brief El resolvedor corriente del comprobador, con la forma que piden
 *        @ref field_from_vxi y @ref method_from_vxi.
 */
struct TypeStringResolver {
    TypeChecker &tc; ///< el comprobador que resuelve

    /**
     * @brief Resuelve un tipo escrito.
     * @param s El tipo.
     * @return El tipo resuelto.
     */
    Type operator()(const std::string &s) const {
        return tc.resolve_type_string(s);
    }
};

} // namespace vx

#endif // VX_MODULE_VXI_MEMBERS_H
