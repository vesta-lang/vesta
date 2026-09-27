/**
 * @file type_fingerprints.cpp
 * @brief La huella de cada tipo agregado.
 * @see vx/type_fingerprints.h
 */
#include "vx/type_fingerprints.h"

#include "vx/type_checker.h"

#include <utility>

namespace vx {

namespace {

/**
 * @brief Copia la colocacion de cada campo a la huella, con los nombres
 *        internados.
 * @tparam Fields La lista de campos del layout (struct o clase).
 * @param fields Los campos.
 * @param tf     La huella que los recibe.
 */
template <typename Fields>
void place_fields(const Fields &fields, analyze::TypeFingerprint &tf) {
    tf.fields.reserve(fields.size());
    for (const auto &f : fields) {
        analyze::FieldPlacement fp;
        fp.name = util::InternedName::intern(f.name);
        fp.type_name = util::InternedName::intern(type_to_string(f.type));
        fp.offset = f.offset;
        fp.size = f.size;
        fp.bit_offset = f.bit_offset;
        fp.bit_width = f.bit_width;
        tf.fields.push_back(fp);
    }
}

} // namespace

analyze::TypeFingerprints compute_type_fingerprints(const TypeChecker &tc) {
    analyze::TypeFingerprints out;
    using TF = analyze::TypeFingerprint;

    /* Solo los tipos que DECLARA este modulo: los que importa los calcula el
     * modulo que los declara, y repetirlos aqui seria calcular y guardar lo
     * mismo una vez por cada modulo que los usa. */
    // Structs: value-types.  @pod = C-representable por valor + sin dtor.
    for (const auto &kv : tc.struct_layouts()) {
        if (tc.is_imported(kv.first)) continue;
        const StructLayout &lay = kv.second;
        TF tf;
        tf.type_name = util::InternedName::intern(lay.name);
        tf.kind = TF::STRUCT;
        tf.size_bytes = lay.size_bytes;
        tf.align_bytes = lay.align_bytes;
        tf.field_count = static_cast<uint32_t>(lay.fields.size());
        bool has_dtor = lay.has_destructible_field;
        for (const auto &m : lay.methods)
            if (m.is_destructor) has_dtor = true;
        tf.has_destructor = has_dtor;
        bool no_heap = true;
        bool all_c_repr = true;
        for (const auto &f : lay.fields) {
            if (tc.type_is_managed(f.type)) no_heap = false;
            if (!tc.type_is_c_representable(f.type)) all_c_repr = false;
        }
        tf.no_heap = no_heap;
        tf.is_pod = all_c_repr && !has_dtor && no_heap;
        tf.is_reference = false;
        tf.is_union = lay.is_union;
        tf.is_overlay = lay.is_overlay;
        tf.is_polymorphic = lay.is_polymorphic;
        place_fields(lay.fields, tf);
        out.push_back(std::move(tf));
    }

    // Clases: tipos por REFERENCIA (viven en el heap gestionado) -> nunca @pod
    // ni @no_heap; @size verifica el tamano de la instancia.
    for (const auto &kv : tc.class_layouts()) {
        if (tc.is_imported(kv.first)) continue;
        const ClassLayout &lay = kv.second;
        if (lay.is_interface) continue; // sin instancias
        TF tf;
        tf.type_name = util::InternedName::intern(lay.name);
        tf.kind = TF::CLASS;
        tf.size_bytes = lay.size_bytes;
        tf.field_count = static_cast<uint32_t>(lay.fields.size());
        bool has_dtor = false;
        for (const auto &m : lay.methods)
            if (m.is_destructor) has_dtor = true;
        tf.has_destructor = has_dtor;
        tf.is_reference = true;
        tf.is_pod = false;
        tf.no_heap = false;
        place_fields(lay.fields, tf);
        out.push_back(std::move(tf));
    }

    // Enums: tagged unions inline (value-types).  @pod si ningun payload es
    // gestionado y todos son C-representables; un enum sin payload es @pod.
    for (const auto &kv : tc.enum_layouts()) {
        if (tc.is_imported(kv.first)) continue;
        const EnumLayout &lay = kv.second;
        TF tf;
        tf.type_name = util::InternedName::intern(lay.name);
        tf.kind = TF::ENUM;
        tf.size_bytes = lay.size_bytes;
        tf.field_count = lay.max_payload_fields;
        bool no_heap = true;
        bool all_c_repr = true;
        for (const auto &v : lay.variants)
            for (const auto &ft : v.field_types) {
                if (tc.type_is_managed(ft)) no_heap = false;
                if (!tc.type_is_c_representable(ft)) all_c_repr = false;
            }
        tf.no_heap = no_heap;
        tf.is_pod = all_c_repr && no_heap;
        tf.is_reference = false;
        tf.variants.reserve(lay.variants.size());
        for (const auto &v : lay.variants) {
            analyze::VariantPlacement vp;
            vp.name = util::InternedName::intern(v.name);
            vp.tag = v.tag;
            vp.int_value = v.int_value;
            vp.payload_fields = static_cast<uint32_t>(v.field_types.size());
            tf.variants.push_back(vp);
        }
        out.push_back(std::move(tf));
    }
    return out;
}

} // namespace vx
