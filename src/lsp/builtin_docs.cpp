/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file builtin_docs.cpp
 * @brief Tabla estatica de documentacion de los builtins de Vesta (LSP).
 *
 * Cada entrada describe la firma, una descripcion breve (que recibe / hace /
 * retorna) y los nombres de los parametros (para los ghost args inline).  El
 * lookup es O(N) sobre una tabla pequena (~50 entradas), cacheable por el
 * caller si hiciera falta -- el hover y los hints no son hot paths.
 */

#include "lsp/builtin_docs.h"

#include <algorithm>
#include <unordered_map>

namespace lsp {

namespace {

/// Construye el mapa nombre -> BuiltinDoc una sola vez (estable durante el
/// proceso, devuelto por referencia).
const std::unordered_map<std::string, BuiltinDoc> &table() {
    static const std::unordered_map<std::string, BuiltinDoc> T = [] {
        std::unordered_map<std::string, BuiltinDoc> m;
        auto add = [&](BuiltinDoc d) { m[d.name] = std::move(d); };

        // ---- Salida de texto ----
        add({"print",
             "print(value) -> void",
             "Imprime el valor sin salto de linea.  Acepta cualquier tipo "
             "(string, enteros, bool, char, punteros) y strings interpolados "
             "${expr}.",
             {"value"}});
        add({"println",
             "println(value) -> void",
             "Imprime el valor seguido de un salto de linea.  Acepta cualquier "
             "tipo y strings interpolados ${expr}.",
             {"value"}});
        add({"echo",
             "echo(value) -> void",
             "Alias de print: imprime el valor sin salto de linea.",
             {"value"}});
        add({"flush",
             "flush() -> void",
             "Vacia el buffer de salida de vesta_io al terminal.",
             {}});
        add({"print_int",
             "print_int(n: i64) -> void",
             "Imprime un entero con signo en decimal.",
             {"n"}});
        add({"print_uint",
             "print_uint(n: u64) -> void",
             "Imprime un entero sin signo en decimal.",
             {"n"}});
        add({"print_hex",
             "print_hex(n: u64) -> void",
             "Imprime un entero en hexadecimal con prefijo 0x.",
             {"n"}});
        add({"print_bin",
             "print_bin(n: u64) -> void",
             "Imprime un entero en binario con prefijo 0b.",
             {"n"}});
        add({"print_oct",
             "print_oct(n: u64) -> void",
             "Imprime un entero en octal con prefijo 0o.",
             {"n"}});
        add({"print_float",
             "print_float(x: f64) -> void",
             "Imprime un numero en coma flotante.",
             {"x"}});
        add({"print_bool",
             "print_bool(b: bool) -> void",
             "Imprime 'true' o 'false'.",
             {"b"}});
        add({"print_char",
             "print_char(cp: u32) -> void",
             "Imprime el caracter del codepoint Unicode dado (UTF-8).",
             {"cp"}});
        add({"print_color",
             "print_color(code: u32) -> void",
             "Emite un codigo de color ANSI al terminal.",
             {"code"}});
        add({"print_ptr",
             "print_ptr(addr) -> void",
             "Imprime una direccion como 0x<hex> compacto.",
             {"addr"}});
        add({"print_gchandle",
             "print_gchandle(handle) -> void",
             "Imprime un GcHandle como <gc:N>.",
             {"handle"}});
        add({"print_cstr",
             "print_cstr(ptr) -> void",
             "Imprime una cadena C (NUL-terminada) desde memoria host.",
             {"ptr"}});
        add({"print_pad",
             "print_pad(fill_cp: u32, width: u64) -> void",
             "Emite @p width copias del codepoint @p fill_cp (alineacion).",
             {"fill_cp", "width"}});

        // ---- Introspeccion comptime ----
        add({"sizeof",
             "sizeof<T>() -> u64",
             "Tamano en bytes del tipo T (comptime).",
             {}});
        add({"alignof",
             "alignof<T>() -> u64",
             "Alineacion en bytes del tipo T (comptime).",
             {}});
        add({"typename",
             "typename<T>() -> string",
             "Nombre del tipo T como string (comptime).",
             {}});
        add({"static_assert",
             "static_assert(cond: bool, msg: string) -> void",
             "Falla la compilacion con @p msg si @p cond es falsa (comptime).",
             {"cond", "msg"}});
        add({"type_id",
             "type_id<T>() -> u32",
             "ID estable (FNV-1a del nombre canonico) del tipo T (comptime).  "
             "Igual ID = mismo tipo logico; util para comparar tipos con 1 "
             "cmp.",
             {}});
        add({"kind",
             "kind<T>() -> i32",
             "Categoria del tipo T (comptime): Primitive/Class/Struct/Enum/"
             "Optional/Result/Array/Ptr/Function/String/Borrow/Future/Unique/"
             "Shared/Collection (ver ComptimeKind).",
             {}});
        // ---- Predicados de tipo (comptime, -> bool) ----
        add({"is_class",
             "is_class<T>() -> bool",
             "true si T es un tipo CLASS (reference type, GcHandle).",
             {}});
        add({"is_struct",
             "is_struct<T>() -> bool",
             "true si T es un STRUCT value-type (no enum ni class).",
             {}});
        add({"is_enum",
             "is_enum<T>() -> bool",
             "true si T es un enum (ADT tagged-union o C-style con valor de "
             "backing entero/float/string).  Los backing struct/clase son su "
             "tipo base -> false.",
             {}});
        add({"is_primitive",
             "is_primitive<T>() -> bool",
             "true si T es un primitivo escalar (i8..u64, f32/f64, bool, char, "
             "void).",
             {}});
        add({"is_newtype",
             "is_newtype<T>() -> bool",
             "true si T es un newtype (typedef T name new).",
             {}});
        add({"is_opaque",
             "is_opaque<T>() -> bool",
             "true si T es un tipo opaco (@opaque).",
             {}});
        add({"is_shared",
             "is_shared<T>() -> bool",
             "true si T es un tipo shared<T> (memoria compartida "
             "cross-process).",
             {}});
        add({"is_same",
             "is_same<A, B>() -> bool",
             "true si A y B son el mismo tipo logico (mismo nombre canonico).",
             {}});
        add({"is_subtype",
             "is_subtype<A, B>() -> bool",
             "true si A es subtipo de B (herencia de clases / interfaces).",
             {}});
        add({"underlying_of",
             "underlying_of<T>() -> string",
             "Nombre del tipo subyacente de un typedef/newtype (comptime).",
             {}});
        // ---- Introspeccion de campos y metodos (comptime) ----
        add({"field_count",
             "field_count<T>() -> u32",
             "Numero de campos de T (struct/class, incluye heredados; enum = "
             "numero de variantes).",
             {}});
        add({"method_count",
             "method_count<T>() -> u32",
             "Numero de metodos de T (solo CLASS; incluye heredados).",
             {}});
        add({"offsetof",
             "offsetof<T>(field: string) -> u64",
             "Offset en bytes del campo @p field dentro de T (struct/class; "
             "tambien overlay).",
             {"field"}});
        add({"has_field",
             "has_field<T>(name: string) -> bool",
             "true si T tiene un campo llamado @p name.",
             {"name"}});
        add({"has_method",
             "has_method<T>(name: string) -> bool",
             "true si T tiene un metodo llamado @p name (solo CLASS).",
             {"name"}});
        add({"field_name",
             "field_name<T>(idx: int) -> string",
             "Nombre del campo idx-esimo de T (orden de declaracion).",
             {"idx"}});
        add({"field_type",
             "field_type<T>(field: string) -> string",
             "Nombre canonico del tipo del campo @p field de T.",
             {"field"}});
        add({"field_type_at",
             "field_type_at<T>(idx: int) -> Type",
             "Tipo del campo idx-esimo de T como valor de primera clase "
             "(comptime).",
             {"idx"}});
        add({"method_name",
             "method_name<T>(idx: int) -> string",
             "Nombre del metodo idx-esimo de T (solo CLASS).",
             {"idx"}});
        add({"method_return_type",
             "method_return_type<T>(idx: int) -> Type",
             "Tipo de retorno del metodo idx-esimo de T (comptime).",
             {"idx"}});
        // ---- Overlay structs (vistas tipadas sobre memoria) ----
        add({"in_bounds",
             "in_bounds(field_addr, buf_size: u64) -> bool",
             "true si el campo de un overlay cae dentro de @p buf_size bytes "
             "del buffer base (chequeo de limites de un formato binario).",
             {"field_addr", "buf_size"}});
        add({"extent",
             "extent<T>() -> u64",
             "Tamano total (en bytes) que un overlay T cubre desde su base, "
             "incluyendo campos con @offset dinamico y arrays con stride.",
             {}});
        // ---- Conceptos built-in (bound <T: C> o predicado C<T>() -> bool)
        // ---- Se usan como restriccion de generico (<T: Numeric>) o como
        // predicado comptime directo (if (Numeric<T>()) { ... }).  Cero
        // runtime.
        auto concept_doc = [&](const char *name, const char *desc) {
            add({name,
                 std::string(name) + "<T>()  |  <T: " + name + ">",
                 std::string("Concepto (comptime): ") + desc +
                     "  Usable como bound `<T: " + name +
                     ">` o como predicado `" + name + "<T>() -> bool`.",
                 {}});
        };
        concept_doc("Numeric", "T es un numero (entero o float).");
        concept_doc("Number", "alias de Numeric.");
        concept_doc("Integer", "T es un entero (i8..u64).");
        concept_doc("Int", "alias de Integer.");
        concept_doc("Float", "T es float (f32/f64).");
        concept_doc("Signed", "T es un entero con signo (i8..i64).");
        concept_doc("Unsigned", "T es un entero sin signo (u8..u64).");
        concept_doc("Bool", "T es bool.");
        concept_doc("Char", "T es char.");
        concept_doc("Pointer", "T es un puntero (T*).");
        concept_doc("String", "T es string.");
        concept_doc("Comparable",
                    "T soporta orden (< >): numericos, char, bool.");
        concept_doc("Ordered", "alias de Comparable.");
        concept_doc("Eq", "T soporta == : primitivos, string, punteros.");
        concept_doc("Sized", "T tiene tamano conocido > 0 (todo salvo void).");
        concept_doc("Copyable",
                    "T es copiable por valor (primitivos, structs, ptr).");
        concept_doc("Hashable", "T tiene hash canonico (primitivos + string).");
        concept_doc("Stringable", "T es string/primitivo o tiene toString().");
        concept_doc("Default",
                    "T tiene valor por defecto (primitivos + punteros).");
        concept_doc("Primitive", "T es un primitivo escalar.");
        concept_doc("Class", "T es una clase (reference type).");
        concept_doc("Struct", "T es un struct value-type.");
        concept_doc("Callable", "T es invocable (fn(...) -> R).");
        concept_doc("Destructible",
                    "T puede tener destructor (~T()): clases/structs.");
        concept_doc("Iterable",
                    "T es iterable (array/string o tiene iter()/next()).");
        concept_doc("Shareable",
                    "T es compartible cross-process (GC + value-types).");
        concept_doc("Enum", "T es un enum (ADT o C-style con valor).");
        concept_doc("ValuedEnum", "T es un enum C-style con valor (no ADT).");

        // ---- Strings ----
        add({"str_length",
             "str_length(s: string) -> u64",
             "Numero de code points (longitud logica) de la cadena.",
             {"s"}});
        add({"str_bytes",
             "str_bytes(s: string) -> u64",
             "Numero de bytes que ocupa la cadena.",
             {"s"}});
        add({"str_concat",
             "str_concat(a: string, b: string) -> string",
             "Concatena dos cadenas y devuelve una nueva.",
             {"a", "b"}});
        add({"str_equals",
             "str_equals(a: string, b: string) -> bool",
             "true si las dos cadenas son iguales byte a byte.",
             {"a", "b"}});
        add({"str_intern",
             "str_intern(s: string) -> string",
             "Devuelve la version canonica internada de la cadena.",
             {"s"}});
        add({"str_cstr",
             "str_cstr(s: string) -> char*",
             "Puntero host NUL-terminado a los bytes (para FFI *A).",
             {"s"}});
        add({"to_str",
             "to_str(value) -> string",
             "Convierte un entero/valor a su representacion en cadena.",
             {"value"}});
        add({"chr",
             "chr(cp: u32) -> string",
             "Cadena de un caracter desde el codepoint Unicode.",
             {"cp"}});
        add({"ord",
             "ord(s: string) -> u32",
             "Codepoint del primer caracter de la cadena.",
             {"s"}});
        add({"substr",
             "substr(s: string, start: u64, len: u64) -> string",
             "Subcadena de @p len code points desde @p start.",
             {"s", "start", "len"}});
        add({"repeat",
             "repeat(s: string, n: u64) -> string",
             "Repite la cadena @p n veces.",
             {"s", "n"}});
        add({"replace",
             "replace(s: string, from: string, to: string) -> string",
             "Reemplaza todas las ocurrencias de @p from por @p to.",
             {"s", "from", "to"}});
        add({"contains",
             "contains(s: string, needle: string) -> bool",
             "true si @p needle aparece en @p s.",
             {"s", "needle"}});

        // ---- Memoria / punteros ----
        add({"malloc",
             "malloc(bytes: u64) -> T*",
             "Reserva @p bytes de memoria host cruda; devuelve el puntero.",
             {"bytes"}});
        add({"free",
             "free(ptr) -> void",
             "Libera memoria previamente reservada con malloc.",
             {"ptr"}});
        add({"panic",
             "panic(msg: string) -> never",
             "Aborta el proceso con FatalError(USER_ABORT) y el mensaje dado.",
             {"msg"}});

        // ---- Concurrencia / proceso ----
        add({"pid", "pid() -> i64", "PID codificado del proceso actual.", {}});
        add({"spawn",
             "spawn { ... }",
             "Crea un proceso ligero en el mismo scheduler.",
             {}});
        add({"await",
             "await(fut) -> T",
             "Bloquea hasta que el Future se resuelve y devuelve su valor.",
             {"fut"}});
        add({"msgsend",
             "msgsend(pid, payload) -> bool",
             "Envia un mensaje al buzon del proceso @p pid.",
             {"pid", "payload"}});
        add({"msgrecv",
             "msgrecv() -> Bytes",
             "Recibe un mensaje del buzon propio (bloquea si esta vacio).",
             {}});
        add({"loadmodule",
             "loadmodule(path: string) -> i64",
             "Carga dinamicamente un .velb adicional desde el filesystem.",
             {"path"}});

        // ---- Matematicas (stdlib vesta_math) ----
        add({"sqrt", "sqrt(x: f64) -> f64", "Raiz cuadrada.", {"x"}});
        add({"pow",
             "pow(base: f64, exp: f64) -> f64",
             "Potencia base^exp.",
             {"base", "exp"}});
        add({"abs", "abs(x: f64) -> f64", "Valor absoluto (f64).", {"x"}});
        add({"fabs", "fabs(x: f64) -> f64", "Valor absoluto (f64).", {"x"}});
        add({"floor", "floor(x: f64) -> f64", "Redondeo hacia abajo.", {"x"}});
        add({"ceil", "ceil(x: f64) -> f64", "Redondeo hacia arriba.", {"x"}});
        add({"round",
             "round(x: f64) -> f64",
             "Redondeo al entero mas cercano.",
             {"x"}});
        add({"trunc",
             "trunc(x: f64) -> f64",
             "Trunca la parte fraccionaria.",
             {"x"}});
        add({"log",
             "log(x: f64) -> f64",
             "Logaritmo natural (base e).",
             {"x"}});
        add({"log2", "log2(x: f64) -> f64", "Logaritmo base 2.", {"x"}});
        add({"log10", "log10(x: f64) -> f64", "Logaritmo base 10.", {"x"}});
        add({"sin", "sin(x: f64) -> f64", "Seno (radianes).", {"x"}});
        add({"cos", "cos(x: f64) -> f64", "Coseno (radianes).", {"x"}});
        add({"tan", "tan(x: f64) -> f64", "Tangente (radianes).", {"x"}});
        add({"fmin",
             "fmin(a: f64, b: f64) -> f64",
             "Minimo de dos f64.",
             {"a", "b"}});
        add({"fmax",
             "fmax(a: f64, b: f64) -> f64",
             "Maximo de dos f64.",
             {"a", "b"}});
        add({"clamp",
             "clamp(x: f64, lo: f64, hi: f64) -> f64",
             "Acota x al rango [lo, hi].",
             {"x", "lo", "hi"}});
        add({"imin",
             "imin(a: i64, b: i64) -> i64",
             "Minimo de dos enteros con signo.",
             {"a", "b"}});
        add({"imax",
             "imax(a: i64, b: i64) -> i64",
             "Maximo de dos enteros con signo.",
             {"a", "b"}});
        add({"iminu",
             "iminu(a: u64, b: u64) -> u64",
             "Minimo de dos enteros sin signo.",
             {"a", "b"}});
        add({"imaxu",
             "imaxu(a: u64, b: u64) -> u64",
             "Maximo de dos enteros sin signo.",
             {"a", "b"}});
        add({"ilog2",
             "ilog2(x: u64) -> u64",
             "Logaritmo entero base 2 (posicion del bit mas alto).",
             {"x"}});

        // ---- Operaciones de bits ----
        add({"popcount",
             "popcount(x: u64) -> u64",
             "Numero de bits a 1 (population count).",
             {"x"}});
        add({"clz",
             "clz(x: u64) -> u64",
             "Ceros a la izquierda (count leading zeros).",
             {"x"}});
        add({"ctz",
             "ctz(x: u64) -> u64",
             "Ceros a la derecha (count trailing zeros).",
             {"x"}});
        add({"bswap",
             "bswap(x: u64) -> u64",
             "Invierte el orden de bytes (byte swap).",
             {"x"}});
        add({"rotl",
             "rotl(x: u64, n: u64) -> u64",
             "Rotacion de bits a la izquierda.",
             {"x", "n"}});
        add({"rotr",
             "rotr(x: u64, n: u64) -> u64",
             "Rotacion de bits a la derecha.",
             {"x", "n"}});

        // ---- Strings (extra) ----
        add({"str_make",
             "str_make(ptr, len: u64, enc: u32) -> string",
             "Construye un StringObject desde un buffer.",
             {"ptr", "len", "enc"}});
        add({"str_hash",
             "str_hash(s: string) -> u64",
             "Hash FNV-1a de la cadena (cacheado).",
             {"s"}});
        add({"str_convert",
             "str_convert(s: string, enc: u32) -> string",
             "Convierte la cadena a otra codificacion.",
             {"s", "enc"}});
        add({"str_wstr",
             "str_wstr(s: string) -> u16*",
             "Puntero host UTF-16LE NUL-terminado (FFI *W de Win32).",
             {"s"}});

        // ---- I/O de fichero ----
        add({"fopen",
             "fopen(path: string, mode: string) -> u64",
             "Abre un fichero; devuelve el FILE* (0 si falla).",
             {"path", "mode"}});
        add({"fclose",
             "fclose(fp: u64) -> i32",
             "Cierra un fichero abierto con fopen.",
             {"fp"}});
        add({"fwrite",
             "fwrite(fp: u64, ptr, len: u64) -> u64",
             "Escribe @p len bytes al fichero.",
             {"fp", "ptr", "len"}});

        // ---- FFI dinamica ----
        add({"ffi_open",
             "ffi_open(lib: string) -> u64",
             "Carga una DLL/.so en runtime (LoadLibrary/dlopen).",
             {"lib"}});
        add({"ffi_sym",
             "ffi_sym(handle: u64, name: string) -> u64",
             "Resuelve la direccion de un simbolo nativo.",
             {"handle", "name"}});
        add({"ffi_call",
             "ffi_call(fn: u64, args...) -> i64",
             "Invoca una funcion nativa resuelta con ffi_sym.",
             {"fn"}});

        // ---- Sistema / misc ----
        add({"cpu_features",
             "cpu_features() -> u64",
             "Bitmask de features del CPU detectadas via cpuid.",
             {}});
        add({"as_native_callback",
             "as_native_callback(fn) -> u64",
             "Adapta una funcion Vesta a un puntero de callback C nativo.",
             {"fn"}});
        add({"dispose",
             "dispose(x) -> void",
             "Libera explicitamente un recurso (unique/coleccion) ahora.",
             {"x"}});
        add({"section_start",
             "section_start(name: string) -> u64",
             "Direccion de inicio de una seccion enlazada.",
             {"name"}});
        add({"section_end",
             "section_end(name: string) -> u64",
             "Direccion de fin de una seccion enlazada.",
             {"name"}});
        add({"section_size",
             "section_size(name: string) -> u64",
             "Tamano en bytes de una seccion enlazada.",
             {"name"}});

        // ---- Smart pointers / memoria gestionada ----
        add({"unique_box",
             "unique_box(value) -> unique<T>",
             "Crea un puntero unique con cleanup automatico (deleter free).",
             {"value"}});
        add({"shared_box",
             "shared_box(value) -> shared<T>",
             "Crea un puntero shared con refcount.",
             {"value"}});
        add({"move",
             "move(p) -> unique<T>",
             "Transfiere la propiedad; invalida el origen (mvtake).",
             {"p"}});
        add({"ptr_of",
             "ptr_of(p) -> T*",
             "Extrae el puntero crudo sin consumir el smart pointer.",
             {"p"}});
        add({"use_count",
             "use_count(s) -> u64",
             "Numero de referencias vivas de un shared<T>.",
             {"s"}});

        // ---- Borrow checker ----
        add({"lend",
             "lend(owner) -> borrow<T>",
             "Prestamo compartido (solo lectura) del owner.",
             {"owner"}});
        add({"lend_mut",
             "lend_mut(owner) -> borrow_mut<T>",
             "Prestamo exclusivo (lectura/escritura) del owner.",
             {"owner"}});
        add({"read_borrow",
             "read_borrow(b) -> T",
             "Lee el valor a traves de un borrow.",
             {"b"}});
        add({"write_borrow",
             "write_borrow(m, v) -> void",
             "Escribe a traves de un borrow_mut.",
             {"m", "v"}});

        // ---- Reflexion ----
        add({"forName",
             "forName(name: string) -> Class",
             "Busca una clase por nombre en el ClassRegistry.",
             {"name"}});
        add({"getClass",
             "getClass(obj) -> Class",
             "Clase del objeto en runtime.",
             {"obj"}});
        add({"newInstance",
             "newInstance(cls: Class) -> Object",
             "Crea una instancia con el constructor por defecto.",
             {"cls"}});

        // ---- Atomics (memoria compartida) ----
        add({"atomic_load_i64",
             "atomic_load_i64(addr) -> i64",
             "Carga atomica (acquire) de un i64 desde host_ptr.",
             {"addr"}});
        add({"atomic_store_i64",
             "atomic_store_i64(addr, val: i64) -> void",
             "Almacenamiento atomico (release) de un i64.",
             {"addr", "val"}});
        add({"atomic_cas_i64",
             "atomic_cas_i64(addr, expected: i64, desired: i64) -> i64",
             "Compare-and-swap atomico; devuelve el valor previo.",
             {"addr", "expected", "desired"}});
        add({"atomic_add_i64",
             "atomic_add_i64(addr, delta: i64) -> i64",
             "Fetch-add atomico; devuelve el valor previo.",
             {"addr", "delta"}});

        return m;
    }();
    return T;
}

} // namespace

const BuiltinDoc *lookup_builtin(const std::string &name) {
    const auto &T = table();
    auto it = T.find(name);
    return it == T.end() ? nullptr : &it->second;
}

const std::vector<std::string> &all_builtin_names() {
    // Se construye una sola vez a partir de las claves de la tabla, ordenadas
    // para que el completado del LSP tenga un orden estable.
    static const std::vector<std::string> names = [] {
        const auto &T = table();
        std::vector<std::string> v;
        v.reserve(T.size());
        for (const auto &kv : T)
            v.push_back(kv.first);
        std::sort(v.begin(), v.end());
        return v;
    }();
    return names;
}

} // namespace lsp
