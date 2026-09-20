/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/builtin_names.cpp
 * @brief La tabla plana de nombres de builtin y su busqueda.
 *
 * La tabla queda en `.rodata`: nombre, longitud e identificador, contiguos,
 * sin punteros que perseguir mas alla del texto.  El orden
 * es por longitud y luego por bytes -- el mismo que usa el comparador --, y no
 * se confia en que quien anada un nombre lo respete: hay un `static_assert`
 * que recorre la tabla al compilar.  Una entrada mal colocada es un error de
 * compilacion, no una busqueda que falla en silencio.
 *
 * Guardar la longitud aparte del puntero no es redundante: es lo que permite
 * que el comparador descarte con un entero.  Y viene gratis, porque el
 * `constexpr` la calcula al compilar.
 */
#include "vx/builtin_names.h"

#include <algorithm>
#include <array>

namespace vx {
namespace {

/**
 * @brief Longitud de un literal, calculada al compilar.
 *
 * No se usa `strlen` porque hace falta en contexto `constexpr`, y porque en la
 * tabla es un valor conocido: nadie lo va a calcular en ejecucion.
 *
 * @param s El literal.
 * @return Cuantos caracteres tiene, sin el terminador.
 */
constexpr uint16_t ct_len(const char *s) {
    uint16_t n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

/**
 * @brief Una entrada de la tabla: el texto, su longitud y que builtin es.
 *
 * La longitud se guarda porque es lo que permite descartar comparando un
 * entero, pero NO se escribe en la tabla: la calcula el constructor al
 * compilar.  Escribirla a mano seria un tercer dato que mantener a la vez que
 * el nombre y el valor del enum, y equivocarse no daria error -- daria una
 * busqueda que no encuentra.
 */
struct BuiltinEntry {
    const char *text; ///< El nombre, sin terminador contado en @ref len.
    uint16_t len;     ///< Su longitud, para descartar sin leer bytes.
    Builtin id;       ///< Que builtin es.

    /**
     * @brief Construye una entrada calculando la longitud del texto.
     *
     * @param t El nombre.
     * @param i Que builtin es.
     */
    constexpr BuiltinEntry(const char *t, Builtin i)
        : text(t), len(ct_len(t)), id(i) {}
};

/// @brief La tabla, ordenada por longitud y luego por bytes.
constexpr BuiltinEntry kTable[] = {
    /* --- 2 caracteres --- */
    {"Ok", Builtin::Ok},

    /* --- 3 caracteres --- */
    {"Err", Builtin::Err},
    {"abs", Builtin::Abs},
    {"chr", Builtin::Chr},
    {"clz", Builtin::Clz},
    {"cos", Builtin::Cos},
    {"ctz", Builtin::Ctz},
    {"log", Builtin::Log},
    {"ord", Builtin::Ord},
    {"pid", Builtin::Pid},
    {"pow", Builtin::Pow},
    {"sin", Builtin::Sin},
    {"tan", Builtin::Tan},

    /* --- 4 caracteres --- */
    {"None", Builtin::None},
    {"Some", Builtin::Some},
    {"ceil", Builtin::Ceil},
    {"echo", Builtin::Echo},
    {"fabs", Builtin::Fabs},
    {"fmax", Builtin::Fmax},
    {"fmin", Builtin::Fmin},
    {"free", Builtin::Free},
    {"imax", Builtin::Imax},
    {"imin", Builtin::Imin},
    {"isOk", Builtin::IsOk},
    {"lend", Builtin::Lend},
    {"log2", Builtin::Log2},
    {"move", Builtin::Move},
    {"rotl", Builtin::Rotl},
    {"rotr", Builtin::Rotr},
    {"sqrt", Builtin::Sqrt},
    {"wait", Builtin::Wait},

    /* --- 5 caracteres --- */
    {"bswap", Builtin::Bswap},
    {"clamp", Builtin::Clamp},
    {"deque", Builtin::Deque},
    {"error", Builtin::Error},
    {"floor", Builtin::Floor},
    {"flush", Builtin::Flush},
    {"fopen", Builtin::Fopen},
    {"ilog2", Builtin::Ilog2},
    {"imaxu", Builtin::Imaxu},
    {"iminu", Builtin::Iminu},
    {"log10", Builtin::Log10},
    {"panic", Builtin::Panic},
    {"print", Builtin::Print},
    {"queue", Builtin::Queue},
    {"round", Builtin::Round},
    {"share", Builtin::Share},
    {"stack", Builtin::Stack},
    {"trunc", Builtin::Trunc},
    {"value", Builtin::Value},
    {"write", Builtin::Write},

    /* --- 6 caracteres --- */
    {"bg_rgb", Builtin::BgRgb},
    {"expect", Builtin::Expect},
    {"fclose", Builtin::Fclose},
    {"fg_rgb", Builtin::FgRgb},
    {"fwrite", Builtin::Fwrite},
    {"gc_box", Builtin::GcBox},
    {"gensym", Builtin::Gensym},
    {"invoke", Builtin::Invoke},
    {"malloc", Builtin::Malloc},
    {"notify", Builtin::Notify},
    {"ptr_of", Builtin::PtrOf},
    {"repeat", Builtin::Repeat},
    {"substr", Builtin::Substr},
    {"to_str", Builtin::ToStr},
    {"unwrap", Builtin::Unwrap},

    /* --- 7 caracteres --- */
    {"bitcast", Builtin::Bitcast},
    {"dispose", Builtin::Dispose},
    {"ffi_sym", Builtin::FfiSym},
    {"forName", Builtin::ForName},
    {"fulfill", Builtin::Fulfill},
    {"hashmap", Builtin::Hashmap},
    {"hashset", Builtin::Hashset},
    {"msgrecv", Builtin::Msgrecv},
    {"msgsend", Builtin::Msgsend},
    {"println", Builtin::Println},
    {"proceed", Builtin::Proceed},
    {"replace", Builtin::Replace},
    {"treemap", Builtin::Treemap},
    {"treeset", Builtin::Treeset},
    {"type.id", Builtin::TypeId},
    {"type.of", Builtin::TypeOf},
    {"unshare", Builtin::Unshare},
    {"vacount", Builtin::Vacount},

    /* --- 8 caracteres --- */
    {"args_get", Builtin::ArgsGet},
    {"contains", Builtin::Contains},
    {"ct_print", Builtin::CtPrint},
    {"ffi_call", Builtin::FfiCall},
    {"ffi_open", Builtin::FfiOpen},
    {"getClass", Builtin::GetClass},
    {"getField", Builtin::GetField},
    {"lend_mut", Builtin::LendMut},
    {"popcount", Builtin::Popcount},
    {"str_cstr", Builtin::StrCstr},
    {"str_hash", Builtin::StrHash},
    {"str_make", Builtin::StrMake},
    {"str_wstr", Builtin::StrWstr},

    /* --- 9 caracteres --- */
    {"arraylist", Builtin::Arraylist},
    {"field.get", Builtin::FieldGet},
    {"field.has", Builtin::HasField},
    {"field.set", Builtin::FieldSet},
    {"getFields", Builtin::GetFields},
    {"getMethod", Builtin::GetMethod},
    {"isPresent", Builtin::IsPresent},
    {"is_shared", Builtin::IsShared},
    {"notifyAll", Builtin::NotifyAll},
    {"print_bin", Builtin::PrintBin},
    {"print_hex", Builtin::PrintHex},
    {"print_int", Builtin::PrintInt},
    {"print_oct", Builtin::PrintOct},
    {"print_pad", Builtin::PrintPad},
    {"print_ptr", Builtin::PrintPtr},
    {"str_bytes", Builtin::StrBytes},
    {"term_move", Builtin::TermMove},
    {"to_string", Builtin::ToString},
    {"type.base", Builtin::TypeBase},
    {"type.find", Builtin::FindType},
    {"type.kind", Builtin::Kind},
    {"type.name", Builtin::Typename},
    {"type.size", Builtin::Sizeof},
    {"unwrap_or", Builtin::UnwrapOr},
    {"use_count", Builtin::UseCount},

    /* --- 10 caracteres --- */
    {"args_count", Builtin::ArgsCount},
    {"atomic_add", Builtin::AtomicAdd},
    {"atomic_cas", Builtin::AtomicCas},
    {"field.each", Builtin::ForEachField},
    {"field.name", Builtin::FieldName},
    {"field.type", Builtin::FieldType},
    {"gc_collect", Builtin::GcCollect},
    {"getFieldAt", Builtin::GetFieldAt},
    {"getMethods", Builtin::GetMethods},
    {"loadmodule", Builtin::Loadmodule},
    {"method.has", Builtin::HasMethod},
    {"print_bool", Builtin::PrintBool},
    {"print_char", Builtin::PrintChar},
    {"print_cstr", Builtin::PrintCstr},
    {"print_uint", Builtin::PrintUint},
    {"shared_box", Builtin::SharedBox},
    {"str_concat", Builtin::StrConcat},
    {"str_equals", Builtin::StrEquals},
    {"str_intern", Builtin::StrIntern},
    {"str_length", Builtin::StrLength},
    {"term_clear", Builtin::TermClear},
    {"term_reset", Builtin::TermReset},
    {"type.align", Builtin::Alignof},
    {"type.error", Builtin::TypeError},
    {"type.inner", Builtin::TypeInner},
    {"unique_box", Builtin::UniqueBox},

    /* --- 11 caracteres --- */
    {"atomic_load", Builtin::AtomicLoad},
    {"fiber_entry", Builtin::FiberEntry},
    {"field.count", Builtin::FieldCount},
    {"getMethodAt", Builtin::GetMethodAt},
    {"method.each", Builtin::ForEachMethod},
    {"method.name", Builtin::MethodNameAt},
    {"newInstance", Builtin::NewInstance},
    {"print_color", Builtin::PrintColor},
    {"print_float", Builtin::PrintFloat},
    {"read_borrow", Builtin::ReadBorrow},
    {"section_end", Builtin::SectionEnd},
    {"shared_free", Builtin::SharedFree},
    {"shared_with", Builtin::SharedWith},
    {"str_convert", Builtin::StrConvert},
    {"type.parent", Builtin::Parent},
    {"type.result", Builtin::TypeResult},
    {"unique_with", Builtin::UniqueWith},

    /* --- 12 caracteres --- */
    {"atomic_store", Builtin::AtomicStore},
    {"comptime.chr", Builtin::ComptimeChr},
    {"comptime.ord", Builtin::ComptimeOrd},
    {"cpu_features", Builtin::CpuFeatures},
    {"field.offset", Builtin::Offsetof},
    {"future_alloc", Builtin::FutureAlloc},
    {"method.count", Builtin::MethodCount},
    {"section_size", Builtin::SectionSize},
    {"type.is_bool", Builtin::IsBool},
    {"type.is_char", Builtin::IsChar},
    {"type.is_enum", Builtin::IsEnum},
    {"type.is_same", Builtin::IsSame},
    {"unloadmodule", Builtin::Unloadmodule},
    {"write_borrow", Builtin::WriteBorrow},

    /* --- 13 caracteres --- */
    {"fiber_swapctx", Builtin::FiberSwapctx},
    {"field.type_at", Builtin::FieldTypeAt},
    {"method.result", Builtin::MethodResult},
    {"section_start", Builtin::SectionStart},
    {"shared_malloc", Builtin::SharedMalloc},
    {"static_assert", Builtin::StaticAssert},
    {"type.has_base", Builtin::HasBase},
    {"type.is_class", Builtin::IsClass},
    {"type.is_float", Builtin::IsFloat},

    /* --- 14 caracteres --- */
    {"atomic_add_i64", Builtin::AtomicAddI64},
    {"atomic_cas_i64", Builtin::AtomicCasI64},
    {"comptime.print", Builtin::ComptimePrint},
    {"overlay.extent", Builtin::Extent},
    {"print_gchandle", Builtin::PrintGchandle},
    {"type.has_inner", Builtin::HasInner},
    {"type.info.kind", Builtin::TypeInfoKind},
    {"type.info.name", Builtin::TypeInfoName},
    {"type.info.size", Builtin::TypeInfoSize},
    {"type.is_opaque", Builtin::IsOpaque},
    {"type.is_result", Builtin::IsResult},
    {"type.is_signed", Builtin::IsSigned},
    {"type.is_string", Builtin::IsString},
    {"type.is_struct", Builtin::IsStruct},

    /* --- 15 caracteres --- */
    {"atomic_load_i64", Builtin::AtomicLoadI64},
    {"comptime.str.eq", Builtin::ComptimeStreq},
    {"comptime.to_str", Builtin::ComptimeToStr},
    {"gc_finalize_all", Builtin::GcFinalizeAll},
    {"term_clear_line", Builtin::TermClearLine},
    {"type.info.align", Builtin::TypeInfoAlign},
    {"type.is_integer", Builtin::IsInteger},
    {"type.is_newtype", Builtin::IsNewtype},
    {"type.is_numeric", Builtin::IsNumeric},
    {"type.is_pointer", Builtin::IsPointer},
    {"type.is_subtype", Builtin::IsSubtype},
    {"type.underlying", Builtin::UnderlyingOf},

    /* --- 16 caracteres --- */
    {"atomic_store_i64", Builtin::AtomicStoreI64},
    {"comptime.str.len", Builtin::ComptimeStrlen},
    {"term_hide_cursor", Builtin::TermHideCursor},
    {"term_save_cursor", Builtin::TermSaveCursor},
    {"term_show_cursor", Builtin::TermShowCursor},
    {"type.is_callable", Builtin::IsCallable},
    {"type.is_unsigned", Builtin::IsUnsigned},
    {"unwrap_unchecked", Builtin::UnwrapUnchecked},

    /* --- 17 caracteres --- */
    {"overlay.in_bounds", Builtin::InBounds},
    {"scoped.method.has", Builtin::HasScopedMethod},
    {"shared_gc_collect", Builtin::SharedGcCollect},
    {"shared_heap_bytes", Builtin::SharedHeapBytes},
    {"type.by_name.kind", Builtin::ComptimeTypeKind},
    {"type.by_name.size", Builtin::ComptimeTypeSizeof},
    {"type.is_primitive", Builtin::IsPrimitive},

    /* --- 18 caracteres --- */
    {"as_native_callback", Builtin::AsNativeCallback},
    {"scoped.method.each", Builtin::ScopedMethodEach},
    {"scoped.method.name", Builtin::ScopedMethodName},
    {"type.by_name.align", Builtin::ComptimeTypeAlignof},

    /* --- 19 caracteres --- */
    {"comptime.str.concat", Builtin::ComptimeConcat},
    {"comptime.str.repeat", Builtin::ComptimeRepeat},
    {"comptime.str.substr", Builtin::ComptimeSubstr},
    {"scoped.method.arity", Builtin::ScopedMethodArity},
    {"scoped.method.count", Builtin::ScopedMethodCount},
    {"scoped.method.param", Builtin::ScopedMethodParam},
    {"term_restore_cursor", Builtin::TermRestoreCursor},

    /* --- 20 caracteres --- */
    {"comptime.str.replace", Builtin::ComptimeReplace},
    {"scoped.method.origin", Builtin::ScopedMethodOrigin},
    {"scoped.method.result", Builtin::ScopedMethodReturn},
    {"type.info.field_name", Builtin::TypeInfoFieldName},
    {"type.info.field_size", Builtin::TypeInfoFieldSize},

    /* --- 21 caracteres --- */
    {"comptime.str.contains", Builtin::ComptimeContains},
    {"type.info.field_count", Builtin::TypeInfoFieldCount},

    /* --- 22 caracteres --- */
    {"shared_heap_live_count", Builtin::SharedHeapLiveCount},
    {"type.info.field_offset", Builtin::TypeInfoFieldOffset},
};

/// @brief Cuantas entradas tiene la tabla.
constexpr size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

/**
 * @brief Compara dos textos con el criterio de la tabla: longitud, luego
 *        bytes.
 *
 * Poner la longitud delante no es un detalle de eficiencia sino LA razon de
 * que la busqueda sea barata: dos nombres de distinta longitud se ordenan sin
 * mirar su contenido.
 *
 * @param a_text Texto del primero.
 * @param a_len  Longitud del primero.
 * @param b_text Texto del segundo.
 * @param b_len  Longitud del segundo.
 * @return Negativo si el primero va antes, cero si son iguales, positivo si va
 *         despues.
 */
constexpr int ct_cmp(const char *a_text, uint16_t a_len, const char *b_text,
                     uint16_t b_len) {
    if (a_len != b_len) return (a_len < b_len) ? -1 : 1;
    for (uint16_t i = 0; i < a_len; ++i) {
        if (a_text[i] != b_text[i]) return (a_text[i] < b_text[i]) ? -1 : 1;
    }
    return 0;
}

/**
 * @brief Comprueba al compilar que la tabla esta en orden.
 *
 * Recorre las entradas y exige que cada una vaya estrictamente despues de la
 * anterior.  Estricta, no laxa: dos nombres iguales serian dos builtins con el
 * mismo texto, y uno de los dos no se alcanzaria nunca.
 *
 * @return @c true si la tabla esta ordenada y sin repetidos.
 */
constexpr bool table_is_sorted() {
    for (size_t i = 1; i < kTableSize; ++i) {
        if (ct_cmp(kTable[i - 1].text, kTable[i - 1].len, kTable[i].text,
                   kTable[i].len) >= 0)
            return false;
    }
    return true;
}

static_assert(table_is_sorted(),
              "kTable tiene que ir ordenada por LONGITUD y luego por bytes, "
              "sin nombres repetidos.  Coloca la entrada nueva en su sitio o "
              "la busqueda binaria no la encontrara.");

static_assert(kTableSize + 1 == static_cast<size_t>(Builtin::Count),
              "kTable y Builtin salen de la misma lista: si una tiene una "
              "entrada que a la otra le falta, hay un builtin que no se "
              "reconoce o un valor del enum que no corresponde a ningun "
              "nombre.  (El +1 es Builtin::Unknown, que no esta en la tabla.)");

} // namespace

/**
 * @copydoc vx::builtin_from_name
 */
Builtin builtin_from_name(std::string_view name) noexcept {
    /* Los nombres mas largos y mas cortos de la tabla acotan la busqueda: un
     * identificador cualquiera del programa casi nunca cae dentro, y salir
     * aqui evita entrar en la busqueda binaria. */
    const uint16_t n = static_cast<uint16_t>(name.size());
    if (n < kTable[0].len || n > kTable[kTableSize - 1].len)
        return Builtin::Unknown;

    const BuiltinEntry *lo = kTable;
    const BuiltinEntry *hi = kTable + kTableSize;
    while (lo < hi) {
        const BuiltinEntry *mid = lo + (hi - lo) / 2;
        const int c = ct_cmp(mid->text, mid->len, name.data(), n);
        if (c < 0)
            lo = mid + 1;
        else if (c > 0)
            hi = mid;
        else
            return mid->id;
    }
    return Builtin::Unknown;
}

bool is_builtin_tree_root(std::string_view head) noexcept {
    /* Las raices son CINCO y no cambian con el uso, asi que se comparan tal
     * cual: recorrer la tabla buscando el prefijo seria O(entradas) en un
     * sitio que el parser toca por cada identificador que lee.
     *
     * Estan aqui y no en el parser porque la tabla es la fuente: anadir una
     * familia nueva es anadir su raiz a esta linea, al lado de sus entradas.
     * Ninguna es palabra clave -- se comprobo al elegirlas --, asi que el
     * lexer las entrega como identificadores normales. */
    return head == "type" || head == "field" || head == "method" ||
           head == "scoped" || head == "overlay" || head == "comptime";
}

/**
 * @copydoc vx::builtin_name
 */
std::string_view builtin_name(Builtin b) noexcept {
    /* La tabla NO esta indexada por el valor del enum -- va ordenada por
     * longitud --, asi que hay que buscarlo.  Solo lo usan los diagnosticos,
     * asi que recorrerla entera sale mas barato que mantener una segunda tabla
     * en el orden del enum solo para esto. */
    for (size_t i = 0; i < kTableSize; ++i) {
        if (kTable[i].id == b)
            return std::string_view(kTable[i].text, kTable[i].len);
    }
    return {};
}

namespace {

/**
 * @brief El reparto: que familia atiende cada builtin.
 *
 * Escrito como `switch` a proposito, y no como una tabla de doscientas
 * entradas indexada a mano: asi el reparto se LEE -- los nombres de una
 * familia van juntos y se ve de un vistazo que no falta ninguno --, y no hay
 * que respetar ningun orden.  La tabla la construye el compilador a partir de
 * esto, de modo que en ejecucion sigue siendo una lectura y no un recorrido.
 *
 * Las familias son disjuntas y eso NO es casual: es lo que hace correcto ir
 * directo a una.  Si un builtin acabara en dos, la segunda no se ejecutaria
 * nunca.
 *
 * @param b El builtin.
 * @return Su familia, u Other si la atiende el despacho general.
 */
constexpr BuiltinFamily family_of(Builtin b) {
    switch (b) {
    case Builtin::ComptimePrint:
    case Builtin::CtPrint:
    case Builtin::Echo:
    case Builtin::Flush:
    case Builtin::GcCollect:
    case Builtin::GcFinalizeAll:
    case Builtin::Print:
    /* El sumidero de bytes.  Es de la familia que imprime porque baja a la
     * misma primitiva que `print` -- `print` no es mas que decidir QUE bytes y
     * mandarselos --, y asi quien lo sobrecargue cubre las dos de una vez. */
    case Builtin::Write:
    case Builtin::PrintBin:
    case Builtin::PrintBool:
    case Builtin::PrintChar:
    case Builtin::PrintColor:
    case Builtin::PrintCstr:
    case Builtin::PrintFloat:
    case Builtin::PrintGchandle:
    case Builtin::PrintHex:
    case Builtin::PrintInt:
    case Builtin::PrintOct:
    case Builtin::PrintPad:
    case Builtin::PrintPtr:
    case Builtin::PrintUint:
    case Builtin::Println:
    case Builtin::TermClear:
    case Builtin::TermClearLine:
    case Builtin::TermHideCursor:
    case Builtin::TermMove:
    case Builtin::TermReset:
    case Builtin::TermRestoreCursor:
    case Builtin::TermSaveCursor:
    case Builtin::TermShowCursor: return BuiltinFamily::Print;

    case Builtin::Dispose:
    case Builtin::Fclose:
    case Builtin::FiberSwapctx:
    case Builtin::Fopen:
    case Builtin::Free:
    case Builtin::Fwrite:
    case Builtin::Loadmodule:
    case Builtin::Malloc:
    case Builtin::Unloadmodule:
    case Builtin::FfiOpen:
    case Builtin::FfiSym:
    case Builtin::FfiCall:
    case Builtin::Pid:
    case Builtin::CpuFeatures:
    case Builtin::ArgsCount:
    case Builtin::ArgsGet: return BuiltinFamily::Runtime;

    case Builtin::AtomicAddI64:
    case Builtin::AtomicCasI64:
    case Builtin::AtomicLoadI64:
    case Builtin::AtomicStoreI64:
    case Builtin::Fulfill:
    case Builtin::FutureAlloc:
    case Builtin::IsShared:
    case Builtin::Msgrecv:
    case Builtin::Msgsend:
    case Builtin::Share:
    case Builtin::SharedFree:
    case Builtin::SharedGcCollect:
    case Builtin::SharedHeapBytes:
    case Builtin::SharedHeapLiveCount:
    case Builtin::SharedMalloc:
    case Builtin::Unshare:
    case Builtin::AtomicLoad:
    case Builtin::AtomicStore:
    case Builtin::AtomicCas:
    case Builtin::AtomicAdd:
    case Builtin::Wait:
    case Builtin::Notify:
    case Builtin::NotifyAll: return BuiltinFamily::Concurrent;

    case Builtin::Err:
    case Builtin::Error:
    case Builtin::Expect:
    case Builtin::IsOk:
    case Builtin::IsPresent:
    case Builtin::None:
    case Builtin::Ok:
    case Builtin::Some:
    case Builtin::Unwrap:
    case Builtin::UnwrapOr:
    case Builtin::UnwrapUnchecked:
    case Builtin::Value: return BuiltinFamily::Optional;

    case Builtin::ForName:
    case Builtin::GetClass:
    case Builtin::GetField:
    case Builtin::GetFieldAt:
    case Builtin::GetFields:
    case Builtin::GetMethod:
    case Builtin::GetMethodAt:
    case Builtin::GetMethods:
    case Builtin::Invoke:
    case Builtin::NewInstance:
    case Builtin::Proceed: return BuiltinFamily::Reflect;

    case Builtin::GcBox:
    case Builtin::Lend:
    case Builtin::LendMut:
    case Builtin::Move:
    case Builtin::PtrOf:
    case Builtin::ReadBorrow:
    case Builtin::SharedBox:
    case Builtin::SharedWith:
    case Builtin::UniqueBox:
    case Builtin::UniqueWith:
    case Builtin::UseCount:
    case Builtin::WriteBorrow: return BuiltinFamily::Ownership;

    case Builtin::Chr:
    case Builtin::ComptimeChr:
    case Builtin::ComptimeConcat:
    case Builtin::ComptimeContains:
    case Builtin::ComptimeOrd:
    case Builtin::ComptimeRepeat:
    case Builtin::ComptimeReplace:
    case Builtin::ComptimeStreq:
    case Builtin::ComptimeStrlen:
    case Builtin::ComptimeSubstr:
    case Builtin::ComptimeToStr:
    case Builtin::Contains:
    case Builtin::Ord:
    case Builtin::Repeat:
    case Builtin::Replace:
    case Builtin::StrBytes:
    case Builtin::StrConcat:
    case Builtin::StrConvert:
    case Builtin::StrCstr:
    case Builtin::StrEquals:
    case Builtin::StrHash:
    case Builtin::StrIntern:
    case Builtin::StrLength:
    case Builtin::StrMake:
    case Builtin::StrWstr:
    case Builtin::Substr:
    case Builtin::ToStr: return BuiltinFamily::String;

    case Builtin::Abs:
    case Builtin::Bswap:
    case Builtin::Ceil:
    case Builtin::Clamp:
    case Builtin::Clz:
    case Builtin::Cos:
    case Builtin::Ctz:
    case Builtin::Fabs:
    case Builtin::Floor:
    case Builtin::Fmax:
    case Builtin::Fmin:
    case Builtin::Ilog2:
    case Builtin::Imax:
    case Builtin::Imaxu:
    case Builtin::Imin:
    case Builtin::Iminu:
    case Builtin::Log:
    case Builtin::Log10:
    case Builtin::Log2:
    case Builtin::Popcount:
    case Builtin::Pow:
    case Builtin::Rotl:
    case Builtin::Rotr:
    case Builtin::Round:
    case Builtin::Sin:
    case Builtin::Sqrt:
    case Builtin::Tan:
    case Builtin::Trunc: return BuiltinFamily::Math;

    case Builtin::Alignof:
    case Builtin::Bitcast:
    case Builtin::ComptimeTypeAlignof:
    case Builtin::ComptimeTypeKind:
    case Builtin::ComptimeTypeSizeof:
    case Builtin::Extent:
    case Builtin::FieldCount:
    case Builtin::FieldGet:
    case Builtin::FieldName:
    case Builtin::FieldSet:
    case Builtin::FieldType:
    case Builtin::FindType:
    case Builtin::ForEachField:
    case Builtin::ForEachMethod:
    case Builtin::HasField:
    case Builtin::HasMethod:
    case Builtin::InBounds:
    case Builtin::IsBool:
    case Builtin::IsChar:
    case Builtin::IsClass:
    case Builtin::IsEnum:
    case Builtin::IsFloat:
    case Builtin::IsInteger:
    case Builtin::IsNewtype:
    case Builtin::IsNumeric:
    case Builtin::IsOpaque:
    case Builtin::IsPointer:
    case Builtin::IsPrimitive:
    case Builtin::IsSame:
    case Builtin::IsSigned:
    case Builtin::IsString:
    case Builtin::IsStruct:
    case Builtin::IsSubtype:
    case Builtin::IsUnsigned:
    case Builtin::Kind:
    case Builtin::MethodCount:
    case Builtin::Offsetof:
    case Builtin::Parent:
    /* Lo ALCANZABLE es introspeccion igual que lo demas, solo que la pregunta
     * depende de lo que este fichero importa: ver `vx/ufcs_scoped.h`. */
    case Builtin::HasScopedMethod:
    case Builtin::ScopedMethodArity:
    case Builtin::ScopedMethodCount:
    case Builtin::ScopedMethodEach:
    case Builtin::ScopedMethodName:
    case Builtin::ScopedMethodOrigin:
    case Builtin::ScopedMethodParam:
    case Builtin::ScopedMethodReturn:
    case Builtin::Sizeof:
    case Builtin::StaticAssert:
    case Builtin::TypeId:
    case Builtin::TypeInfoAlign:
    case Builtin::TypeInfoFieldCount:
    case Builtin::TypeInfoFieldName:
    case Builtin::TypeInfoFieldOffset:
    case Builtin::TypeInfoFieldSize:
    case Builtin::TypeInfoKind:
    case Builtin::TypeInfoName:
    case Builtin::TypeInfoSize:
    /* Los que devuelven un `Type` en vez de un valor: tambien es preguntarle
     * al tipo, solo que la respuesta es otro tipo. */
    case Builtin::TypeOf:
    case Builtin::TypeBase:
    case Builtin::TypeInner:
    case Builtin::TypeError:
    case Builtin::TypeResult:
    case Builtin::HasInner:
    case Builtin::HasBase:
    case Builtin::IsResult:
    case Builtin::IsCallable:
    case Builtin::Typename:
    case Builtin::UnderlyingOf: return BuiltinFamily::Introspect;

    default: return BuiltinFamily::Other;
    }
}

/// @brief Cuantas entradas tiene la tabla: una por builtin, mas Unknown.
constexpr size_t kFamilyCount = static_cast<size_t>(Builtin::Count);

/// @brief La tabla, construida al compilar desde @ref family_of.
constexpr std::array<BuiltinFamily, kFamilyCount> kFamilyTable = [] {
    std::array<BuiltinFamily, kFamilyCount> t{};
    for (size_t i = 0; i < kFamilyCount; ++i)
        t[i] = family_of(static_cast<Builtin>(i));
    return t;
}();

} // namespace

/**
 * @copydoc vx::builtin_family
 */
/**
 * @copydoc vx::builtin_yields_type
 */
bool builtin_yields_type(Builtin b) noexcept {
    switch (b) {
    case Builtin::TypeOf:
    case Builtin::TypeBase:
    case Builtin::TypeInner:
    case Builtin::TypeError:
    case Builtin::TypeResult:
    case Builtin::FieldTypeAt:
    case Builtin::MethodResult:
    case Builtin::ScopedMethodReturn: return true;
    default: return false;
    }
}

BuiltinFamily builtin_family(Builtin b) noexcept {
    const size_t i = static_cast<size_t>(b);
    if (i >= kFamilyCount) return BuiltinFamily::Other;
    return kFamilyTable[i];
}

} // namespace vx
