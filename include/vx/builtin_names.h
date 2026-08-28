/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/builtin_names.h
 * @brief Reconocer de una vez que builtin es un nombre.
 *
 * Un builtin se reconocia comparando el nombre contra literales, uno detras de
 * otro: doscientas y pico comparaciones de cadena por llamada, y todas se
 * hacian SIEMPRE -- eran variables `const bool` inicializadas en fila, no un
 * `if/else`, asi que acertar con la primera no ahorraba las demas.  El
 * compilador tampoco podia ahorrarlas: no sabe que si el nombre es `print`
 * entonces no es `println`.
 *
 * Y cada comparacion costaba mas de lo que parece.  `name == "println"` acaba
 * en `compare(const char *)`, que compara bytes sobre el minimo de las dos
 * longitudes ANTES de mirar los tamanos: no descarta por longitud, aunque una
 * mida tres y la otra veinte.
 *
 * Aqui el nombre se resuelve UNA vez a un valor de @ref Builtin, y a partir de
 * ahi quien despacha usa un `switch`, que el compilador convierte en tabla de
 * saltos.  La busqueda es binaria sobre una tabla plana ordenada por longitud
 * y bytes, asi que descarta comparando enteros antes de tocar un caracter:
 * ocho pasos en vez de doscientos.
 *
 * La lista es UNA: antes estaba escrita dos veces, en el bajador y en el
 * comprobador de tipos, con criterios distintos -- unos por `reg_builtin`,
 * otros comparando el nombre a mano --, asi que anadir un builtin exigia
 * acordarse de los dos sitios y no acordarse no daba error: daba un nombre que
 * type-checkea y no baja, o al reves.
 *
 * El enum de aqui y la tabla del fuente salen de la misma lista y el
 * compilador no deja que se separen: anadir a la tabla sin anadir al enum no
 * compila, y al reves salta un `static_assert` de tamano.
 */
#ifndef VX_BUILTIN_NAMES_H
#define VX_BUILTIN_NAMES_H

#include <cstdint>
#include <string_view>

namespace vx {

/**
 * @brief Los nombres que el lenguaje reconoce sin que nadie los declare.
 *
 * El valor concreto de cada uno NO es estable: sale del orden de la lista, que
 * va por LONGITUD para que la busqueda sea barata, asi que anadir un nombre
 * corto corre todos los de detras.  No serializarlo ni guardarlo en ningun
 * sitio que sobreviva a una compilacion.
 */
enum class Builtin : uint16_t {
    Unknown = 0, ///< El nombre no es de ningun builtin.

    /* --- 2 caracteres --- */
    Ok,

    /* --- 3 caracteres --- */
    Err,
    Abs,
    Chr,
    Clz,
    Cos,
    Ctz,
    Log,
    Ord,
    Pid,
    Pow,
    Sin,
    Tan,

    /* --- 4 caracteres --- */
    None,
    Some,
    Ceil,
    Echo,
    Fabs,
    Fmax,
    Fmin,
    Free,
    Imax,
    Imin,
    IsOk,
    Kind,
    Lend,
    Log2,
    Move,
    Rotl,
    Rotr,
    Sqrt,
    Wait,

    /* --- 5 caracteres --- */
    Bswap,
    Clamp,
    Deque,
    Error,
    Floor,
    Flush,
    Fopen,
    Ilog2,
    Imaxu,
    Iminu,
    Log10,
    Panic,
    Print,
    Queue,
    Round,
    Share,
    Stack,
    Trunc,
    Value,

    /* --- 6 caracteres --- */
    BgRgb,
    Extent,
    Fclose,
    FgRgb,
    Fwrite,
    GcBox,
    Gensym,
    Invoke,
    Malloc,
    Notify,
    Parent,
    PtrOf,
    Repeat,
    Sizeof,
    Substr,
    ToStr,
    Unwrap,

    /* --- 7 caracteres --- */
    Alignof,
    Bitcast,
    Dispose,
    FfiSym,
    ForName,
    Fulfill,
    Hashmap,
    Hashset,
    IsBool,
    IsChar,
    IsEnum,
    IsSame,
    Msgrecv,
    Msgsend,
    Println,
    Proceed,
    Replace,
    Treemap,
    Treeset,
    TypeId,
    Unshare,
    Vacount,

    /* --- 8 caracteres --- */
    ArgsGet,
    Contains,
    CtPrint,
    FfiCall,
    FfiOpen,
    GetClass,
    GetField,
    IsClass,
    IsFloat,
    LendMut,
    Offsetof,
    Popcount,
    StrCstr,
    StrHash,
    StrMake,
    StrWstr,
    Typename,

    /* --- 9 caracteres --- */
    Arraylist,
    FieldGet,
    FieldSet,
    FindType,
    GetFields,
    GetMethod,
    HasField,
    InBounds,
    IsPresent,
    IsOpaque,
    IsShared,
    IsSigned,
    IsString,
    IsStruct,
    NotifyAll,
    PrintBin,
    PrintHex,
    PrintInt,
    PrintOct,
    PrintPad,
    PrintPtr,
    StrBytes,
    TermMove,
    ToString,
    UseCount,

    /* --- 10 caracteres --- */
    ArgsCount,
    AtomicAdd,
    AtomicCas,
    FieldName,
    FieldType,
    GcCollect,
    GetFieldAt,
    GetMethods,
    HasMethod,
    IsInteger,
    IsNewtype,
    IsNumeric,
    IsPointer,
    IsSubtype,
    Loadmodule,
    PrintBool,
    PrintChar,
    PrintCstr,
    PrintUint,
    SharedBox,
    StrConcat,
    StrEquals,
    StrIntern,
    StrLength,
    TermClear,
    TermReset,
    UniqueBox,

    /* --- 11 caracteres --- */
    AtomicLoad,
    FiberEntry,
    FieldCount,
    GetMethodAt,
    IsUnsigned,
    NewInstance,
    PrintColor,
    PrintFloat,
    ReadBorrow,
    SectionEnd,
    SharedFree,
    SharedWith,
    StrConvert,
    UniqueWith,

    /* --- 12 caracteres --- */
    AtomicStore,
    ComptimeChr,
    ComptimeOrd,
    CpuFeatures,
    FutureAlloc,
    IsPrimitive,
    MethodCount,
    SectionSize,
    Unloadmodule,
    WriteBorrow,

    /* --- 13 caracteres --- */
    FiberSwapctx,
    SectionStart,
    SharedMalloc,
    StaticAssert,
    UnderlyingOf,

    /* --- 14 caracteres --- */
    AtomicAddI64,
    AtomicCasI64,
    ComptimePrint,
    ComptimeStreq,
    ForEachField,
    PrintGchandle,
    TypeInfoKind,
    TypeInfoName,
    TypeInfoSize,

    /* --- 15 caracteres --- */
    AtomicLoadI64,
    ComptimeConcat,
    ComptimeRepeat,
    ComptimeStrlen,
    ComptimeSubstr,
    ComptimeToStr,
    ForEachMethod,
    GcFinalizeAll,
    TermClearLine,
    TypeInfoAlign,

    /* --- 16 caracteres --- */
    AtomicStoreI64,
    ComptimeReplace,
    TermHideCursor,
    TermSaveCursor,
    TermShowCursor,
    UnwrapUnchecked,

    /* --- 17 caracteres --- */
    ComptimeContains,
    SharedGcCollect,
    SharedHeapBytes,

    /* --- 18 caracteres --- */
    AsNativeCallback,
    ComptimeTypeKind,

    /* --- 19 caracteres --- */
    TermRestoreCursor,

    /* --- 20 caracteres --- */
    ComptimeTypeSizeof,
    TypeInfoFieldName,
    TypeInfoFieldSize,

    /* --- 21 caracteres --- */
    ComptimeTypeAlignof,
    TypeInfoFieldCount,

    /* --- 22 caracteres --- */
    SharedHeapLiveCount,
    TypeInfoFieldOffset,

    Count ///< Cuantos hay, para dimensionar tablas por builtin.
};

/**
 * @brief Que builtin es @p name, o Builtin::Unknown si no es ninguno.
 *
 * Busqueda binaria sobre la tabla plana; el comparador mira primero la
 * longitud, que descarta la mayoria sin leer un solo byte del nombre.
 *
 * @param name El nombre invocado.
 * @return El builtin, o Builtin::Unknown.
 */
Builtin builtin_from_name(std::string_view name) noexcept;

/**
 * @brief A que familia del bajador pertenece un builtin.
 *
 * Las familias son disjuntas: cada builtin esta en una y solo una.  Eso es lo
 * que permite ir DIRECTO a la que le toca en vez de preguntarle a las siete
 * por turno, que era lo que se hacia -- y como cada una empieza descartando
 * los nombres que no son suyos, preguntarles a todas costaba recorrer las
 * listas de las seis que iban a decir que no.
 */
enum class BuiltinFamily : uint8_t {
    Other = 0,  ///< No es de ninguna familia separada; lo atiende el general.
    Print,      ///< Averiguar QUE se escribe y con que forma.
    Runtime,    ///< Pedirle algo al mundo: ficheros, memoria, fibras, modulos.
    Concurrent, ///< Suponer que hay alguien mas: compartida, atomicos, buzones.
    Optional,   ///< Lo que puede no estar: Optional y Result.
    Reflect,    ///< Preguntarle al programa por si mismo.
    Ownership,  ///< Quien es dueno de que, y quien lo suelta.
    String      ///< Lo que se hace con una cadena.
};

/**
 * @brief La familia de @p b, o BuiltinFamily::Other si no tiene una separada.
 *
 * Una lectura de una tabla plana indexada por el builtin.  La tabla se
 * construye AL COMPILAR desde el mismo reparto que se lee en el fuente, asi
 * que no hay dos sitios que mantener.
 *
 * @param b El builtin.
 * @return Su familia.
 */
BuiltinFamily builtin_family(Builtin b) noexcept;

/**
 * @brief El texto de un builtin, para diagnosticos.
 *
 * @param b El builtin.
 * @return Su nombre, o cadena vacia si @p b es Unknown o esta fuera de rango.
 */
std::string_view builtin_name(Builtin b) noexcept;

} // namespace vx

#endif // VX_BUILTIN_NAMES_H
