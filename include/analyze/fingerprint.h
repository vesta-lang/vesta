/**
 * @file fingerprint.h
 * @brief Huella computacional por-declaracion: propiedades de recurso y efecto
 *        que el compilador INFIERE del IR y compone interprocedural.
 *
 * Es el "resumen por funcion" del modelo de codegen dirigido por resumenes
 * (ThinLTO-style) Y, a la vez, la base de las anotaciones comprobables (`@pure`,
 * `@alloc(0)`, `@nothrow`, `@stack(N)`, ...): la huella es la VERDAD inferida
 * contra la que se verifica el contrato del usuario.
 *
 * Decidibilidad (regla de soundness): estas propiedades son EXACTAS/sound sobre
 * el IR (contar alloc-sites, sumar ALLOCA, detectar THROW/PANIC, ciclos del
 * callgraph).  La complejidad temporal/espacial (aproximada) vive en el
 * subsistema de coste (@c analyze/bigo.h) y se reporta junto a esta huella.
 *
 * Composicion interprocedural: los totales (`*_total`) agregan la funcion + su
 * cierre transitivo por el callgraph ESTATICO.  Si en ese cierre hay una
 * llamada DINAMICA/externa no resuelta (@c CALLVIRT / @c CALLN a simbolo
 * externo / ...), @c effects_known queda false y los totales de efecto se
 * vuelven CONSERVADORES (no se puede probar la ausencia) -> nunca se afirma
 * `@nothrow`/`@alloc(0)` sin poder demostrarlo.
 */
#ifndef VESTA_ANALYZE_FINGERPRINT_H
#define VESTA_ANALYZE_FINGERPRINT_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ir {
struct IrFunction;
struct IrModule;
} // namespace ir

namespace analyze {

/// Sentinela de `stack_bytes_total`: la profundidad de pila NO es acotable
/// (hay recursion en el callgraph, o un callee externo cuyo frame no se ve).
/// `verify` lo trata como inverificable (no se puede PROBAR una cota).
constexpr uint64_t STACK_UNBOUNDED = UINT64_MAX;

/**
 * @struct FunctionFingerprint
 * @brief Propiedades de recurso/efecto de una funcion (locales + compuestas).
 */
struct FunctionFingerprint {
    std::string function; ///< nombre de la funcion.

    // -- Locales (solo esta funcion, sin componer) --------------------------
    uint32_t alloc_sites = 0;      ///< sitios de alloc en heap (GC/raw/newobj/closure-GC).
    uint64_t stack_bytes = 0;      ///< bytes reservados por ALLOCA (count * sizeof T).
    bool throws = false;           ///< THROW/RETHROW propio.
    bool panics = false;           ///< PANIC propio.
    bool self_recursive = false;   ///< se llama a si misma directamente.
    bool frame_opaque = false;     ///< tiene `asm { }` (INLINE_ASM): su marco de
                                   ///< pila REAL no se ve en el IR (los register()
                                   ///< + asm no son ALLOCAs).  Para el TOTAL de sus
                                   ///< callers se usa su @stack declarado, no el 0
                                   ///< medido.
    bool has_dynamic_call = false; ///< CALLVIRT/CALLM/CALLCLOSURE/CALLIND (efecto opaco).
    bool pure_local = true;        ///< sin efectos de dato observables PROPIOS.
    std::vector<std::string> calls; ///< callees ESTATICOS (CALL/TAILCALL/CALLN).

    // -- Compuestas (transitivas, tras compose_fingerprints) ----------------
    uint32_t alloc_sites_total = 0; ///< sitios de alloc alcanzables (SUMA del cierre).
    uint64_t stack_bytes_total = 0; ///< profundidad de pila peor caso = frame propio
                                    ///< + MAX de callees; STACK_UNBOUNDED si no acotable.
    bool throws_total = false;      ///< la funcion o alguna alcanzable lanza.
    bool panics_total = false;      ///< idem panic.
    bool recursive = false;         ///< en un ciclo del callgraph (o self).
    bool effects_known = true;      ///< false => hay dinamica/externa -> totales conservadores.
    bool pure = false;              ///< @pure: sin efectos de dato en TODO el cierre (sound).
};

/**
 * @brief Computa la huella LOCAL de una funcion (sin componer).
 */
FunctionFingerprint compute_fingerprint(const ir::IrFunction &fn);

/**
 * @brief Huellas locales de todas las funciones del modulo.
 */
std::vector<FunctionFingerprint>
compute_module_fingerprints(const ir::IrModule &mod);

struct FunctionContracts; // definido abajo.

/**
 * @brief Compone los totales interprocedurales in-place: llena los campos
 *        `*_total`, `recursive` y `effects_known` recorriendo el callgraph
 *        estatico (cierre transitivo).  Conservador ante llamadas dinamicas
 *        o callees externos no presentes en @p fps.
 *
 * @param contracts opcional.  Si se da, las funciones con marco OPACO
 *        (`frame_opaque`: tienen `asm { }`, su pila no se ve en el IR)
 *        contribuyen al `stack_bytes_total` de sus callers con su @stack
 *        DECLARADO en vez del 0 medido -- asi un wrapper que llama a una
 *        primitiva de asm refleja el marco real de esa primitiva.  El
 *        `stack_bytes` (parcial) medido NO se toca (la verificacion sigue
 *        siendo por cota superior sobre lo medido).
 */
void compose_fingerprints(
    std::vector<FunctionFingerprint> &fps,
    const std::unordered_map<std::string, FunctionContracts> *contracts =
        nullptr);

/**
 * @struct FunctionContracts
 * @brief Contratos de huella declarados por el usuario para UNA funcion.
 *
 * Se llevan APARTE del IR (no en @c IrFunction) por DISENO: son metadata
 * puramente de compile-time (modo @c --analyze) que ni el JIT ni el AOT ni la
 * serializacion del IR necesitan; mantener @c IrFunction esbelto evita
 * hincharlo.  Viajan en el @c CompileResult (una instancia, sin serializar) y
 * se verifican por NOMBRE.
 */
struct FunctionContracts {
    bool pure = false;    ///< @pure.
    bool nothrow = false; ///< @nothrow.
    bool nopanic = false; ///< @nopanic.
    // @alloc y @stack tienen DOS dimensiones (como @complexity): parcial (lo
    // propio de la funcion) y total (el cierre / la cadena de callees).  La
    // forma corta `@alloc(N)`/`@stack(N)` es azucar del TOTAL (el peor caso
    // que importa desde fuera).  -1 = esa dimension no se declaro.
    int64_t alloc_partial = -1; ///< @alloc(partial: N).
    int64_t alloc_total = -1;   ///< @alloc(total: N) o `@alloc(N)`.
    int64_t stack_partial = -1; ///< @stack(partial: N).
    int64_t stack_total = -1;   ///< @stack(total: N) o `@stack(N)`.
    bool any() const {
        return pure || nothrow || nopanic || alloc_partial >= 0 ||
               alloc_total >= 0 || stack_partial >= 0 || stack_total >= 0;
    }
};

/**
 * @struct ContractCheck
 * @brief Resultado de verificar UN contrato de huella declarado por el usuario
 *        (@pure/@nothrow/@nopanic/@alloc(N)/@stack(N)) contra la huella inferida.
 */
struct ContractCheck {
    enum Status {
        OK,           ///< probado que se cumple (verde).
        VIOLATED,     ///< probado que NO se cumple (rojo, error de compilacion).
        UNVERIFIABLE, ///< no se pudo decidir (efectos desconocidos) -> ni si ni no.
    };
    std::string function;
    std::string contract; ///< "@pure", "@nothrow", "@alloc", ...
    Status status = OK;
    std::string detail; ///< "esperado vs inferido".
};

/**
 * @brief Verifica los contratos de huella declarados en @p mod contra las
 *        huellas @p fps (ya compuestas).  SOUND/ASIMETRICO: solo marca
 *        @c VIOLATED cuando la violacion es DEMOSTRABLE; si los efectos no se
 *        conocen del todo, marca @c UNVERIFIABLE (nunca un falso VIOLATED).
 *        @p fps debe estar alineado con @p mod.functions por nombre.
 */
std::vector<ContractCheck> verify_contracts(
    const std::vector<FunctionFingerprint> &fps,
    const std::unordered_map<std::string, FunctionContracts> &contracts);

/**
 * @struct TypeFingerprint
 * @brief Huella de un TIPO agregado (struct / clase / enum): propiedades de
 *        layout y de recurso que el compilador INFIERE de sus campos.
 *
 * INVARIANTE DEL LENGUAJE: TODO @c struct tiene layout C-compatible SIEMPRE
 * (orden de campos + alineamiento natural + padding estilo C, sin necesidad de
 * `@repr(C)`).  Por tanto "tener layout C" NO es una propiedad que se contrate
 * (es universal).  Lo que SI varia -- y por eso se contrata -- es si el tipo es
 * @pod: TRIVIALMENTE copiable/pasable a C POR VALOR, es decir sin destructor
 * `~Tipo()` ni campos gestionados (@c unique<T> / @c shared<T> / @c string /
 * referencias de clase).  Un struct con un campo gestionado o un dtor conserva
 * su layout C pero NO es @pod (carril move-only + RAII).
 *
 * Se computa a partir del LAYOUT ya resuelto (tamano, alineamiento, tipos de
 * campo) y de la composicion sobre los CAMPOS: @pod sii todos sus campos son
 * C-representables por valor y no hay destructor; @no_heap sii ningun campo
 * referencia el heap gestionado.  Son propiedades EXACTAS/sound (decidibles del
 * layout), asi que su contrato es DURO (OK / VIOLATED, sin UNVERIFIABLE).
 */
struct TypeFingerprint {
    std::string type_name;
    enum Kind { STRUCT, CLASS, ENUM } kind = STRUCT;
    uint64_t size_bytes = 0;    ///< tamano total del tipo (con padding).
    uint32_t align_bytes = 1;   ///< alineamiento requerido.
    uint32_t field_count = 0;   ///< numero de campos de instancia.
    bool is_pod = false;        ///< value-type C-representable, sin dtor ni campos gestionados.
    bool no_heap = false;       ///< ningun campo referencia el heap gestionado (GC/string/smart-ptr).
    bool has_destructor = false; ///< declara `~Tipo()` (o algun campo destructible).
    bool is_reference = false;  ///< tipo por referencia (clase): vive en el heap por naturaleza.
};

/**
 * @struct TypeContracts
 * @brief Contratos de layout/recurso declarados por el usuario sobre un TIPO
 *        (`@pod`, `@no_heap`, `@size(N)`).  Se llevan en el @c CompileResult
 *        (compile-time, sin serializar) y se verifican por NOMBRE de tipo.
 */
struct TypeContracts {
    bool pod = false;     ///< @pod: value-type sin recursos gestionados ni dtor.
    bool no_heap = false; ///< @no_heap: ningun campo apunta al heap gestionado.
    int64_t size = -1;    ///< @size(N): tamano exacto en bytes; -1 = no declarado.
    bool any() const { return pod || no_heap || size >= 0; }
};

/**
 * @brief Verifica los contratos de TIPO @p contracts contra las huellas de tipo
 *        @p fps.  Todas las propiedades son decidibles del layout -> solo
 *        produce @c OK o @c VIOLATED (nunca @c UNVERIFIABLE).
 */
std::vector<ContractCheck> verify_type_contracts(
    const std::vector<TypeFingerprint> &fps,
    const std::unordered_map<std::string, TypeContracts> &contracts);

} // namespace analyze

#endif // VESTA_ANALYZE_FINGERPRINT_H
