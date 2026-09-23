/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ssa_ir_serialize.h
 * @brief Serializacion binaria compacta de @c IrFunction para persistir
 *        IR junto al bytecode en `.velb`
 *
 * = Diseno =
 *
 * Formato binario little-endian, sin padding, sin alineacion (cada
 * lectura va con memcpy 1/2/4/8 bytes).  Las cstrings llevan u32 length
 * + bytes (sin terminador nulo).  Las constantes pequenas son u8/u16
 * donde caben; los IDs de valores/blocks van como u32.
 *
 * = Layout =
 *
 *   IrFunction:
 *     name            cstring  (u32 len + bytes)
 *     ret_type        u8       (IrType enum value)
 *     flags           u8       (bit0=is_native, bit1=is_variadic)
 *     param_count     u32
 *     params          u32 * param_count
 *     value_count     u32
 *     for each value: u8 type + u8
 * flags(host_ptr|gc_object|pointee_host_ptr|...)
 *                     + u64 const_val (solo si bit is_const)
 *     block_count     u32
 *     for each block:
 *         name        cstring
 *         instr_count u32
 *         for each instr:
 *             op           u16
 *             type         u8
 *             dst          u32 (IR_NO_VALUE = 0xFFFFFFFF)
 *             flags        u8 (bit0=preserve, bit1=is_call_site)
 *             source_line  u32
 *             imm          u64
 *             operand_count u8
 *             operands     u32 * operand_count
 *             func_name    cstring (vacia si no aplica)
 *             func_ptr     u32 (IR_NO_VALUE si no aplica)
 *             target_block u32 (IR_NO_BLOCK si no aplica)
 *             false_block  u32 (idem)
 *             phi_count    u8
 *             phi_args     (u32 value + u32 block) * phi_count
 *         pred_count       u32
 *         preds            u32 * pred_count
 *         succ_count       u32
 *         succs            u32 * succ_count
 *     template_name        cstring (vacia si no aplica - B.3)
 *     type_args_count      u32
 *     type_args            cstring * type_args_count
 *
 * = Tamano tipico =
 *
 *   Funcion pequena (~10 instrs): ~300 bytes serializada.
 *   Funcion media (100 instrs): ~3 KB.
 *   Modulo Vesta tipico (`vxed_modular.vx` ~30 fns x 50 instrs/fn):
 *      ~30 * 1.5 KB = 45 KB de IR vs ~150 KB de bytecode -> 1.3x total.
 *
 *   La compresion LZ4 (no implementada en v1) reduce IR ~3x adicional.
 */

#ifndef VESTA_IR_SSA_IR_SERIALIZE_H
#define VESTA_IR_SSA_IR_SERIALIZE_H

#include <cstdint>
#include <vector>

#include "ir/ssa_ir.h"
#include "util/byte_buffer.h"

namespace ir {

/**
 * @brief Que ES la seccion @c @ir: nombre, magic, version y como reserva.
 *
 * Su funcion de reserva es SUYA, y esa es la razon de que exista un descriptor
 * en vez de un buffer de bytes cualquiera: el intermedio es, con diferencia,
 * lo mas grande que produce una compilacion -- medido en un proyecto de 6k
 * lineas, 5,75 MiB de los 7,56 del artefacto --, y hasta ahora esos megabytes
 * se reservaban por el mismo camino que todo lo demas y el informe no podia
 * atribuirlos.
 */
extern const util::ByteKind kIrSectionKind;

/**
 * @brief Y que es el cache de IR por modulo (@c .vxir).
 *
 * Aparte del anterior aunque el contenido se parezca: son dos cosas con dos
 * vidas -- uno viaja DENTRO del artefacto y el otro vive en el cache --, y
 * mezclarlos en el informe es no poder distinguir lo que cuesta producir un
 * programa de lo que cuesta guardarlo para la proxima vez.
 */
extern const util::ByteKind kIrModuleCacheKind;

/**
 * @brief Serializa una @c IrFunction a bytes en @p out.
 *        Append (no clear) -- permite serializar varias funciones
 *        a un mismo buffer.
 * @return numero de bytes escritos para esta funcion.
 */
size_t serialize_function(const IrFunction &fn, util::ByteBuffer &out);

/**
 * @brief Deserializa una @c IrFunction por donde vaya @p c.
 *
 * El cursor avanza, y si algo va mal se queda con el MOTIVO -- se salio del
 * final, o una longitud del propio artefacto mentia --, que son cosas
 * distintas: la primera acusa a quien lee y la segunda al fichero.
 *
 * @param c Cursor sobre el buffer.
 * @param out Funcion reconstruida; puede quedar a medias si falla.
 * @return true si la deserializacion fue exitosa.
 */
bool deserialize_function(util::ByteCursor &c, IrFunction &out);

/* ===================================================================== */
/* Seccion @ir del archivo .velb                                         */
/* ===================================================================== */

/**
 * @brief Magic "VEIR" (little-endian) que abre la seccion @ir.
 *        El loader valida esto antes de deserializar para detectar
 *        corrupcion / version incorrecta de manera temprana.
 */
static constexpr uint32_t IR_SECTION_MAGIC = 0x52494556U; /* 'V''E''I''R' */

/**
 * @brief Version del formato @ir.  Bump cuando cambia el layout.
 */
static constexpr uint16_t IR_SECTION_VERSION =
    19; // v19: el IR dice de que FICHERO salio cada cosa.  Una tabla de rutas
        // por modulo, un indice en cada funcion y otro en cada sitio de
        // inlinado; la instruccion no cambia.  Hacia falta porque el modulo
        // que se compila es un FUSIONADO -- dentro conviven funciones de
        // muchos ficheros -- y hasta aqui cada consumidor adivinaba: el
        // informe ponia la ruta raiz, con lo que un error de la biblioteca se
        // leia como del fichero del usuario y en una linea que no existe.
        // v18: los prestamos dejan de ir en una tabla aparte -- ahora son
        // instrucciones `borrow` del cuerpo --, asi que su bloque desaparece
        // del formato.  Leer un artefacto v17 con este lector desalinearia el
        // resto del cuerpo, que es por lo que sube la version y no por lo que
        // se quito.
        // v17: cada nivel dice ademas QUIEN afirma cada promesa, aparte de si
        // esta demostrada.  Son dos ejes y no uno: `borrow_mut<T>` la DERIVA el
        // compilador del tipo sin que este demostrada, y un `out T*` la declara
        // el programador y hay que comprobarsela.  Con un solo bit, lo primero
        // se publicaba como lo segundo y el verificador de contratos se iba a
        // comprobar declaraciones que nadie habia escrito.
        // v16: el contrato de un parametro es POR NIVEL de indireccion, y cada
        // nivel lleva ademas lo que se NIEGA.  Hacian falta las dos cosas:
        // `const T*` habla de lo apuntado y `T* const` del puntero, asi que con
        // un solo registro las dos se guardaban igual; y sin la negacion, "no
        // consta que escriba" y "consta que NO escribe" tampoco se
        // distinguian.  v15: + direccion declarada de cada parametro
        // (`in`/`out`/`inout`) en
        // dos mascaras.  Sin ella, una funcion que llega de otro modulo pierde
        // la marca y su memoria vuelve a lo conservador: correcto, pero peor --
        // y lo peor es que dependeria de si el modulo se recompilo junto o
        // aparte.  v14: el cuerpo va COMPRIMIDO (deflate).  v13: + clase
        // declarada de cada ligadura de asm (AsmRegBinding::reg_class);
        // comparte serialize_function con la cache, asi que su formato cambia a
        // la vez

/**
 * @brief Banderas del campo reservado de la cabecera de la seccion.
 *
 * El hueco estaba puesto desde el principio "para futuras flags" y el lector
 * comprobaba que fuera cero; esta es la primera que se usa.
 */
static constexpr uint16_t kIrFlagDeflate = 0x0001; ///< el cuerpo va comprimido

/**
 * @brief Con que fuerza se comprime la seccion.
 *
 * Sale de medir sobre una seccion REAL de 5,75 MiB extraida de un artefacto:
 *
 *     nivel 1    5,94x    21,4 ms comprimir    6,3 ms descomprimir
 *     nivel 3   13,80x    22,9 ms              4,1 ms
 *     nivel 6   15,18x    56,8 ms              3,5 ms
 *     nivel 9   15,73x   191,4 ms              4,3 ms
 *
 * El 3 gana por lo que NO cuesta: frente al 6, la diferencia de tamano son 40
 * KiB sobre un artefacto de 7,5 MiB -- nada -- y tarda menos de la mitad.  El 1
 * comprime la mitad por el mismo tiempo que el 3, asi que no tiene sitio.
 */
static constexpr int kIrCompressionLevel = 3;

/**
 * @brief Por que no se pudo leer la seccion @c @ir.
 *
 * No es un detalle interno: de aqui sale lo que se le cuenta a quien ejecuta
 * el programa.  Que la seccion se descartara sin decir nada hacia que un
 * artefacto de otra version siguiera corriendo -- correctamente -- pero SIN
 * JIT, o sea del orden de diecisiete veces mas lento, sin que nada lo
 * explicara.  Y un programa lento no se investiga como se investiga un error.
 *
 * @c Ninguno es tambien la respuesta cuando el artefacto sencillamente no
 * trae intermedio, que es normal y no hay nada que contar.
 */
enum class IrSectionReject : uint8_t {
    None = 0,   ///< Se leyo bien, o no habia nada que leer.
    Version,    ///< La trae, pero de otra version del formato.
    Corrupt,    ///< Ni el magic ni las banderas cuadran.
    Truncated,  ///< Se acaba antes de lo que ella misma dice.
    Decompress, ///< Venia comprimida y no se pudo deshacer.
    Function    ///< Una de las funciones no se pudo reconstruir.
};

/**
 * @brief Lo que se sabe de por que no se pudo leer la seccion.
 *
 * Junto y no como parametros sueltos porque el motivo y su detalle son UNA
 * respuesta: la version que se encontro solo significa algo si el motivo es
 * @c IrSectionReject::Version, y separarlos deja que uno viaje sin el otro.
 */
struct IrSectionReport {
    IrSectionReject reject = IrSectionReject::None; ///< Que paso.
    /// Que version decia traer, cuando el motivo es @c Version.  Cero si no
    /// se llego a leer o el motivo es otro.
    uint16_t found_version = 0;
};

/**
 * @brief Emit del bytes de la seccion @c @ir lista para append a
 *        un `.velb`.  Layout:
 *
 *            +0  [4]  magic "VEIR"
 *            +4  [2]  version (u16, IR_SECTION_VERSION)
 *            +6  [2]  reserved (0)
 *            +8  [4]  function_count
 *            +12 [..] cuerpo: tabla de ficheros + functions (ver abajo)
 *
 * El cuerpo empieza por la tabla de ficheros del modulo -- u32 con cuantos,
 * y luego las rutas -- y sigue con las funciones concatenadas.  Va DENTRO del
 * cuerpo y no en la cabecera para no tocarla y porque se comprime con el
 * resto, que es donde una lista de rutas parecidas casi desaparece.
 *
 * @param functions IR functions a incluir en la seccion.
 * @param source_files Tabla de ficheros del modulo del que salen, contra la
 *        que se leen los indices @c IrFunction::source_file y
 *        @c InlineSite::source_file.  Sin ella los indices quedan apuntando a
 *        una tabla vacia y nadie puede decir de donde vino nada; por eso NO
 *        tiene valor por defecto -- quien emite la seccion tiene el modulo
 *        delante, y olvidarla seria perder el dato en silencio.
 * @param out Recibe los bytes.  Se inicializa aqui con la clase
 *        @ref kIrSectionKind, asi que lo que llegue dentro se pierde.
 *
 *        Sale por parametro y no por retorno porque un @c util::ByteBuffer es
 *        el DUENO de su memoria y no tiene constructor de movimiento que lo
 *        diga: devolverlo por valor dejaria dos copias apuntando al mismo
 *        bloque, que no falla al devolverlo sino al soltarlo.
 */
void emit_ir_section(
    const std::vector<IrFunction> &functions,
    const util::SmallVector<const std::string *, 4> &source_files,
    util::ByteBuffer &out);

/**
 * @brief Parse de la seccion @c @ir desde @p data leyendo
 *        @p section_size bytes a partir de @p offset.
 *
 * Valida magic + version antes de deserializar las funciones.
 * Si magic/version no coinciden, retorna false y deja @c functions
 * vacio (puede ser un .velb v2 sin IR -> graceful degradation).
 *
 * @param report Si no es nulo, recibe POR QUE no se pudo leer.  Existe porque
 *        un `false` pelado no distingue "este artefacto no trae intermedio"
 *        -- normal, no hay nada que decir -- de "lo trae pero es de otra
 *        version", que quien lo ejecuta necesita saber: el programa corre
 *        igual pero sin JIT, y eso no se nota como un error sino como que va
 *        lento.  Un analisis que renuncia sin decir por que parece que
 *        funciona.
 * @param source_files Si no es nulo, recibe la tabla de ficheros del modulo.
 *        Aqui SI es opcional -- al contrario que al emitir --: la tabla se lee
 *        siempre para avanzar por el cuerpo, y quien no vaya a citar un sitio
 *        no tiene por que quedarse con ella.  Omitirla no pierde nada, solo no
 *        lo pide.
 * @return true si parseo exitoso; false si magic/version invalido
 *         o si alguna funcion fallo deserializacion.
 */
bool parse_ir_section(const uint8_t *data, size_t section_size,
                      std::vector<IrFunction> &functions,
                      IrSectionReport *report = nullptr,
                      util::SmallVector<const std::string *, 4> *source_files =
                          nullptr);

/* ===================================================================== */
/* Cache de IR por modulo (.vxir)                                       */
/* ===================================================================== */

/**
 * @brief Magic "VXMC" del cache de IR por modulo (`.vxir`).
 *        DISTINTO de @c IR_SECTION_MAGIC ("VEIR") a proposito: un
 *        `.vxir` viejo (formato solo-funciones, magic "VEIR") falla
 *        el check de magic en @c parse_ir_module_cache y fuerza
 *        recompilar el dep (auto-invalidacion del cache obsoleto).
 */
static constexpr uint32_t IR_MODULE_CACHE_MAGIC =
    0x434D5856U; /* 'V''X''M''C' */
static constexpr uint16_t IR_MODULE_CACHE_VERSION =
    19; // v19: de que fichero salio cada funcion y cada trozo inlinado.  Sube
        // A LA VEZ que IR_SECTION_VERSION -- ver la nota de la v15: comparten
        // `serialize_function`, y olvidar una no da error de version sino un
        // cuerpo leido con el reparto equivocado.
        // v18: fuera el bloque de prestamos (ahora son instrucciones).
        // v17: cada promesa dice ademas QUIEN la afirma, aparte de si esta
        // demostrada.
        // v16: el contrato de cada parametro, por NIVEL y con la cara negativa.
        // v15: + el contrato de cada parametro.  Sube A LA VEZ que
        // IR_SECTION_VERSION porque las dos comparten `serialize_function`:
        // olvidarla no da un error de version -- la comprobacion pasa -- sino
        // un cuerpo leido con el reparto equivocado, y de ahi salen funciones
        // PERDIDAS y un simbolo sin resolver muy lejos del sitio.
        // v14: + los ejes `blocks` y `traps` de una nativa declarada

/**
 * @brief Serializa el IR de UN modulo COMPLETO para el cache `.vxir`.
 *
 * A diferencia de @c emit_ir_section (que solo guarda @c functions y
 * sirve para la seccion @c @ir del `.velb` consumida por el JIT), este
 * formato persiste TODO lo que el merge cross-module necesita del dep:
 *   - functions    (via @c emit_ir_section, con su header VEIR).
 *   - static_data  (pool + entries + meta).  CRITICO: sin esto, un dep
 *                  cache-hit aporta sus `code.s_N` refs pero no sus
 *                  slots -> relocaciones colgadas en el `.velb`.
 *   - globals      (map nombre -> IrValueId).
 *
 * @param mod  IrModule del dep a cachear.
 * @return     bytes listos para escribir al `.vxir`.
 */
void emit_ir_module_cache(const IrModule &mod, util::ByteBuffer &out);

/**
 * @brief Lo mismo, pero devolviendo un `std::vector`.  COSTURA TEMPORAL.
 *
 * Existe solo mientras la cadena del intermedio ya habla
 * @c util::ByteBuffer y los campos que lo transportan -- los de
 * @c CompileResult, y detras el enlazador, el capi, el LSP y el AOT -- siguen
 * siendo `std::vector<uint8_t>`.
 *
 * COPIA, y no es poco: son ~5,75 MiB en un proyecto de 6k lineas.  Esta aqui,
 * en UN sitio y con nombre, en vez de repartida por los ocho llamantes,
 * precisamente para que se vea lo que cuesta y para que quitarla sea borrar
 * una funcion.
 *
 * @param mod Modulo a serializar.
 * @return Sus bytes.
 */
std::vector<uint8_t> emit_ir_module_cache_vec(const IrModule &mod);

/**
 * @brief Reconstruye un @c IrModule completo desde un buffer `.vxir`
 *        producido por @c emit_ir_module_cache.
 *
 * Rellena @c out.functions, @c out.static_data y @c out.globals.
 *
 * @return @c true si el parseo fue exitoso; @c false si magic/version
 *         no coinciden (p.ej. un `.vxir` del formato viejo) o el
 *         buffer esta truncado.  En false, el caller debe recompilar.
 */
bool parse_ir_module_cache(const uint8_t *data, size_t len, IrModule &out);

/**
 * @brief Se lleva a @p dst TODO lo que trae un modulo restaurado del cache.
 *
 * Existe porque el traslado se hacia campo a campo en el sitio del acierto, y
 * **se ha olvidado un campo TRES veces** -- cada una con su arreglo y su
 * comentario en `compiler_project.cpp`:
 *
 *   - `static_data` y `globals`: sin ellos, un dep servido del cache no
 *     aportaba sus ranuras `code.s_*` al fusionar;
 *   - `native_imports`: sin ellos el enlazador dejaba simbolos colgando, y
 *     solo con el cache CALIENTE -- en frio compilaba bien;
 *   - `source_files`: sin ellos, **toda funcion de un modulo cacheado se
 *     quedaba sin fichero**, porque el remapeo contra una tabla vacia da
 *     @c IR_NO_SOURCE_FILE.  O sea que un error en una funcion servida del
 *     cache no sabia decir de que fichero era; y no daba ningun error.
 *
 * El patron es el mismo las tres veces: una copia campo a campo que hay que
 * acordarse de ampliar cuando @c IrModule crece, y olvidarse no rompe la
 * compilacion -- da un binario distinto, o un diagnostico que miente.
 *
 * Por eso vive AQUI y no alli: la lista de lo que hay que llevarse es
 * exactamente la de lo que @ref parse_ir_module_cache rellena, y las dos se
 * tocan juntas o no se tocan.  Quien anyada un campo al formato lo anyade en
 * las dos funciones, que estan una al lado de la otra.
 *
 * @param dst Modulo destino.
 * @param src Modulo recien parseado; queda vaciado.
 */
void adopt_cached_module(IrModule &dst, IrModule &&src);

/**
 * @brief Serializa una @c StaticDataStore verbatim (pool + entries + meta).
 *
 * Expuesto para el driver incremental (fragmentos de IR por-simbolo): los
 * blobs de static_data que referencia una funcion forman un mini-store que
 * viaja con su fragmento.  Round-trip byte-exacto con @c
 * deserialize_static_data.
 */
void serialize_static_data(const IrModule::StaticDataStore &sd,
                           util::ByteBuffer &out);

/**
 * @brief Reconstruye una @c StaticDataStore por donde vaya @p c.
 * @param c Cursor sobre el buffer.
 * @param sd Destino.
 * @return @c false si el buffer esta truncado o un rango cae fuera del pool.
 */
bool deserialize_static_data(util::ByteCursor &c,
                             IrModule::StaticDataStore &sd);

/* -------------------------------------------------------------------------
 * Escribir y leer bytes sueltos.
 *
 * DELEGAN en @c util/byte_buffer.h y no tienen logica propia, que es todo el
 * cambio: habia CUATRO implementaciones de esto mismo en el arbol -- aqui,
 * `util/serialize.h`, `emmit/bytewriter.h` + `bytereader.h`, y los lectores
 * sueltos del loader --, ninguna escrita para divergir; simplemente escribir
 * la quinta siempre fue el camino corto.
 *
 * Se conservan los nombres cortos porque son ~300 llamadas en este fichero y
 * renombrarlas no anyade nada: lo que importaba era que hubiera UN sitio donde
 * se decide como se pone un entero en un byte.
 * ---------------------------------------------------------------------- */

/** @brief Escribe un byte.  @param o Destino.  @param v Valor. */
inline void write_u8(util::ByteBuffer &o, uint8_t v) {
    util::byte_buffer_append_u8(o, v);
}
/** @brief Escribe 16 bits little-endian.  @param o Destino.  @param v Valor. */
inline void write_u16(util::ByteBuffer &o, uint16_t v) {
    util::byte_buffer_append_u16(o, v);
}
/** @brief Escribe 32 bits little-endian.  @param o Destino.  @param v Valor. */
inline void write_u32(util::ByteBuffer &o, uint32_t v) {
    util::byte_buffer_append_u32(o, v);
}
/** @brief Escribe 64 bits little-endian.  @param o Destino.  @param v Valor. */
inline void write_u64(util::ByteBuffer &o, uint64_t v) {
    util::byte_buffer_append_u64(o, v);
}
/**
 * @brief Escribe una cadena: longitud delante y detras sus bytes.
 * @param o Destino.  @param s Texto; puede llevar bytes nulos dentro.
 */
inline void write_str(util::ByteBuffer &o, const std::string &s) {
    util::byte_buffer_append_bytes_with_len(o, s.data(), s.size());
}

/* Lectura.  Un fallo no se devuelve solo: se queda apuntado en el cursor con
 * su MOTIVO, asi que una secuencia larga se comprueba una vez al final. */

/** @brief Lee un byte.  @param c Cursor.  @param out Destino.  @return false
 * si no quedaba. */
inline bool read_u8(util::ByteCursor &c, uint8_t &out) {
    return util::byte_cursor_read_u8(c, out);
}
/** @brief Lee 16 bits little-endian.  @param c Cursor.  @param out Destino.
 * @return false si no quedaban. */
inline bool read_u16(util::ByteCursor &c, uint16_t &out) {
    return util::byte_cursor_read_u16(c, out);
}
/** @brief Lee 32 bits little-endian.  @param c Cursor.  @param out Destino.
 * @return false si no quedaban. */
inline bool read_u32(util::ByteCursor &c, uint32_t &out) {
    return util::byte_cursor_read_u32(c, out);
}
/** @brief Lee 64 bits little-endian.  @param c Cursor.  @param out Destino.
 * @return false si no quedaban. */
inline bool read_u64(util::ByteCursor &c, uint64_t &out) {
    return util::byte_cursor_read_u64(c, out);
}
/**
 * @brief Lee una cadena escrita por @ref write_str.
 *
 * Esta SI copia, al contrario que @c util::byte_cursor_read_bytes_with_len:
 * el destino es una `std::string` que sobrevive al buffer.  Donde se pueda
 * evitar la copia conviene usar la de la capa comun directamente.
 *
 * @param c Cursor.  @param out Destino.  @return false si no cabia.
 */
inline bool read_str(util::ByteCursor &c, std::string &out) {
    const char *p = nullptr;
    size_t n = 0;
    if (!util::byte_cursor_read_bytes_with_len(c, &p, &n)) return false;
    out.assign(p, n);
    return true;
}

} // namespace ir

#endif // VESTA_IR_SSA_IR_SERIALIZE_H
