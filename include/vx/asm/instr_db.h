/**
 * @file instr_db.h
 * @brief Base de datos de instrucciones EMBEBIDA en el compilador
 * (autocontenida, sin archivos externos).
 *
 * Las tablas (sintaxis por-ISA, y mas adelante coste por microarquitectura) las
 * genera @c tools/import/gen_cpp_db.py a partir de la DB (@c arch-data) y se
 * compilan DENTRO del compilador como @c .rodata estatica.  Asi el analisis de
 * asm (ASA), el LSP (hover) y el optimizer JIT/AOT resuelven cada instruccion a
 * su FORMA (identidad ISA) sin depender de ficheros ni parsear texto en
 * runtime.
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

/**
 * @brief Clase de operando.  Debe casar con @c _KIND del generador.
 *
 * Y no casaba: el generador emite @c flags como **6** y aqui se declaraba como
 * 7, que es el valor con el que el generador rellena lo que NO reconoce.  El
 * comentario de los dos sitios decia que tenian que coincidir; solo faltaba que
 * alguien los comparase.
 *
 * La consecuencia era silenciosa y grande: los 3893 operandos de banderas de
 * x86 no se reconocian como tales, asi que contaban como operandos ESCRITOS EN
 * EL TEXTO.  Con uno de mas, la aridad de la forma no casa nunca con la de la
 * linea
 * -- `add rax, rbx` tiene dos operandos y la forma parecia tener tres --, y el
 * emparejador se quedaba en el nivel del mnemonico en vez de resolver la forma.
 * De ahi que casi 450 clases de x86 no se pudieran modelar.
 */
enum DbOpKind : uint8_t {
    OP_REG = 0,
    OP_MEM = 1,
    OP_IMM = 2,
    OP_AGEN = 3,
    OP_RELBR = 4,
    OP_ABSBR = 5,
    OP_FLAGS = 6, ///< banderas de condicion: no se escriben en el texto.
    /**
     * Operando que la fuente no clasifica.  El importador de ARM lo marca `?` y
     * el generador lo emite con su valor por defecto, que es este.
     *
     * Son 1357 en ARM, y NO son un error de etiquetado: son cosas que en A64 no
     * se escriben como un operando suelto -- la condicion de `b.eq`, que va
     * pegada al mnemonico, o el nombre de la operacion de sistema de un `at`
     * --. Por eso no cuentan como operando del texto, igual que las banderas.
     *
     * Lo que SI queda por hacer es la OPCIONALIDAD: `ADDS_32_addsub_shift`
     * declara cinco operandos porque incluye el desplazamiento opcional
     * (`{, <shift> #<amount>}`), y un `adds x0, x1, x2` escribe tres.  Esa es
     * la razon real de que la aridad no case en ARM -- no esta clase --, y
     * arreglarla pide que la fuente diga cuales son opcionales.
     */
    OP_OTHER = 7,
};

/// @c true si @p kind no es un operando que se escriba en el texto.  Los dos
/// casos van juntos siempre, y tenerlo en un sitio evita que uno se quede
/// atras.
inline bool op_kind_is_textual(uint8_t kind) {
    return kind != OP_FLAGS && kind != OP_OTHER;
}

/// Bits de overlay (semantica que el encoding no da; casa con @c _OVL).
enum DbOverlayBit : uint16_t {
    OVL_BARRIER = 1u << 0,
    OVL_SERIALIZING = 1u << 1,
    OVL_ATOMIC = 1u << 2,
    OVL_LL_SC = 1u << 3,
    OVL_MEM_ACQUIRE = 1u << 4,
    OVL_MEM_RELEASE = 1u << 5,
    OVL_MEM_SEQ_CST = 1u << 6,
    OVL_NO_REORDER = 1u << 7,
    OVL_BRANCH = 1u << 8,
    OVL_CALL = 1u << 9,
    OVL_RET = 1u << 10,
    OVL_SYSCALL = 1u << 11,
};

/// Un operando de una forma (kind + ancho en bits + flags r/w/impl/suppr).
struct DbOperand {
    uint8_t kind;   ///< DbOpKind.
    uint16_t width; ///< bits (0 si no aplica).
    uint8_t flags;  ///< bit0=read bit1=write bit2=implicit bit3=suppressed
                    ///< bit4=OPCIONAL (se puede omitir al escribirla: en la
                    ///< plantilla del MRAS va entre llaves).  Sin el, una forma
                    ///< con opcionales no casa nunca por aridad.
    uint16_t regset; ///< indice a kStr del conjunto de registros permitido
                     ///< (p.ej. "AX", "DX", "GPR64", "-").  Para operandos
                     ///< IMPLICITOS de registro fijo nombra el registro
                     ///< concreto (rax:rdx de div, rax de cmpxchg) sin
                     ///< re-derivarlo a mano.
};

/// Una FORMA (encoding) indexada por FormID.  Los campos string son indices al
/// pool @c kStr de la ISA.
struct DbForm {
    uint32_t iclass;   ///< mnemonico (indice a kStr).
    uint16_t ext;      ///< extension (indice a kStr).
    uint16_t isa_set;  ///< conjunto ISA que EXIGE esta forma concreta (indice a
                       ///< kStr): "AVX512F_512", "AVX2", "SSE2", "BMI2"...  Es
                       ///< mas fino que @c ext (que agrupa las 14518 formas
                       ///< AVX-512 bajo un solo "AVX512EVEX") y es lo que hay
                       ///< que comparar contra lo que el procesador declara.
    uint16_t overlay;  ///< bitmask DbOverlayBit.
    uint8_t rmask;     ///< operandos leidos (bit i = operando i).
    uint8_t wmask;     ///< operandos escritos.
    uint8_t memflags;  ///< bit0=mem bit1=imm bit2=wflags bit3=rflags.
    uint32_t ops_off;  ///< offset en el pool de operandos (>64K -> 32 bits).
    uint8_t ops_count; ///< numero de operandos.
    uint32_t opcode;   ///< opcode (indice a kStr; documentacion).
    /**
     * @name QUE banderas toca, no solo si toca alguna.
     *
     * Un bit por bandera; el nombre de cada bit esta en @ref
     * IsaData::flag_names, que es la leyenda de ESA ISA --
     * `cf`/`pf`/`af`/`zf`/`sf`/`of` en x86, `n`/`z`/`c`/`v` en ARM, ninguna en
     * RISC-V --.  El juego sale de los datos y no de una lista escrita en el
     * codigo, que seria la de una sola.
     *
     * `memflags` dice SI toca alguna; esto dice CUALES, y es la diferencia
     * entre un `bt` -- que solo deja el acarreo -- y un `cmp` -- que deja las
     * seis --, o entre un `inc` y un `add`: `inc` no toca el acarreo, que es
     * justo lo que permite encadenarlo con un `adc`.
     *
     * 0 = la fuente no lo dijo, que NO es lo mismo que no tocar ninguna.
     * @{
     */
    uint16_t wflags_set;
    uint16_t rflags_set;
    /// @}
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
    /// Leyenda de banderas de la ISA: el indice de cada nombre es su bit en
    /// @ref DbForm::wflags_set y @ref DbForm::rflags_set.  Vacia en las que no
    /// tienen banderas de condicion (RISC-V).
    const char *const *flag_names = nullptr;
    unsigned flag_count = 0;
};

/// Accesores de las tablas de cada ISA (definidos en @c
/// gen/instr_db_*_gen.cpp).
const IsaData &db_x86();
const IsaData &db_arm64();
const IsaData &db_arm32();
const IsaData &db_riscv();

// ------------------------------------------------------------------------
// Capa de COSTE por microarquitectura (latencia + puertos = ejecucion
// paralela superescalar).  La consume el optimizer (scheduling) y el LSP
// (hover: coste por microarq).  Tablas generadas en
// gen/instr_db_<isa>_cost_gen.cpp.
// ------------------------------------------------------------------------

/// Uso de un puerto de ejecucion por una clase (para el modelo superescalar):
/// @c port indexa el legado de puertos de la microarquitectura.
struct AsmPortSlot {
    uint8_t port; ///< indice al legado de puertos de la microarq.
    float uops;   ///< uops repartidos a ese grupo de puertos.
};

/// Clase de scheduling deduplicada (formas con el mismo coste comparten clase).
struct AsmClass {
    float recip_tp;   ///< throughput reciproco (1/IPC).
    float latency;    ///< latencia maxima (proxy del camino critico del nodo).
    float div_cycles; ///< ciclos de division (-1 si no aplica).
    uint16_t uops;    ///< uops emitidas.
    uint8_t flags;    ///< bit0 microcoded, bit1 macro_fusible.
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
    const AsmPortSlot *slots;  ///< pool de puertos de todas las clases.
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
    bool found = false;    ///< false = la microarq no cronometra esta forma.
    float recip_tp = 0.0f; ///< throughput reciproco.
    float latency = 0.0f;  ///< latencia (camino critico del nodo).
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

// ------------------------------------------------------------------------
// Capa de FEATURES por CPU: que extensiones de ISA implementa cada core
// (para saber que instrucciones admite -- especializacion de codegen).
// Tablas generadas en gen/instr_db_<isa>_feat_gen.cpp.
// ------------------------------------------------------------------------

/// Features (extensiones de ISA) que implementa una CPU real.
struct CpuFeatures {
    const char *name;      ///< nombre de la CPU.
    const char *sched;     ///< modelo de scheduling asociado (documentacion).
    const uint16_t *feats; ///< indices al pool de nombres de feature.
    uint16_t feat_count;
};

/// Tabla de features de una ISA (accesor del .cpp de features generado).
struct FeatData {
    const char *const *feat_names = nullptr;
    uint16_t feat_name_count = 0;
    const CpuFeatures *cpus = nullptr;
    uint16_t cpu_count = 0;
};
const FeatData &feat_x86();
const FeatData &feat_arm(); ///< AArch64 y A32/T32 comparten features.
const FeatData &feat_riscv();

/// Numero de CPUs con features para la ISA.
uint32_t cpu_count(Isa isa);
/// Nombre de la CPU @p cpu_id (o "" si fuera de rango).
const char *cpu_name(Isa isa, uint32_t cpu_id);
/// Indice de una CPU por nombre (-1 si no existe).
int32_t cpu_by_name(Isa isa, const std::string &name);
/// La CPU @p cpu_id implementa la feature @p feature (extension de ISA).
bool cpu_has_feature(Isa isa, uint32_t cpu_id, const std::string &feature);

// ------------------------------------------------------------------------
// Coste de un BLOQUE de asm: modelo superescalar (latencia + presion de
// puertos = ejecucion en paralelo).  Lo consumen el LSP (hover: coste del
// bloque) y el optimizer (scheduling).
// ------------------------------------------------------------------------

/// Coste agregado de un bloque de asm en una microarquitectura.
struct AsmBlockCost {
    uint32_t instr_count = 0; ///< instrucciones (lineas no vacias/label).
    uint32_t matched = 0;     ///< con forma reconocida en la DB.
    uint32_t costed = 0;      ///< con coste en esta microarq.
    uint32_t total_uops = 0;  ///< suma de uops.
    /// Cota SUPERIOR de latencia (suma serie).  El camino critico EXACTO
    /// necesita el grafo de dependencias entre registros (futuro, ASA.2).
    float latency_sum = 0.0f;
    /// Ciclos por el modelo SUPERESCALAR: max entre la presion del puerto mas
    /// cargado (ejecucion paralela) y la suma de throughput reciproco.  Es la
    /// cota INFERIOR de ciclos del bloque bien planificado.
    float throughput = 0.0f;
    /// Presion por puerto (uops acumuladas por grupo de puertos): muestra que
    /// unidad de ejecucion es el cuello de botella.
    std::vector<std::pair<std::string, float>> port_pressure;
};

/// Analiza el coste de @p body (cuerpo de un @c asm) en la microarq @p ua_id:
/// empareja cada linea (@ref match_asm_line), acumula uops/latencia y reparte
/// las uops por puerto (modelo de ejecucion paralela superescalar).
AsmBlockCost analyze_asm_cost(Isa isa, const std::string &body, uint32_t ua_id);

// ------------------------------------------------------------------------
// Planificacion (scheduling) de un bloque de asm: analisis de dependencias
// + reordenacion valida por latencia/puertos.  SEGURIDAD: toda reordenacion
// respeta dependencias, barreras y memoria; lo no modelable es conservador.
// Funcion PURA (no reescribe codegen todavia): se valida antes de cablear.
// ------------------------------------------------------------------------

/// Semantica de UNA instruccion de asm (para el grafo de dependencias).
struct AsmInsnSem {
    int32_t form_id = -1;
    bool modeled = false; ///< false = operandos implicitos / no emparejada ->
                          ///< se trata CONSERVADOR (no se reordena alrededor).
    bool barrier = false; ///< overlay barrera/serializante/atomica/rama/call/
                          ///< ret/syscall: nada la cruza.
    std::vector<std::string> reads;  ///< registros canonicos leidos.
    std::vector<std::string> writes; ///< registros canonicos escritos.
    bool reads_mem = false, writes_mem = false;
    bool reads_flags = false, writes_flags = false;
    /**
     * @name QUE banderas, como mascara (bit = indice en la leyenda de la ISA).
     *
     * Los booleanos de arriba dicen SI toca alguna; estas dicen CUALES, y es lo
     * que permite no estorbar de mas: un `inc` no toca el acarreo, asi que no
     * choca con el `adc` que lo consume -- y con el bit grueso si chocaba --.
     *
     * Son mascaras y no nombres a proposito: esto lo pregunta el planificador
     * por cada par de instrucciones, y comparar dos enteros es una operacion.
     * Los nombres, que son para leer, los da @ref flag_names_of.
     *
     * 0 = la base no trae el detalle para esta ISA.  Ahi mandan los booleanos,
     * que siguen siendo ciertos.
     * @{
     */
    uint16_t reads_flags_set = 0, writes_flags_set = 0;
    /// @}
    /**
     * @name ESTADO del procesador que no es un registro general.
     *
     * Nombrado (@c "cr0", @c "gdtr", @c "msrs", @c "mxcsr", @c "st(0)"...) en
     * vez de tratado como un efecto opaco.  Una instruccion privilegiada tiene
     * efectos igual de concretos que una aritmetica, y modelarlos es lo que
     * permite decir que una `rdmsr` y una `stmxcsr` no se estorban -- mientras
     * que "toca algo" obliga a no mover nada alrededor de ninguna de las dos.
     * @{
     */
    std::vector<std::string> reads_state, writes_state;
    /// @}
    float latency =
        0.0f;         ///< latencia en la microarq (prioridad del scheduler).
    std::string text; ///< linea original (para reemitir).
};

/// Semantica de una instruccion de asm (@p line) en la microarq @p ua_id.
AsmInsnSem asm_insn_sem(Isa isa, const std::string &line, uint32_t ua_id);

/// ¿Hay dependencia entre @p a (antes) y @p b (despues) que OBLIGA a conservar
/// su orden?  True si RAW/WAR/WAW en registros, dependencia de flags, ambas
/// tocan memoria (conservador: no se sabe si solapan), o alguna es barrera / no
/// modelada.  Es la regla de SEGURIDAD del scheduler.
bool asm_dep_conflict(const AsmInsnSem &a, const AsmInsnSem &b);

/// Resultado de planificar un bloque de asm.
struct AsmSchedule {
    std::vector<uint32_t> order; ///< permutacion de indices (orden nuevo).
    bool moved = false;          ///< true si el orden cambio.
    bool valid = true; ///< invariante: respeta TODAS las dependencias.
};

/// Planifica (list scheduling) las instrucciones de @p body por su altura de
/// camino critico (latencia), respetando dependencias y barreras.  No cambia la
/// semantica; devuelve una permutacion valida.  @p ua_id da las latencias.
AsmSchedule schedule_asm_block(Isa isa, const std::string &body,
                               uint32_t ua_id);

/// Reordena las instrucciones de @p body (via @ref schedule_asm_block) y
/// devuelve el cuerpo REORDENADO **solo si es seguro**: sin labels (un salto a
/// una etiqueta rompe si algo la cruza), invariante @c valid, y el orden
/// realmente cambio.  En CUALQUIER otro caso devuelve @p body SIN TOCAR.  Es la
/// funcion que cablea el reoptimizador de asm de forma conservadora.
std::string reschedule_asm(Isa isa, const std::string &body, uint32_t ua_id);

/**
 * @brief Clasifica un operando de asm (token) a su @c (kind, width) para el
 *        matcher.  Conoce los registros de la ISA (x86 rax/eax/xmm...; ARM
 *        x/w/v...; RISC-V x0-31/ABI/f), la memoria (x86/ARM @c [...], RISC-V
 *        @c disp(reg)) y los inmediatos (numero, @c #imm de ARM).
 */
ParsedOp parse_operand(Isa isa, const std::string &token);

/**
 * @brief Empareja una LINEA de asm completa (mnemonico + operandos, sintaxis
 *        del ensamblador) a su FormID.  Parte el mnemonico, trocea los
 *        operandos (respetando @c [...]/@c (...)), los clasifica con
 *        @ref parse_operand y llama a @ref match.  Devuelve -1 si la linea no
 *        tiene mnemonico (label/vacia) o el mnemonico no existe.
 */
int32_t match_asm_line(Isa isa, const std::string &line);

/**
 * @brief Resuelve el texto de una instruccion (mnemonico + operandos) a su
 *        FormID en la DB de la ISA dada.  Devuelve -1 si el mnemonico no
 * existe.
 *
 * Espeja el emparejador del analizador: busca el rango del iclass (binaria) y
 * puntua por clase+ancho de operando (aridad exacta); la forma de mayor
 * puntuacion gana.  Si el mnemonico existe pero ninguna forma casa por
 * operandos, devuelve la primera del rango (nivel mnemonico).
 */
int32_t match(Isa isa, const std::string &mnemonic,
              const std::vector<ParsedOp> &ops, bool *por_operandos = nullptr);

/// Nombre del iclass de una forma (o "" si el FormID no es valido).
const char *iclass_name(Isa isa, int32_t form_id);
/// Bitmask de overlay de una forma (0 si el FormID no es valido).
uint16_t overlay_of(Isa isa, int32_t form_id);
/// Extension de una forma ("AVX512EVEX", "SSE2", "BASE"...), "" si no vale.
const char *ext_of(Isa isa, int32_t form_id);
/**
 * @brief Rol (lee / escribe) del operando EXPLICITO numero @p idx de una forma.
 *
 * El rol se estaba deduciendo buscando el nombre del registro en las listas de
 * lectura y escritura, y eso solo funciona con los generales: para `movdqa
 * xmm1, xmm0` no encontraba nada y la instruccion acababa pareciendo que no
 * toca ningun registro.  La forma lo dice por POSICION, que es como esta
 * modelado y vale para cualquier clase y cualquier ISA.
 *
 * Se cuentan solo los operandos EXPLICITOS -- los que se escriben en el texto
 * -- porque es contra ellos contra los que se indexa; los implicitos van
 * aparte.
 *
 * @param isa     ISA de la forma.
 * @param form_id Forma.
 * @param idx     Posicion del operando explicito (0 = el primero del texto).
 * @param reads   Sale a true si el operando se lee.
 * @param writes  Sale a true si se escribe.
 * @return false si la forma o el indice no existen (no se toca nada).
 */
bool explicit_operand(Isa isa, int32_t form_id, size_t idx, bool &reads,
                      bool &writes);

/**
 * @brief Igual que la anterior, diciendo ademas de que CLASE es el operando.
 *
 * El rol sin la clase no distingue `add rax, rbx` de `add rax, [rbx]`: los dos
 * leen su segundo operando, pero uno lo lee de un registro y el otro de
 * memoria. Y esa es justo la diferencia que decide si un bloque se puede mover,
 * borrar o reordenar, asi que no puede quedarse fuera.
 *
 * @param kind Sale con la clase del operando (@ref OP_REG, @ref OP_MEM,
 *             @ref OP_IMM...).
 */
bool explicit_operand(Isa isa, int32_t form_id, size_t idx, bool &reads,
                      bool &writes, DbOpKind &kind);

/**
 * @brief Si una forma LEE memoria y si la ESCRIBE, por separado.
 *
 * Los dos sentidos no son lo mismo y no se pueden colapsar en un "toca
 * memoria": una lectura de mas convierte un bloque inocente en una barrera para
 * todo lo que le rodea, y una escritura de menos deja pasar una optimizacion
 * que rompe.  Un `movdqa [rdi], xmm0` solo escribe.
 *
 * Y no se puede responder por MNEMONICO, que es lo que se intentaba: la misma
 * `movdqa` lee con `movdqa xmm0, [rdi]` y escribe con `movdqa [rdi], xmm0`.  Lo
 * que lo decide es la FORMA -- cual de sus operandos es el de memoria y en que
 * rol --, y eso ya esta modelado aqui: se DERIVA, no se declara.
 *
 * Cuando la forma toca memoria sin nombrarla en el texto (`push`, `pop`) se
 * responde que en los dos sentidos: es lo unico honesto sin saber cual, y se
 * queda del lado que no habilita transformaciones.
 *
 * @param reads  Sale a true si algun operando de memoria se lee.
 * @param writes Sale a true si alguno se escribe.
 * @return false si la forma no existe (no se afirma nada de ella).
 */
bool memory_of(Isa isa, int32_t form_id, bool &reads, bool &writes);

/**
 * @brief Si una forma LEE las banderas y si las ESCRIBE, por separado.
 *
 * Los dos sentidos son cosas distintas y colapsarlos pierde justo lo que hace
 * falta: un `add` ESCRIBE las banderas y un `setz` las LEE, y con un solo bit
 * las dos salen iguales.  Con el bit unico, mover un `cmp` por encima de un
 * `setz` parece igual de seguro que moverlo por encima de un `add`, y no lo es
 * -- el `setz` consume justo lo que el `cmp` produjo --.  Al reves, declarar
 * que un `setz` las escribe lo hace pasar por destructor de un valor que no
 * toca.
 *
 * @param reads  Sale a true si la forma lee las banderas.
 * @param writes Sale a true si las escribe.
 * @return false si la forma no existe (no se afirma nada de ella).
 */
bool flags_of(Isa isa, int32_t form_id, bool &reads, bool &writes);

/**
 * @brief QUE banderas lee y escribe una forma, por NOMBRE.
 *
 * @ref flags_of dice si toca alguna; esto dice cuales.  Es la diferencia entre
 * un `bt` -- que solo deja el acarreo -- y un `cmp` -- que deja las seis --, y
 * entre un `inc` y un `add`: `inc` no toca el acarreo, que es justo lo que
 * permite encadenarlo con un `adc`.  Sin ese detalle, cualquier instruccion que
 * toque banderas parece destruir el trabajo de cualquier otra.
 *
 * Los nombres son los de la ISA (`cf`, `zf`, `of` en x86; `n`, `z`, `c`, `v` en
 * ARM; ninguno en RISC-V, que no tiene banderas).
 *
 * @param reads  Sale con las banderas que la forma consume.
 * @param writes Sale con las que modifica.
 * @return false si la forma no existe, o si la base aun no trae el detalle por
 *         bandera para esa ISA (que no es lo mismo que no tocar ninguna).
 */
bool flag_names_of(Isa isa, int32_t form_id, std::vector<std::string> &reads,
                   std::vector<std::string> &writes);

/**
 * @brief Igual, pero por MNEMONICO, cuando no se ha podido resolver la forma.
 *
 * Se responde solo si TODAS las formas del mnemonico coinciden en que banderas
 * tocan.  Si discrepan, no se contesta: elegir una seria inventar cual de ellas
 * se escribio.  Es el mismo criterio que se usa para decir que rasgo del
 * procesador exige un mnemonico.
 *
 * Hace falta porque una linea no siempre casa con una forma concreta: un
 * `adds x0, x1, x2` tiene tres operandos y la forma con desplazamiento tiene
 * cuatro, asi que la aridad no cuadra y el emparejador se queda a nivel de
 * mnemonico.  Las banderas, sin embargo, son las mismas en las cuatro formas de
 * `adds`, asi que ahi si se puede afirmar.
 *
 * @return false si el mnemonico no existe, si sus formas discrepan, o si la
 * base no trae el detalle para esa ISA.
 */
bool flag_names_of_mnemonic(Isa isa, const std::string &mnemonic,
                            std::vector<std::string> &reads,
                            std::vector<std::string> &writes);

/**
 * @brief Si la base puede modelar una forma ELLA SOLA, sin ayuda de la tabla.
 *
 * Es la misma condicion que @ref asm_insn_sem aplica sobre una linea concreta,
 * pero preguntada sobre la FORMA: no vale si tiene operandos de registro
 * IMPLICITOS -- los que no se escriben en el texto, como el `rdx:rax` de una
 * `div` -- ni memoria implicita sin operando que la nombre.  En esos casos la
 * base sabe que existen pero no puede emparejarlos con lo que hay escrito, y
 * ahi es donde hace falta una entrada a mano.
 *
 * Sirve para saber DE DONDE sale cada respuesta.  Contar solo las de la tabla
 * mide la tabla, no lo que el compilador sabe: la mayoria de las instrucciones
 * las contesta la base sin que nadie las escriba, y las que no son justo las
 * que hay que escribir.  Sin separarlo, la lista de "lo que falta" mezcla
 * trabajo real con instrucciones que ya funcionan.
 *
 * @return false si la forma no existe o necesita una entrada a mano.
 */
bool form_is_modelable(Isa isa, int32_t form_id);

/**
 * @struct ImplicitOperand
 * @brief Un operando que la instruccion toca SIN escribirlo en el texto.
 */
struct ImplicitOperand {
    /// Registro en forma canonica (@c "rax", @c "rdx"...), vacio si lo que toca
    /// no es un registro general.
    std::string reg;
    /**
     * ESTADO del procesador que no es un registro general, en minusculas:
     * @c "cr0", @c "gdtr", @c "idtr", @c "msrs", @c "mxcsr", @c "rip",
     * @c "fsbase", @c "st(0)", @c "x87status"...
     *
     * Se nombra en vez de tratarse como un efecto opaco.  Son 28 en x86 -- y
     * ninguno en ARM ni RISC-V, donde todo canonicaliza --, y darles nombre es
     * lo que separa "esta instruccion toca algo que no se cual es" de "escribe
     * `gdtr`": lo primero obliga a no mover NADA a su alrededor, lo segundo
     * solo choca con quien toque `gdtr`.  Una `rdmsr` y una `stmxcsr` no se
     * estorban.
     */
    std::string state;
    bool is_memory = false; ///< el acceso implicito es a MEMORIA, no a un reg.
    bool reads = false;
    bool writes = false;
};

/**
 * @brief Los operandos IMPLICITOS de una forma, con su registro y su rol.
 *
 * Es lo que hacia falta para no escribir a mano las ~270 instrucciones de x86
 * que el analisis no conocia: TODAS estan en la base, y la base ya dice que
 * registro tocan -- que una `div` usa `rdx:rax`, que una `rdmsr` escribe
 * `eax:edx` y lee `ecx`, que una `pcmpestri` deja el resultado en `ecx` --.  Lo
 * unico que faltaba era leerlo.
 *
 * Escribirlas a mano habria sido copiar a otro sitio lo que ya estaba aqui, con
 * lo que eso trae: dos listas que se separan en cuanto una se quede atras.
 *
 * @param isa     ISA de la forma.
 * @param form_id Forma.
 * @return Los implicitos en el orden en que la forma los declara.  Vacio si la
 *         forma no existe o no tiene ninguno.
 */
std::vector<ImplicitOperand> implicit_operands(Isa isa, int32_t form_id);

/// Conjunto ISA que EXIGE una forma ("AVX512F_512", "AVX2"...), "" si no vale.
const char *isa_set_of(Isa isa, int32_t form_id);

/**
 * @brief Que RASGO del procesador exige un mnemonico, sin conocer sus
 *        operandos.
 *
 * Al recoger un fallo lo unico que hay es el mnemonico que desensamblo el
 * anfitrion; no hay una linea de asm que emparejar.  Se miran todas las formas
 * del mnemonico y se responde solo si TODAS coinciden en lo que exigen -- si un
 * mnemonico tiene formas SSE y formas AVX-512, decir una de las dos seria
 * inventar cual fallo.
 *
 * @param isa      ISA de la maquina.
 * @param mnemonic Mnemonico, en cualquier caja.
 * @return El rasgo comun (p.ej. "AVX512F"), o "" si el mnemonico no esta, sus
 *         formas exigen rasgos distintos, o lo que exige es el conjunto base.
 */
std::string requisito_de_mnemonico(Isa isa, const std::string &mnemonic);

/**
 * @brief Nombre del rasgo, a secas, a partir del conjunto ISA de una forma.
 *
 * El conjunto ISA lleva pegado el ancho con el que se codifico
 * (`AVX512F_512`, `AVX512BW_128`, `AVX512F_SCALAR`), que es un detalle del
 * encoding y no un rasgo que ningun procesador declare por separado: quien
 * tiene AVX512F lo tiene para los tres anchos.  Esto devuelve `AVX512F`.
 *
 * @param isa_set Conjunto ISA de una forma.
 * @return El nombre sin el ancho.  Cadena vacia si el conjunto es el base
 *         (`I86`, `I386`, `LONGMODE`, `-`): eso no es un rasgo que falte.
 */
std::string nombre_de_rasgo(const std::string &isa_set);
/// Numero de formas de la ISA.
uint32_t form_count(Isa isa);

} // namespace instr_db
} // namespace vx

#endif // VX_INSTR_DB_H
