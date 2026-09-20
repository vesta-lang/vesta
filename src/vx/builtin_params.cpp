/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file builtin_params.cpp
 * @brief La tabla de ranuras por builtin, y su busqueda.
 *
 * Ordenada por nombre para poder buscar en binario.  Un builtin que no aparece
 * aqui simplemente no tiene ranuras conocidas: no es un error, es que no se le
 * han decidido todavia -- y mientras no las tenga, no se puede sobrecargar.
 */

#include "vx/builtin_params.h"

#include <algorithm>

namespace vx {
namespace {

/// Una fila: el nombre del builtin y como se llaman sus ranuras.
struct Row {
    const char *name;
    const char *const *params;
    uint8_t count;
};

constexpr const char *k_Err[] = {"error"};
constexpr const char *k_Ok[] = {"value"};
constexpr const char *k_Some[] = {"value"};
constexpr const char *k_abs[] = {"x"};
constexpr const char *k_args_get[] = {"i"};
constexpr const char *k_arraylist[] = {"capacity"};
constexpr const char *k_as_native_callback[] = {"fn"};
constexpr const char *k_atomic_add[] = {"addr", "delta"};
constexpr const char *k_atomic_add_i64[] = {"addr", "delta"};
constexpr const char *k_atomic_cas[] = {"addr", "expected", "desired"};
constexpr const char *k_atomic_cas_i64[] = {"addr", "expected", "desired"};
constexpr const char *k_atomic_load[] = {"addr"};
constexpr const char *k_atomic_load_i64[] = {"addr"};
constexpr const char *k_atomic_store[] = {"addr", "val"};
constexpr const char *k_atomic_store_i64[] = {"addr", "val"};
constexpr const char *k_bg_rgb[] = {"r", "g", "b"};
constexpr const char *k_bitcast[] = {"value"};
constexpr const char *k_bswap[] = {"x"};
constexpr const char *k_ceil[] = {"x"};
constexpr const char *k_chr[] = {"cp"};
constexpr const char *k_clamp[] = {"x", "lo", "hi"};
constexpr const char *k_clz[] = {"x"};
constexpr const char *k_comptime_chr[] = {"cp"};
constexpr const char *k_comptime_concat[] = {"a", "b"};
constexpr const char *k_comptime_contains[] = {"s", "needle"};
constexpr const char *k_comptime_ord[] = {"s"};
constexpr const char *k_comptime_print[] = {"value"};
constexpr const char *k_comptime_repeat[] = {"s", "n"};
constexpr const char *k_comptime_replace[] = {"s", "from", "to"};
constexpr const char *k_comptime_streq[] = {"a", "b"};
constexpr const char *k_comptime_strlen[] = {"s"};
constexpr const char *k_comptime_substr[] = {"s", "start", "len"};
constexpr const char *k_comptime_to_str[] = {"value"};
constexpr const char *k_contains[] = {"s", "needle"};
constexpr const char *k_cos[] = {"x"};
constexpr const char *k_ct_print[] = {"value"};
constexpr const char *k_ctz[] = {"x"};
constexpr const char *k_deque[] = {"capacity"};
constexpr const char *k_dispose[] = {"x"};
constexpr const char *k_echo[] = {"value"};
constexpr const char *k_error[] = {"r"};
constexpr const char *k_expect[] = {"opt", "msg"};
constexpr const char *k_fabs[] = {"x"};
constexpr const char *k_fclose[] = {"fp"};
constexpr const char *k_ffi_call[] = {"fn"};
constexpr const char *k_ffi_open[] = {"lib"};
constexpr const char *k_ffi_sym[] = {"handle", "name"};
constexpr const char *k_fg_rgb[] = {"r", "g", "b"};
constexpr const char *k_fiber_entry[] = {"ctx"};
constexpr const char *k_fiber_swapctx[] = {"from_ctx", "to_ctx"};
constexpr const char *k_field_get[] = {"obj", "field"};
constexpr const char *k_field_name[] = {"idx"};
constexpr const char *k_field_set[] = {"obj", "field", "value"};
constexpr const char *k_field_type[] = {"field"};
constexpr const char *k_find_type[] = {"name"};
constexpr const char *k_floor[] = {"x"};
constexpr const char *k_fmax[] = {"a", "b"};
constexpr const char *k_fmin[] = {"a", "b"};
constexpr const char *k_fopen[] = {"path", "mode"};
constexpr const char *k_forName[] = {"name"};
constexpr const char *k_free[] = {"ptr"};
constexpr const char *k_fulfill[] = {"fut", "value"};
constexpr const char *k_fwrite[] = {"fp", "ptr", "len"};
constexpr const char *k_gc_box[] = {"value"};
constexpr const char *k_gensym[] = {"prefix"};
constexpr const char *k_getClass[] = {"obj"};
constexpr const char *k_getField[] = {"cls", "name"};
constexpr const char *k_getFieldAt[] = {"cls", "index"};
constexpr const char *k_getFields[] = {"cls"};
constexpr const char *k_getMethod[] = {"cls", "name"};
constexpr const char *k_getMethodAt[] = {"cls", "index"};
constexpr const char *k_getMethods[] = {"cls"};
constexpr const char *k_has_field[] = {"name"};
constexpr const char *k_has_method[] = {"name"};
constexpr const char *k_hashmap[] = {"capacity"};
constexpr const char *k_hashset[] = {"capacity"};
constexpr const char *k_ilog2[] = {"x"};
constexpr const char *k_imax[] = {"a", "b"};
constexpr const char *k_imaxu[] = {"a", "b"};
constexpr const char *k_imin[] = {"a", "b"};
constexpr const char *k_iminu[] = {"a", "b"};
constexpr const char *k_in_bounds[] = {"field_addr", "buf_size"};
constexpr const char *k_isOk[] = {"r"};
constexpr const char *k_isPresent[] = {"opt"};
constexpr const char *k_is_bool[] = {"x"};
constexpr const char *k_is_float[] = {"x"};
constexpr const char *k_is_integer[] = {"x"};
constexpr const char *k_is_numeric[] = {"x"};
constexpr const char *k_is_signed[] = {"x"};
constexpr const char *k_is_unsigned[] = {"x"};
constexpr const char *k_lend[] = {"owner"};
constexpr const char *k_lend_mut[] = {"owner"};
constexpr const char *k_loadmodule[] = {"path"};
constexpr const char *k_log[] = {"x"};
constexpr const char *k_log10[] = {"x"};
constexpr const char *k_log2[] = {"x"};
constexpr const char *k_malloc[] = {"count"};
constexpr const char *k_move[] = {"p"};
constexpr const char *k_msgsend[] = {"pid", "payload"};
constexpr const char *k_newInstance[] = {"cls"};
constexpr const char *k_notify[] = {"obj"};
constexpr const char *k_notifyAll[] = {"obj"};
constexpr const char *k_offsetof[] = {"field"};
constexpr const char *k_ord[] = {"s"};
constexpr const char *k_panic[] = {"msg"};
constexpr const char *k_popcount[] = {"x"};
constexpr const char *k_pow[] = {"base", "exp"};
constexpr const char *k_print[] = {"value"};
constexpr const char *k_print_bin[] = {"n"};
constexpr const char *k_print_bool[] = {"b"};
constexpr const char *k_print_char[] = {"cp"};
constexpr const char *k_print_color[] = {"code"};
constexpr const char *k_print_cstr[] = {"ptr"};
constexpr const char *k_print_float[] = {"x"};
constexpr const char *k_print_gchandle[] = {"handle"};
constexpr const char *k_print_hex[] = {"n"};
constexpr const char *k_print_int[] = {"n"};
constexpr const char *k_print_oct[] = {"n"};
constexpr const char *k_print_pad[] = {"fill_cp", "width"};
constexpr const char *k_print_ptr[] = {"addr"};
constexpr const char *k_print_uint[] = {"n"};
constexpr const char *k_println[] = {"value"};
constexpr const char *k_ptr_of[] = {"p"};
constexpr const char *k_queue[] = {"capacity"};
constexpr const char *k_read_borrow[] = {"b"};
constexpr const char *k_repeat[] = {"s", "n"};
constexpr const char *k_replace[] = {"s", "from", "to"};
constexpr const char *k_rotl[] = {"x", "n"};
constexpr const char *k_rotr[] = {"x", "n"};
constexpr const char *k_round[] = {"x"};
constexpr const char *k_section_end[] = {"name"};
constexpr const char *k_section_size[] = {"name"};
constexpr const char *k_section_start[] = {"name"};
constexpr const char *k_share[] = {"value"};
constexpr const char *k_shared_box[] = {"value"};
constexpr const char *k_shared_free[] = {"ptr"};
constexpr const char *k_shared_malloc[] = {"bytes"};
constexpr const char *k_shared_with[] = {"value", "deleter"};
constexpr const char *k_sin[] = {"x"};
constexpr const char *k_sqrt[] = {"x"};
constexpr const char *k_stack[] = {"capacity"};
constexpr const char *k_static_assert[] = {"cond", "msg"};
constexpr const char *k_str_bytes[] = {"s"};
constexpr const char *k_str_concat[] = {"a", "b"};
constexpr const char *k_str_convert[] = {"s", "enc"};
constexpr const char *k_str_cstr[] = {"s"};
constexpr const char *k_str_equals[] = {"a", "b"};
constexpr const char *k_str_hash[] = {"s"};
constexpr const char *k_str_intern[] = {"s"};
constexpr const char *k_str_length[] = {"s"};
constexpr const char *k_str_make[] = {"ptr", "len", "enc"};
constexpr const char *k_str_wstr[] = {"s"};
constexpr const char *k_substr[] = {"s", "start", "len"};
constexpr const char *k_tan[] = {"x"};
constexpr const char *k_term_move[] = {"row", "col"};
constexpr const char *k_to_str[] = {"value"};
constexpr const char *k_to_string[] = {"value"};
constexpr const char *k_trunc[] = {"x"};
constexpr const char *k_type_info_align[] = {"p"};
constexpr const char *k_type_info_field_count[] = {"p"};
constexpr const char *k_type_info_field_name[] = {"p", "idx"};
constexpr const char *k_type_info_field_offset[] = {"p", "idx"};
constexpr const char *k_type_info_field_size[] = {"p", "idx"};
constexpr const char *k_type_info_kind[] = {"p"};
constexpr const char *k_type_info_name[] = {"p"};
constexpr const char *k_type_info_size[] = {"p"};
constexpr const char *k_unique_box[] = {"value"};
constexpr const char *k_unique_with[] = {"value", "deleter"};
constexpr const char *k_unloadmodule[] = {"handle"};
constexpr const char *k_unshare[] = {"handle"};
constexpr const char *k_unwrap[] = {"opt"};
constexpr const char *k_unwrap_or[] = {"opt", "def"};
constexpr const char *k_unwrap_unchecked[] = {"opt"};
constexpr const char *k_use_count[] = {"s"};
constexpr const char *k_value[] = {"r"};
constexpr const char *k_wait[] = {"obj"};
constexpr const char *k_write[] = {"ptr", "len"};
constexpr const char *k_write_borrow[] = {"m", "v"};

/// La tabla, ORDENADA por nombre: la busqueda es binaria.
constexpr Row kRows[] = {
    {"Err", k_Err, 1},
    {"Ok", k_Ok, 1},
    {"Some", k_Some, 1},
    {"abs", k_abs, 1},
    {"args_get", k_args_get, 1},
    {"arraylist", k_arraylist, 1},
    {"as_native_callback", k_as_native_callback, 1},
    {"atomic_add", k_atomic_add, 2},
    {"atomic_add_i64", k_atomic_add_i64, 2},
    {"atomic_cas", k_atomic_cas, 3},
    {"atomic_cas_i64", k_atomic_cas_i64, 3},
    {"atomic_load", k_atomic_load, 1},
    {"atomic_load_i64", k_atomic_load_i64, 1},
    {"atomic_store", k_atomic_store, 2},
    {"atomic_store_i64", k_atomic_store_i64, 2},
    {"bg_rgb", k_bg_rgb, 3},
    {"bitcast", k_bitcast, 1},
    {"bswap", k_bswap, 1},
    {"ceil", k_ceil, 1},
    {"chr", k_chr, 1},
    {"clamp", k_clamp, 3},
    {"clz", k_clz, 1},
    {"comptime.chr", k_comptime_chr, 1},
    {"comptime.ord", k_comptime_ord, 1},
    {"comptime.print", k_comptime_print, 1},
    {"comptime.str.concat", k_comptime_concat, 2},
    {"comptime.str.contains", k_comptime_contains, 2},
    {"comptime.str.eq", k_comptime_streq, 2},
    {"comptime.str.len", k_comptime_strlen, 1},
    {"comptime.str.repeat", k_comptime_repeat, 2},
    {"comptime.str.replace", k_comptime_replace, 3},
    {"comptime.str.substr", k_comptime_substr, 3},
    {"comptime.to_str", k_comptime_to_str, 1},
    {"contains", k_contains, 2},
    {"cos", k_cos, 1},
    {"ct_print", k_ct_print, 1},
    {"ctz", k_ctz, 1},
    {"deque", k_deque, 1},
    {"dispose", k_dispose, 1},
    {"echo", k_echo, 1},
    {"error", k_error, 1},
    {"expect", k_expect, 2},
    {"fabs", k_fabs, 1},
    {"fclose", k_fclose, 1},
    {"ffi_call", k_ffi_call, 1},
    {"ffi_open", k_ffi_open, 1},
    {"ffi_sym", k_ffi_sym, 2},
    {"fg_rgb", k_fg_rgb, 3},
    {"fiber_entry", k_fiber_entry, 1},
    {"fiber_swapctx", k_fiber_swapctx, 2},
    {"field.get", k_field_get, 2},
    {"field.has", k_has_field, 1},
    {"field.name", k_field_name, 1},
    {"field.offset", k_offsetof, 1},
    {"field.set", k_field_set, 3},
    {"field.type", k_field_type, 1},
    {"floor", k_floor, 1},
    {"fmax", k_fmax, 2},
    {"fmin", k_fmin, 2},
    {"fopen", k_fopen, 2},
    {"forName", k_forName, 1},
    {"free", k_free, 1},
    {"fulfill", k_fulfill, 2},
    {"fwrite", k_fwrite, 3},
    {"gc_box", k_gc_box, 1},
    {"gensym", k_gensym, 1},
    {"getClass", k_getClass, 1},
    {"getField", k_getField, 2},
    {"getFieldAt", k_getFieldAt, 2},
    {"getFields", k_getFields, 1},
    {"getMethod", k_getMethod, 2},
    {"getMethodAt", k_getMethodAt, 2},
    {"getMethods", k_getMethods, 1},
    {"hashmap", k_hashmap, 1},
    {"hashset", k_hashset, 1},
    {"ilog2", k_ilog2, 1},
    {"imax", k_imax, 2},
    {"imaxu", k_imaxu, 2},
    {"imin", k_imin, 2},
    {"iminu", k_iminu, 2},
    {"isOk", k_isOk, 1},
    {"isPresent", k_isPresent, 1},
    {"lend", k_lend, 1},
    {"lend_mut", k_lend_mut, 1},
    {"loadmodule", k_loadmodule, 1},
    {"log", k_log, 1},
    {"log10", k_log10, 1},
    {"log2", k_log2, 1},
    {"malloc", k_malloc, 1},
    {"method.has", k_has_method, 1},
    {"move", k_move, 1},
    {"msgsend", k_msgsend, 2},
    {"newInstance", k_newInstance, 1},
    {"notify", k_notify, 1},
    {"notifyAll", k_notifyAll, 1},
    {"ord", k_ord, 1},
    {"overlay.in_bounds", k_in_bounds, 2},
    {"panic", k_panic, 1},
    {"popcount", k_popcount, 1},
    {"pow", k_pow, 2},
    {"print", k_print, 1},
    {"print_bin", k_print_bin, 1},
    {"print_bool", k_print_bool, 1},
    {"print_char", k_print_char, 1},
    {"print_color", k_print_color, 1},
    {"print_cstr", k_print_cstr, 1},
    {"print_float", k_print_float, 1},
    {"print_gchandle", k_print_gchandle, 1},
    {"print_hex", k_print_hex, 1},
    {"print_int", k_print_int, 1},
    {"print_oct", k_print_oct, 1},
    {"print_pad", k_print_pad, 2},
    {"print_ptr", k_print_ptr, 1},
    {"print_uint", k_print_uint, 1},
    {"println", k_println, 1},
    {"ptr_of", k_ptr_of, 1},
    {"queue", k_queue, 1},
    {"read_borrow", k_read_borrow, 1},
    {"repeat", k_repeat, 2},
    {"replace", k_replace, 3},
    {"rotl", k_rotl, 2},
    {"rotr", k_rotr, 2},
    {"round", k_round, 1},
    {"section_end", k_section_end, 1},
    {"section_size", k_section_size, 1},
    {"section_start", k_section_start, 1},
    {"share", k_share, 1},
    {"shared_box", k_shared_box, 1},
    {"shared_free", k_shared_free, 1},
    {"shared_malloc", k_shared_malloc, 1},
    {"shared_with", k_shared_with, 2},
    {"sin", k_sin, 1},
    {"sqrt", k_sqrt, 1},
    {"stack", k_stack, 1},
    {"static_assert", k_static_assert, 2},
    {"str_bytes", k_str_bytes, 1},
    {"str_concat", k_str_concat, 2},
    {"str_convert", k_str_convert, 2},
    {"str_cstr", k_str_cstr, 1},
    {"str_equals", k_str_equals, 2},
    {"str_hash", k_str_hash, 1},
    {"str_intern", k_str_intern, 1},
    {"str_length", k_str_length, 1},
    {"str_make", k_str_make, 3},
    {"str_wstr", k_str_wstr, 1},
    {"substr", k_substr, 3},
    {"tan", k_tan, 1},
    {"term_move", k_term_move, 2},
    {"to_str", k_to_str, 1},
    {"to_string", k_to_string, 1},
    {"trunc", k_trunc, 1},
    {"type.find", k_find_type, 1},
    {"type.info.align", k_type_info_align, 1},
    {"type.info.field_count", k_type_info_field_count, 1},
    {"type.info.field_name", k_type_info_field_name, 2},
    {"type.info.field_offset", k_type_info_field_offset, 2},
    {"type.info.field_size", k_type_info_field_size, 2},
    {"type.info.kind", k_type_info_kind, 1},
    {"type.info.name", k_type_info_name, 1},
    {"type.info.size", k_type_info_size, 1},
    {"type.is_bool", k_is_bool, 1},
    {"type.is_float", k_is_float, 1},
    {"type.is_integer", k_is_integer, 1},
    {"type.is_numeric", k_is_numeric, 1},
    {"type.is_signed", k_is_signed, 1},
    {"type.is_unsigned", k_is_unsigned, 1},
    {"unique_box", k_unique_box, 1},
    {"unique_with", k_unique_with, 2},
    {"unloadmodule", k_unloadmodule, 1},
    {"unshare", k_unshare, 1},
    {"unwrap", k_unwrap, 1},
    {"unwrap_or", k_unwrap_or, 2},
    {"unwrap_unchecked", k_unwrap_unchecked, 1},
    {"use_count", k_use_count, 1},
    {"value", k_value, 1},
    {"wait", k_wait, 1},
    {"write", k_write, 2},
    {"write_borrow", k_write_borrow, 2},
};

constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

/**
 * @brief Compara dos nombres al compilar, como lo hara la busqueda.
 *
 * @param a Uno.
 * @param b El otro.
 * @return true si @p a va ESTRICTAMENTE antes que @p b.
 */
constexpr bool ct_before(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != '\0' && a[i] == b[i])
        ++i;
    return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[i]);
}

/**
 * @brief Recorre la tabla al compilar comprobando que esta ordenada.
 * @return true si cada entrada va antes que la siguiente.
 */
constexpr bool ct_rows_sorted() {
    for (size_t i = 1; i < kRowCount; ++i)
        if (!ct_before(kRows[i - 1].name, kRows[i].name)) return false;
    return true;
}

/* El orden NO se confia a quien anade una entrada.
 *
 * La busqueda es binaria, asi que una entrada fuera de sitio no da un error:
 * hace que NO SE ENCUENTREN otras -- las que quedan al otro lado del salto --,
 * y sin nombres de ranura un builtin "no separa" (ver overload.h), con lo que
 * choca con cualquier funcion del usuario de sus mismos tipos en vez de
 * sobrecargarse con ella.  Eso fue exactamente lo que paso: un renombrado
 * cambio los nombres en su sitio, dejando las entradas en la posicion vieja, y
 * 90 de 178 dejaron de encontrarse sin que nada lo dijera.
 *
 * La tabla de al lado (`builtin_names.cpp`) ya se guardaba asi; esta no, y era
 * la unica diferencia entre las dos. */
static_assert(ct_rows_sorted(),
              "vx/builtin_params.cpp: la tabla kRows tiene que estar ordenada "
              "por nombre -- la busqueda es binaria y una entrada fuera de "
              "sitio esconde otras en silencio");

} // namespace

BuiltinParams builtin_params_of(std::string_view name) noexcept {
    const Row *lo = kRows;
    const Row *hi = kRows + kRowCount;
    while (lo < hi) {
        const Row *mid = lo + (hi - lo) / 2;
        const int c = name.compare(mid->name);
        if (c == 0) return BuiltinParams{mid->params, mid->count};
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return BuiltinParams{};
}

} // namespace vx
