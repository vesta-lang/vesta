/**
 * @file instr_db.h
 * @brief Base de datos de instrucciones EMBEBIDA en el compilador (autocontenida,
 *        sin archivos externos).
 *
 * Las tablas (sintaxis por-ISA, y mas adelante coste por microarquitectura) las
 * genera @c tools/import/gen_cpp_db.py a partir de la DB (@c arch-data) y se
 * compilan DENTRO del compilador como @c .rodata estatica.  Asi el analisis de
 * asm (ASA), el LSP (hover) y el optimizer JIT/AOT resuelven cada instruccion a
 * su FORMA (identidad ISA) sin depender de ficheros ni parsear texto en runtime.
 *
 * El FormID es el indice denso del orden lexicografico de la clave estructural
 * (misma identidad que la DB); un @c match(mnemonico, operandos) resuelve el
 * texto de un @c asm { } a su FormID.
 */
#ifndef VX_INSTR_DB_H
#define VX_INSTR_DB_H

#include <cstdint>
#include <string>
#include <vector>

namespace vx {
namespace instr_db {

/// Clase de operando (debe casar con @c _KIND del generador).
enum DbOpKind : uint8_t {
    OP_REG = 0, OP_MEM = 1, OP_IMM = 2, OP_AGEN = 3, OP_RELBR = 4,
    OP_ABSBR = 5, OP_FLAGS = 7,
};

/// Bits de overlay (semantica que el encoding no da; casa con @c _OVL).
enum DbOverlayBit : uint16_t {
    OVL_BARRIER = 1u << 0, OVL_SERIALIZING = 1u << 1, OVL_ATOMIC = 1u << 2,
    OVL_LL_SC = 1u << 3, OVL_MEM_ACQUIRE = 1u << 4, OVL_MEM_RELEASE = 1u << 5,
    OVL_MEM_SEQ_CST = 1u << 6, OVL_NO_REORDER = 1u << 7, OVL_BRANCH = 1u << 8,
    OVL_CALL = 1u << 9, OVL_RET = 1u << 10, OVL_SYSCALL = 1u << 11,
};

/// Un operando de una forma (kind + ancho en bits + flags r/w/impl/suppr).
struct DbOperand {
    uint8_t kind;    ///< DbOpKind.
    uint16_t width;  ///< bits (0 si no aplica).
    uint8_t flags;   ///< bit0=read, bit1=write, bit2=implicit, bit3=suppressed.
};

/// Una FORMA (encoding) indexada por FormID.  Los campos string son indices al
/// pool @c kStr de la ISA.
struct DbForm {
    uint32_t iclass;   ///< mnemonico (indice a kStr).
    uint16_t ext;      ///< extension (indice a kStr).
    uint16_t overlay;  ///< bitmask DbOverlayBit.
    uint8_t rmask;     ///< operandos leidos (bit i = operando i).
    uint8_t wmask;     ///< operandos escritos.
    uint8_t memflags;  ///< bit0=mem bit1=imm bit2=wflags bit3=rflags.
    uint32_t ops_off;  ///< offset en el pool de operandos (>64K -> 32 bits).
    uint8_t ops_count; ///< numero de operandos.
    uint32_t opcode;   ///< opcode (indice a kStr; documentacion).
};

/// Rango de FormIDs con el mismo iclass (para el matcher; ordenado por nombre).
struct DbIclassRange {
    uint32_t iclass;    ///< indice a kStr.
    uint32_t first_fid; ///< primer FormID del rango.
    uint32_t count;     ///< numero de formas.
};

/// Operando parseado de una linea de asm (entrada del matcher).
struct ParsedOp {
    DbOpKind kind = OP_REG;
    uint16_t width = 0; ///< 0 = desconocido (no restringe).
};

/// ISA soportadas por la DB embebida (ARM64 = AArch64, ARM32 = A32/T32).
enum class Isa { X86, ARM64, ARM32, RISCV };

/// Punteros a las tablas generadas de una ISA.  Lo rellena el @c .cpp generado
/// (@c gen/instr_db_<isa>_gen.cpp) via su accesor @c db_<isa>(); el resto del
/// modulo lo consume sin conocer las tablas concretas.
struct IsaData {
    const char *const *str = nullptr;
    unsigned str_count = 0;
    const DbOperand *ops = nullptr;
    unsigned ops_count = 0;
    const DbForm *forms = nullptr;
    unsigned form_count = 0;
    const DbIclassRange *iclass = nullptr;
    unsigned iclass_count = 0;
};

/// Accesores de las tablas de cada ISA (definidos en @c gen/instr_db_*_gen.cpp).
const IsaData &db_x86();
const IsaData &db_arm64();
const IsaData &db_arm32();
const IsaData &db_riscv();

// ------------------------------------------------------------------------
// Capa de COSTE por microarquitectura (latencia + puertos = ejecucion
// paralela superescalar).  La consume el optimizer (scheduling) y el LSP
// (hover: coste por microarq).  Tablas generadas en gen/instr_db_<isa>_cost_gen.cpp.
// ------------------------------------------------------------------------

/// Uso de un puerto de ejecucion por una clase (para el modelo superescalar):
/// @c port indexa el legado de puertos de la microarquitectura.
struct AsmPortSlot {
    uint8_t port;  ///< indice al legado de puertos de la microarq.
    float uops;    ///< uops repartidos a ese grupo de puertos.
};

/// Clase de scheduling deduplicada (formas con el mismo coste comparten clase).
struct AsmClass {
    float recip_tp;    ///< throughput reciproco (1/IPC).
    float latency;     ///< latencia maxima (proxy del camino critico del nodo).
    float div_cycles;  ///< ciclos de division (-1 si no aplica).
    uint16_t uops;     ///< uops emitidas.
    uint8_t flags;     ///< bit0 microcoded, bit1 macro_fusible.
    uint16_t ports_off; ///< offset en el pool de AsmPortSlot de la microarq.
    uint8_t ports_count;
};

/// Tablas de coste de UNA microarquitectura (las rellena el .cpp generado).
struct MicroarchData {
    const char *name;
    const char *const *port_names; ///< legado: indice -> nombre de puerto.
    uint16_t port_count;
    const AsmClass *classes;
    uint16_t class_count;
    const AsmPortSlot *slots; ///< pool de puertos de todas las clases.
    const int16_t *form_class; ///< FormID -> class_id (-1 = sin coste aqui).
    uint32_t form_count;
};

/// Lista de microarquitecturas de una ISA (accesor del .cpp de coste generado).
struct CostData {
    const MicroarchData *uarchs = nullptr;
    uint32_t count = 0;
};
const CostData &cost_x86();
const CostData &cost_arm64();
const CostData &cost_arm32();
const CostData &cost_riscv();

/// Coste resuelto de una forma en una microarquitectura (resultado publico).
struct AsmCost {
    bool found = false;         ///< false = la microarq no cronometra esta forma.
    float recip_tp = 0.0f;      ///< throughput reciproco.
    float latency = 0.0f;       ///< latencia (camino critico del nodo).
    float div_cycles = -1.0f;
    uint16_t uops = 0;
    bool microcoded = false;
    bool macro_fusible = false;
    /// Puertos usados (para el modelo de ejecucion paralela).  Punteros a la
    /// tabla de la microarq (sin copia); @c ports_count entradas.
    const AsmPortSlot *ports = nullptr;
    uint8_t ports_count = 0;
    const char *const *port_names = nullptr; ///< resuelve port index -> nombre.
};

/// Numero de microarquitecturas con coste para la ISA.
uint32_t microarch_count(Isa isa);
/// Nombre de la microarquitectura @p ua_id (o "" si fuera de rango).
const char *microarch_name(Isa isa, uint32_t ua_id);
/// Indice de una microarquitectura por nombre (-1 si no existe).
int32_t microarch_by_name(Isa isa, const std::string &name);
/// Coste de la forma @p form_id en la microarquitectura @p ua_id.
AsmCost cost(Isa isa, int32_t form_id, uint32_t ua_id);

/**
 * @brief Resuelve el texto de una instruccion (mnemonico + operandos) a su
 *        FormID en la DB de la ISA dada.  Devuelve -1 si el mnemonico no existe.
 *
 * Espeja el emparejador del analizador: busca el rango del iclass (binaria) y
 * puntua por clase+ancho de operando (aridad exacta); la forma de mayor
 * puntuacion gana.  Si el mnemonico existe pero ninguna forma casa por
 * operandos, devuelve la primera del rango (nivel mnemonico).
 */
int32_t match(Isa isa, const std::string &mnemonic,
              const std::vector<ParsedOp> &ops);

/// Nombre del iclass de una forma (o "" si el FormID no es valido).
const char *iclass_name(Isa isa, int32_t form_id);
/// Bitmask de overlay de una forma (0 si el FormID no es valido).
uint16_t overlay_of(Isa isa, int32_t form_id);
/// Numero de formas de la ISA.
uint32_t form_count(Isa isa);

} // namespace instr_db
} // namespace vx

#endif // VX_INSTR_DB_H
