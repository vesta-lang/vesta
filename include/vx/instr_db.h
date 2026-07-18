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

/// ISA soportadas por la DB embebida.
enum class Isa { X86 };

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

/// Accesor de las tablas x86 (definido en @c gen/instr_db_x86_gen.cpp).
const IsaData &db_x86();

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
