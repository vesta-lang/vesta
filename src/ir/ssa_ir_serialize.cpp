/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ssa_ir_serialize.cpp
 * @brief Serializer/deserializer binario del IR SSA para embeber en .velb.
 *
 * El IR generado por el frontend Vesta se puede persistir dentro del
 * binario @c .velb en una seccion @c @ir (magic @c VEIR).  Esto permite:
 *
 *   - **JIT a partir del binario sin recompilar**: el loader recupera el
 *     IR del .velb y lo pasa al JitCompiler.  Sin esto, el JIT solo
 *     podria trabajar con codigo compilado in-process durante una
 *     ejecucion (perdiendo el speedup post-warmup en runs cortos).
 *
 *   - **AOT futuro ( D.10+)**: el optimizer del JIT puede operar
 *     ANTES de la ejecucion sobre el IR del .velb y persistir codigo
 *     nativo en un .velao adicional.
 *
 *   - **Debugging / tooling**: dumps del IR sirven para ver lo que el
 *     frontend genero sin tener que recompilar con flags especiales.
 *
 * Diseno del formato:
 *   - **Little-endian** consistente con el resto del .velb.
 *   - **Schema fijo por @c IrInstr**: serializamos SIEMPRE los mismos
 *     campos (op, type, dst, flags, source_line, imm, operands,
 *     func_name, func_ptr, target_block, false_block, phi_args)
 *     incluso si una op concreta no usa alguno.  Trade-off: ~10-20% mas
 *     de espacio que un schema variable, pero parser/writer mucho mas
 *     simples y robustos.  Una v2 podria optimizar via "op -> field set"
 *     (variable-length encoding por op) cuando el espacio importe.
 *   - **Strings con length prefix u32**: simple, no requiere escapes ni
 *     terminadores; tolera bytes arbitrarios (incluyendo NUL embebidos).
 *   - **IDs como u32**: cabe cualquier IR razonable (4G values/blocks)
 *     sin gastar el espacio de un u64.
 */

#include "ir/ssa_ir_serialize.h"

// deflate para la seccion @ir (ver kIrCompressionLevel).
#include "miniz.h"

// El asignador del proyecto.  NO `malloc`: el nuestro es el que la cuenta de
// reservas ve, y lo que salga por otra puerta no aparece en el informe --
// que es justo lo que este trabajo esta arreglando.
#include "util/alloc/host_allocator.h"

#include <cstring>

namespace ir {

/* =====================================================================
 * QUE es cada buffer de esta cadena, y COMO reserva.
 *
 * Las dos funciones de reserva son lo que hace que el informe pueda
 * atribuirles sus megabytes: sin plantillas ni virtuales, la unica forma de
 * que una reserva salga con nombre propio en el paseo de la pila es que la
 * funcion llamada sea suya.  Antes las dos, y todo lo demas del compilador,
 * aparecian como `std::vector<unsigned char>`.
 *
 * Son DOS y no una aunque el contenido se parezca: el intermedio que viaja
 * dentro del artefacto y el que se guarda para la proxima compilacion tienen
 * vidas distintas, y juntarlos seria no poder separar lo que cuesta producir
 * un programa de lo que cuesta cachearlo.
 * ===================================================================== */

/**
 * @brief A cuantos bytes se alinea lo que reserva esta cadena.
 *
 * Una linea de cache.  No es por capricho: estos buffers se llenan y se
 * vacian a base de copias grandes, y con la alineacion natural del asignador
 * -- 16 -- una copia vectorizada alineada no se puede ni demostrar ni emitir.
 * Cuesta unos pocos bytes en UNA reserva que ya es de megabytes.
 */
static constexpr size_t kIrBufferAlign = 64;

/**
 * @brief Reserva para la seccion @c @ir.
 *
 * Las tres lineas se repiten en la cadena de al lado, y es a proposito: que
 * cada una tenga SU funcion es lo unico que hace que el informe de reservas
 * pueda separarlas.  Una funcion comun y bien factorizada las volveria a
 * juntar en un solo simbolo, que es el problema del que venimos.
 *
 * El ambito dice de que va lo que se reserva: vive lo que dure una fase de
 * compilacion (@c Medium) y el buffer se dobla segun se llena, abandonando el
 * anterior (@c Growing).
 *
 * @param n Bytes.
 * @return El bloque, o nulo.
 */
static void *ir_section_alloc(size_t n) {
    util::AllocScope scope{util::AllocUse::Medium, util::AllocShape::Growing};
    return util::host_alloc_aligned(n, kIrBufferAlign);
}
/// @brief La suelta, por la MISMA puerta por la que entro.  @param p Bloque.
static void ir_section_free(void *p) noexcept { util::host_free_aligned(p); }

/**
 * @brief Reserva para el cache de IR por modulo.  @param n Bytes.
 * @return El bloque, o nulo.  @see ir_section_alloc
 */
static void *ir_module_cache_alloc(size_t n) {
    util::AllocScope scope{util::AllocUse::Medium, util::AllocShape::Growing};
    return util::host_alloc_aligned(n, kIrBufferAlign);
}
/// @brief Lo suelta.  @param p Bloque.
static void ir_module_cache_free(void *p) noexcept {
    util::host_free_aligned(p);
}

const util::ByteKind kIrSectionKind = {
    "ir-section",           IR_SECTION_MAGIC, IR_SECTION_VERSION,
    kIrBufferAlign,         ir_section_alloc, ir_section_free};

const util::ByteKind kIrModuleCacheKind = {
    "ir-module-cache",     IR_MODULE_CACHE_MAGIC, IR_MODULE_CACHE_VERSION,
    kIrBufferAlign,        ir_module_cache_alloc, ir_module_cache_free};

/* ===================================================================== */
/* IrValue                                                                */
/* ===================================================================== */

namespace {

// Bits del byte flags por IrValue.  Compactamos 5 bools en un byte
// en lugar de un byte por flag (ahorra ~5 bytes por SSA value).
// El orden es estable -- NO reordenar entre versiones del formato
// sin bump del version field, porque los .velb antiguos quedarian
// ininterpretables.
constexpr uint8_t IRVAL_FLAG_PARAM = 1 << 0; ///< Parametro formal de la funcion
constexpr uint8_t IRVAL_FLAG_CONST =
    1 << 1; ///< Constante compile-time (const_val valido)
constexpr uint8_t IRVAL_FLAG_HOST_PTR =
    1 << 2; ///< Puntero a memoria host (no VM)
constexpr uint8_t IRVAL_FLAG_POINTEE_HOST_PTR =
    1 << 3; ///< Puntero VM cuyo contenido apunta a host
constexpr uint8_t IRVAL_FLAG_GC_OBJECT = 1 << 4;
/// v11: el valor lleva el registro fisico donde lo dejo el asignador.  Es un
/// bit y no un byte fijo porque la mayoria de valores no lo tienen (murieron o
/// se derramaron) y el intermedio se llevaria un byte por cada uno.
constexpr uint8_t IRVAL_FLAG_HAS_REG = 1 << 5; ///< Host_ptr a objeto GC-managed

/**
 * @brief COMO se supo de que memoria es la direccion, en los dos bits altos.
 *
 * Sin esto el viaje por la cache colapsa la clase: el bit @c HOST_PTR solo
 * dice si LO ES, asi que un `HostByType` volvia como deducido, y la
 * distincion se perdia justo donde mas se nota -- en el modulo que se relee
 * en vez de recompilarse --.
 *
 * SIN cambio de version, y por como estan elegidos los valores: el CERO
 * significa "no se dijo", y entonces la clase se deriva del bit de siempre.
 * Un fichero viejo trae ceros ahi y se lee exactamente como antes; un lector
 * viejo ignora estos bits y sigue leyendo el suyo.
 */
constexpr uint8_t IRVAL_KIND_SHIFT = 6;
constexpr uint8_t IRVAL_KIND_MASK = 0x3u << IRVAL_KIND_SHIFT;
constexpr uint8_t IRVAL_KIND_UNSAID = 0;       ///< derivar del bit de siempre
constexpr uint8_t IRVAL_KIND_CONSTRUCTION = 1; ///< lo construyo el compilador
constexpr uint8_t IRVAL_KIND_TYPE = 2;         ///< lo declara el tipo
constexpr uint8_t IRVAL_KIND_UNKNOWN = 3;      ///< nadie lo sabe

/// La clase de @p m, para el byte de banderas.  @c HostByInference sale como
/// "no se dijo": es lo que el bit de siempre ya significaba, asi que no gasta
/// codigo propio.
inline uint8_t kind_bits_of(ir::MemorySpace m) {
    switch (m) {
    case ir::MemorySpace::HostByConstruction: return IRVAL_KIND_CONSTRUCTION;
    case ir::MemorySpace::HostByType: return IRVAL_KIND_TYPE;
    case ir::MemorySpace::Unknown: return IRVAL_KIND_UNKNOWN;
    case ir::MemorySpace::HostByInference:
    case ir::MemorySpace::NotHost: break;
    }
    return IRVAL_KIND_UNSAID;
}

/// Lo contrario: de los bits y del bit de siempre, la clase.
inline ir::MemorySpace memory_of_flags(uint8_t flags) {
    switch ((flags & IRVAL_KIND_MASK) >> IRVAL_KIND_SHIFT) {
    case IRVAL_KIND_CONSTRUCTION: return ir::MemorySpace::HostByConstruction;
    case IRVAL_KIND_TYPE: return ir::MemorySpace::HostByType;
    case IRVAL_KIND_UNKNOWN: return ir::MemorySpace::Unknown;
    default: break;
    }
    /* No se dijo: un fichero de antes, o una deduccion.  El bit de siempre lo
     * contesta igual que lo contestaba entonces. */
    return (flags & IRVAL_FLAG_HOST_PTR) != 0 ? ir::MemorySpace::HostByInference
                                              : ir::MemorySpace::NotHost;
}

// Bits del byte flags por IrInstr.  Solo 2 bits usados: el resto
// queda para extensiones futuras sin cambio de formato.
constexpr uint8_t INSTR_FLAG_PRESERVE =
    1 << 0; ///< No eliminar aunque dst este muerto (side-effect implicito)
constexpr uint8_t INSTR_FLAG_IS_CALL_SITE =
    1 << 1; ///< Marca instruccion como call site (para stackmaps)
constexpr uint8_t INSTR_FLAG_HOST_ALLOCA =
    1 << 2; ///< ALLOCA auto-promovida a host stack ( D.jit-mem-model)
constexpr uint8_t INSTR_FLAG_HOST_ALLOCA_EXPLICIT_FREE =
    1 << 3; ///< Sprint mem-loop-fix: RAW_FREE preservado para liberar in-loop
constexpr uint8_t INSTR_FLAG_RET_IMPLICIT =
    1 << 4; ///< @Naked: RET sintetico de caida-al-final (no `return` explicito)
constexpr uint8_t INSTR_FLAG_SHARED_CTRL =
    1 << 5; ///< RAW_ALLOC del bloque de control de un `shared<T>`

// Bits del byte flags por IrFunction.
constexpr uint8_t FN_FLAG_NATIVE =
    1 << 0; ///< Funcion FFI (sin body IR; solo declaracion)
constexpr uint8_t FN_FLAG_VARIADIC =
    1 << 1; ///< Acepta nargs variable (R15 contiene el count)
constexpr uint8_t FN_FLAG_NAKED =
    1 << 2; ///<  NR @Naked: sin prologo/epilogo/ret (ISRs/stubs)
constexpr uint8_t FN_FLAG_NO_FP_CONTRACT =
    1 << 3; ///< @fp(strict)/-ffp-contract=off: fp_contract=false.  Bit NEGATIVO
            ///< (se pone cuando es false) -> caches viejas sin el bit -> true.
constexpr uint8_t FN_FLAG_PRIVATE =
    1 << 4; ///< Privada al modulo: nadie de fuera puede llamarla, asi que el
            ///< modulo tiene TODOS sus sitios de llamada a la vista.  Bit
            ///< NEGATIVO a proposito (se pone cuando NO es publica), igual que
            ///< el de contraccion: el default del lenguaje es publico, y asi un
            ///< IR viejo sin el bit se lee como publico, que es lo prudente --
            ///< de una publica no se afirma nada de sus llamantes.
/**
 * @brief `@Inline`: metela aunque no quepa por tamano.
 *
 * Tiene que viajar: un modulo que se RELEE de la cache en vez de
 * recompilarse tiene que tomar la misma decision, o el mismo programa se
 * optimiza distinto segun si la cache estaba caliente.  Bit POSITIVO -- lo
 * normal es no pedirlo --, asi que una cache anterior sin el se lee como
 * "no lo pidio", que es lo que era.
 */
constexpr uint8_t FN_FLAG_WANTS_INLINE = 1 << 5;

/**
 * @brief Serializa un @c IrValue al stream binario.
 *
 * Layout: @c [u8 type][u8 flags][u64 const_val if is_const].
 * El @c id NO se serializa porque se recupera del indice en
 * @c IrFunction::values[].  Total: 2 bytes para values normales,
 * 10 bytes para constantes (el @c const_val solo se emite si
 * @c is_const, ahorrando 8 bytes por value no-constante; en una
 * funcion tipica >70% de los values no son constantes, asi que
 * el ahorro acumulado es significativo).
 */
void write_value(util::ByteBuffer &o, const IrValue &v) {
    write_u8(o, static_cast<uint8_t>(v.type));
    // Compactar los 5 bools en un byte para minimizar el footprint
    // del IR serializado (los values son la mayoria del .velb IR).
    uint8_t flags = 0;
    if (v.is_param) flags |= IRVAL_FLAG_PARAM;
    if (v.is_const) flags |= IRVAL_FLAG_CONST;
    if (v.is_host_ptr()) flags |= IRVAL_FLAG_HOST_PTR;
    /* Y COMO se supo, en los dos bits altos.  Sin esto el viaje por la cache
     * colapsa la clase y todo vuelve como deducido. */
    flags |= static_cast<uint8_t>(kind_bits_of(v.memory) << IRVAL_KIND_SHIFT);
    if (v.pointee_is_host_ptr) flags |= IRVAL_FLAG_POINTEE_HOST_PTR;
    if (v.is_gc_object) flags |= IRVAL_FLAG_GC_OBJECT;
    if (v.reg != IR_NO_REG) flags |= IRVAL_FLAG_HAS_REG;
    write_u8(o, flags);
    // Solo paga el byte quien vive en un registro (ver IRVAL_FLAG_HAS_REG).
    if (v.reg != IR_NO_REG) write_u8(o, v.reg);
    // Optimizacion: const_val solo ocupa espacio si el value es
    // realmente una constante.  Una funcion tipica tiene ~30%
    // constantes, el resto son SSA values normales que no necesitan
    // const_val.  El deserializer respeta el flag.
    if (v.is_const) {
        write_u64(o, v.const_val);
    }
}

/**
 * @brief Deserializa un @c IrValue desde el stream binario.
 *
 * Inverso exacto de @c write_value.  Los @c read_u8 / @c read_u64
 * devuelven false si no hay suficientes bytes restantes; cualquier
 * fallo propaga inmediatamente.  Si el bit @c is_const no esta
 * activo, @c const_val queda en 0 (valor default del IrValue
 * recien construido).
 */
bool read_value(util::ByteCursor &c, IrValue &v) {
    uint8_t type_byte = 0, flags = 0;
    // Lecturas defensivas: si el buffer se acaba, abort temprano.
    if (!read_u8(c,type_byte)) return false;
    if (!read_u8(c,flags)) return false;
    v.type = static_cast<IrType>(type_byte);
    // Decodificacion explicita de cada bit a su bool correspondiente.
    // El compilador idealmente lo colapsa a operaciones bitwise + cmovs.
    v.is_param = (flags & IRVAL_FLAG_PARAM) != 0;
    v.is_const = (flags & IRVAL_FLAG_CONST) != 0;
    v.memory = memory_of_flags(flags);
    v.pointee_is_host_ptr = (flags & IRVAL_FLAG_POINTEE_HOST_PTR) != 0;
    v.is_gc_object = (flags & IRVAL_FLAG_GC_OBJECT) != 0;
    // v11: el registro solo viaja si el valor tenia uno.
    if (flags & IRVAL_FLAG_HAS_REG) {
        if (!read_u8(c,v.reg)) return false;
    } else {
        v.reg = IR_NO_REG;
    }
    // const_val solo se leyo si el writer la emitio; sin el flag
    // queda en su valor default (0).  Mismo principio que write_value.
    if (v.is_const) {
        if (!read_u64(c,v.const_val)) return false;
    }
    return true;
}

/**
 * @brief Serializa un @c IrInstr completo al stream binario.
 *
 * Tamano por instr: ~30 bytes minimo + 4 por operando + tamano
 * del func_name + 8 por phi_arg.  En IR tipico (~200 instrs por
 * funcion) -> ~10-15 KB por funcion serializada.
 *
 * Schema fijo: emitimos TODOS los campos aunque la op especifica
 * no los use.  Ejemplo: @c IrOp::ADD no usa @c target_block ni
 * @c phi_args pero igual los emitimos como 0 / vacio.  Esto
 * sacrifica espacio por simplificar el formato y permite que el
 * deserializer use un solo path sin tener que conocer el set de
 * campos por op.
 */
void write_instr(util::ByteBuffer &o, const IrInstr &i) {
    // op va en u16 porque el set de opcodes ya pasa los 200 (no
    // cabe en u8) y queremos espacio para crecer sin cambio de
    // formato.
    write_u16(o, static_cast<uint16_t>(i.op));
    write_u8(o, static_cast<uint8_t>(i.type));
    // dst es IrValueId (u32 en el storage).  IR_NO_VALUE (UINT32_MAX)
    // indica "esta op no produce valor" (e.g. STORE, RET void).
    write_u32(o, static_cast<uint32_t>(i.dst));
    uint8_t flags = 0;
    if (i.preserve) flags |= INSTR_FLAG_PRESERVE;
    if (i.is_call_site) flags |= INSTR_FLAG_IS_CALL_SITE;
    if (i.ret_implicit) flags |= INSTR_FLAG_RET_IMPLICIT;
    if (i.host_alloca) flags |= INSTR_FLAG_HOST_ALLOCA;
    if (i.host_alloca_explicit_free)
        flags |= INSTR_FLAG_HOST_ALLOCA_EXPLICIT_FREE;
    if (i.is_shared_ctrl) flags |= INSTR_FLAG_SHARED_CTRL;
    write_u8(o, flags);
    // source_line: util para diagnosticos y stack traces.  0 si
    // el frontend no aporto info de linea.
    write_u32(o, i.source_line);
    // source_column: sin ella no se puede senalar CUAL de las cosas que caben
    // en una linea fallo, solo en cual.  Viaja desde v8.
    write_u32(o, i.source_column);
    // source_len: donde ACABA el trozo de fuente.  Con la columna sola se
    // sabe donde empieza, y sin el final no se puede recortar el texto para
    // nombrar un operando.  Viaja desde v9.
    write_u32(o, i.source_len);
    // inline_site: de que llamada aplanada vino la instruccion (v10).
    write_u32(o, i.inline_site);
    // imm: campo polivalente.  Para CONST contiene el valor; para
    // CALL contiene flags/args adicionales; para ops sin imm es 0.
    write_u64(o, i.imm);
    // operands: lista de IrValueId.  Maximo 255 por op (suficiente
    // para cualquier op concebible; CALL con mas args es raro pero
    // tampoco esperamos > 255 args reales en codigo Vesta normal).
    const size_t opc = i.operands.size();
    write_u8(o, opc > 255 ? 255 : static_cast<uint8_t>(opc));
    for (size_t k = 0; k < opc && k < 255; ++k) {
        write_u32(o, static_cast<uint32_t>(i.operands[k]));
    }
    // func_name: solo significativo para CALL/CALLN/CALLIND.
    // Para otras ops queda como cadena vacia (write_str emite
    // un u32=0 length sin payload).
    write_str(o, i.func_name);
    // func_ptr (puntero a funcion para CALLIND/CALLCLOSURE) +
    // target_block y false_block (para BR/BR_COND).  Cero si la
    // op no los usa.
    write_u32(o, static_cast<uint32_t>(i.func_ptr));
    write_u32(o, static_cast<uint32_t>(i.target_block));
    write_u32(o, static_cast<uint32_t>(i.false_block));
    // phi_args: lista de (block, value) para PHI nodes.  Vacio
    // para ops no-PHI.  Mismo limite de 255 por simetria con
    // operands.
    const size_t pc = i.phi_args.size();
    write_u8(o, pc > 255 ? 255 : static_cast<uint8_t>(pc));
    for (size_t k = 0; k < pc && k < 255; ++k) {
        write_u32(o, static_cast<uint32_t>(i.phi_args[k].value));
        write_u32(o, static_cast<uint32_t>(i.phi_args[k].block));
    }
    // jump_targets: tabla de bloques del SWITCH_DENSE (jump table denso).
    // Count u32 (un switch denso puede tener >255 entradas).  Vacio en el
    // resto de ops.  (Formato v6/v8.)
    const size_t jtc = i.jump_targets.size();
    write_u32(o, static_cast<uint32_t>(jtc));
    for (size_t k = 0; k < jtc; ++k)
        write_u32(o, i.jump_targets[k]);
    // call_abi_regs: ABI custom del CALLIND (registro por arg desde el tipo del
    // puntero).  Count 0 = ABI estandar (todas las ops no-CALLIND).  (Formato
    // v7.)
    const size_t abc = i.call_abi_regs.size();
    write_u32(o, static_cast<uint32_t>(abc));
    for (size_t k = 0; k < abc; ++k)
        write_str(o, i.call_abi_regs[k]);
    // proven_callee: destino DEMOSTRADO de un despacho dinamico; vacio = no se
    // sabe.  (Formato v20.)
    write_str(o, i.proven_callee.str());
}

/**
 * @brief Deserializa un @c IrInstr completo desde el stream.
 *
 * Inverso exacto de @c write_instr.  Cualquier @c read_* que
 * falle (buffer truncado) propaga inmediatamente con false.
 *
 * Garantia post-condicion: si retorna true, todos los campos de
 * @c i estan poblados con valores consistentes.  Los vectors
 * (@c operands, @c phi_args) fueron clear+reserve+push para
 * evitar fragmentacion del heap incluso en programas con muchas
 * funciones.
 */
bool read_instr(util::ByteCursor &c, IrInstr &i) {
    uint16_t op_v = 0;
    uint8_t type_v = 0, flags = 0;
    uint32_t dst_v = 0, source_line = 0;
    uint64_t imm = 0;
    // Lecturas en exactamente el mismo orden que el writer; los
    // fallos cortan toda la operacion (NO intentamos recovery
    // parcial: una IR malformada no es util incluso si recuperamos
    // la primera mitad).
    if (!read_u16(c,op_v)) return false;
    if (!read_u8(c,type_v)) return false;
    if (!read_u32(c,dst_v)) return false;
    if (!read_u8(c,flags)) return false;
    if (!read_u32(c,source_line)) return false;
    uint32_t source_column = 0;
    if (!read_u32(c,source_column)) return false;
    uint32_t source_len = 0;
    if (!read_u32(c,source_len)) return false;
    uint32_t inline_site = IR_NO_INLINE_SITE;
    if (!read_u32(c,inline_site)) return false;
    if (!read_u64(c,imm)) return false;
    i.op = static_cast<IrOp>(op_v);
    i.type = static_cast<IrType>(type_v);
    i.dst = static_cast<IrValueId>(dst_v);
    i.preserve = (flags & INSTR_FLAG_PRESERVE) != 0;
    i.is_call_site = (flags & INSTR_FLAG_IS_CALL_SITE) != 0;
    i.ret_implicit = (flags & INSTR_FLAG_RET_IMPLICIT) != 0;
    i.host_alloca = (flags & INSTR_FLAG_HOST_ALLOCA) != 0;
    i.host_alloca_explicit_free =
        (flags & INSTR_FLAG_HOST_ALLOCA_EXPLICIT_FREE) != 0;
    i.is_shared_ctrl = (flags & INSTR_FLAG_SHARED_CTRL) != 0;
    i.source_line = source_line;
    i.source_column = source_column;
    i.source_len = source_len;
    i.inline_site = inline_site;
    i.imm = imm;
    /* operands */
    uint8_t opc = 0;
    if (!read_u8(c,opc)) return false;
    i.operands.clear();
    i.operands.reserve(opc);
    for (uint8_t k = 0; k < opc; ++k) {
        uint32_t v = 0;
        if (!read_u32(c,v)) return false;
        i.operands.push_back(static_cast<IrValueId>(v));
    }
    /* func_name */
    if (!read_str(c,i.func_name)) return false;
    /* func_ptr / target_block / false_block */
    uint32_t fp = 0, tb = 0, fb = 0;
    if (!read_u32(c,fp)) return false;
    if (!read_u32(c,tb)) return false;
    if (!read_u32(c,fb)) return false;
    i.func_ptr = static_cast<IrValueId>(fp);
    i.target_block = static_cast<IrBlockId>(tb);
    i.false_block = static_cast<IrBlockId>(fb);
    /* phi_args */
    uint8_t pc = 0;
    if (!read_u8(c,pc)) return false;
    i.phi_args.clear();
    i.phi_args.reserve(pc);
    for (uint8_t k = 0; k < pc; ++k) {
        uint32_t v = 0, b = 0;
        if (!read_u32(c,v)) return false;
        if (!read_u32(c,b)) return false;
        IrPhiArg a{static_cast<IrValueId>(v), static_cast<IrBlockId>(b)};
        i.phi_args.push_back(a);
    }
    /* jump_targets (SWITCH_DENSE) -- formato v6/v8. */
    uint32_t jtc = 0;
    if (!read_u32(c,jtc)) return false;
    i.jump_targets.clear();
    i.jump_targets.reserve(jtc);
    for (uint32_t k = 0; k < jtc; ++k) {
        uint32_t t = 0;
        if (!read_u32(c,t)) return false;
        i.jump_targets.push_back(ir::IrBlockId(t));
    }
    /* call_abi_regs (ABI custom del CALLIND) -- formato v7. */
    uint32_t abc = 0;
    if (!read_u32(c,abc)) return false;
    i.call_abi_regs.clear();
    i.call_abi_regs.reserve(abc);
    for (uint32_t k = 0; k < abc; ++k) {
        std::string r;
        if (!read_str(c,r)) return false;
        i.call_abi_regs.push_back(std::move(r));
    }
    /* proven_callee -- formato v20. */
    std::string proven;
    if (!read_str(c, proven)) return false;
    i.proven_callee = proven.empty() ? util::InternedName()
                                     : util::InternedName::intern(proven);
    return true;
}

void write_block(util::ByteBuffer &o, const IrBlock &b) {
    write_str(o, b.name);
    write_u32(o, static_cast<uint32_t>(b.instrs.size()));
    for (const auto &i : b.instrs)
        write_instr(o, i);
    write_u32(o, static_cast<uint32_t>(b.preds.size()));
    for (auto p : b.preds)
        write_u32(o, static_cast<uint32_t>(p));
    write_u32(o, static_cast<uint32_t>(b.succs.size()));
    for (auto s : b.succs)
        write_u32(o, static_cast<uint32_t>(s));
}

bool read_block(util::ByteCursor &c, IrBlock &b) {
    if (!read_str(c,b.name)) return false;
    uint32_t n_instrs = 0;
    if (!read_u32(c,n_instrs)) return false;
    b.instrs.clear();
    b.instrs.reserve(n_instrs);
    for (uint32_t k = 0; k < n_instrs; ++k) {
        IrInstr i;
        if (!read_instr(c, i)) return false;
        b.instrs.push_back(std::move(i));
    }
    uint32_t n_preds = 0;
    if (!read_u32(c,n_preds)) return false;
    b.preds.clear();
    b.preds.reserve(n_preds);
    for (uint32_t k = 0; k < n_preds; ++k) {
        uint32_t v = 0;
        if (!read_u32(c,v)) return false;
        b.preds.push_back(static_cast<IrBlockId>(v));
    }
    uint32_t n_succs = 0;
    if (!read_u32(c,n_succs)) return false;
    b.succs.clear();
    b.succs.reserve(n_succs);
    for (uint32_t k = 0; k < n_succs; ++k) {
        uint32_t v = 0;
        if (!read_u32(c,v)) return false;
        b.succs.push_back(static_cast<IrBlockId>(v));
    }
    return true;
}

} // namespace

/* ===================================================================== */
/* serialize_function / deserialize_function                              */
/* ===================================================================== */

/**
 * @brief Serializa una @c IrFunction completa.
 *
 * @return Numero de bytes que se escribieron en @c out (la posicion
 *         final menos la inicial).  Util si el caller quiere saber
 *         el tamano de esta funcion concreta sin recalcular.
 *
 * Orden de campos en el stream:
 *   1. name (string)
 *   2. ret_type (u8) + fn_flags (u8: native, variadic)
 *   3. params: count u32 + IrValueId[count]
 *   4. values: count u32 + IrValue[count]  (cada uno con su layout)
 *   5. blocks: count u32 + IrBlock[count]
 *   6. metadata de generics: template_name + n type_args + type_args[]
 */
size_t serialize_function(const IrFunction &fn, util::ByteBuffer &out) {
    const size_t start = out.size;

    write_str(out, fn.name);
    write_u8(out, static_cast<uint8_t>(fn.ret_type));
    // Empaquetar los 2 bools de la funcion en un solo byte (mismo
    // patron que en values).
    uint8_t fn_flags = 0;
    if (fn.is_native) fn_flags |= FN_FLAG_NATIVE;
    if (fn.is_variadic) fn_flags |= FN_FLAG_VARIADIC;
    if (fn.is_naked) fn_flags |= FN_FLAG_NAKED;
    if (fn.wants_inline) fn_flags |= FN_FLAG_WANTS_INLINE;
    if (!fn.fp_contract) fn_flags |= FN_FLAG_NO_FP_CONTRACT;
    if (!fn.is_public) fn_flags |= FN_FLAG_PRIVATE;
    write_u8(out, fn_flags);

    // Params: lista de IrValueId que apuntan a entries en values[]
    // que tienen is_param=true.  Duplicacion intencional para que el
    // lookup "i-esimo parametro" sea O(1) sin filtrar values[].
    write_u32(out, static_cast<uint32_t>(fn.params.size()));
    for (auto p : fn.params)
        write_u32(out, static_cast<uint32_t>(p));

    // ABI custom por funcion (register("rXX") en params): registro fisico de
    // entrada por parametro, alineado con params[].  Count 0 = ABI estandar (el
    // caso comun; no ocupa mas que el u32 del count).
    write_u32(out, static_cast<uint32_t>(fn.param_abi_regs.size()));
    for (const auto &r : fn.param_abi_regs)
        write_str(out, r);

    /* El contrato de cada parametro.  Viaja porque el modelo de memoria lo
     * consulta: sin el, una funcion que llega de otro modulo pierde sus
     * promesas y sus parametros vuelven a lo conservador.  Seria correcto, pero
     * haria que el codigo dependiera de si el modulo se compilo junto o aparte
     * -- justo la clase de diferencia que no debe existir.
     *
     * Cada registro lleva su TAMANO delante.  Anadir una promesa manana sube la
     * version igual, pero con el tamano el cuerpo se puede SALTAR sin
     * conocerlo: un lector lee lo que entiende y descarta el resto, en vez de
     * descuadrarse y leer el campo siguiente como si fuera otra cosa. */
    write_u32(out, static_cast<uint32_t>(fn.param_contracts.size()));
    for (const IrParamContract &c : fn.param_contracts) {
        /* Cuantos NIVELES lleva este parametro.  Van dentro porque el contrato
         * es por nivel: `const T*` habla de lo apuntado y `T* const` del
         * puntero, y son cosas distintas. */
        write_u32(out, static_cast<uint32_t>(c.levels.size()));
        for (const IrParamLevel &l : c.levels) {
            write_u32(out, 48u); // bytes del cuerpo: 8*5 + 4 + 4
            write_u64(out, l.holds);
            write_u64(out, l.denied);
            write_u64(out, l.proven);
            write_u64(out, l.declared);
            write_u64(out, static_cast<uint64_t>(l.extent_bytes));
            write_u32(out, l.extent_from_param);
            write_u32(out, l.align_bytes);
        }
    }

    // Values: TODOS los SSA values de la funcion.  El indice en este
    // array ES el IrValueId (los ids son densos 0..N-1).
    write_u32(out, static_cast<uint32_t>(fn.values.size()));
    for (const auto &v : fn.values)
        write_value(out, v);

    // Blocks: cada uno incluye sus propias instrs + preds + succs.
    // El indice en este array ES el IrBlockId.
    write_u32(out, static_cast<uint32_t>(fn.blocks.size()));
    for (const auto &b : fn.blocks)
        write_block(out, b);

    // Metadata para generics monomorphizados: el frontend Vesta setea
    // template_name (e.g. "List") y type_args (e.g. ["i32"]) al
    // monomorphizar @c List<i32>.  Util para tools que quieran
    // mostrar el template original en stack traces; el JIT no lo usa.
    write_str(out, fn.generic_template_name);
    write_u32(out, static_cast<uint32_t>(fn.generic_type_args.size()));
    for (const auto &s : fn.generic_type_args)
        write_str(out, s);

    //  AS inc.5: bindings register() + clobber-lists del inline-asm.
    // Necesarios para que el JIT (que compila desde el @ir del .velb)
    // reconstruya el pin de registros del INLINE_ASM.  La mayoria de
    // funciones tienen ambos vacios (8 bytes: dos counts a 0).
    write_u32(out, static_cast<uint32_t>(fn.asm_reg_bindings.size()));
    for (const auto &b : fn.asm_reg_bindings) {
        write_u32(out, static_cast<uint32_t>(b.alloca_value));
        write_str(out, b.reg);
        write_u8(out, static_cast<uint8_t>(b.type));
        write_u8(out, b.is_vector ? 1u : 0u);
        write_str(out, b.name);
        // operando `reg` auto (RA elige el fisico) + su placeholder $N.
        write_u8(out, b.reg_auto ? 1u : 0u);
        write_u32(out, static_cast<uint32_t>(b.ph_index));
        /* La clase declarada (v13).  Tiene que cruzar esta frontera: el AOT y
         * el JIT compilan desde el IR serializado, no desde el de memoria, y
         * sin la clase el ancho del operando se pierde justo donde hace falta
         * comprobarlo. */
        write_str(out, b.reg_class);
    }
    write_u32(out, static_cast<uint32_t>(fn.asm_clobber_lists.size()));
    for (const auto &lst : fn.asm_clobber_lists) {
        write_u32(out, static_cast<uint32_t>(lst.size()));
        for (const auto &s : lst)
            write_str(out, s);
    }
    /* Los prestamos ya NO viajan aparte: son instrucciones `borrow` del cuerpo,
     * asi que cruzan esta frontera con el resto del codigo y sin poder quedarse
     * atras.  Antes iban en una tabla propia cuyos ids de valor no mantenia
     * ningun pase. */
    // ASA: instrucciones ASM_MICRO (asm opaco liftado).  El @c imm de cada
    // IrInstr ASM_MICRO indexa esta tabla; necesaria para que JIT/AOT (que
    // compilan desde el @ir del .velb/.vxir) reconstruyan la plantilla + los
    // efectos.  Casi siempre vacio (4 bytes: un count a 0).
    write_u32(out, static_cast<uint32_t>(fn.asm_micros.size()));
    for (const auto &am : fn.asm_micros) {
        write_u8(out, am.isa);
        write_u32(out, am.form_id);
        write_str(out, am.tmpl);
        write_u8(out, am.eff);
        // Lista PLANA de operandos (orden textual; roles en flags).
        write_u32(out, static_cast<uint32_t>(am.operands.size()));
        for (const auto &op : am.operands) {
            write_u8(out, static_cast<uint8_t>(op.kind));
            write_u8(out, op.flags);
            write_u8(out, op.regclass);
            write_u32(out, static_cast<uint32_t>(op.width));
            write_u32(out, static_cast<uint32_t>(
                               static_cast<uint16_t>(op.fixed_phys)));
            write_u32(out, static_cast<uint32_t>(op.value));
            write_u64(out, static_cast<uint64_t>(op.imm));
        }
    }

    // AOT 2b: seccion de salida del codigo + permisos (dev OS).
    write_str(out, fn.section);
    write_str(out, fn.section_perms);
    // AOT: ubicacion fija (@at) + orden (@order) de la seccion.
    write_u64(out, (uint64_t)fn.section_at);
    write_u32(out, (uint32_t)fn.section_order);

    // Subsistema de coste (--analyze): contrato @complexity.  Metadata pura;
    // viaja en el cache para que el modo --analyze (que reusa el .vxir /
    // ir_module_cache_bytes) pueda comparar contra la complejidad inferida.
    write_str(out, fn.complexity_expr);
    write_u32(out, static_cast<uint32_t>(fn.complexity_vars.size()));
    for (const auto &s : fn.complexity_vars)
        write_str(out, s);
    // v5/v6: contratos @complexity por dimension (PARCIAL/TOTAL x PRE/POST).
    write_str(out, fn.complexity_partial_pre);
    write_str(out, fn.complexity_partial_post);
    write_str(out, fn.complexity_total_pre);
    write_str(out, fn.complexity_total_post);

    // v10: llamadas que se aplanaron aqui al inlinar.  Sin ellas, el codigo
    // que vino de otra funcion conserva SUS lineas pero se atribuye a esta, y
    // la traza senala un sitio que no es.  Casi siempre vacio (4 bytes).
    write_u32(out, static_cast<uint32_t>(fn.inline_sites.size()));
    for (const auto &s : fn.inline_sites) {
        write_str(out, s.callee);
        write_u32(out, s.line);
        write_u32(out, s.column);
        write_u32(out, s.parent);
        // v19: y de que FICHERO vino ese trozo.  Va aqui y no se busca luego
        // por `callee` porque el camino nativo borra las funciones que se
        // quedan sin usos DESPUES de inlinar.
        write_u32(out, s.source_file);
    }
    // v19: el fichero de la propia funcion, como indice en la tabla del
    // modulo.  Por funcion y no por instruccion: son unos pocos ficheros
    // frente a millones de instrucciones.
    write_u32(out, fn.source_file);

    return out.size - start;
}

bool deserialize_function(util::ByteCursor &c, IrFunction &out) {
    out = IrFunction{};

    if (!read_str(c,out.name)) return false;
    uint8_t ret_type_b = 0, fn_flags = 0;
    if (!read_u8(c,ret_type_b)) return false;
    if (!read_u8(c,fn_flags)) return false;
    out.ret_type = static_cast<IrType>(ret_type_b);
    out.is_native = (fn_flags & FN_FLAG_NATIVE) != 0;
    out.is_variadic = (fn_flags & FN_FLAG_VARIADIC) != 0;
    out.is_naked = (fn_flags & FN_FLAG_NAKED) != 0;
    out.wants_inline = (fn_flags & FN_FLAG_WANTS_INLINE) != 0;
    out.fp_contract = (fn_flags & FN_FLAG_NO_FP_CONTRACT) == 0;
    out.is_public = (fn_flags & FN_FLAG_PRIVATE) == 0;

    /* params */
    uint32_t n_params = 0;
    if (!read_u32(c,n_params)) return false;
    out.params.clear();
    out.params.reserve(n_params);
    for (uint32_t k = 0; k < n_params; ++k) {
        uint32_t v = 0;
        if (!read_u32(c,v)) return false;
        out.params.push_back(static_cast<IrValueId>(v));
    }

    /* ABI custom por funcion (register en params): registro por parametro. */
    uint32_t n_abi = 0;
    if (!read_u32(c,n_abi)) return false;
    out.param_abi_regs.clear();
    out.param_abi_regs.reserve(n_abi);
    for (uint32_t k = 0; k < n_abi; ++k) {
        std::string r;
        if (!read_str(c,r)) return false;
        out.param_abi_regs.push_back(std::move(r));
    }

    /* Direccion declarada de cada parametro (ver el lado que escribe). */
    /* El contrato de cada parametro (ver el lado que escribe).  El TAMANO de
     * cada registro se lee y se respeta: lo que este lector no entienda se
     * salta, en vez de descuadrar el resto. */
    uint32_t n_contracts = 0;
    if (!read_u32(c,n_contracts)) return false;
    out.param_contracts.clear();
    out.param_contracts.reserve(n_contracts);
    for (uint32_t k = 0; k < n_contracts; ++k) {
        uint32_t n_levels = 0;
        if (!read_u32(c,n_levels)) return false;
        if (n_levels > IrParamContract::kMaxLevels) return false;
        IrParamContract pc;
        pc.levels.reserve(n_levels);
        for (uint32_t li = 0; li < n_levels; ++li) {
            uint32_t body = 0;
            if (!read_u32(c, body)) return false;
            /* Cada nivel dice cuanto ocupa, asi que uno de una version mas
             * nueva -- con campos que aqui no se conocen -- se SALTA en vez de
             * descuadrar todo lo que viene detras. */
            const size_t fin = c.off + body;
            if (fin > c.buf->size) return false;
            IrParamLevel l;
            uint64_t ext = 0;
            if (!read_u64(c, l.holds)) return false;
            if (!read_u64(c, l.denied)) return false;
            if (!read_u64(c, l.proven)) return false;
            if (!read_u64(c, l.declared)) return false;
            if (!read_u64(c, ext)) return false;
            if (!read_u32(c, l.extent_from_param)) return false;
            if (!read_u32(c, l.align_bytes)) return false;
            l.extent_bytes = static_cast<int64_t>(ext);
            c.off = fin; // lo que sobre es de una version que no se conoce
            pc.levels.push_back(l);
        }
        out.param_contracts.push_back(std::move(pc));
    }

    /* values */
    uint32_t n_values = 0;
    if (!read_u32(c,n_values)) return false;
    out.values.clear();
    out.values.reserve(n_values);
    for (uint32_t k = 0; k < n_values; ++k) {
        IrValue v;
        v.id = static_cast<IrValueId>(k); /* id es el indice en el vector */
        if (!read_value(c, v)) return false;
        out.values.push_back(std::move(v));
    }

    /* blocks */
    uint32_t n_blocks = 0;
    if (!read_u32(c,n_blocks)) return false;
    out.blocks.clear();
    out.blocks.reserve(n_blocks);
    for (uint32_t k = 0; k < n_blocks; ++k) {
        IrBlock b;
        b.id = static_cast<IrBlockId>(k); /* id es el indice */
        if (!read_block(c, b)) return false;
        out.blocks.push_back(std::move(b));
    }

    /* metadata */
    if (!read_str(c,out.generic_template_name)) return false;
    uint32_t n_args = 0;
    if (!read_u32(c,n_args)) return false;
    out.generic_type_args.clear();
    out.generic_type_args.reserve(n_args);
    for (uint32_t k = 0; k < n_args; ++k) {
        std::string s;
        if (!read_str(c,s)) return false;
        out.generic_type_args.push_back(std::move(s));
    }

    /*  AS inc.5: bindings register() + clobber-lists del inline-asm. */
    uint32_t n_bind = 0;
    if (!read_u32(c,n_bind)) return false;
    out.asm_reg_bindings.clear();
    out.asm_reg_bindings.reserve(n_bind);
    for (uint32_t k = 0; k < n_bind; ++k) {
        AsmRegBinding b;
        uint32_t av = 0;
        uint8_t ty = 0, vec = 0;
        if (!read_u32(c,av)) return false;
        b.alloca_value = static_cast<IrValueId>(av);
        if (!read_str(c,b.reg)) return false;
        if (!read_u8(c,ty)) return false;
        b.type = static_cast<IrType>(ty);
        if (!read_u8(c,vec)) return false;
        b.is_vector = (vec != 0);
        if (!read_str(c,b.name)) return false;
        // reg_auto + ph_index.
        uint8_t ra_auto = 0;
        uint32_t phi = 0;
        if (!read_u8(c,ra_auto)) return false;
        if (!read_u32(c,phi)) return false;
        b.reg_auto = (ra_auto != 0);
        b.ph_index = static_cast<int>(static_cast<int32_t>(phi));
        // Clase declarada (v13): ver la nota del emisor.
        if (!read_str(c,b.reg_class)) return false;
        out.asm_reg_bindings.push_back(std::move(b));
    }
    uint32_t n_clob = 0;
    if (!read_u32(c,n_clob)) return false;
    out.asm_clobber_lists.clear();
    out.asm_clobber_lists.reserve(n_clob);
    for (uint32_t k = 0; k < n_clob; ++k) {
        uint32_t n_s = 0;
        if (!read_u32(c,n_s)) return false;
        std::vector<std::string> lst;
        lst.reserve(n_s);
        for (uint32_t j = 0; j < n_s; ++j) {
            std::string s;
            if (!read_str(c,s)) return false;
            lst.push_back(std::move(s));
        }
        out.asm_clobber_lists.push_back(std::move(lst));
    }
    /* (los prestamos ya no van aparte: ver la nota del emisor) */
    // ASA: instrucciones ASM_MICRO (asm opaco liftado).
    uint32_t n_micro = 0;
    if (!read_u32(c,n_micro)) return false;
    out.asm_micros.clear();
    out.asm_micros.reserve(n_micro);
    for (uint32_t k = 0; k < n_micro; ++k) {
        AsmMicro am;
        uint8_t isa_u = 0, eff_u = 0;
        if (!read_u8(c,isa_u)) return false;
        am.isa = isa_u;
        if (!read_u32(c,am.form_id)) return false;
        if (!read_str(c,am.tmpl)) return false;
        if (!read_u8(c,eff_u)) return false;
        am.eff = eff_u;
        uint32_t n_ops = 0;
        if (!read_u32(c,n_ops)) return false;
        am.operands.reserve(n_ops);
        for (uint32_t j = 0; j < n_ops; ++j) {
            AsmMicroOperand op;
            uint8_t kind = 0, flags = 0, rc = 0;
            uint32_t wid = 0, fx = 0, val = 0;
            uint64_t imm = 0;
            if (!read_u8(c,kind)) return false;
            if (!read_u8(c,flags)) return false;
            if (!read_u8(c,rc)) return false;
            if (!read_u32(c,wid)) return false;
            if (!read_u32(c,fx)) return false;
            if (!read_u32(c,val)) return false;
            if (!read_u64(c,imm)) return false;
            op.kind = static_cast<AsmOperandKind>(kind);
            op.flags = flags;
            op.regclass = rc;
            op.width = static_cast<uint16_t>(wid);
            op.fixed_phys = static_cast<int16_t>(static_cast<uint16_t>(fx));
            op.value = static_cast<IrValueId>(val);
            op.imm = static_cast<int64_t>(imm);
            am.operands.push_back(op);
        }
        out.asm_micros.push_back(std::move(am));
    }
    // AOT 2b: seccion de salida del codigo + permisos.
    if (!read_str(c,out.section)) return false;
    if (!read_str(c,out.section_perms)) return false;
    // AOT: ubicacion fija (@at) + orden (@order).
    uint64_t at_u = 0;
    uint32_t ord_u = 0;
    if (!read_u64(c,at_u)) return false;
    if (!read_u32(c,ord_u)) return false;
    out.section_at = (int64_t)at_u;
    out.section_order = (int32_t)ord_u;
    // Subsistema de coste (--analyze): contrato @complexity.
    if (!read_str(c,out.complexity_expr)) return false;
    uint32_t cvn = 0;
    if (!read_u32(c,cvn)) return false;
    out.complexity_vars.clear();
    out.complexity_vars.reserve(cvn);
    for (uint32_t k = 0; k < cvn; ++k) {
        std::string s;
        if (!read_str(c,s)) return false;
        out.complexity_vars.push_back(std::move(s));
    }
    // v5/v6: contratos @complexity por dimension (PARCIAL/TOTAL x PRE/POST).
    if (!read_str(c,out.complexity_partial_pre)) return false;
    if (!read_str(c,out.complexity_partial_post)) return false;
    if (!read_str(c,out.complexity_total_pre)) return false;
    if (!read_str(c,out.complexity_total_post)) return false;
    // v10: llamadas aplanadas al inlinar (ver el lado de escritura).
    uint32_t n_sites = 0;
    if (!read_u32(c,n_sites)) return false;
    out.inline_sites.clear();
    out.inline_sites.reserve(n_sites);
    for (uint32_t k = 0; k < n_sites; ++k) {
        InlineSite s;
        if (!read_str(c,s.callee)) return false;
        if (!read_u32(c,s.line)) return false;
        if (!read_u32(c,s.column)) return false;
        if (!read_u32(c,s.parent)) return false;
        if (!read_u32(c,s.source_file)) return false; // v19
        out.inline_sites.push_back(std::move(s));
    }
    if (!read_u32(c,out.source_file)) return false; // v19
    return true;
}

/* ===================================================================== */
/* emit_ir_section / parse_ir_section                                     */
/* ===================================================================== */

/**
 * @brief Emite la seccion @c @ir completa para el .velb.
 *
 * Layout de la seccion:
 *   - Header (12 bytes):
 *     - u32 magic (@c VEIR)
 *     - u16 version
 *     - u16 reserved (0 para alineamiento, futuras flags)
 *     - u32 fn_count
 *   - fn_count funciones concatenadas (cada una serializada por
 *     @c serialize_function).
 *
 * El caller (linker .velb) toma el resultado y lo escribe en una
 * seccion dedicada del binario.  El loader, al detectar la seccion,
 * llama a @c parse_ir_section para reconstruir el vector de IrFunctions.
 */
void emit_ir_section(
    const std::vector<IrFunction> &functions,
    const util::SmallVector<const std::string *, 4> &source_files,
    util::ByteBuffer &out) {
    util::byte_buffer_init(out, &kIrSectionKind);
    // Primero el cuerpo en claro: las funciones concatenadas.  No hay padding
    // entre ellas; el parser avanza con el tamano de cada una.
    util::ByteBuffer cuerpo;
    util::byte_buffer_init(cuerpo, &kIrSectionKind);
    util::byte_buffer_reserve(cuerpo, functions.size() * 512);
    /* v19: la tabla de ficheros abre el cuerpo.  Delante de las funciones
     * porque es contra ella contra la que se leen sus indices: quien parsea la
     * tiene ya montada cuando le llega la primera. */
    write_u32(cuerpo, static_cast<uint32_t>(source_files.size()));
    for (const std::string *f : source_files) write_str(cuerpo, *f);
    for (const auto &fn : functions) {
        (void)serialize_function(fn, cuerpo); // ignoramos el retorno aqui
    }

    /* Y se COMPRIME.  Esta seccion es, con diferencia, la parte mas grande del
     * artefacto: medido en un proyecto de 6k lineas, 5,75 MiB de los 7,56 --
     * el 76% --, y el fichero entero se reescribe en cada reconstruccion.
     *
     * Comprime muchisimo porque es una serializacion muy repetitiva.  Sobre esa
     * seccion real, con miniz: nivel 3 da 13,8x en 22,9 ms, y el 6 da 15,2x en
     * 56,8.  Se usa el 3 porque la diferencia de tamano son 40 KiB sobre 7,5
     * MiB -- nada -- y el tiempo es menos de la mitad.
     *
     * El reparto del coste cae donde debe: comprimir se paga UNA vez, al
     * producir el artefacto (~23 ms sobre los ~1,9 s de una compilacion en
     * frio), mientras que el ahorro se cobra cada vez que el artefacto se
     * escribe, se lee o se guarda en un cache.  Descomprimir (~4 ms) solo lo
     * paga quien de verdad use el intermedio: el JIT y el AOT. */
    util::byte_buffer_reserve(out, 64 + cuerpo.size / 8);
    write_u32(out, IR_SECTION_MAGIC);
    write_u16(out, IR_SECTION_VERSION);

    mz_ulong tope = mz_compressBound(static_cast<mz_ulong>(cuerpo.size));
    util::ByteBuffer comprimido;
    util::byte_buffer_init(comprimido, &kIrSectionKind);
    util::byte_buffer_reserve(comprimido, tope);
    mz_ulong final_len = tope;
    const bool ok = cuerpo.size != 0 && comprimido.data != nullptr &&
                    mz_compress2(comprimido.data, &final_len, cuerpo.data,
                                 static_cast<mz_ulong>(cuerpo.size),
                                 kIrCompressionLevel) == MZ_OK &&
                    final_len < cuerpo.size;

    if (ok) {
        write_u16(out, kIrFlagDeflate);
        write_u32(out, static_cast<uint32_t>(functions.size()));
        // Cuanto ocupa en claro: quien lo lea necesita saber cuanto reservar
        // antes de descomprimir, y fiarse de lo que diga el propio flujo seria
        // fiarse de un fichero que puede estar corrupto.
        write_u32(out, static_cast<uint32_t>(cuerpo.size));
        util::byte_buffer_append(out, comprimido.data,
                                 static_cast<size_t>(final_len));
    } else {
        /* Sin comprimir, y no es un caso de error: una seccion vacia, o una que
         * no encoge, se guarda tal cual.  Que el formato admita las dos formas
         * evita tener que decidir aqui si "no comprimio" es un fallo. */
        write_u16(out, 0);
        write_u32(out, static_cast<uint32_t>(functions.size()));
        util::byte_buffer_append(out, cuerpo.data, cuerpo.size);
    }
    util::byte_buffer_release(comprimido);
    util::byte_buffer_release(cuerpo);
}

/**
 * @brief Parsea la seccion @c @ir desde un buffer (tipicamente el
 *        mmap del .velb).
 *
 * @param data         Buffer completo del .velb (lectura solo).
 * @param offset       Offset dentro de @c data donde empieza @c VEIR.
 * @param section_size Tamano de la seccion (segun el header del .velb).
 * @param functions    Vector que rellenamos.  Se @c clear primero, asi
 *                     llamar repetidamente sobre el mismo vector funciona.
 * @return @c true si el parseo fue exitoso; @c false en cualquier error
 *         (magic incorrecto, version no soportada, buffer truncado,
 *         fn_count sospechosamente grande).
 *
 * Politica de seguridad: este parser puede leer bytes que vienen de
 * un .velb potencialmente corrupto o malicioso.  Por eso validamos:
 *   - Magic correcto (VEIR).
 *   - Version exacta (rechazamos versiones futuras desconocidas; un
 *     v2 podria tener layout incompatible).
 *   - fn_count <= 100000 (hard cap defensivo).
 *   - Cada deserializacion individual no excede @c section_end.
 */
/**
 * @brief Apunta el motivo del rechazo y devuelve @c false.
 *
 * Existe para que cada salida de @c parse_ir_section diga POR QUE se va, sin
 * repetir tres lineas en cada una -- y sobre todo sin que ninguna pueda
 * marcharse callando, que es lo que hacian todas.
 *
 * @param report Donde anotarlo, o nulo si a quien llama no le interesa.
 * @param why El motivo.
 * @return Siempre @c false, para poder escribirlo en el propio @c return.
 */
static bool note_reject(IrSectionReport *report, IrSectionReject why) {
    if (report != nullptr) report->reject = why;
    return false;
}

bool parse_ir_section(const uint8_t *data, size_t section_size,
                      std::vector<IrFunction> &functions,
                      IrSectionReport *report,
                      util::SmallVector<const std::string *, 4> *source_files) {
    functions.clear();
    if (source_files != nullptr) source_files->clear();
    if (report != nullptr) *report = IrSectionReport{};

    if (data == nullptr)
        return note_reject(report, IrSectionReject::Truncated);
    // Validar el tamano minimo: al menos el header completo de 12 bytes
    // (magic 4 + version 2 + reserved 2 + fn_count 4).
    if (section_size < 12)
        return note_reject(report, IrSectionReject::Truncated);

    /* Los bytes son de quien llama -- un tramo DENTRO de un artefacto mas
     * grande --, asi que se PRESTAN: copiar megabytes solo para poder
     * recorrerlos seria copiarlos por nada.  Y prestado es de solo lectura por
     * construccion, no por convenio: su clase no sabe reservar. */
    const util::ByteBuffer section =
        util::byte_buffer_borrow(data, section_size);
    util::ByteCursor c = util::byte_cursor_at_start(section);

    uint32_t magic = 0;
    if (!read_u32(c, magic))
        return note_reject(report, IrSectionReject::Truncated);
    // Magic check: si no es VEIR, el caller paso un offset
    // equivocado o la seccion esta corrupta.  Abortar antes de
    // intentar parsear nada interpretable.
    if (magic != IR_SECTION_MAGIC)
        return note_reject(report, IrSectionReject::Corrupt);

    uint16_t version = 0;
    uint16_t flags = 0;
    if (!read_u16(c, version))
        return note_reject(report, IrSectionReject::Truncated);
    if (!read_u16(c, flags))
        return note_reject(report, IrSectionReject::Truncated);
    /* Version exacta: un schema distinto parseado como este daria basura.
     *
     * Este es el caso que MaS se da -- cada subida del formato deja atras todo
     * lo compilado hasta entonces -- y el unico con arreglo evidente, asi que
     * se distingue de los demas para poder decir que basta con recompilar. */
    if (version != IR_SECTION_VERSION) {
        if (report != nullptr) report->found_version = version;
        return note_reject(report, IrSectionReject::Version);
    }
    // Y ninguna bandera que no se sepa interpretar.  Ignorar una bandera
    // desconocida es leer un cuerpo que no es el que se cree estar leyendo.
    if ((flags & ~kIrFlagDeflate) != 0)
        return note_reject(report, IrSectionReject::Corrupt);

    uint32_t fn_count = 0;
    if (!read_u32(c, fn_count))
        return note_reject(report, IrSectionReject::Truncated);

    /* Si viene comprimida, se descomprime a un bufer propio y se parsea DE AHI.
     * Se recorre el mismo codigo en los dos casos: tener dos caminos de
     * deserializacion era pedir que uno de los dos se quedara atras.
     *
     * `claro` es el UNICO buffer que esta funcion posee.  Se suelta en todas
     * las salidas -- ver `salir` mas abajo --; el resto son prestados. */
    util::ByteBuffer claro;
    util::byte_buffer_init(claro, &kIrSectionKind);
    /* Sin comprimir, el cuerpo son los bytes que siguen a la cabecera -- no la
     * seccion entera --.  Se tomaba la seccion entera, y como el cursor vuelve
     * a cero mas abajo, se leia la cabecera como si fuera el cuerpo: "VEIR"
     * como cuenta de ficheros.  Nunca se leia una seccion sin comprimir, que
     * es la que se escribe cuando comprimir no encoge -- vacia o muy pequena
     * --: un modulo sin funciones no salia NUNCA de la cache, y se
     * recompilaba cada vez sin decirlo. */
    util::ByteBuffer cuerpo =
        util::byte_buffer_borrow(data + c.off, section_size - c.off);
    if ((flags & kIrFlagDeflate) != 0) {
        uint32_t sin_comprimir = 0;
        if (!read_u32(c, sin_comprimir))
            return note_reject(report, IrSectionReject::Truncated);
        // Tope defensivo: el tamano en claro lo dice el propio fichero, que
        // puede estar corrupto.  Sin limite, uno alterado pediria reservar
        // gigabytes antes de descubrir que no cuadra.
        if (sin_comprimir > (1u << 30))
            return note_reject(report, IrSectionReject::Corrupt);
        if (!util::byte_buffer_reserve(claro, sin_comprimir))
            return note_reject(report, IrSectionReject::Decompress);
        claro.size = sin_comprimir;
        mz_ulong salida = sin_comprimir;
        const size_t comprimido_len = section_size - c.off;
        const bool inflated_ok =
            mz_uncompress(claro.data, &salida, data + c.off,
                          static_cast<mz_ulong>(comprimido_len)) == MZ_OK &&
            salida == sin_comprimir;
        if (!inflated_ok) {
            util::byte_buffer_release(claro);
            return note_reject(report, IrSectionReject::Decompress);
        }
        cuerpo = claro;
    }
    /* A partir de aqui se lee del CUERPO, venga de donde venga. */
    c = util::byte_cursor_at_start(cuerpo);

    /* v19: la tabla de ficheros abre el cuerpo.  Se lee SIEMPRE, la quiera
     * quien llama o no: sus bytes estan ahi y saltarselos dejaria el cursor en
     * mitad de la primera funcion -- que no da un error, da un cuerpo leido
     * con el reparto equivocado --. */
    {
        uint32_t n_files = 0;
        if (!read_u32(c, n_files)) {
            util::byte_buffer_release(claro);
            return note_reject(report, IrSectionReject::Truncated);
        }
        // Mismo tope defensivo que el de las funciones, y por lo mismo: la
        // cuenta la dice el propio fichero, que puede estar corrupto.
        if (n_files > 100000) {
            util::byte_buffer_release(claro);
            return note_reject(report, IrSectionReject::Corrupt);
        }
        if (source_files != nullptr) source_files->reserve(n_files);
        for (uint32_t k = 0; k < n_files; ++k) {
            std::string ruta;
            if (!read_str(c, ruta)) {
                util::byte_buffer_release(claro);
                return note_reject(report, IrSectionReject::Truncated);
            }
            /* Internada al leerla: asi la ruta que sale del artefacto es EL
             * MISMO puntero que la que trae una coordenada del AST, y no dos
             * copias de la misma cadena que haya que comparar caracter a
             * caracter cada vez. */
            if (source_files != nullptr)
                source_files->push_back(util::intern_name(ruta));
        }
    }

    // Hard cap defensivo: programas con >100000 funciones IR son
    // imposibles en la practica (incluso un programa enorme tendria
    // ~5000 funciones).  Sin este cap, un .velb malicioso con un
    // fn_count enorme (UINT32_MAX) forzaria a @c functions.reserve a
    // intentar alocar TBs de memoria, crasheando con OOM.
    if (fn_count > 100000) {
        util::byte_buffer_release(claro);
        return note_reject(report, IrSectionReject::Corrupt);
    }

    functions.reserve(fn_count);
    /* Cada funcion avanza el cursor por su propio tamano.  Ya no hace falta
     * comprobar el final a mano antes y despues de cada una: el cursor lo
     * lleva, y en cuanto una lectura se sale se queda con el motivo. */
    for (uint32_t i = 0; i < fn_count; ++i) {
        IrFunction fn;
        if (!deserialize_function(c, fn)) {
            /* El cursor sabe si fue que los bytes se acabaron o que el
             * artefacto declaraba mal una longitud, y son cosas distintas: la
             * primera es un fichero cortado y la segunda uno mal escrito. */
            const IrSectionReject why =
                (c.fault == util::ByteFault::PastEnd)
                    ? IrSectionReject::Truncated
                    : IrSectionReject::Function;
            util::byte_buffer_release(claro);
            return note_reject(report, why);
        }
        functions.push_back(std::move(fn));
    }
    util::byte_buffer_release(claro);
    return true;
}

/* ===================================================================== */
/* emit_ir_module_cache / parse_ir_module_cache  (.vxir)                 */
/* ===================================================================== */

/**
 * @brief Serializa una @c StaticDataStore verbatim (pool + entries + meta).
 *
 * Se persiste el pool de bytes y los @c byte_offset/@c byte_len tal cual
 * para que la reconstruccion sea byte-exacta (el merge cross-module
 * depende de offsets estables dentro del pool del dep).
 */
void serialize_static_data(const IrModule::StaticDataStore &sd,
                           util::ByteBuffer &out) {
    write_u8(out, sd.alignment_default);
    // Pool de bytes contiguo.
    write_u32(out, static_cast<uint32_t>(sd.bytes.size()));
    util::byte_buffer_append(out, sd.bytes.data(), sd.bytes.size());
    // Entries (rangos + meta).
    write_u32(out, static_cast<uint32_t>(sd.entries.size()));
    for (const auto &e : sd.entries) {
        write_u32(out, e.byte_offset);
        write_u32(out, e.byte_len);
        write_u64(out, e.meta.content_hash);
        write_u16(out, e.meta.alignment);
        write_u8(out, e.meta.flags);
        write_u16(out, e.meta.source_module_idx);
        write_str(out, e.meta.section_name);
        write_str(out, e.meta.section_perms);           // AOT 2b
        write_u64(out, (uint64_t)e.meta.section_at);    // AOT @at
        write_u32(out, (uint32_t)e.meta.section_order); // AOT @order
        // AOT Inc 3: referencias a simbolos en bloques `bytes` (dq main).
        write_u32(out, static_cast<uint32_t>(e.meta.sym_refs.size()));
        for (const auto &sr : e.meta.sym_refs) {
            write_u32(out, sr.offset);
            write_u8(out, sr.width);
            write_u8(out, sr.is_rel);
            write_str(out, sr.sym);
        }
        // Global compartido a nivel de programa (CPU dispatch fp-table).
        write_str(out, e.meta.shared_key);
        //  NR / dev-OS: nombre exportado del bloque (cross-block symref).
        write_str(out, e.meta.symbol_name);
    }
}

bool deserialize_static_data(util::ByteCursor &c,
                             IrModule::StaticDataStore &sd) {
    sd.clear();
    if (!read_u8(c, sd.alignment_default)) return false;
    uint32_t pool_len = 0;
    if (!read_u32(c, pool_len)) return false;
    /* El pool se copia porque su destino -- @c sd.bytes -- sobrevive al
     * buffer del que se lee.  La longitud viene del propio artefacto, asi que
     * si declara mas de lo que hay el que miente es el, no quien lee. */
    if (util::byte_cursor_left(c) < pool_len) {
        c.fault = util::ByteFault::BadLength;
        return false;
    }
    sd.bytes.assign(c.buf->data + c.off, c.buf->data + c.off + pool_len);
    c.off += pool_len;
    uint32_t entry_count = 0;
    if (!read_u32(c,entry_count)) return false;
    // Cap defensivo: un dep real tiene a lo sumo unos pocos miles de slots.
    if (entry_count > 2000000u) return false;
    sd.entries.reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        IrModule::StaticDataStore::Entry e{};
        if (!read_u32(c,e.byte_offset)) return false;
        if (!read_u32(c,e.byte_len)) return false;
        if (!read_u64(c,e.meta.content_hash)) return false;
        if (!read_u16(c,e.meta.alignment)) return false;
        if (!read_u8(c,e.meta.flags)) return false;
        if (!read_u16(c,e.meta.source_module_idx)) return false;
        if (!read_str(c,e.meta.section_name)) return false;
        if (!read_str(c,e.meta.section_perms)) return false; // AOT 2b
        uint64_t sat_u = 0;
        uint32_t sord_u = 0;
        if (!read_u64(c,sat_u)) return false;  // AOT @at
        if (!read_u32(c,sord_u)) return false; // AOT @order
        e.meta.section_at = (int64_t)sat_u;
        e.meta.section_order = (int32_t)sord_u;
        // AOT Inc 3: referencias a simbolos.
        uint32_t nrefs = 0;
        if (!read_u32(c,nrefs)) return false;
        if (nrefs > 1000000u) return false; // cap defensivo
        e.meta.sym_refs.reserve(nrefs);
        for (uint32_t k = 0; k < nrefs; ++k) {
            IrModule::StaticDataMeta::SymRef sr;
            if (!read_u32(c,sr.offset)) return false;
            if (!read_u8(c,sr.width)) return false;
            if (!read_u8(c,sr.is_rel)) return false;
            if (!read_str(c,sr.sym)) return false;
            e.meta.sym_refs.push_back(std::move(sr));
        }
        // Global compartido a nivel de programa (CPU dispatch fp-table).
        if (!read_str(c,e.meta.shared_key)) return false;
        //  NR / dev-OS: nombre exportado del bloque (cross-block symref).
        if (!read_str(c,e.meta.symbol_name)) return false;
        // Validar que el rango cae dentro del pool.
        if (static_cast<uint64_t>(e.byte_offset) + e.byte_len > sd.bytes.size())
            return false;
        sd.entries.push_back(std::move(e));
    }
    return true;
}

/**
 * @brief Escribe la tabla de structs con metodos (@c IrModule::struct_types).
 * @param types La tabla.
 * @param out   Destino.
 */
static void serialize_struct_types(const std::vector<IrStructType> &types,
                                   util::ByteBuffer &out) {
    write_u32(out, static_cast<uint32_t>(types.size()));
    for (const IrStructType &st : types) {
        write_str(out, st.name);
        write_str(out, st.super_name);
        write_u32(out, static_cast<uint32_t>(st.methods.size()));
        for (const IrMethod &m : st.methods) {
            write_str(out, m.name);
            write_str(out, m.ir_fn_name);
            write_u8(out, static_cast<uint8_t>(m.return_type));
            write_u32(out, static_cast<uint32_t>(m.param_types.size()));
            for (const IrType t : m.param_types)
                write_u8(out, static_cast<uint8_t>(t));
            write_u32(out, static_cast<uint32_t>(m.vtable_index));
            write_u8(out, uint8_t((m.is_static ? 1u : 0u) |
                                  (m.is_final ? 2u : 0u) |
                                  (m.is_constructor ? 4u : 0u) |
                                  (m.is_destructor ? 8u : 0u) |
                                  (m.is_inline ? 16u : 0u)));
            write_str(out, m.defining_class);
            write_str(out, m.inherited_from);
        }
    }
}

/**
 * @brief Lee la tabla que escribio @ref serialize_struct_types.
 * @param c   Cursor.
 * @param out Tabla a rellenar.
 * @return false si el flujo esta truncado o trae tamanos imposibles.
 */
static bool deserialize_struct_types(util::ByteCursor &c,
                                     std::vector<IrStructType> &out) {
    out.clear();
    uint32_t ntypes = 0;
    if (!read_u32(c, ntypes) || ntypes > 2000000u) return false;
    out.reserve(ntypes);
    for (uint32_t i = 0; i < ntypes; ++i) {
        IrStructType st;
        uint32_t nmethods = 0;
        if (!read_str(c, st.name) || !read_str(c, st.super_name) ||
            !read_u32(c, nmethods) || nmethods > 2000000u)
            return false;
        st.methods.reserve(nmethods);
        for (uint32_t k = 0; k < nmethods; ++k) {
            IrMethod m;
            uint8_t ret = 0, flags = 0;
            uint32_t nparams = 0, vt = 0;
            if (!read_str(c, m.name) || !read_str(c, m.ir_fn_name) ||
                !read_u8(c, ret) || !read_u32(c, nparams) || nparams > 4096u)
                return false;
            m.return_type = static_cast<IrType>(ret);
            m.param_types.reserve(nparams);
            for (uint32_t p = 0; p < nparams; ++p) {
                uint8_t t = 0;
                if (!read_u8(c, t)) return false;
                m.param_types.push_back(static_cast<IrType>(t));
            }
            if (!read_u32(c, vt) || !read_u8(c, flags) ||
                !read_str(c, m.defining_class) ||
                !read_str(c, m.inherited_from))
                return false;
            m.vtable_index = static_cast<int32_t>(vt);
            m.is_static = (flags & 1u) != 0;
            m.is_final = (flags & 2u) != 0;
            m.is_constructor = (flags & 4u) != 0;
            m.is_destructor = (flags & 8u) != 0;
            m.is_inline = (flags & 16u) != 0;
            st.methods.push_back(std::move(m));
        }
        out.push_back(std::move(st));
    }
    return true;
}

void emit_ir_module_cache(const IrModule &mod, util::ByteBuffer &out) {
    util::byte_buffer_init(out, &kIrModuleCacheKind);
    write_u32(out, IR_MODULE_CACHE_MAGIC);
    write_u16(out, IR_MODULE_CACHE_VERSION);
    write_u16(out, 0); // reserved

    // 1) Funciones: reusa el formato @ir (con su header VEIR propio).
    //    Guardamos su longitud para poder delimitar la sub-seccion al leer.
    // La tabla de ficheros viaja DENTRO de esa sub-seccion, que es donde se
    // leen sus indices: no hace falta un bloque propio aqui.
    util::ByteBuffer fn_bytes;
    emit_ir_section(mod.functions, mod.source_files, fn_bytes);
    write_u32(out, static_cast<uint32_t>(fn_bytes.size));
    util::byte_buffer_append(out, fn_bytes.data, fn_bytes.size);
    util::byte_buffer_release(fn_bytes);

    // 2) static_data (lo que faltaba: la causa del bug code.s_* colgante).
    serialize_static_data(mod.static_data, out);

    // 3) globals (nombre -> IrValueId).
    write_u32(out, static_cast<uint32_t>(mod.globals.size()));
    for (const auto &g : mod.globals) {
        write_str(out, g.first);
        write_u32(out, static_cast<uint32_t>(g.second));
    }

    // 4) tabla de nombres de valores SSA (debug-info para el LSP: args/vars).
    //    NO va en el @ir del .velb (emit_ir_section, produccion) -> cero
    //    coste en el binario; solo en este cache (LSP + .vxir dev).  Por
    //    funcion: count + un string por value (vacio si el value no tiene
    //    nombre de fuente).  El indice ES el IrValueId.
    write_u32(out, static_cast<uint32_t>(mod.functions.size()));
    for (const auto &fn : mod.functions) {
        write_u32(out, static_cast<uint32_t>(fn.values.size()));
        for (const auto &v : fn.values)
            write_str(out, v.name);
    }

    // 5) native_imports (lib, name): el AOT los usa para mapear cada simbolo
    //    FFI extern a su DLL real (kernel32.dll, user32.dll, ...) en vez de
    //    asumir msvcrt.dll.  Sin esto, `extern "kernel32.dll" {...}` resolvia
    //    desde msvcrt -> fallo de carga del PE.
    /*    Con lo DECLARADO sobre cada una (v11).  Va aqui y no se recalcula
     *    despues porque quien lo sabe es el frontend, y a partir de este punto
     *    -- el JIT, el AOT, `--analyze` -- todos leen el IR de la cache: una
     *    declaracion que no cruce esta frontera no la ve nadie. */
    write_u32(out, static_cast<uint32_t>(mod.native_imports.size()));
    for (const auto &ni : mod.native_imports) {
        write_str(out, ni.lib);
        write_str(out, ni.name);
        const IrNativeEffects &fx = ni.effects;
        write_u8(out, fx.declared ? 1u : 0u);
        write_u32(out, fx.reads_pointee);
        write_u32(out, fx.writes_pointee);
        /* Los efectos van BIT A BIT, asi que un eje nuevo que no se escriba
         * aqui se PIERDE al pasar por el cache -- y no se nota: la nativa
         * vuelve sin el, o sea mas inofensiva de lo que es.  Ya paso: `blocks`
         * y `traps` se declaraban, se parseaban, y al leerlos de vuelta una
         * funcion que bloquea salia PURA (mas limpia que no declarar nada,
         * que es lo contrario de lo que tiene que pasar).
         *
         * El primer byte estaba LLENO, asi que los nuevos van en un segundo y
         * el formato sube de version: un cache viejo se rechaza entero en vez
         * de leerse a medias. */
        write_u8(out,
                 uint8_t((fx.reads_global ? 1u : 0u) |
                         (fx.writes_global ? 2u : 0u) | (fx.io ? 4u : 0u) |
                         (fx.may_throw ? 8u : 0u) |
                         (fx.nondeterministic ? 16u : 0u) |
                         (fx.comptime ? 32u : 0u) | (fx.may_panic ? 64u : 0u) |
                         (fx.allocates ? 128u : 0u)));
        write_u8(out,
                 uint8_t((fx.may_block ? 1u : 0u) | (fx.may_trap ? 2u : 0u) |
                         (uint8_t(fx.throw_origin) << 2) |
                         (uint8_t(fx.panic_origin) << 4)));
        write_u16(out, fx.trap_kinds);
        write_u16(out, fx.reads_world);
        write_u16(out, fx.writes_world);
        write_u8(out, uint8_t(fx.returns_fresh ? 1u : 0u));
        write_u32(out, fx.frees_pointee);
    }

    // 6) structs con metodos y de donde viene cada metodo (v20).
    serialize_struct_types(mod.struct_types, out);
}

std::vector<uint8_t> emit_ir_module_cache_vec(const IrModule &mod) {
    util::ByteBuffer buf;
    emit_ir_module_cache(mod, buf);
    std::vector<uint8_t> out(buf.data, buf.data + buf.size);
    util::byte_buffer_release(buf);
    return out;
}

bool parse_ir_module_cache(const uint8_t *data, size_t len, IrModule &out) {
    if (data == nullptr) return false;
    /* Los bytes son de quien llama; aqui solo se miran. */
    const util::ByteBuffer cache = util::byte_buffer_borrow(data, len);
    util::ByteCursor c = util::byte_cursor_at_start(cache);

    uint32_t magic = 0;
    if (!read_u32(c, magic)) return false;
    if (magic != IR_MODULE_CACHE_MAGIC)
        return false; // formato viejo -> recompilar
    uint16_t version = 0, reserved = 0;
    if (!read_u16(c, version)) return false;
    if (!read_u16(c, reserved)) return false;
    if (version != IR_MODULE_CACHE_VERSION) return false;

    // 1) Funciones.
    uint32_t fn_len = 0;
    if (!read_u32(c, fn_len)) return false;
    if (util::byte_cursor_left(c) < fn_len) return false;
    // Y la tabla de ficheros contra la que se leen sus indices: aqui SI se
    // pide, porque un modulo restaurado de cache tiene que poder decir de
    // donde salio cada funcion igual que si se acabara de compilar.
    /* El motivo no se pide: un cache que no se puede leer se RECOMPILA, asi
     * que no se pierde nada y no hay nada que contar.  Es la diferencia con el
     * artefacto que carga la maquina, donde rechazarlo si cuesta -- el
     * programa corre igual pero sin JIT --, y por eso alli si se dice. */
    if (!parse_ir_section(data + c.off, fn_len, out.functions,
                          /*report=*/nullptr, &out.source_files))
        return false;
    c.off += fn_len;

    // 2) static_data.
    if (!deserialize_static_data(c, out.static_data)) return false;

    // 3) globals.
    out.globals.clear();
    uint32_t gcount = 0;
    if (!read_u32(c,gcount)) return false;
    if (gcount > 2000000u) return false;
    for (uint32_t i = 0; i < gcount; ++i) {
        std::string name;
        uint32_t vid = 0;
        if (!read_str(c,name)) return false;
        if (!read_u32(c,vid)) return false;
        out.globals.emplace(std::move(name), static_cast<IrValueId>(vid));
    }

    // 4) tabla de nombres de valores SSA (debug-info).  Si el stream se acabo
    //    (cache mas viejo sin la tabla pero con misma version -> no deberia
    //    pasar por el bump, pero somos defensivos), se omite sin error.
    uint32_t nfns = 0;
    if (read_u32(c,nfns)) {
        if (nfns > 2000000u) return false;
        for (uint32_t f = 0; f < nfns; ++f) {
            uint32_t nvals = 0;
            if (!read_u32(c,nvals)) return false;
            if (nvals > 50000000u) return false;
            for (uint32_t v = 0; v < nvals; ++v) {
                std::string nm;
                if (!read_str(c,nm)) return false;
                if (f < out.functions.size() &&
                    v < out.functions[f].values.size())
                    out.functions[f].values[v].name = std::move(nm);
            }
        }
    }

    // 5) native_imports (lib, name).  Defensivo: si el stream se acabo (no
    //    deberia por el bump de version), se omite sin error.
    out.native_imports.clear();
    uint32_t nimp = 0;
    if (read_u32(c,nimp)) {
        if (nimp > 2000000u) return false;
        for (uint32_t i = 0; i < nimp; ++i) {
            std::string lib, name;
            if (!read_str(c,lib)) return false;
            if (!read_str(c,name)) return false;
            IrNativeEffects fx;
            uint8_t decl = 0, bits = 0;
            if (!read_u8(c,decl)) return false;
            if (!read_u32(c,fx.reads_pointee)) return false;
            if (!read_u32(c,fx.writes_pointee)) return false;
            if (!read_u8(c,bits)) return false;
            fx.declared = decl != 0;
            fx.reads_global = (bits & 1u) != 0;
            fx.writes_global = (bits & 2u) != 0;
            fx.io = (bits & 4u) != 0;
            fx.may_throw = (bits & 8u) != 0;
            fx.nondeterministic = (bits & 16u) != 0;
            fx.comptime = (bits & 32u) != 0;
            fx.may_panic = (bits & 64u) != 0;
            fx.allocates = (bits & 128u) != 0;
            uint8_t bits2 = 0;
            if (!read_u8(c,bits2)) return false;
            fx.may_block = (bits2 & 1u) != 0;
            fx.may_trap = (bits2 & 2u) != 0;
            fx.throw_origin = static_cast<ir::UnwindOrigin>((bits2 >> 2) & 3u);
            fx.panic_origin = static_cast<ir::UnwindOrigin>((bits2 >> 4) & 3u);
            if (!read_u16(c,fx.trap_kinds)) return false;
            if (!read_u16(c,fx.reads_world)) return false;
            if (!read_u16(c,fx.writes_world)) return false;
            uint8_t fresco = 0;
            if (!read_u8(c,fresco)) return false;
            fx.returns_fresh = fresco != 0;
            if (!read_u32(c,fx.frees_pointee)) return false;
            out.register_native_import(std::move(lib), std::move(name), fx);
        }
    }

    // 6) structs con metodos (v20).  Sin defensa por fin de flujo: la version
    //    lo garantiza, y una cache que no la trae se recompila.
    if (!deserialize_struct_types(c, out.struct_types)) return false;
    return true;
}

void adopt_cached_module(IrModule &dst, IrModule &&src) {
    /* Esta lista es la de lo que rellena @ref parse_ir_module_cache, que esta
     * justo encima.  Se tocan juntas; ver la cabecera para las tres veces que
     * olvidarse de un campo aqui costo un fallo mudo. */
    dst.functions = std::move(src.functions);
    dst.source_files = std::move(src.source_files);
    dst.static_data = std::move(src.static_data);
    dst.globals = std::move(src.globals);
    dst.native_imports = std::move(src.native_imports);
    dst.struct_types = std::move(src.struct_types);
}

} // namespace ir
