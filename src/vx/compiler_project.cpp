/**
 * @file compiler_project.cpp
 * @brief Compilador multi-modulo ( M.2.e).
 *
 * Detecta @c import declaraciones en el fichero raiz, construye el dep
 * graph via @c ModuleGraph, ordena topologicamente, compila cada modulo
 * en orden (con .vxi compartido cross-modulo) y mergea las IrFunctions
 * en un solo @c IrModule final que el emitter produce como un unico
 * @c .vel.  El linker se encarga (M5 futuro) de empaquetar al .velb.
 *
 * Diseno:
 *   - Reusa @c ModuleGraph (M1) para paths + topo + ciclos.
 *   - Reusa @c export_typechecker_to_vxi (M2.d) + @c vxi_emit (M2.c)
 *     para producir las interfaces.
 *   - Reusa @c import_vxi_into_typechecker (M2.d) para inyectar en
 *     consumidores.
 *   - Cada modulo del path se compila con su propio @c TypeChecker y
 *     @c Lowering, produciendo un @c IrModule local.  Al final, todos
 *     los modulos se mergean en uno solo.
 *
 * MVP (M2.e):
 *   - Solo @c "only" imports inyectan simbolos (alias/namespace son M2.x).
 *   - Las @c IrFunctions de cada dep se appendea al IrModule final.
 *   - El @c static_data y los @c globals tambien se merge.
 *   - Sin checks de colision de nombres cross-module todavia (M5).
 */

#include "util/cache_paths.h"  // el reparto de la cache por tipo y alcance
#include "util/crash_report.h" // dejar dicho QUE modulo se esta compilando
#include "util/fnv.h"          // la semilla y el primo, en UN sitio
#include "util/file_read.h"

/* El ensamblador, DECLARADO y no incluido.  Su cabecera arrastra `windows.h`,
 * que define `VOID` como macro y hace que el `void` del enum de tipos de Vesta
 * deje de compilar.  Se declara lo que se usa y punto. */
namespace emmit {
class NodeStream;
}
/* Declarada a mano y no por su cabecera: `util/assembler_multiprocess.h`
 * arrastra aqui definiciones que chocan con `vx/types.h`.  El riesgo conocido
 * de repetir una firma es que diverja, y ya paso -- esta se quedo pidiendo un
 * `std::vector<uint8_t>` cuando el intermedio dejo de viajar asi --, pero al
 * menos eso no compila en vez de colar. */
#include "util/byte_buffer.h"
namespace asm_multi_process {
int run_worker_from_source(std::string code, const std::string &file_name,
                           const std::string &output_prefix,
                           bool skip_preprocessor, bool keep_labels,
                           const util::ByteBuffer *ir_section_bytes,
                           bool emit_map, emmit::NodeStream *nodes,
                           const std::string &debug_source_file);
} // namespace asm_multi_process
#include "vx/comptime/comptime_collect.h"
#include "vx/borrow/borrow_ir_check.h" // la exclusividad, cruzando la llamada
#include "vx/compiler.h"
#include "vx/contracts_collect.h" // lo que el programa DECLARA
#include "vx/helper_override.h"   // que ayudantes admiten sustituto
#include "vx/module_checks.h" // lo que se comprueba antes de optimizar
#include "vx/project/module_cache_key.h" // cuando un artefacto guardado sirve
#include "vx/project/module_work.h" // lo que se lleva de cada modulo
#include "vx/project/module_imports.h" // que importa cada modulo, y a quien
#include "vx/project/module_artifact.h" // el artefacto en cache y su adopcion
#include "vx/project/module_names.h" // los simbolos derivados del modulo
#include "vx/project/root_weaving.h" // lo que el raiz teje en todos
#include "vx/project/vxdbg_artifact.h" // el mapa del artefacto en el grafo
#include "vx/unit/unit_results.h" // lo de cada modulo, al resultado
#include "vx/unit/compile_unit.h" // compilar UN modulo del proyecto
#include "ir/synthetic_symbols.h" // la familia de `__module_init`
#include "ir/runtime_symbols.h"   // lo que aporta el runtime, y su orden
#include "vx/project/module_paths.h" // donde van sus ficheros
#include "vx/source_text.h"   // un solo fin de linea para todo el pipeline
#include "vx/vxdbg_emit.h"    // grafo de conocimiento del programa
#include "vxdbg/pack_store.h"
#include "vxdbg/codec.h"
#include "vxdbg/roots.h"
#include "analysis/facts/alignment.h"    // de cuanto es multiplo un valor
#include "analysis/facts/asm_bindings.h" // de que valor habla un operando de asm
#include "analyze/fingerprint.h"         // verificacion de contratos
#include "vx/asm/asm_effects.h"          // que exige cada instruccion
#include "vx/asm/asm_phys_reg.h" // clases de registro (ancho de cada operando)
#include "vx/incremental.h" // CAS global direccionado por contenido (cross-proyecto)
#include <algorithm>        // UCRT64: no transitivo
#include <chrono>           // reparto del coste por fase
#include <unordered_set>

#include "ir/ir_emitter.h"
#include "analysis/escape/fn_addr_escape.h" // que direcciones tienen que ser reales
#include "ir/ir_optimizer.h"
#include "ir/module_spill.h" // bajar los cuerpos a disco mientras no hablan
#include "ir/parallel_for.h"
#include "util/env_flags.h"
#include <climits>
#include "util/alloc/host_allocator.h" // san_mark, para nombrar la primera fase
#include "util/phase_memory.h"         // la frontera entre fases
#include "util/alloc/sanitizer.h" // marcar las fases en el eje del comprobador
#include "util/crono_tramo.h"
#include "analysis/asa/aggregate_facts.h"
#include "analysis/effects/bounds.h" // accesos fuera de region -> diagnostico
#include "vx/diag/diag_format.h"
#include "ir/passes/select_policy.h"
#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include "vx/lexer.h"
#include "vx/lowering.h"
#include "vx/module/module_interop.h"
#include "vx/module/module_resolver.h"
#include "vx/module/namespace_flatten.h" // NS.2: flatten inline namespaces por modulo
#include "vx/module/namespace_names.h" // la forma fisica de un nombre con ruta
#include "vx/parser.h"
// IMPORTANTE: incluir los headers de diagramas DESPUES de parser.h / lowering.h
// para que la fwd decl @c namespace ast { struct ModuleNode; } del header
// resuelva correctamente al tipo @c vx::ast::ModuleNode ya conocido en
// este punto.  De otra forma el compilador interpreta @c ast::ModuleNode
// como @c ::ast::ModuleNode (global), causando mismatch de tipos.
#include "vx/diagram/graphviz_diagrams.h"
#include "vx/diagram/html_diagrams.h"
#include "vx/diagram/mermaid_diagrams.h"
#include "vx/type_checker.h"
#include "pkg/manifest.h" // las dependencias DECLARADAS del proyecto
#include "vx/module/vxi_format.h"
#include "vx/source_hash.h" // la identidad de un fuente son sus tokens
#include "analysis/asa/fact_file.h"
#include "analysis/asa/producers.h"     // produce() + FactStore::find
#include "analysis/facts/value_range.h" // soltar la memoizacion de rangos
#include "util/fs_utils.h"              // fs::get_executable_path()

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

//  M5.A: cabeceras para PID + atomic rename portable.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h define VOID como macro -> colisiona con PrimitiveKind::VOID que
// usamos en la validacion de firma de @HelperOverride.  Lo deshacemos aqui
// (este TU no usa el macro VOID de WinAPI).
#ifdef VOID
#undef VOID
#endif
#else
#include <unistd.h>
#endif

namespace vx {

namespace {

/**
 * @brief Guarda junto al modulo lo que el ASA supo de el AL BAJARLO.
 *
 * Existe porque ese conocimiento no se puede recalcular mirando el modulo ya
 * compilado: nacio antes, mientras se bajaba el codigo.  Sin esto, la primera
 * compilacion avisa de algo y la segunda -- con la cache caliente -- se calla,
 * y el aviso acaba dependiendo de si alguien borro un directorio.
 *
 * Se escribe junto al @c .vxi y al @c .vxir, con la misma identidad que ellos:
 * la huella del modulo dice de que programa habla, y la del compilador que
 * analisis lo concluyo.
 *
 * @param ruta       Donde (de @c RutasCache::hechos, ya calculada).
 * @param almacen    Hechos a guardar.
 * @param huella     Identidad del modulo.
 * @param costes     Lo que cada dominio dice de si mismo (coste y de que
 *                   depende), con lo que se decide que merece guardarse.
 * @return @c true si quedo algo escrito.
 */
static bool
guardar_hechos_(const std::string &ruta,
                const analysis::asa::FactStore &almacen, uint64_t huella,
                const std::vector<analysis::asa::DomainCost> &costes) {
    const std::vector<uint8_t> bytes =
        analysis::asa::serialize(almacen, huella, analysis::asa::cache_level(),
                                 costes, compiler_fingerprint());
    if (bytes.empty()) return false;
    return ::fs::write_file_atomic(ruta, bytes);
}

/**
 * @brief Recupera lo guardado por @ref guardar_hechos_.
 *
 * Que no haya nada NO es un error: es la primera compilacion, o alguien limpio
 * la cache, o el modulo cambio.  El motivo viaja dentro del resultado para que
 * quien quiera pueda contarlo -- callarse por que no se sabe algo es lo unico
 * que el ASA no puede hacer.
 *
 * @param ruta     De donde.
 * @param destino  Donde se depositan (se AÑADEN a lo que ya haya).
 * @param huella   La que debe llevar dentro.
 * @param vigentes De que depende hoy cada dominio, para anular solo lo suyo.
 * @return Que se leyo, o por que no.
 */
static analysis::asa::ReadResult
recuperar_hechos_(const std::string &ruta, analysis::asa::FactStore &destino,
                  uint64_t huella,
                  const std::vector<analysis::asa::DomainCost> &vigentes) {
    return analysis::asa::read_facts_file(ruta, huella, destino, vigentes,
                                          compiler_fingerprint());
}

/**
 * @brief Deja en @p store los hechos de @p wanted, vengan de donde vengan.
 *
 * Es la puerta que usan los consumidores, y esconde a proposito de DoNDE sale
 * el conocimiento: de la cache de una compilacion anterior, o producido ahora.
 * Un consumidor que tuviera que saberlo acabaria decidiendo por su cuenta
 * cuando fiarse de la cache, y esa decision estaria escrita en tantos sitios
 * como consumidores haya.
 *
 * El orden es el que ahorra trabajo: primero se LEE lo guardado -- que marca
 * sus dominios en el almacen --, y solo despues se produce, que se salta lo que
 * ya esta.  Si no habia nada, se produce todo y se guarda para la proxima.
 *
 * Que no haya cache NO es un error: es la primera compilacion, o el modulo
 * cambio, o alguien la limpio.  El motivo viaja dentro del resultado.
 *
 * @param mod         Modulo del que se sabe.
 * @param store       Almacen de esta compilacion.
 * @param wanted      Dominios que alguien va a consultar.  Vacio = todos.
 * @param path        Fichero de hechos, o vacio para no tocar disco.
 * @param fingerprint Identidad del modulo: si no coincide, lo guardado no vale.
 */
/// Pidio quien compila hechos en ese MOMENTO?  Delega en la peticion, que es
/// donde vive la pregunta: escrita aqui Y en el camino de fichero suelto, los
/// dos podian acabar contestando distinto al mismo modulo.
bool wants_stage_(const CompileOptions &opts, const char *stage) {
    return opts.asa.wants_stage(stage);
}

/**
 * @brief Lo que el manifiesto del proyecto de @p source_path dice de la cache
 *        de analisis, o 0 si no dice nada.
 *
 * Se recuerda por RUTA: el manifiesto no cambia a mitad de compilacion y
 * parsearlo por modulo seria releerlo decenas de veces para la misma respuesta.
 *
 * Con cerrojo: el memo es del PROCESO, y el servidor de lenguaje compila
 * documentos desde varios hilos a la vez.
 */
uint32_t analysis_unused_runs_for_(const std::string &source_path) {
    static std::mutex memo_mtx;
    static std::unordered_map<std::string, uint32_t> memo;
    std::lock_guard<std::mutex> lk(memo_mtx);
    auto it = memo.find(source_path);
    if (it != memo.end()) return it->second;

    uint32_t runs = 0;
    std::string manifest_path;
    (void)derive_package_id(source_path, &manifest_path);
    if (!manifest_path.empty()) {
        const pkg::ParseResult pr = pkg::parse_manifest_file(manifest_path);
        /* Un manifiesto que no parsea NO es motivo para fallar aqui: quien lo
         * valida es el gestor de paquetes, con su mensaje.  Esto solo queria un
         * ajuste, y sin el se usa el defecto. */
        if (pr.ok) runs = pr.manifest.cache.analysis_unused_runs;
    }
    memo.emplace(source_path, runs);
    return runs;
}

std::string asa_analysis_path_for(const std::string &facts_path) {
    if (facts_path.empty()) return facts_path;
    /* Mismo NOMBRE que el de hechos, otro cajon.  El nombre comun es lo que
     * ata los dos -- quien mire los dos directorios ve de un vistazo cual va
     * con cual --, y el cajon aparte es lo que permite tirar los analisis sin
     * tocar los hechos: son cosas distintas, uno es lo que se supo y el otro
     * lo que costo averiguarlo. */
    namespace fs = std::filesystem;
    return (fs::path(util::cache_dir(util::CacheKind::Analysis)) /
            (fs::path(facts_path).filename().string() + ".analysis"))
        .string();
}

std::vector<analysis::asa::ProductionSummary>
ensure_facts_impl_(const ir::IrModule &mod, analysis::asa::FactStore &store,
                   const std::vector<const char *> &wanted,
                   const std::string &path, uint64_t fingerprint,
                   const char *stage, const std::string &source_path) {
    /* Sin ruta no hay cache entre compilaciones: se produce y ya.  Pasa en los
     * caminos que no tienen un fichero al que atribuir el modulo. */
    if (path.empty()) return analysis::asa::produce(mod, store, wanted, stage);

    /* Lo de antes, validado POR DOMINIO con lo que hoy depende cada uno.
     *
     * Antes esto iba vacio y la invalidacion era todo-o-nada: una sola huella,
     * la del modulo, decidia por todos, asi que tocar una linea de una funcion
     * tiraba tambien los hechos que no dependen del codigo.  El formato ya
     * contemplaba lo contrario -- guarda una huella por registro --; lo que
     * faltaba era quien la calculara.
     *
     * El dominio que no sepa decirlo no sale de `current_inputs`, y entonces lo
     * suyo se acepta sin comprobar, que es lo de siempre.  Asi la cache se
     * vuelve granular de uno en uno, segun cada dominio aprenda a responder. */
    /* De que depende HOY cada dominio, y ademas funcion a funcion.  Se calcula
     * UNA vez y sirve para las dos puntas -- validar lo que se lee y sellar lo
     * que se escribe --: son la misma cuenta, y hacerla dos veces seria otro
     * recorrido del modulo entero. */
    std::vector<analysis::asa::DomainCost> keys =
        analysis::asa::current_inputs(mod);

    /* Al LEER, la via granular solo se le ofrece a los dominios que despues se
     * van a producir.  Es la condicion que la hace correcta: una carga parcial
     * deja adrede las funciones que cambiaron sin hechos, contando con que el
     * productor las rehaga.  Si ese dominio no esta pedido, nadie las rehace --
     * y al reescribir el fichero se sellaria como completo, con lo que la
     * proxima compilacion daria por bueno un agujero.  Sin tabla por funcion la
     * lectura vuelve a ser todo-o-nada, que es lo de siempre: correcto. */
    std::vector<analysis::asa::DomainCost> read_keys = keys;
    if (!wanted.empty())
        for (analysis::asa::DomainCost &c : read_keys) {
            bool asked = false;
            for (const char *w : wanted)
                if (w != nullptr && c.domain != nullptr &&
                    std::strcmp(w, c.domain) == 0) {
                    asked = true;
                    break;
                }
            if (!asked) c.by_function.clear();
        }
    const analysis::asa::ReadResult read =
        recuperar_hechos_(path, store, fingerprint, read_keys);

    /* Y se puede MIRAR lo que hizo la cache.  Sin esto el mecanismo no se
     * distingue de uno roto: los dos compilan igual, y este ya estuvo mal
     * mucho tiempo sin que nada lo dijera.  Lo importante son las tres ultimas
     * cifras -- son las que dicen si la granularidad por funcion ahorra algo o
     * es contabilidad. */
    static const bool log_cache = util::flag_on(util::FlagId::AsaFactsDebug);
    if (log_cache) {
        /* Por el CATALOGO: que sea una traza de depuracion no la saca de la
         * regla, porque la lee una persona.  Aqui solo van los DATOS. */
        const std::string msg = vx::diag::format(
            "VXA074", vx::diag::current_language(),
            {path, stage != nullptr ? stage : "",
             /* El CoDIGO del motivo, que es consultable y no hay que
              * traducirlo.  Vacio cuando se leyo bien: una palabra suelta como
              * "ok" seria texto de usuario escrito aqui, que es justo lo que no
              * se hace. */
             std::string(analysis::asa::diag_code(read.reason)),
             std::to_string(read.facts), std::to_string(read.domains),
             std::to_string(read.stale), std::to_string(read.skipped),
             std::to_string(read.corrupt), std::to_string(read.partial_domains),
             std::to_string(read.stale_facts),
             std::to_string(read.reused_functions)});
        std::fprintf(stderr, "%s\n", msg.c_str());
    }

    /* El almacen de ANALISIS, que es la otra mitad: los hechos que se acaban de
     * leer son las CONCLUSIONES, y esto guarda el RAZONAMIENTO con el que se
     * sacan -- def-use, points-to, rangos --.  Sin el, lo que la cache de
     * arriba no cubra se rehace entero.
     *
     * Misma vida que la produccion: se abre antes (UNA lectura) y se vuelca
     * despues (UNA escritura).  La clave lleva la misma capa de configuracion
     * que los hechos de este momento, para que dos niveles de optimizacion no
     * se sirvan analisis el uno al otro. */
    analysis::AnalysisStore analyses(compiler_fingerprint(), fingerprint);
    /* Y lo que el PROYECTO diga de sus caches: cuanto aguanta un analisis
     * guardado sin que se lo pidan.  Cero deja el defecto. */
    analyses.set_unused_runs(analysis_unused_runs_for_(source_path));
    analyses.open(asa_analysis_path_for(path));

    /* Y lo que falte.  `producir` se salta los dominios que la lectura ya
     * marco, asi que esto es exactamente el trabajo que la cache no cubrio. */
    const std::vector<analysis::asa::ProductionSummary> summaries =
        analysis::asa::produce(mod, store, wanted, stage, &analyses);
    analyses.flush();
    analyses.dump_if_asked();
    // Todo vino de la cache: nada que guardar, y nada que contar tampoco.
    if (summaries.empty()) return summaries;

    /* Lo que se va a escribir sale de `keys`, no de los resumenes, y la
     * diferencia era un fallo MUDO: los resumenes solo traen los dominios que
     * ACABAN de producirse, asi que uno servido entero desde la cache se
     * reescribia con huella cero -- "no se puede comprobar" --.  A la segunda
     * compilacion su registro se aceptaba pasara lo que pasara: la validacion
     * granular se borraba sola en cuanto acertaba una vez, sin dar ningun
     * error y pareciendo que la cache iba de maravilla. */
    for (analysis::asa::DomainCost &c : keys)
        for (const analysis::asa::ProductionSummary &r : summaries)
            if (r.domain != nullptr && c.domain != nullptr &&
                std::strcmp(r.domain, c.domain) == 0) {
                /* Lo que costo, para que el nivel de cache decida que merece ir
                 * a disco: un dominio que se produce en 3 us no compensa. */
                c.micros = r.micros;
                break;
            }
    /* Y los que no saben decir de que dependen tampoco salen de `keys`, asi que
     * se anaden con su coste: se guardan igual, solo que sin poder validarse.
     */
    for (const analysis::asa::ProductionSummary &r : summaries) {
        bool known = false;
        for (const analysis::asa::DomainCost &c : keys)
            if (c.domain != nullptr && r.domain != nullptr &&
                std::strcmp(c.domain, r.domain) == 0) {
                known = true;
                break;
            }
        if (known) continue;
        analysis::asa::DomainCost c;
        c.domain = r.domain;
        c.micros = r.micros;
        c.fingerprint = r.fingerprint;
        keys.push_back(c);
    }
    guardar_hechos_(path, store, fingerprint, keys);
    return summaries;
}

/// Lee el fichero a string.  Devuelve cadena vacia en error (el caller
/// detecta el error via @c diags).
///
/// Los fines de linea se normalizan aqui, en la PUERTA: de ahi en adelante todo
/// el pipeline ve `\n` y nadie mas tiene que acordarse (ver @ref
/// vx::leer_fuente).
std::string read_source_(const std::string &path) {
    std::string s;
    if (!vx::leer_fuente(path, s)) return {};
    return s;
}

/**
 * @brief El techo de intermedio vivo, en bytes.  0 = sin techo.
 *
 * Se pregunta UNA vez: es una variable de entorno y no cambia a mitad de una
 * compilacion.
 *
 * @return Bytes, o 0 si no se puso.
 */
size_t ir_ram_ceiling_bytes() {
    static const size_t ceiling = [] {
        const long mib = util::flag_int(util::FlagId::IrRamMaxMib, 0);
        if (mib <= 0) return size_t{0};
        return static_cast<size_t>(mib) * 1024u * 1024u;
    }();
    return ceiling;
}

/**
 * @brief Baja a disco los cuerpos de modulos ya compilados hasta caber.
 *
 * Se llama donde NO hay nadie compilando -- el camino secuencial entre modulo
 * y modulo, y el paralelo tras la barrera de cada lote --, asi que lee el
 * estado de los otros modulos sin candado y sin carrera.
 *
 * A quien desaloja: al que lleva mas tiempo quieto, que en orden topologico es
 * el de indice menor.  Nunca al raiz -- sus cuerpos los sigue mirando la
 * emision -- ni a uno que no llego a compilar.
 *
 * Y no desaloja porque si: solo mientras lo vivo pase del techo.  Un proyecto
 * que cabe no paga ni una escritura ni una lectura.
 *
 * @param work    Todos los modulos.
 * @param live    Bytes de intermedio residentes.  Se actualiza.
 * @param diags   Donde avisar si un modulo no se pudo bajar.
 * @param verbose Si contar lo que se baja.
 */
/**
 * @brief TEMPORAL: cuenta lo que tiraria un barrido de alcanzabilidad.
 *
 * NO BORRA NADA.  Borrar codigo es de las cosas que no se pueden equivocar a
 * medias, asi que primero se mide el premio y despues se decide si merece el
 * riesgo.  Esta funcion es el "primero".
 *
 * COMO CUENTA UNA REFERENCIA, y es lo unico delicado.  No mira QUE opcode es:
 * cualquier instruccion cuyo `func_name` nombre a una funcion definida cuenta
 * como que la alcanza.  Enumerar los opcodes de llamada seria mas fino y
 * dejaria fuera las formas de nombrar una funcion que no son una llamada -- la
 * direccion tomada, la entrada de una tabla de metodos, lo que registra
 * `__module_init` --, y olvidarse de una convertiria a una funcion VIVA en
 * "inalcanzable".  Para contar, pasarse por conservador solo hace el numero mas
 * pequeno; quedarse corto lo hace mentiroso.
 *
 * LAS SEMILLAS: alcanzables por definicion y no porque alguien las llame.
 *   - el punto de entrada;
 *   - todo lo `public` -- y con ello lo `internal`, porque el intermedio NO
 *     distingue las dos (ver `IrFunction::is_public`).  De ahi que se cuenten
 *     aparte: la diferencia entre los dos numeros es exactamente lo que se
 *     ganaria trayendo `is_internal` hasta aqui;
 *   - `__module_init`, que es por donde se registran las clases;
 *   - los stubs nativos, y lo que tiene seccion fija o no lleva prologo, que se
 *     referencian desde una tabla y no desde una llamada.
 *
 * @param mod El modulo fundido, ya optimizado.
 */
/// Un recuento del barrido: cuantas funciones quedan fuera y cuanto pesan.
struct ReachTally {
    size_t seeds = 0;       ///< Cuantas se dieron por alcanzables de entrada.
    size_t dead = 0;        ///< Cuantas no alcanzo nadie.
    size_t dead_instrs = 0; ///< Y cuantas instrucciones suman.
    size_t all_instrs = 0;  ///< De cuantas en total.
    /// Unas cuantas por su nombre, para poder MIRAR si el cierre se dejo una
    /// via de alcanzar.  Un porcentaje no delata eso; un nombre si.
    std::vector<std::string> names;
};

/**
 * @brief El cierre desde las semillas, y lo que queda fuera.
 *
 * @param seed_public Si las `public` (y con ellas las `internal`, que el
 *                    intermedio no distingue) cuentan como semilla.  Correrlo
 *                    con y sin es lo que mide QUE aportaria distinguirlas: con
 *                    el paquete entero delante las llamadas de una `internal`
 *                    se ven TODAS, asi que no tendria por que ser semilla.
 */
ReachTally
reach_from_seeds(const ir::IrModule &mod,
                 const std::unordered_map<std::string, size_t> &by_name,
                 bool seed_public) {
    ReachTally t;
    std::vector<uint8_t> reached(mod.functions.size(), 0);
    std::vector<size_t> pending;

    for (size_t i = 0; i < mod.functions.size(); ++i) {
        const ir::IrFunction &fn = mod.functions[i];
        /* Por PREFIJO, como todos los demas: "contiene" era un criterio
         * distinto del mismo nombre. */
        const bool is_init = ir::is_module_init_family(fn.name);
        const bool is_seed = fn.name == "main" || is_init || fn.is_native ||
                             fn.is_naked || (seed_public && fn.is_public);
        if (!is_seed) continue;
        ++t.seeds;
        reached[i] = 1;
        pending.push_back(i);
    }

    /* LO QUE EL MODULO NOMBRA FUERA DE UNA INSTRUCCION, que es donde se
     * escondia el error: mirar solo el codigo daba por muerto al ASIGNADOR del
     * programa, que es de lo mas vivo que hay -- se referencia por una cadena
     * del modulo, no por una llamada --.  Lo mismo valdria para un metodo, que
     * se registra por su nombre cualificado.  Si esto se olvida, el barrido no
     * se equivoca "un poco": borra algo que nadie llama y todo el mundo usa. */
    const std::string *const module_syms[] = {&mod.alloc_sym, &mod.free_sym};
    for (const std::string *s : module_syms) {
        if (s->empty()) continue;
        const auto it = by_name.find(*s);
        if (it == by_name.end() || reached[it->second]) continue;
        ++t.seeds;
        reached[it->second] = 1;
        pending.push_back(it->second);
    }
    for (const ir::IrClass &c : mod.classes)
        for (const ir::IrMethod &m : c.methods) {
            if (m.ir_fn_name.empty()) continue; // abstracto: no hay cuerpo
            const auto it = by_name.find(m.ir_fn_name);
            if (it == by_name.end() || reached[it->second]) continue;
            ++t.seeds;
            reached[it->second] = 1;
            pending.push_back(it->second);
        }

    /* Una funcion alcanzada alcanza a todas las que NOMBRA, sea como sea que
     * las nombre. */
    while (!pending.empty()) {
        const ir::IrFunction &fn = mod.functions[pending.back()];
        pending.pop_back();
        for (const ir::IrBlock &b : fn.blocks)
            for (const ir::IrInstr &in : b.instrs) {
                if (in.func_name.empty()) continue;
                const auto it = by_name.find(in.func_name);
                if (it == by_name.end() || reached[it->second]) continue;
                reached[it->second] = 1;
                pending.push_back(it->second);
            }
    }

    for (size_t i = 0; i < mod.functions.size(); ++i) {
        size_t n = 0;
        for (const ir::IrBlock &b : mod.functions[i].blocks)
            n += b.instrs.size();
        t.all_instrs += n;
        if (reached[i]) continue;
        ++t.dead;
        t.dead_instrs += n;
        /* LOS NOMBRES, y no por curiosidad: la unica forma de saber si el
         * cierre se dejo una via de alcanzar -- un metodo que `__module_init`
         * registra por etiqueta, algo cuya direccion se toma -- es MIRAR que
         * salio muerto.  Un porcentaje no lo dice; un nombre que resulta ser
         * un metodo vivo, si. */
        if (t.names.size() < 12) t.names.push_back(mod.functions[i].name);
    }
    return t;
}

void count_unreachable_functions(const ir::IrModule &mod) {
    std::unordered_map<std::string, size_t> by_name;
    by_name.reserve(mod.functions.size() * 2);
    for (size_t i = 0; i < mod.functions.size(); ++i)
        by_name.emplace(mod.functions[i].name, i);

    const ReachTally hoy = reach_from_seeds(mod, by_name, true);
    const ReachTally sin_publicas = reach_from_seeds(mod, by_name, false);

    const double pct = mod.functions.empty() ? 0.0
                                             : 100.0 * double(hoy.dead) /
                                                   double(mod.functions.size());
    const double pct_i = hoy.all_instrs == 0 ? 0.0
                                             : 100.0 * double(hoy.dead_instrs) /
                                                   double(hoy.all_instrs);
    std::fprintf(stderr,
                 "[dead-fn] %zu funciones, %zu semillas\n"
                 "[dead-fn] no alcanzables: %zu (%.1f%%), con %zu de %zu "
                 "instrucciones (%.1f%%)\n",
                 mod.functions.size(), hoy.seeds, hoy.dead, pct,
                 hoy.dead_instrs, hoy.all_instrs, pct_i);
    /* Y lo que se ganaria si `internal` se distinguiera de `public`: las que
     * hoy se salvan SOLO por ser semilla publica. */
    for (const std::string &n : hoy.names)
        std::fprintf(stderr, "[dead-fn]   %s\n", n.c_str());
    std::fprintf(stderr,
                 "[dead-fn] sin sembrar las publicas serian %zu (%zu mas): eso "
                 "es lo que vale distinguir `internal` de `public`\n",
                 sin_publicas.dead, sin_publicas.dead - hoy.dead);
}

void spill_until_under_ceiling(std::vector<ProjectModuleWork> &work,
                               size_t &live, Diagnostics &diags, bool verbose) {
    const size_t ceiling = ir_ram_ceiling_bytes();
    if (ceiling == 0 || live <= ceiling) return;

    for (size_t i = 0; i + 1 < work.size() && live > ceiling; ++i) {
        ProjectModuleWork &pm = work[i];
        if (!pm.ok) continue;                    // no llego a compilar
        if (!pm.ir_spill_path.empty()) continue; // ya esta fuera
        if (pm.ir.functions.empty()) continue;   // no hay nada que bajar

        /* Si la cache ya publico su `.vxir`, ESE es el fichero: desalojar no
         * cuesta ni una escritura.  Si no (cache apagada, o un modulo que no
         * se cachea), se escribe en el area de TRABAJO -- que no es una cache:
         * son bytes de esta compilacion y de ninguna otra. */
        const bool already_written = !pm.ir_cache_path.empty();
        const std::string dest =
            already_written ? pm.ir_cache_path
                            : (util::cache_dir(util::CacheKind::Work) + "/ir_" +
                               std::to_string(pm.module_id) + ".vxir");

        const size_t count = pm.ir.functions.size();
        const size_t held = pm.ir_footprint;
        if (!ir::spill_functions(pm.ir, dest, already_written)) {
            /* No se pudo dejar a salvo, asi que no se suelta -- la compilacion
             * sigue siendo CORRECTA, solo que el techo no se cumple.  Y se
             * DICE: sin esto, el techo parece que no funciona y nadie sabe por
             * que.  Avisa UNA vez por modulo: el bucle no vuelve a intentarlo
             * porque `ir_spill_path` se queda vacio y el siguiente barrido
             * pasa al siguiente candidato. */
            SourceLoc loc;
            loc.set_file(pm.canonical_path);
            diags.diag(std::move(loc), DiagLevel::WARN, "VX4007",
                       {pm.module_name.str(), dest,
                        util::flag_info(util::FlagId::IrRamMaxMib).name});
            continue;
        }
        pm.ir_spill_path = dest;
        pm.ir_spilled_fns = count;
        live -= (held < live ? held : live);
        if (verbose)
            std::cerr << "[ir] " << pm.module_name.str() << ": " << count
                      << " funciones a disco, " << (held / 1024u / 1024u)
                      << " MiB libres\n";
    }
}

/**
 * @brief Apunta lo que ocupa el modulo recien compilado y aplica el techo.
 *
 * Con el techo sin poner no hace NADA -- ni siquiera mide --, que es lo que
 * tiene que pasar: quien no pidio un techo no paga por tenerlo.
 *
 * @param work    Todos los modulos.
 * @param idx     El que acaba de compilarse.
 * @param live    Bytes de intermedio residentes.  Se actualiza.
 * @param diags   Donde avisar si un modulo no se pudo bajar.
 * @param verbose Si contar lo que se baja.
 */
void account_and_spill(std::vector<ProjectModuleWork> &work, size_t idx,
                       size_t &live, Diagnostics &diags, bool verbose) {
    if (ir_ram_ceiling_bytes() == 0) return;
    ProjectModuleWork &pm = work[idx];
    pm.ir_footprint = ir::functions_footprint(pm.ir.functions);
    live += pm.ir_footprint;
    spill_until_under_ceiling(work, live, diags, verbose);
}

/**
 * @brief Devuelve a la RAM los cuerpos de @p pm si estaban en disco.
 *
 * @param pm  El modulo.
 * @param err Que paso, si fallo.
 * @return true si el modulo quedo utilizable -- lo que incluye que nunca
 *         hubiera salido de la RAM.
 */
bool restore_module_ir(ProjectModuleWork &pm, std::string &err) {
    if (pm.ir_spill_path.empty()) return true;
    if (!ir::restore_functions(pm.ir, pm.ir_spill_path, pm.ir_spilled_fns, err))
        return false;
    pm.ir_spill_path.clear();
    pm.ir_spilled_fns = 0;
    return true;
}

/* Los contratos declarados se leen en `vx/contracts_collect.h`.  Aqui habia
 * una copia -- `collect_contracts` -- y el camino de un fichero suelto tenia la
 * suya: la misma lectura del AST escrita dos veces. */

/* `declares_classes` y `mangle_top_level_` viven ahora con el paso de la
 * compilacion de un modulo que los usa: `src/vx/unit/unit_interface.cpp` y
 * `src/vx/unit/unit_prepare.cpp`. */

/// Recoge los nombres de tipo que menciona un nodo de tipo, a cualquier
/// profundidad.  Un alias puede derivar de otro por debajo de un puntero, de un
/// array o de la firma de una funcion, asi que mirar solo la raiz se dejaria
/// fuera los casos que no son `typedef A B`.
void nombres_de_tipo_(const ast::TypeNode *t, std::vector<std::string> &out) {
    if (t == nullptr) return;
    switch (t->kind) {
    case ast::NodeKind::NamedTypeNode: {
        const auto *n = static_cast<const ast::NamedTypeNode *>(t);
        out.push_back(n->name);
        for (const auto &a : n->type_args)
            nombres_de_tipo_(a.get(), out);
        break;
    }
    case ast::NodeKind::PrimitiveTypeNode: {
        const auto *p = static_cast<const ast::PrimitiveTypeNode *>(t);
        for (const auto &a : p->type_args)
            nombres_de_tipo_(a.get(), out);
        break;
    }
    case ast::NodeKind::PointerTypeNode:
        nombres_de_tipo_(
            static_cast<const ast::PointerTypeNode *>(t)->pointee.get(), out);
        break;
    case ast::NodeKind::ArrayTypeNode:
        nombres_de_tipo_(
            static_cast<const ast::ArrayTypeNode *>(t)->element_type.get(),
            out);
        break;
    case ast::NodeKind::FunctionTypeNode: {
        const auto *f = static_cast<const ast::FunctionTypeNode *>(t);
        for (const auto &p : f->param_types)
            nombres_de_tipo_(p.get(), out);
        nombres_de_tipo_(f->return_type.get(), out);
        break;
    }
    default: break;
    }
}

/// Reordena los @c TypeAliasDecl de un namespace fusionado para que cada uno
/// vaya DESPUES de aquellos de los que deriva.
///
/// Solo se mueven los alias entre si: las posiciones que ocupaban se rellenan
/// en el nuevo orden y el resto de decls no se toca.  Un alias que participa en
/// un ciclo conserva su sitio -- callarlo o inventarle un orden esconderia un
/// error que el type checker sabe nombrar.
void ordenar_alias_por_dependencia_(
    std::vector<std::unique_ptr<ast::Node>> &decls) {
    std::vector<size_t> huecos; // posiciones que ocupan los alias
    std::unordered_map<std::string, size_t> por_nombre;
    for (size_t i = 0; i < decls.size(); ++i) {
        if (!decls[i] || decls[i]->kind != ast::NodeKind::TypeAliasDecl)
            continue;
        por_nombre.emplace(
            static_cast<ast::TypeAliasDecl *>(decls[i].get())->name,
            huecos.size());
        huecos.push_back(i);
    }
    if (huecos.size() < 2) return;

    // Aristas alias -> alias del que deriva, restringidas a este namespace: un
    // nombre de fuera ya esta resuelto cuando llega el pase.
    const size_t n = huecos.size();
    std::vector<std::vector<size_t>> deriva_de(n);
    std::vector<std::string> nombres;
    std::vector<ast::TypeNode *> tipos_del_typedef;
    for (size_t k = 0; k < n; ++k) {
        const auto *al =
            static_cast<const ast::TypeAliasDecl *>(decls[huecos[k]].get());
        nombres.clear();
        // El tipo subyacente y, si es un tipo fuerte, sus conversiones:
        // depende de todo lo que nombra.
        tipos_del_typedef.clear();
        ast::typedef_type_nodes(*al, tipos_del_typedef);
        for (const ast::TypeNode *t : tipos_del_typedef)
            nombres_de_tipo_(t, nombres);
        for (const auto &nm : nombres) {
            auto it = por_nombre.find(nm);
            if (it != por_nombre.end() && it->second != k)
                deriva_de[k].push_back(it->second);
        }
    }

    // DFS post-orden: cada alias se emite tras aquellos de los que deriva.  Un
    // nodo en la pila actual (marca 1) cierra un ciclo; se deja pasar sin
    // reordenar para que el diagnostico lo de quien sabe explicarlo.
    std::vector<uint8_t> marca(n, 0); // 0 sin ver, 1 en pila, 2 emitido
    std::vector<size_t> orden;
    orden.reserve(n);
    std::function<void(size_t)> visitar = [&](size_t k) {
        if (marca[k] != 0) return;
        marca[k] = 1;
        for (size_t d : deriva_de[k])
            visitar(d);
        marca[k] = 2;
        orden.push_back(k);
    };
    for (size_t k = 0; k < n; ++k)
        visitar(k);

    std::vector<std::unique_ptr<ast::Node>> movidos(n);
    for (size_t k = 0; k < n; ++k)
        movidos[k] = std::move(decls[huecos[k]]);
    for (size_t k = 0; k < n; ++k)
        decls[huecos[k]] = std::move(movidos[orden[k]]);
}

///  NS.3: deriva el PackageId del proyecto.  Camina hacia arriba desde el
/// directorio del fichero raiz buscando @c "vx.toml" (o @c "vx.json"); lee el
/// @c [package] con un scan minimo (sin dependencia de @c src/pkg).  El id es:
///   - el valor explicito de @c id / @c "id" si esta presente, o
///   - @c fnv1a_hex(name @ version) derivado, o
///   - vacio (paquete anonimo) si no hay manifest ni nombre.

} // namespace

/* Las dos puertas que el camino de un fichero suelto tambien usa.  Salen del
 * namespace anonimo -- y no se duplican alli -- porque el criterio de cuando
 * fiarse de lo guardado tiene que ser UNO: con dos, basta que uno se quede
 * atras para que el mismo programa se compile distinto segun por donde entre.
 */
std::string vxfacts_path_for(const std::string &source_path,
                             const std::string &tgt_suffix) {
    /* Se interna aqui porque quien pregunta lo hace una vez por compilacion. */
    return module_cache_paths(util::InternedName::intern(source_path),
                              util::InternedName::intern(tgt_suffix))
        .facts;
}

std::vector<analysis::asa::ProductionSummary>
ensure_facts(const ir::IrModule &mod, analysis::asa::FactStore &store,
             const std::vector<const char *> &wanted, const std::string &path,
             uint64_t fingerprint, const char *stage,
             const std::string &source_path) {
    return ensure_facts_impl_(mod, store, wanted, path, fingerprint, stage,
                              source_path);
}

uint64_t asa_module_id(const std::string &source_path) {
    /* Solo la ruta: es QUIEN es el modulo, no que dice.  Que el contenido no
     * entre aqui es justo lo que permite que las claves por dominio y por
     * funcion lleguen a actuar. */
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : source_path) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t asa_facts_key(uint64_t module_id, const CompileOptions &opts,
                       const char *stage) {
    /* La configuracion, con los MISMOS campos que usa el CAS: dos criterios
     * distintos de "que configuracion es esta" acabarian con uno invalidando y
     * el otro no. */
    BuildConfig cfg;
    cfg.asm_target_bits = opts.asm_target_bits;
    cfg.native_poo = opts.native_poo;
    cfg.exceptions_enabled = opts.exceptions_enabled;
    cfg.instrument_mode = opts.instrument_mode;
    cfg.opt_level = opts.opt_level;
    cfg.emit_debug = opts.emit_debug;
    cfg.aot_vec_width = opts.aot_vec_width;

    const bool pre = (stage != nullptr &&
                      std::strcmp(stage, analysis::asa::kStagePreOpt) == 0);
    /* La CAPA que corresponde al momento.  Ver la nota de `asa_facts_key` en la
     * cabecera: pre-opt habla del IR tal y como se bajo, post-opt depende del
     * optimizador. */
    const uint64_t cfg_fp = pre ? cfg.ir_fingerprint() : cfg.full_fingerprint();

    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 0x100000001b3ULL;
    };
    mix(0x41534146414354ull); // dominio: "ASA facts key".
    /* QUE modulo, no que dice.  El contenido lo comprueban las claves por
     * dominio y por funcion, que saben de que depende cada una; meterlo aqui
     * descartaba el fichero entero ante cualquier edicion y dejaba esas dos
     * sin llegar a actuar nunca. */
    mix(module_id);
    mix(cfg_fp);
    /* Los mandos de entorno que CAMBIAN lo emitido.  La tabla ya los clasifica
     * (`FlagScope::Emitted`), asi que esto no es una lista que mantener: es
     * preguntarle a la que hay. */
    mix(util::emitted_fingerprint());
    // Y el momento, que es lo que hace que las dos claves no puedan coincidir.
    for (const char *p = (stage != nullptr ? stage : ""); *p != '\0'; ++p)
        mix(static_cast<uint64_t>(static_cast<unsigned char>(*p)));
    return h;
}

std::string asa_facts_path_for_stage(const std::string &base_facts_path,
                                     const char *stage) {
    if (base_facts_path.empty()) return base_facts_path;
    static const std::string kExt = ".vxfacts";
    const std::string short_name =
        (stage != nullptr &&
         std::strcmp(stage, analysis::asa::kStagePreOpt) == 0)
            ? "pre"
        : (stage != nullptr &&
           std::strcmp(stage, analysis::asa::kStageDuringOpt) == 0)
            ? "mid"
            : "post";
    if (base_facts_path.size() > kExt.size() &&
        base_facts_path.compare(base_facts_path.size() - kExt.size(),
                                kExt.size(), kExt) == 0) {
        return base_facts_path.substr(0, base_facts_path.size() - kExt.size()) +
               "." + short_name + kExt;
    }
    return base_facts_path + "." + short_name;
}

/**
 * @brief El asignador escrito en el lenguaje, compilado UNA vez por ejecucion.
 *
 * El binario nativo lo trae desde hace tiempo; el JIT no, y por eso un fallo
 * que solo se ve en el nativo hay que perseguirlo en el nativo.  Compartiendo
 * mecanismo, ese mismo camino se puede recorrer dentro de la maquina, con el
 * depurador y los volcados de IR delante.  No es por velocidad.
 *
 * Se compila una sola vez y se guarda: hacerlo por cada modulo que reserva
 * memoria multiplicaba el tiempo de compilacion hasta agotar el limite en los
 * proyectos con varias dependencias.
 *
 * @param opts Opciones de la compilacion en curso (nivel y objetivo).
 * @param alloc_sym Recibe el nombre de la funcion que reserva.
 * @param free_sym Recibe el de la que libera.
 * @return El modulo, o @c nullptr si no se encuentra o no lo expone.
 */
static const ir::IrModule *asignador_del_lenguaje_(const CompileOptions &opts,
                                                   std::string &alloc_sym,
                                                   std::string &free_sym) {
    struct Cache {
        bool intentado = false;
        std::unique_ptr<ir::IrModule> mod;
        std::string alloc, libera;
    };
    static Cache c;
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    if (!c.intentado) {
        c.intentado = true;
        namespace stdfs = std::filesystem;
        const std::string dir =
            stdfs::path(fs::get_executable_path()).parent_path().string();
        for (const std::string &p :
             {dir + "/stdlib/vx/vx_mem.vx", dir + "/../stdlib/vx/vx_mem.vx",
              std::string("stdlib/vx/vx_mem.vx")}) {
            if (!stdfs::exists(p)) continue;
            CompileOptions mo;
            mo.module_name = "vx_mem";
            mo.opt_level = opts.opt_level;
            mo.asm_target_bits = opts.asm_target_bits;
            mo.sin_asignador_vesta = true; // no puede traerse a si mismo
            const CompileResult r = compile_vx_project(p, mo, nullptr, nullptr);
            if (!r.ok || r.aot_alloc_sym.empty() || r.aot_free_sym.empty())
                break;
            std::unique_ptr<ir::IrModule> mm(new ir::IrModule());
            if (!ir::parse_ir_module_cache(r.ir_module_cache_bytes.buf.data,
                                           r.ir_module_cache_bytes.buf.size,
                                           *mm))
                break;
            c.mod = std::move(mm);
            c.alloc = r.aot_alloc_sym;
            c.libera = r.aot_free_sym;
            break;
        }
    }
    if (!c.mod) return nullptr;
    alloc_sym = c.alloc;
    free_sym = c.libera;
    return c.mod.get();
}

/**
 * @brief Deja el asignador del lenguaje DENTRO del modulo, sin tocar las
 *        reservas.
 *
 * Sus funciones quedan disponibles para quien quiera llamarlas -- el selector
 * del JIT --, y quien no las llame no las nota: el interprete sigue con la
 * instruccion de la maquina.  Reescribir aqui las reservas seria decidir por
 * los dos, y ese fue el error del primer intento: se llevo por delante al
 * interprete.
 *
 * @param mod Modulo ya fusionado, antes de optimizar.
 * @param opts Opciones de la compilacion en curso.
 * @param root_path Fuente raiz, para no traerse a si mismo.
 */
void traer_asignador_del_lenguaje(ir::IrModule &mod, const CompileOptions &opts,
                                  const std::string &root_path) {
    if (opts.sin_asignador_vesta) return;
    if (root_path.find("vx_mem") != std::string::npos) return;
    /* El binario nativo ya lo trae por su cuenta -- su driver lo compila y lee
     * sus simbolos --, asi que metiendolo tambien desde aqui acabaria con dos
     * copias del mismo asignador.  Esto es para el camino de maquina, que es el
     * que no lo tenia. */
    if (opts.native_poo) return;

    bool reserva = false;
    for (const ir::IrFunction &f : mod.functions) {
        if (f.name == ir::rt::kMalloc) return; // ya esta dentro
        for (const ir::IrBlock &b : f.blocks)
            for (const ir::IrInstr &in : b.instrs)
                if (in.op == ir::IrOp::RAW_ALLOC || in.op == ir::IrOp::RAW_FREE)
                    reserva = true;
    }
    if (!reserva) return;

    std::string alloc_sym, free_sym;
    const ir::IrModule *mem =
        asignador_del_lenguaje_(opts, alloc_sym, free_sym);
    if (mem == nullptr) return; // sin el, todo sigue como estaba.
    /* Y QUIEN es, para que la maquina no tenga que adivinarlo.  Si el programa
     * declara el suyo con @AllocatorOverride manda ese; el de la biblioteca
     * solo cubre a quien no lo hace. */
    mod.alloc_sym = alloc_sym;
    mod.free_sym = free_sym;

    /* Los nombres que ya estan, en un indice construido UNA vez.  Preguntarlo
     * recorriendo todas las funciones por cada candidata seria multiplicar dos
     * listas y comparar cadenas en el bucle interno. */
    std::unordered_set<std::string> presentes;
    presentes.reserve(mod.functions.size() * 2);
    for (const ir::IrFunction &g : mod.functions)
        presentes.insert(g.name);

    std::unordered_set<std::string> traidas;
    for (const ir::IrFunction &f : mem->functions) {
        if (presentes.count(f.name) == 0) {
            /* Alcanzable aunque nadie la llame TODAVIA: quien la va a llamar es
             * el selector del JIT, que trabaja despues de optimizar, y para
             * entonces el pase que limpia lo que no se usa ya se la habria
             * llevado. */
            ir::IrFunction copia = f;
            copia.is_public = true;
            traidas.insert(copia.name);
            mod.functions.push_back(std::move(copia));
        }
    }
    /* Los datos, y con ellos el DESPLAZAMIENTO de sus indices.
     *
     * Cada modulo numera sus ranuras desde cero.  Al concatenar, las del
     * asignador se van al final y sus indices cambian, pero sus instrucciones
     * siguen pidiendo el numero viejo -- que ahora es otra cosa.  Ademas de
     * leer lo que no es, se pierde su naturaleza: una ranura de variable global
     * vive en memoria del HOST y el resto en la de la maquina, asi que el
     * asignador acababa usando una direccion de la maquina como si fuera del
     * host y el programa caia con un acceso invalido.
     *
     * Es el mismo desplazamiento que ya hace el merge de dependencias. */
    ir::IrModule copia_datos = *mem;
    const uint64_t desplazamiento = mod.static_data.size();
    mod.static_data.append_raw_entries(std::move(copia_datos.static_data));
    /* Con la operacion compartida: corre los indices Y las referencias
     * textuales del ensamblador embebido, que esta fusion se dejaba. */
    for (ir::IrFunction &f : mod.functions)
        if (traidas.count(f.name))
            ir::ir_correr_indices_de_datos(f, desplazamiento);
    for (const auto &gv : mem->globals)
        mod.globals.emplace(gv.first, gv.second);
    // Lo que el asignador llama por su cuenta: pide memoria al sistema por la
    // interfaz nativa, asi que sus importaciones tienen que venir con el.
    for (const auto &ni : mem->native_imports)
        mod.register_native_import(ni.lib, ni.name);
    for (const auto &nl : mem->native_libs) {
        bool ya = false;
        for (const auto &x : mod.native_libs)
            if (x == nl) {
                ya = true;
                break;
            }
        if (!ya) mod.native_libs.push_back(nl);
    }
    /* Y el punto de entrada de nombre conocido que lleva hasta el.  Al final,
     * cuando el asignador YA esta dentro: el puente comprueba que existe a
     * quien llamar antes de ponerse, asi que antes de copiarlo no encontraba
     * nada.  Aqui pasan los dos caminos de compilacion. */
}

/**
 * @brief Traduce un indice de fichero de la tabla de un modulo a la de otro.
 *
 * Al fusionar, los indices que traen las funciones de una dependencia se leen
 * contra la tabla del modulo fusionado, que es otra.  Fuera de rango se
 * devuelve "no consta" en vez de un indice cualquiera: citar el fichero
 * equivocado es peor que no citar ninguno.
 *
 * @param remap Correspondencia indice-del-dep -> indice-del-fusionado.
 * @param idx Indice tal como venia en la dependencia.
 * @return El indice equivalente, o @c ir::IR_NO_SOURCE_FILE.
 */
static uint32_t remap_source_file(const util::SmallVector<uint32_t, 4> &remap,
                                  uint32_t idx) {
    return idx < remap.size() ? remap[idx] : ir::IR_NO_SOURCE_FILE;
}

CompileResult compile_vx_project(
    const std::string &root_path, const CompileOptions &opts,
    const std::unordered_map<std::string, std::string> *source_overlay,
    const std::vector<std::string> *extra_search_paths) {
    CompileResult res;

    /* Reparto del coste, con el mismo criterio que el camino de fichero
     * suelto: se mide siempre, porque una medida que hay que pedir es una
     * medida que nadie mira.  Aqui interesa sobre todo separar RESOLVER el
     * grafo -- que crece con el numero de modulos -- de compilarlos. */
    using RelojProyecto = std::chrono::steady_clock;
    auto marca = RelojProyecto::now();
    /* El segundo parametro nombra la fase que EMPIEZA aqui, para el eje del
     * tiempo del comprobador de memoria: una curva sin nombres dice "el pico
     * esta en la muestra 251 de 289", y quien la mira necesita saber que la 251
     * era el emisor.  El unico que lo sabe es esto, y lo sabe gratis porque ya
     * tenia el corchete puesto para cronometrar.
     *
     * VA UNA CLAVE, NO UNA FRASE.  Escribir aqui el texto seria texto de cara
     * al usuario puesto a mano, que es justo lo que el catalogo multi-idioma
     * viene a impedir -- y ademas acabaria dentro de un informe de OTRO
     * proyecto, que no tiene por que llevar nuestras palabras ni nuestro
     * idioma.  La clave es estable y la traduce quien ENSENA la curva.
     *
     * NO CUESTA NADA sin comprobador: fuera de ese build `san_mark` es un
     * cuerpo vacio en linea.  Ver `util::san_mark`. */
    auto cerrar_fase = [&marca](long &destino,
                                const char *siguiente = nullptr) {
        const auto ahora = RelojProyecto::now();
        destino += (long)std::chrono::duration_cast<std::chrono::microseconds>(
                       ahora - marca)
                       .count();
        marca = ahora;
        /* Y lo que la fase que termina solto, de vuelta al reparto comun antes
         * de que empiece la siguiente.  Ya estaba puesto el corchete para
         * cronometrar, asi que la frontera no cuesta ni una linea de mas: el
         * QUE devuelve y en que orden, en `release_between_phases`. */
        util::release_between_phases(siguiente);
    };
    util::san_mark("vx.phase.resolve");

    // 1. Construir el dep graph + topo sort.
    ModuleGraph graph(res.diagnostics);
    // LSP: overlay del buffer en memoria (root con ediciones sin guardar).  Se
    // aplica ANTES de build_from_root para que la lectura del root use el texto
    // inyectado en vez del disco; los imports se siguen leyendo del disco.
    if (source_overlay) {
        for (const auto &kv : *source_overlay)
            graph.set_source_overlay(kv.first, kv.second);
    }
    // LSP: directorios extra donde resolver imports (ancestros del fichero
    // analizado), para que un modulo con imports relativos al root del proyecto
    // (p.ej. `import "modules/buffer"`) resuelva aunque se abra standalone.
    if (extra_search_paths) {
        for (const auto &d : *extra_search_paths)
            graph.add_search_path(d);
    }
    // Permitir override del directorio de busqueda via env var VX_PATH.
    graph.add_vx_path_env();

    /* Las dependencias DECLARADAS del proyecto.
     *
     * Hasta aqui un `import` resolvia contra lo que hubiera suelto por el disco
     * -- el directorio del fuente, `VX_PATH`, la stdlib --, y el manifiesto no
     * pintaba nada: el gestor de paquetes sabia descargar y verificar, pero el
     * compilador no miraba lo que se habia declarado.  De ahi que dos copias de
     * una libreria fueran indistinguibles.
     *
     * Ahora lo que el `vx.toml` declara entra como sitio donde buscar, y con
     * PRIORIDAD sobre la stdlib: si el proyecto dice de que depende, eso es lo
     * que quiere, no lo que se encuentre por ahi. */
    {
        std::string manifiesto;
        (void)derive_package_id(root_path, &manifiesto);
        if (!manifiesto.empty()) {
            const pkg::ParseResult pr = pkg::parse_manifest_file(manifiesto);
            if (pr.ok) {
                namespace fs = std::filesystem;
                const fs::path dir_man = fs::path(manifiesto).parent_path();
                for (const auto &dep : pr.manifest.dependencies) {
                    /* Con `path` se toma tal cual (relativo al manifiesto);
                     * si no, donde el gestor deja lo instalado.  Se usa el
                     * nombre REAL del paquete, que con un alias no es la clave
                     * de la entrada. */
                    const std::string &nombre =
                        dep.paquete.empty() ? dep.name : dep.paquete;
                    /* El override del usuario manda sobre lo declarado: es como
                     * se trabaja SOBRE una libreria sin tocar el manifiesto de
                     * cada proyecto que la usa. */
                    const std::string ov = override_de_paquete(nombre);
                    fs::path cand =
                        !ov.empty() ? fs::path(ov)
                                    : (dep.path.empty()
                                           ? (dir_man / "vx_modules" / nombre)
                                           : (dir_man / dep.path));
                    std::error_code ec;
                    if (fs::exists(cand, ec) && fs::is_directory(cand, ec))
                        graph.add_search_path(cand.lexically_normal().string());
                }
            }
        }
    }
    /* Los namespaces que el manifiesto declare auto-importables.  Se llena
     * justo debajo, al localizar la stdlib, y se pasa a quien recoge los
     * imports: un dato, no estado escondido. */
    AutoImportNs auto_imports;
    /* El arbol del paquete que las declara.  Sus propios modulos NO reciben la
     * auto-importacion: ahi el servicio se ofrece, no se consume, y aplicarsela
     * cerraria el grafo en ciclo -- el asignador depende de los tipos, que
     * pedirian el asignador --. */
    std::string auto_import_owner_dir;
    // Cablear el directorio de la stdlib Vesta (stdlib/vx).  Permite que
    // `import "simd_string"` (y futuras libs Vesta de la stdlib) resuelva sin
    // que el usuario tenga que copiar la lib a su proyecto.  Autodetect por
    // candidatos comunes desde el cwd (override via env var VX_STDLIB_DIR).
    {
        // Autodetect de la stdlib Vesta (env VX_STDLIB_DIR, cwd, o relativo al
        // ejecutable).  Factorizado en detect_stdlib_vx_dir() para reuso del
        // LSP.
        std::string sd = detect_stdlib_vx_dir();
        if (!sd.empty()) graph.set_stdlib_dir(sd);
        /* Y lo que la stdlib declare AUTO-IMPORTABLE en su manifiesto.
         *
         * Aqui, porque es el sitio donde ya se sabe donde vive: leerlo en otro
         * lado obligaria a volver a buscarla.  Lo que se trae es una lista de
         * nombres, asi que el compilador no conoce ninguno.
         *
         * Se lee UNA vez y viaja por la firma de quien la necesita.  Nada de
         * estado por hilo: ni hace falta -- el dato no cambia en toda la
         * compilacion -- ni seria gratis, porque en Windows una variable de
         * hilo es una LLAMADA (ver `util::ThreadSlot`, que es lo que se usa
         * cuando de verdad hay estado por hilo).
         *
         * Solo en el camino NATIVO: en la maquina virtual reservar memoria es
         * una instruccion suya y no hay a quien ver.  Y nunca al compilar la
         * propia stdlib, que se traeria a si misma. */
        if (!sd.empty() && opts.native_poo && !opts.sin_asignador_vesta) {
            const std::string manifest_path = stdlib_manifest_path();
            if (!manifest_path.empty()) {
                auto_imports = auto_import_modules(manifest_path);
                /* El arbol del paquete es el del manifiesto que lo declaro: el
                 * que dice que algo se auto-importa es quien delimita a quien
                 * NO se le aplica. */
                const size_t slash = manifest_path.find_last_of("/\\");
                if (slash != std::string::npos)
                    auto_import_owner_dir = manifest_path.substr(0, slash);
                for (char &c : auto_import_owner_dir)
                    if (c == '\\') c = '/';
            }
        }
        /* Al GRAFO tambien, y antes de recorrerlo: que el modulo este en el
         * ambito de quien reserva no sirve de nada si no esta en el CONJUNTO.
         * Son las dos mitades de lo mismo, y con una sola el import resolvia a
         * un modulo que nadie habia cargado. */
        graph.set_auto_import_ns(auto_imports);
    }
    // añadir como search path implicito la carpeta del modulo root.  Asi
    // los modulos hermanos pueden importarse con paths relativos al root
    // (`import "modules/foo"` desde @c src/modules/bar.vx resuelve a
    // @c src/modules/foo.vx aunque el importer dir sea @c src/modules/).
    // Sin esto, cada modulo tendria que usar paths siblings (`import "foo"`)
    // que cambian segun donde vive el archivo -- frustrante a escala.
    {
        std::string norm = root_path;
        for (char &c : norm)
            if (c == '\\') c = '/';
        size_t slash = norm.find_last_of('/');
        if (slash != std::string::npos) {
            graph.add_search_path(norm.substr(0, slash));
        }
    }
    const uint32_t root_id = graph.build_from_root(root_path);
    if (root_id == UINT32_MAX || res.diagnostics.has_errors()) {
        res.ok = false;
        return res;
    }
    auto topo = graph.topological_order();
    if (graph.has_cycle()) {
        res.ok = false;
        return res;
    }

    cerrar_fase(res.times.resolve_us, "vx.phase.modules");

    /* El reparto de instanciaciones genericas, UNO por proyecto.
     *
     * Vive aqui -- y no como global del proceso -- porque `libvesta` deja
     * compilar varios proyectos en el mismo proceso y un reparto global los
     * mezclaria.  Tiene que sobrevivir a todos los modulos, asi que se declara
     * antes que ellos. */
    vx::GenericInstanceRegistry generic_instances;

    /* CON TREE-SHAKE NO SE REPARTE, y no es una limitacion cosmetica.
     *
     * El reparto decide quien produce cada instancia MIENTRAS se compilan los
     * modulos; el tree-shake decide que modulos sobran DESPUES, y poda el
     * modulo entero.  Si el que se quedo con `procesa<S0>` es justo el que se
     * poda, quien la usaba se queda sin ella -- reproducido: `Relocacion:
     * simbolo no resuelto: code.bench__lib__procesa_bench__tipos__S0`.
     *
     * Sin reparto cada modulo vuelve a llevar su copia, que es lo que hacia
     * que podar fuera inofensivo.  Se paga la memoria de los clones solo en
     * las compilaciones que enciendan la bandera, que es opcional y viene
     * apagada.
     *
     * El arreglo bueno -- no podar un modulo del que cuelgan instancias que
     * otros usan, o re-emitirlas al podarlo -- pide saber que reclamo cada
     * uno, y eso es otra tanda. */
    const bool share_instances = !util::flag_on(util::FlagId::TreeShake);

    // 2. Mover los AST parseados del graph a estructuras de trabajo.
    std::vector<ProjectModuleWork> work(topo.size());
    for (size_t i = 0; i < topo.size(); ++i) {
        const uint32_t mid = topo[i];
        const ResolvedModule *rm = graph.module(mid);
        // El ModuleGraph mantiene el AST como unique_ptr.  Necesitamos
        // tomarlo prestado SIN const_cast del puntero, asi que pedimos
        // que el graph nos lo entregue via un getter que mueva el unique_ptr.
        // Como simplificacion, hacemos const_cast aqui (el ModuleGraph
        // no se usa mas despues de este punto).
        ResolvedModule *rm_mut = const_cast<ResolvedModule *>(rm);
        work[i].module_id = mid;
        /* Internados UNA vez aqui, al nacer el modulo: de aqui en adelante su
         * identidad se copia como un puntero. */
        work[i].canonical_path =
            util::InternedName::intern(rm_mut->canonical_path);
        work[i].module_name = util::InternedName::intern(rm_mut->module_name);
        work[i].ast = std::move(rm_mut->parsed_ast);
        // Cargar source de disco para el lexer (necesario para el
        // diagnostics: queremos preservar locs).
        work[i].source = read_source_(rm_mut->canonical_path);
    }

    /* ------------------------------------------------------------------
     * El conjunto comptime, compilado ANTES que ningun modulo.
     *
     * El codigo que se ejecuta al compilar -- los `@Macro`, las `comptime fn`
     * -- necesita bytecode que ejecutar.  Hasta ahora ese bytecode se producia
     * compilando el programa ENTERO y volviendo a compilarlo todo con el ya
     * cargado; mientras tanto, los modulos que se compilaban antes de que
     * existiera salian con lo que ese codigo debia generar SIN generar: cuerpos
     * de `asm` vacios, sobre todo.
     *
     * Aqui se hace al reves y una sola vez: se extrae el conjunto -- solo las
     * decls que se ejecutan al compilar, que son pocas: seis funciones de las
     * treinta y cinco en `std.memory.x86_64` --, se compila, y el bucle de
     * modulos arranca con el ya disponible.
     *
     * Se puede hacer AQUi porque en este punto TODOS los modulos estan
     * parseados (el resolvedor lo hace para construir el grafo de
     * dependencias), tambien los que luego se serviran del cache.  Extraer el
     * conjunto solo necesita el AST y el fuente, y los dos estan.
     * ------------------------------------------------------------------ */
    std::vector<uint8_t> artefacto_comptime;
    CompileOptions opts_modulos = opts;
    /* De momento APAGADO por defecto (`VESTA_ARTEFACTO_TEMPRANO=1` lo activa).
     * El conjunto extraido todavia no compila por si solo: se lleva las
     * funciones comptime y sus dependencias de FUNCION, pero no las
     * declaraciones de TIPO que ese codigo usa, asi que falla con "el operando
     * de '.' debe ser un struct" y "sizeof: tipo no reconocido".  Hasta que el
     * cierre arrastre los tipos, encenderlo solo gasta tiempo -- y compilar el
     * conjunto EN PROCESO toca estado global, que se noto en un caso que dejo
     * de pasar. */
    if (!opts.building_comptime_artifact &&
        util::flag_on(util::FlagId::ArtefactoTemprano)) {
        /* Cada modulo se SUSTITUYE por su propio conjunto comptime, y se
         * compila el mismo programa.
         *
         * Concatenar los conjuntos de todos en un solo texto no vale: al
         * resolverse los `import`, los modulos reales entran tambien y todo
         * queda por duplicado ("redefinicion de simbolo", "alias de tipo
         * redefinido").  Sustituyendolos, el grafo de dependencias es el mismo,
         * cada `import` resuelve a la version recortada y no hay duplicados --
         * ni hace falta arrastrar los tipos de otros modulos, porque cada uno
         * sigue trayendo los suyos.
         *
         * Se puede hacer aqui porque TODOS los modulos estan ya parseados: el
         * resolvedor lo hace para construir el grafo, tambien los que luego se
         * serviran del cache.  Extraer solo necesita el AST y el fuente. */
        std::unordered_map<std::string, std::string> overlay_ct;
        if (source_overlay) overlay_ct = *source_overlay;
        bool hay_comptime = false;
        /* Primero, QUE llama el codigo comptime y no encuentra en su modulo.
         *
         * El cierre de cada modulo es local: solo ve sus decls.  Una funcion
         * que el comptime de un modulo llama por un `import` no viaja en el
         * conjunto del que la DEFINE, y al compilarlo todo junto el que la
         * exporta ya no la tiene.  Se juntan las de todos y se pide el conjunto
         * otra vez, diciendole a cada uno que ademas incluya las que le tocan.
         */
        std::unordered_set<std::string> pedidas;
        for (auto &w : work) {
            if (!w.ast) continue;
            const ComptimeUnit prev = collect_comptime_unit(*w.ast, w.source);
            pedidas.insert(prev.external_calls.begin(),
                           prev.external_calls.end());
        }
        if (util::flag_on(util::FlagId::McVerbose)) {
            std::fprintf(stderr, "[pedidas] %zu:", pedidas.size());
            for (const auto &n : pedidas)
                std::fprintf(stderr, " %s", n.c_str());
            std::fprintf(stderr, "%c", 10);
        }
        for (auto &w : work) {
            if (!w.ast) continue;
            const ComptimeUnit cu =
                collect_comptime_unit(*w.ast, w.source, &pedidas);
            std::string texto;
            /* El `namespace` delante: se lee del AST, donde el `namespace X;`
             * todavia es un nodo -- el aplanado, mas adelante, lo deshace. */
            for (const auto &d : w.ast->decls) {
                if (!d || d->kind != ast::NodeKind::NamespaceDecl) continue;
                const auto *nd =
                    static_cast<const ast::NamespaceDecl *>(d.get());
                if (nd->name.empty()) break;
                texto += "namespace " + nd->name + ";";
                texto.push_back(static_cast<char>(10));
                break;
            }
            texto += cu.unit_source;
            if (!cu.empty()) hay_comptime = true;
            overlay_ct[w.canonical_path.str()] = texto;
            /* El texto que de VERDAD se compila para este modulo.  El volcado
             * por conjunto se salta los modulos sin codigo comptime -- los que
             * solo aportan tipos --, que son justo los que hacen falta mirar
             * cuando una cadena de tipos no resuelve. */
            if (!util::flag_text(util::FlagId::VolcarUnidad).empty()) {
                const std::string &dd =
                    util::flag_text(util::FlagId::VolcarUnidad);
                std::error_code oec;
                std::filesystem::create_directories(dd, oec);
                std::ofstream fo(module_dump_path(dd, "overlay_",
                                                  w.module_name.str(),
                                                  w.canonical_path.str(), ".vx"));
                if (fo) fo << texto;
            }
        }
        if (hay_comptime) {
            /* Compilarlo SIN volver a adelantar nada: el conjunto se contiene a
             * si mismo -- `inject` es un `@Macro` -- y sin la marca construiria
             * el artefacto DEL artefacto, nivel tras nivel. */
            CompileOptions o_ct = opts;
            o_ct.building_comptime_artifact = true;
            o_ct.native_poo = false; // lo ejecuta la VM, no codigo nativo.
            CompileResult cr_ct = compile_vx_project(
                root_path, o_ct, &overlay_ct, extra_search_paths);
            if (util::flag_on(util::FlagId::McVerbose) && !cr_ct.ok) {
                int n = 0;
                for (const auto &dg : cr_ct.diagnostics.all()) {
                    if (dg.level != DiagLevel::ERR) continue;
                    if (++n > 3) break;
                    std::fprintf(stderr, "[conjunto] %u: %s%c", dg.loc.line,
                                 formatted_message(dg).c_str(), 10);
                }
            }
            if (cr_ct.ok && !cr_ct.vel_text.empty()) {
                std::error_code cec;
                /* En la cache, junto al resto de lo que sale de ejecutar al
                 * compilar.  No en el temporal del sistema: esto se REUSA
                 * entre compilaciones -- el nombre sale del hash del texto --,
                 * asi que es cache, y lo que es cache vive donde se limpia. */
                const std::string dir =
                    util::cache_dir(util::CacheKind::Comptime) + "/build";
                std::filesystem::create_directories(dir, cec);
                const std::string pref =
                    dir + "/ct_" +
                    std::to_string(static_cast<unsigned long long>(vxi_fnv1a(
                        cr_ct.vel_text.data(), cr_ct.vel_text.size())));
                if (asm_multi_process::run_worker_from_source(
                        std::string(cr_ct.vel_text), pref + ".vel.tmp", pref,
                        /*skip_preprocessor=*/true, /*keep_labels=*/false,
                        &cr_ct.ir_section_bytes.buf,
                        /*emit_map=*/false, /*nodes=*/nullptr,
                        /*debug_source_file=*/std::string()) == EXIT_SUCCESS) {
                    if (util::read_whole_file(pref + ".velb",
                                              artefacto_comptime) &&
                        !artefacto_comptime.empty()) {
                        opts_modulos.comptime_artifact = &artefacto_comptime;
                        /* Cuantos modulos entraron y cuanto ocupa lo producido.
                         * Es la medida que dice si el conjunto llego COMPLETO:
                         * compila limpio y aun asi puede registrar menos macros
                         * de los que el programa necesita, y entonces lo que
                         * dependa de ellos sale con valores de relleno. */
                        if (util::flag_on(util::FlagId::McVerbose))
                            std::fprintf(
                                stderr, "[conjunto] %zu modulos -> %zu B%c",
                                work.size(), artefacto_comptime.size(), 10);
                    }
                }
            }
        }
    }

    /* Donde esta cada modulo y como se resuelve un import: por namespace
     * completo, que es unico, y solo si no por nombre de fichero, que no lo
     * es.  Se construye aqui, con todos los AST y antes de fusionar los
     * namespaces parciales. */
    const ModuleLookup lookup =
        build_module_lookup(work, auto_imports, auto_import_owner_dir);

    // Simbolos que el parser dejo fuera por @Target, agregados de TODOS los
    // modulos del build.  Usar uno de ellos no es "no existe": existe para
    // otro objetivo, y el diagnostico tiene que distinguirlo.  Se agrega a
    // nivel de proyecto porque el simbolo puede estar descartado en un dep y
    // usarse desde el modulo raiz.
    std::unordered_map<std::string, std::vector<std::string>>
        target_skipped_proyecto;
    for (const auto &w : work) {
        if (!w.ast) continue;
        for (const auto &kv : w.ast->target_skipped) {
            auto &dst = target_skipped_proyecto[kv.first];
            for (const auto &spec : kv.second) {
                if (std::find(dst.begin(), dst.end(), spec) == dst.end())
                    dst.push_back(spec);
            }
        }
    }

    // NS.parcial fix: un mismo `namespace X;` declarado por VARIOS ficheros
    // (p.ej. std.types = types.vx base + types/<arch>.vx) se parsea como
    // modulos SEPARADOS, cada uno con su propio TypeChecker.  Una ref
    // CROSS-FICHERO -- `typedef usize size_t` en la base, con `usize` (newtype)
    // definido en el fichero del arch -- NO resolvia: el TC de la base no ve
    // los simbolos del arch, y el flatten (por-modulo) no manglea la ref.
    // Fix: fusionar las decls de los ficheros SECUNDARIOS en el NamespaceDecl
    // del PRINCIPAL antes de compilar.  Asi el flatten usa un rename_map COMuN
    // (manglea `usize` -> `std__types__usize`) y el TC ve todas las decls en
    // el mismo modulo.  Los secundarios quedan con el NamespaceDecl vacio (se
    // compilan a un .vxi vacio, sin romper el registro del importador).
    {
        auto find_ns_decl = [](ast::ModuleNode *m,
                               const std::string &ns) -> ast::NamespaceDecl * {
            if (!m) return nullptr;
            for (auto &d : m->decls)
                if (d && d->kind == ast::NodeKind::NamespaceDecl) {
                    auto *nd = static_cast<ast::NamespaceDecl *>(d.get());
                    if (nd->name == ns) return nd;
                }
            return nullptr;
        };
        for (const auto &kv : lookup.ns_to_modules) {
            if (kv.second.size() < 2) continue; // no es namespace parcial
            const std::string &ns = kv.first;
            const size_t pidx = kv.second[0];
            ast::NamespaceDecl *pns = find_ns_decl(work[pidx].ast.get(), ns);
            if (!pns) continue;
            for (size_t k = 1; k < kv.second.size(); ++k) {
                ProjectModuleWork &sec = work[kv.second[k]];
                ast::NamespaceDecl *sns = find_ns_decl(sec.ast.get(), ns);
                if (!sns) continue;
                for (auto &d : sns->decls)
                    pns->decls.push_back(std::move(d));
                sns->decls.clear();
                // El source del secundario entra en el hash del principal para
                // que el cache del .vxi se invalide si cualquier fichero del
                // namespace parcial cambia.
                work[pidx].source += "\n";
                work[pidx].source += sec.source;
            }
            // El check de aliases (type_checker) es un SOLO pase ordenado: un
            // alias exige que el tipo del que se deriva ya este procesado.  El
            // orden de las decls fusionadas no lo da el usuario -- lo da la
            // fusion --, asi que ordenarlas es responsabilidad de aqui.
            //
            // Antes se movian los alias PUROS al final, dando por hecho que un
            // newtype nunca deriva de otro newtype del mismo namespace.  Eso es
            // falso: `typedef isize offset new` en el fichero base deriva de
            // `typedef i64 isize new` del fichero del arch, y ambos son
            // newtypes, asi que la particion los dejaba al reves y `std.types`
            // -- del que depende media stdlib -- no compilaba suelto.
            //
            // Se ordenan por DEPENDENCIA REAL entre los alias del namespace,
            // que es la condicion que el pase necesita, en vez de por una
            // propiedad que se le parece.  Los ciclos se dejan en su sitio: el
            // type checker es quien tiene que decir que un alias es circular.
            ordenar_alias_por_dependencia_(pns->decls);
        }
    }

    //  NS.3: PackageId del proyecto (derivado de vx.toml o anonimo).
    // Compartido por todos los modulos del proyecto salvo override @id.
    const std::string project_package_id = derive_package_id(root_path);
    //  NS.3: override @id por-modulo, capturado AQUI (antes de que el
    // flatten elimine el NamespaceDecl del AST durante compile_unit).
    std::vector<std::string> module_pkgid_override(work.size());
    for (size_t i = 0; i < work.size(); ++i) {
        if (!work[i].ast) continue;
        for (const auto &d : work[i].ast->decls) {
            if (!d || d->kind != ast::NodeKind::NamespaceDecl) continue;
            const auto *ns = static_cast<const ast::NamespaceDecl *>(d.get());
            if (!ns->package_id_override.empty()) {
                module_pkgid_override[i] = ns->package_id_override;
                break;
            }
        }
    }

    // 3. Compilar cada modulo en orden topologico (deps primero).
    //
    // Estrategia con cache (M3+M4):
    //   - El ROOT (work.back()) SIEMPRE se compila (es lo que el usuario
    //     pidio compilar; su .velb es el output del comando).
    //   - Los DEPS comprueban el cache `.vxi` + `.vxir` junto al source:
    //     si ambos existen y source_hash coincide con el del .vxi, se
    //     SKIPEAN el lex+parse+typecheck+lower del dep y se carga el IR
    //     directo desde .vxir.  Speedup esperado: 5-20x en builds
    //     incrementales con cache hit.
    //   - Cache desactivable via env VX_NO_CACHE=1.
    const bool cache_enabled = !util::cache_disabled();
    const bool verbose_cache = util::flag_on(util::FlagId::VerboseCache);
    //  M.L21: progreso/feedback al usuario durante compile de
    // proyectos grandes.  Activado via @c VX_VERBOSE_COMPILE=1 .
    // Emit @c [i/N] compiling <name>... al iniciar cada modulo +
    // @c (hit/wrote) al cerrar.  Util para identificar cuellos de
    // botella en build incrementales.
    const bool verbose_compile = util::flag_on(util::FlagId::VerboseCompile);
    //  M.L20: computar niveles topologicos para reporting.  Modulos
    // del MISMO nivel son independientes (pueden paralelizarse).  Esto
    // detecta el potencial paralelismo del proyecto; la ejecucion
    // paralela queda como infraestructura para M8 (ThreadPool dedicado
    // al compiler) -- el refactor del loop a paralelo requiere thread
    // safety review del TypeChecker compartido + file lock cache que
    // M5.A ya cubre via atomic write.
    const std::vector<int> module_levels =
        compute_module_levels(work, lookup);
    int max_level = 0;
    for (int L : module_levels) {
        if (L > max_level) max_level = L;
    }
    if (verbose_compile && max_level > 0) {
        // Reporte de paralelismo potencial.
        std::vector<int> per_level_count(max_level + 1, 0);
        for (int L : module_levels)
            per_level_count[L]++;
        int parallel_opportunities = 0;
        for (int c : per_level_count) {
            if (c > 1) parallel_opportunities += c - 1;
        }
        std::cerr << "[topo] " << work.size() << " modulos en "
                  << (max_level + 1) << " niveles, " << parallel_opportunities
                  << " modulos paralelizables\n";
    }
    /* Lo que el raiz teje en todos -- `@HelperOverride` de cualquier modulo
     * con precedencia del raiz, `@Hook` y `@NoInstrument` del raiz, y la
     * huella de ese tejido --, recogido antes de compilar ningun modulo: los
     * demas se bajan antes que el raiz.  La misma pieza que usa el camino de
     * fichero suelto. */
    RootWeaving root_weaving;
    if (!collect_root_weaving(work, res, root_weaving)) return res;
    const uint64_t hooks_source_fp = root_weaving.hooks_source_fp;

    //  M8: refactor del loop body a lambda para enable dispatch paralelo
    // por nivel topo.  La lambda captura todo el entorno por referencia.
    // Cada thread tiene su propio @c pm = work[i] por diseno (slots distintos
    // del vector, no overlap).  Lectura cross-thread de @c work[dep_idx] es
    // safe porque los deps estan en niveles topologicos ANTERIORES y ya
    // finalizaron antes de que este nivel empiece (barrier por nivel).
    // Mutex para verbose output: en modo paralelo, los @c std::cerr de
    // multiples threads pueden interleaved a nivel de byte (cerr no es
    // line-buffered atomic).  Construimos la linea en un string local y
    // hacemos una sola @c cerr<< con lock.  En modo secuencial, el lock
    // es un no-op virtual (un solo thread nunca contiende).
    std::mutex verbose_mtx;
    // HALLAZGO-2: capturar el override de @Target (thread_local del parser) en
    // el main thread.  Se usa para (a) mezclarlo en el source_hash del cache
    // -> PE y ELF del MISMO proyecto no comparten `.vxir` (si no,
    // cross-compilar a un target y luego a otro reusaba el IR del target
    // equivocado), y (b) re-aplicarlo en los workers del compile paralelo (el
    // thread_local arranca vacio en un thread nuevo).
    std::string cc_tgt_os, cc_tgt_arch;
    vx::get_aot_condcomp_target(cc_tgt_os, cc_tgt_arch);

    // CAS global direccionado por contenido (opt-in via VX_CAS_DIR): tier de
    // cache ADICIONAL, independiente de la ruta, que permite reusar el
    // artefacto de un modulo (interfaz + IR) entre proyectos distintos y entre
    // maquinas. Ejemplo: la stdlib se compila una sola vez para toda la
    // maquina.  Default OFF -> comportamiento y builds actuales intactos.
    // Comparte un solo CasStore entre threads (sus ops de fichero son atomicas
    // / read-only).
    std::unique_ptr<CasStore> cas;
    if (cache_enabled && util::flag_present(util::FlagId::CasDir))
        cas = std::make_unique<CasStore>(CasStore::open_default());
    // Config de build que afecta al IR pre-optimize (fold en la clave del CAS).
    // Layered: opt_level / aot_vec_width / os/arch de codegen NO entran (son
    // post-merge) -> el IR se comparte entre esas configs.  Su fingerprint
    // COMPLETO (BuildConfig::full_fingerprint) queda para el futuro cache del
    // ARTEFACTO FINAL (.velb/.exe AOT).
    uint64_t cas_config_fp = 0;
    if (cas) {
        BuildConfig bcfg;
        bcfg.asm_target_bits = opts.asm_target_bits;
        bcfg.native_poo = opts.native_poo;
        bcfg.exceptions_enabled = opts.exceptions_enabled;
        bcfg.instrument_mode = opts.instrument_mode;
        /* Los `@Hook` del raiz tejen llamadas en el IR de los DEMAS modulos,
         * cuyo fuente no ha cambiado por ello.  Sin meterlos en la huella se
         * les serviria el artefacto cacheado SIN instrumentar: no daria error,
         * daria un programa que dice medir y no mide.
         *
         * Basta la huella de lo que cambia el tejido -- punto, selector y
         * campos pedidos --, no el cuerpo del gancho: cambiarle el cuerpo
         * cambia SU modulo, y de eso ya se encarga el hash del fuente. */
        // La MISMA huella que invalida los artefactos de junto al fuente: dos
        // criterios distintos acabarian con uno invalidando y el otro no.
        bcfg.hooks_fp = hooks_source_fp;
        // tgt_os/tgt_arch quedan vacios: la precision por-modulo de @Target la
        // aporta cache_tgt_suffix (solo divide los modulos que USAN @Target).
        cas_config_fp = bcfg.ir_fingerprint();
    }

    /* Lo que se sabe del programa vive TODA la compilacion, no lo que dure un
     * consumidor.  Declararlo dentro de quien pregunta -- como estaba -- hacia
     * que el conocimiento naciera y muriera con la pregunta, asi que el
     * siguiente que llegara volveria a calcularlo: justo lo contrario de
     * centralizarlo. */
    analysis::asa::FactStore facts;
    /* Y su identidad, para que sobreviva a la compilacion.  Es la MISMA clave
     * con la que se identifica el modulo en el almacen por contenido: el
     * fuente, de que depende, y con que se compilo.  Cero mientras no se sepa,
     * y entonces no se toca el disco -- un fichero de hechos sin identidad no
     * se puede comprobar, y aceptar hechos de otro programa es peor que no
     * tener cache. */
    uint64_t root_facts_key = 0;

    /* Lo que la compilacion de cada modulo lee del proyecto, por nombre: ver
     * `vx/unit/unit_env.h`.  Cada modulo lo compila `compile_unit`. */
    UnitEnv env;
    env.work = &work;
    env.lookup = &lookup;
    env.levels = &module_levels;
    env.opts = &opts;
    env.opts_modules = &opts_modulos;
    env.cache.enabled = cache_enabled;
    env.cache.verbose = verbose_cache;
    env.cache.cas = cas.get();
    env.cache.cas_config_fp = cas_config_fp;
    env.cache.hooks_source_fp = hooks_source_fp;
    env.cache.target_os = &cc_tgt_os;
    env.cache.target_arch = &cc_tgt_arch;
    env.root = root_weaving.view();
    env.generic_instances = share_instances ? &generic_instances : nullptr;
    env.target_skipped = &target_skipped_proyecto;
    env.project_package_id = &project_package_id;
    env.module_package_override = &module_pkgid_override;
    env.verbose_compile = verbose_compile;
    env.verbose_mtx = &verbose_mtx;
    env.res = &res;
    env.root_facts_key = &root_facts_key;

    /**
     * Suelta de un modulo YA COMPILADO lo que nadie va a volver a mirar.
     *
     * POR QUE AQUI Y NO AL FINAL DE `compile_unit`: el objeto `Lowering`
     * guarda REFERENCIAS al AST y al comprobador de tipos, y destruirlos con
     * una referencia viva encima es un fallo que no da la cara hasta que
     * alguien anada algo al destructor de `Lowering`.  Hoy `Lowering` ya muere
     * al salir de `lower_unit`, pero soltar desde fuera, cuando la compilacion
     * del modulo ha vuelto entera, no depende de eso.
     *
     * EL ROOT NO SE SUELTA: su AST lo siguen mirando los diagramas y la
     * inyeccion diferida, y es UNO de veintiun modulos.
     *
     * @param pm      El modulo, ya compilado.
     * @param is_root Si es el modulo raiz.
     */
    auto release_compiled_module = [](ProjectModuleWork &pm, bool is_root) {
        if (is_root) return;
        pm.ast.reset();
        pm.tc.reset();
        /* Y el fuente, que tampoco lo lee nadie mas: son megabytes por
         * proyecto y no hay ninguna pregunta pendiente sobre el texto.  Con
         * `swap` y no con `clear`, que conserva la reserva. */
        std::string().swap(pm.source);
    };

    //  M8: dispatch.  Por defecto secuencial (preserve cache hit
    // determinism y el orden de @c verbose_compile output).  Activado via
    // env @c VX_PARALLEL_COMPILE=N (N >= 1).  N=1 fuerza secuencial (util
    // para diagnostico).  N >= 2 corre hasta N modulos del MISMO nivel
    // topologico en paralelo via @c std::thread + join al final de cada
    // nivel (barrier natural).  Modulos en niveles distintos NUNCA se
    // solapan: el barrier garantiza que los deps esten finalizados antes
    // de que un consumer empiece.
    int parallel_threads = 0;
    const bool env_present = util::flag_present(util::FlagId::ParallelCompile);
    if (env_present) {
        /* Un valor que no sea un numero cuenta como 0, igual que antes: el
         * registro ya devolvio 0 si no pudo leerlo, sin lanzar. */
        const long n = util::flag_int(util::FlagId::ParallelCompile, 0);
        parallel_threads = (n < 0 || n > INT_MAX) ? 0 : static_cast<int>(n);
    }
    /* Cuantos modulos a la vez, DINAMICO con la maquina.
     *
     * `VX_PARALLEL_COMPILE=1` fuerza secuencial (diagnostico o salida
     * determinista) y `>=2` fija N exacto.  Sin la variable, sale de los
     * nucleos que haya.
     *
     * Habia un tope FIJO de 8, puesto en 2026-06-05 porque ">8 daba
     * rendimientos decrecientes por contencion en las escrituras de cache y en
     * el mutex de verbose".  Un tope fijo envejece con la maquina: en una de
     * veinticuatro nucleos dejaba dieciseis sin usar, y la contencion que lo
     * motivo no es la misma de entonces.
     *
     * Ahora el suelo es LA MITAD de los nucleos -- nunca menos, aunque la
     * contencion aparezca -- y el techo son todos.  Se queda en el numero de
     * modulos del nivel: mas hilos que trabajo no aceleran nada y pagan su
     * creacion.
     *
     * Los proyectos triviales (un modulo por nivel) no pagan overhead: el
     * reparto solo crea hilos cuando un nivel tiene dos o mas. */
    if (!env_present || parallel_threads == 0) {
        unsigned cores = std::thread::hardware_concurrency();
        if (cores < 1) cores = 1;
        /* Todos los nucleos, y nunca menos de la mitad.  El minimo se escribe
         * aunque hoy la primera parte ya lo cumpla: es la garantia que se
         * quiere sostener si manana se vuelve a poner un tope. */
        const unsigned half = (cores + 1u) / 2u;
        parallel_threads = static_cast<int>(cores < half ? half : cores);
    }
    /* Cuanto intermedio hay vivo.  Con `VX_IR_RAM_MAX_MIB` puesto, lo que pase
     * del techo baja a disco -- ver `spill_until_under_ceiling` --.  Sin el,
     * este contador se queda en cero y nadie lo mira. */
    size_t ir_ram_live = 0;

    if (parallel_threads <= 1) {
        // Path secuencial: identico al comportamiento pre-M8.
        for (size_t i = 0; i < work.size(); ++i) {
            compile_unit(env, i);
            release_compiled_module(work[i], i + 1 == work.size());
            account_and_spill(work, i, ir_ram_live, res.diagnostics,
                              verbose_compile);
            /* Un modulo terminado es una frontera tan buena como una fase: su
             * AST y su comprobador de tipos acaban de irse, y son megabytes de
             * un tamano que el modulo siguiente no tiene por que volver a
             * pedir.  Una vez por modulo no es un bucle caliente. */
            util::release_between_phases();
        }
    } else {
        // Path paralelo: agrupar modulos por nivel topologico.
        std::vector<std::vector<size_t>> by_level(max_level + 1);
        for (size_t i = 0; i < module_levels.size(); ++i) {
            by_level[module_levels[i]].push_back(i);
        }
        if (verbose_compile) {
            std::cerr << "[parallel] threads=" << parallel_threads
                      << " niveles=" << (max_level + 1) << "\n";
        }
        for (int L = 0; L <= max_level; ++L) {
            const auto &mods = by_level[L];
            if (mods.empty()) continue;
            // Si el nivel tiene 1 solo modulo, no merece thread.
            if (mods.size() == 1) {
                compile_unit(env, mods[0]);
                release_compiled_module(work[mods[0]],
                                        mods[0] + 1 == work.size());
                account_and_spill(work, mods[0], ir_ram_live, res.diagnostics,
                                  verbose_compile);
                continue;
            }
            // Particionar @c mods en lotes de tamano @c parallel_threads .
            // Cada lote se ejecuta concurrentemente; los lotes son
            // secuenciales entre si.  Ejemplo: 5 modulos en L0 con
            // threads=2 -> lotes [0,1], [2,3], [4].
            const size_t chunk = static_cast<size_t>(parallel_threads);
            for (size_t base = 0; base < mods.size(); base += chunk) {
                const size_t end_idx = std::min(base + chunk, mods.size());
                /* Reservar los hilos de ESTE lote en el presupuesto comun.
                 *
                 * Sin esto, cada modulo vuelve a repartir sus funciones en el
                 * pool -- que es UNO para todo el proceso -- y N modulos por N
                 * workers dejan N*N hilos listos sobre los nucleos que haya;
                 * los que esperan giran con `yield()` quitandole el nucleo al
                 * que trabaja.  Reservando, el reparto de dentro ve la maquina
                 * ya ocupada y se ajusta a lo que sobre en vez de apagarse: la
                 * fase mas cara trabaja sobre UN modulo, y prohibirlo del todo
                 * dejaba la maquina al 2 %. */
                const ir::OuterParallelScope batch_reservation(
                    static_cast<unsigned>(end_idx - base));
                std::vector<std::thread> threads;
                threads.reserve(end_idx - base);
                for (size_t k = base; k < end_idx; ++k) {
                    std::string cc_modo;
                    vx::get_aot_condcomp_mode(cc_modo);
                    std::string cc_tier;
                    bool cc_sin_libc = false;
                    vx::get_aot_condcomp_tier(cc_tier, cc_sin_libc);
                    threads.emplace_back([&env,
                                          &release_compiled_module, &work,
                                          idx = mods[k], cc_tgt_os, cc_tgt_arch,
                                          cc_modo, cc_tier, cc_sin_libc]() {
                        // HALLAZGO-2: re-aplicar el target de @Target en
                        // este worker antes de parsear (el thread_local del
                        // parser arranca vacio en un thread nuevo).
                        if (!cc_tgt_os.empty() || !cc_tgt_arch.empty())
                            vx::set_aot_condcomp_target(cc_tgt_os, cc_tgt_arch);
                        /* Los otros dos ejes van por el MISMO thread_local y
                         * arrancan igual de vacios aqui.  Sin esto, un modulo
                         * compilado en paralelo veia `mode:aot` falso y
                         * cualquier `tier:` falso -- y una variante marcada
                         * para el tier del binario desaparecia sin decir nada,
                         * segun en que hilo cayera el modulo. */
                        if (!cc_modo.empty())
                            vx::set_aot_condcomp_mode(cc_modo);
                        if (!cc_tier.empty())
                            vx::set_aot_condcomp_tier(cc_tier, cc_sin_libc);
                        compile_unit(env, idx);
                        /* Cada hilo suelta LO SUYO: el modulo es de este hilo
                         * hasta la barrera, asi que no hace falta cerrojo. */
                        release_compiled_module(work[idx],
                                                idx + 1 == work.size());
                    });
                }
                // Barrier: esperar todos los threads del lote antes de
                // pasar al siguiente.  Necesario porque modulos del mismo
                // nivel pueden compartir cache writes que deben terminar
                // antes de que el siguiente nivel intente cache hit.
                for (auto &t : threads)
                    t.join();
                /* Y lo que dejaron los hilos que acaban de morir, de vuelta al
                 * reparto comun.
                 *
                 * Al morir, un hilo hace lo MINIMO -- devuelve su identificador
                 * y nada mas --, porque el desmontaje de un hilo es donde el
                 * asignador ya se colgo una vez.  Sus listas libres y los
                 * bloques que otros hilos le soltaron se quedan esperando a que
                 * alguien vuelva a pedir ese identificador Y esa clase de
                 * tamano.  Medido sobre 21 modulos: 5,4 millones de bloques
                 * liberados desde un hilo distinto del que los pidio.
                 *
                 * La barrera es el momento exacto: aqui no queda ningun hilo
                 * del lote trabajando, y es lo que el propio asignador
                 * documenta como uso de esta llamada -- entre fases, nunca en
                 * un bucle caliente. */
                util::release_between_phases();
                /* El techo se aplica AQUI y no dentro del hilo: desalojar mira
                 * el estado de los OTROS modulos, y dentro del lote los hay
                 * compilandose.  Tras la barrera no queda nadie trabajando,
                 * asi que se lee sin candado y sin carrera. */
                for (size_t k = base; k < end_idx; ++k)
                    account_and_spill(work, mods[k], ir_ram_live,
                                      res.diagnostics, verbose_compile);
            }
        }
    }

    /* Lo de cada modulo al resultado -- diagnosticos, conjunto comptime, lo de
     * cada `@Macro`, las huellas del grafo de depuracion --, DESPUES del bucle
     * y en orden de indice: dentro, hasta ocho hilos escribian sobre el mismo
     * resultado sin candado (moria con 0xC0000374).  La misma pieza que el
     * camino de fichero suelto. */
    if (!gather_unit_results(work, res)) return res;

    /* LAS INTERFACES, QUE YA NO LAS LEE NADIE.
     *
     * El `.vxi` de un modulo se emite y se escribe DENTRO de su compilacion, y
     * lo unico que le piden despues sus sucesores es el `abi_hash` -- ocho
     * bytes -- para su tabla de dependencias, y eso pasa dentro de este mismo
     * bucle.  De aqui en adelante no lo mira nadie: ni el merge, ni el
     * optimizador, ni el emisor.
     *
     * Y pesa: es la tabla de simbolos exportados de cada modulo, con sus
     * namespaces.  Medido en el corte del pico, 17,9 MiB solo en lo que el
     * export deja puesto.
     *
     * Se asigna uno vacio en vez de vaciarlo campo a campo: no hay que saber
     * que lleva dentro para soltarlo, y el dia que lleve otra cosa esto sigue
     * valiendo. */
    for (ProjectModuleWork &pm : work)
        pm.vxi = VxiModule{};

    /* Y aqui acaba de compilar modulos, que es una fase y no se llamaba de
     * ninguna forma: en la curva se veia como una caida de 231 MiB sin nombre
     * -- las estructuras de cada modulo muriendo segun se funden en el root --
     * cuatro cortes antes de la marca de optimizar. */
    cerrar_fase(res.times.modules_us, "vx.phase.merge");

    // 4. Merge IR de todos los modulos en uno solo.
    //
    // Estrategia: el modulo ROOT (work.back() en topo order, ya que el
    // root es el ultimo) se usa como destino base.  Los demas modulos
    // contribuyen sus IrFunctions, static_data y globals.
    //
    // Nota: el orden topologico garantiza que el root es el ULTIMO.
    // Lo verificamos defensivamente.
    if (work.empty()) {
        res.ok = false;
        return res;
    }
    ir::IrModule &merged = work.back().ir;

    //  M.L25: tree-shaking opt-in via VX_TREE_SHAKE=1 .  Si el root
    // hace `import "lib" only X, Y` y NINGUNO de X, Y aparece en
    // referenced_names() del root, el dep NO se mergea al .velb final.
    // Reduce el tamano de programas con muchos deps opcionales.
    //
    // SEMANTICA: solo se tree-shakean deps cuyos imports son TODOS `only`
    // sin usar Y el dep NO declara clases (las clases requieren ejecutar
    // __module_init para registrarse en el ClassRegistry runtime; saltar
    // el merge perderia eso).  Plain imports (`import "x";` sin only)
    // jamas se shake-an (son referencia opaca, dificil de demostrar
    // unused).
    const bool tree_shake = util::flag_on(util::FlagId::TreeShake);
    std::unordered_set<size_t> shaken_indices;
    if (tree_shake && work.size() >= 2) {
        const auto &root_pm = work.back();
        const auto &root_refs = root_pm.tc ? root_pm.tc->referenced_names()
                                           : std::unordered_set<std::string>{};
        const auto root_imports = lookup.imports_of(root_pm);
        for (const auto &req : root_imports) {
            if (req.is_plain) continue;           // namespace -> nunca shake
            if (req.is_public_reexport) continue; // re-export consume el dep
            const size_t dep_idx = lookup.find(req);
            if (dep_idx >= work.size()) continue;
            // Verificar si el dep declara clases (no shake-able).
            const auto &dep_pm = work[dep_idx];
            /* DEL RESUMEN DEL DEP, no de su AST, que para cuando se llega aqui
             * ya se solto.  Mismo criterio y mismo resultado que mirarlo a
             * mano: lo apunta quien compila el modulo, en su propio AST.
             *
             * SIGUE ABIERTO -- y no lo abre esto -- el caso del dep SERVIDO DEL
             * CACHE: no se parsea nunca, asi que nadie le pone el bit y aqui
             * cuenta como "no declara clases".  Antes pasaba igual, por la via
             * de un `ast` nulo.  Cerrarlo pide guardar el bit en el `.vxi`, que
             * es tocar el formato del artefacto. */
            if (dep_pm.has_classes) continue;
            // Verificar si TODOS los only simbolos estan sin usar.
            bool all_unused = true;
            for (const auto &os : req.only_symbols) {
                const std::string &local =
                    os.rename.empty() ? os.name : os.rename;
                if (root_refs.find(local) != root_refs.end()) {
                    all_unused = false;
                    break;
                }
            }
            if (all_unused && !req.only_symbols.empty()) {
                shaken_indices.insert(dep_idx);
                if (verbose_compile) {
                    std::cerr << "[tree-shake] dep '" << req.module_name
                              << "' eliminado (ninguno de "
                              << req.only_symbols.size()
                              << " simbolos importados se usa)\n";
                }
            }
        }
    }

    /* Y EL COMPROBADOR DE TIPOS DEL ROOT, que era el ultimo que quedaba vivo.
     *
     * Los demas se sueltan al acabar su modulo (`release_compiled_module`); el
     * del root se conservaba junto con su AST, pero solo el AST hace falta
     * despues -- los diagramas y el escaneo de `@AllocatorOverride` --.  Del
     * comprobador, la ultima pregunta es `referenced_names()` del tree-shake,
     * aqui mismo unas lineas arriba.
     *
     * Y pesa porque el root IMPORTA TODO: sus `imported_namespaces_` guardan
     * los simbolos publicos de los veinticuatro modulos.  Medido en el corte
     * del pico, 21,6 MiB en `register_namespace_symbol`. */
    /* Antes de soltarlo, LO QUE SOLO EL SABE: que instancia del proveedor
     * reserva y cual suelta.
     *
     * Mas abajo se decide con que simbolo se cablea el asignador, y alli se
     * preguntaba por el comprobador -- que para entonces ya no existe --, asi
     * que la pregunta se saltaba en silencio y quedaba el nombre de la
     * DECLARACION, que para una plantilla no es ningun simbolo.  No se notaba
     * mientras todas las liberaciones estuvieran ESCRITAS (esas se reescriben a
     * la instancia al comprobar); asomaba en cuanto el compilador emitia una
     * por su cuenta -- la limpieza de un parametro `string` --, y entonces el
     * enlazado pedia `...__vx_free` y nadie lo habia emitido. */
    std::string root_alloc_sym;
    std::string root_free_sym;
    if (!work.empty() && work.back().tc) {
        root_alloc_sym = work.back().tc->raw_alloc_symbol();
        root_free_sym = work.back().tc->raw_free_symbol();
    }
    work.back().tc.reset();

    // #cross-module-generics: dedup de funciones por nombre al mergear.  Una
    // misma instanciacion `Caja_i64__leer` puede producirse en VARIOS modulos
    // (cada uno inyecta la plantilla y monomorphiza on-use); son IDENTICAS por
    // construccion (mismo template + mismos args), asi que se conserva UNA.
    // Sin esto, el linker veria simbolos duplicados.  Modelo COMDAT de C++.
    std::unordered_set<std::string> merged_fn_names;
    merged_fn_names.reserve(merged.functions.size() * 2);
    for (const auto &fn : merged.functions)
        merged_fn_names.insert(fn.name);

    /* SE PROBO A RESERVAR `merged.functions` aqui -- el total se sabe sumando
     * lo que trae cada modulo -- y NO PAGA: pico 2.848 -> 2.849 MiB y tiempo
     * 1.660 -> 1.675 ms, las dos cosas dentro del ruido.  Las diecisiete
     * reasignaciones son trafico que el asignador recicla, no memoria
     * residente, y el pico esta en otra fase.  Se deja dicho para que no se
     * vuelva a intentar a ciegas. */

    for (size_t i = 0; i + 1 < work.size(); ++i) {
        if (shaken_indices.count(i)) continue; // L.25: skip dep no usado
        /* Y si sus cuerpos estaban en disco, vuelven AHORA -- justo antes de
         * que alguien pregunte, que es el momento en que hacen falta.  Se
         * restaura DESPUES del tree-shake: un dep que no entra al programa no
         * se lee de vuelta para nada.
         *
         * Fallar aqui no se puede tragar: sin los cuerpos el modulo aporta
         * cero funciones y el programa sale sin ellas -- no un error, otro
         * programa --.  Asi que se dice y se aborta. */
        {
            std::string spill_err;
            if (!restore_module_ir(work[i], spill_err)) {
                SourceLoc loc;
                loc.set_file(work[i].canonical_path);
                res.diagnostics.diag(std::move(loc), DiagLevel::ERR, "VX4006",
                                     {work[i].module_name.str(), spill_err});
                res.ok = false;
                return res;
            }
        }
        auto &dep_ir = work[i].ir;
        // BugFix M.sd: remapeo de STR_LIT_ADDR.imm al mergear static_data.
        // Cada modulo usa indices locales 0..N-1 para sus literales.  Al
        // concatenar el static_data del dep tras el del root, los indices
        // del dep deben desplazarse en @c offset = merged.static_data.size()
        // ANTES de mover las funciones.  Sin esto, las STR_LIT_ADDR del dep
        // (`s_0`, `s_1`, ...) apuntan a las strings del root tras el merge
        // y se imprime garbage (cross-module string aliasing).
        // BugFix M.mi: renombrar el `__module_init` del dep a un nombre
        // unico (`__module_init_<modname>`) y NO permitir colision con la
        // del root.  Sin esto, el merge genera multiples labels
        // `__module_init` y solo se ejecuta la primera -- las clases de
        // los deps no se registran y los `new dep.Class()` crashean.  El
        // root encadena llamadas a las del dep via injeccion de CALLs en
        // su propia __module_init (mas abajo).
        const std::string dep_mod_init =
            module_init_symbol(work[i].module_name.str());
        /* Se renombran TODAS las `__module_init*` del dep, no solo la
         * principal.  Desde que se parte en tandas, cada modulo trae ademas
         * sus `__module_init_partN`, y esos nombres son los MISMOS en todos
         * los modulos: sin esto, dos deps aportan `__module_init_part0` y el
         * merge se queda con una.
         *
         * Y renombrar obliga a reescribir las llamadas, porque a diferencia de
         * la principal -- que el cargador invoca por `init_pc` y nadie nombra
         * -- las tandas SI se llaman por nombre desde ella. */
        std::unordered_map<std::string, std::string> init_renames;
        for (auto &fn : dep_ir.functions) {
            if (!ir::is_module_init_family(fn.name)) continue;
            const std::string renamed =
                ir::is_module_init(fn.name)
                    ? dep_mod_init
                    : module_init_part_symbol(fn.name, work[i].module_name.str());
            init_renames.emplace(fn.name, renamed);
            fn.name = renamed;
        }
        if (!init_renames.empty()) {
            for (auto &fn : dep_ir.functions)
                for (auto &blk : fn.blocks)
                    for (auto &in : blk.instrs) {
                        if (in.op != ir::IrOp::CALL &&
                            in.op != ir::IrOp::TAILCALL)
                            continue;
                        auto it = init_renames.find(in.func_name);
                        if (it != init_renames.end()) in.func_name = it->second;
                    }
        }
        const uint64_t sd_offset =
            static_cast<uint64_t>(merged.static_data.size());
        if (sd_offset != 0) {
            // Helper: en cualquier string que contenga subcadenas
            // `code.s_<N>` (referencias a static_data del dep), reemplaza
            // <N> por <N + sd_offset>.  Cubre RAW_ASM, etiquetas de
            // findclass, cache slots de clase y otros literales s_*.
            auto remap_static_refs = [sd_offset](std::string &s) {
                std::string out;
                out.reserve(s.size());
                size_t i = 0;
                while (i < s.size()) {
                    size_t p = s.find("code.s_", i);
                    if (p == std::string::npos) {
                        out.append(s, i, s.size() - i);
                        break;
                    }
                    out.append(s, i, p - i);
                    out.append("code.s_");
                    p += 7;
                    // Parsear los digitos.
                    size_t j = p;
                    uint64_t num = 0;
                    while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                        num = num * 10 + static_cast<uint64_t>(s[j] - '0');
                        ++j;
                    }
                    out.append(std::to_string(num + sd_offset));
                    i = j;
                }
                s = std::move(out);
            };
            for (auto &fn : dep_ir.functions) {
                for (auto &bb : fn.blocks) {
                    for (auto &ins : bb.instrs) {
                        if (ins.op == ir::IrOp::STR_LIT_ADDR) {
                            ins.imm += sd_offset;
                            continue;
                        }
                        if (ins.op == ir::IrOp::RAW_ASM) {
                            // El texto del RAW_ASM esta en func_name.
                            if (!ins.func_name.empty()) {
                                remap_static_refs(ins.func_name);
                            }
                        }
                    }
                }
            }
        }
        /* Los indices de fichero del dep son de SU tabla, y al pasar sus
         * funciones a la fusionada pasan a leerse contra OTRA.  Es el mismo
         * problema que los literales de arriba, y falla igual de callado: el
         * indice sigue siendo valido, asi que nadie da un error -- se cita un
         * fichero que no tiene nada que ver.
         *
         * Se traduce una vez por dep, no una por funcion: son las mismas pocas
         * rutas para todas. */
        util::SmallVector<uint32_t, 4> file_remap;
        file_remap.reserve(dep_ir.source_files.size());
        for (const std::string *p : dep_ir.source_files)
            file_remap.push_back(merged.intern_source_file(p));
        for (auto &fn : dep_ir.functions) {
            // #cross-module-generics: dedup -- saltar funciones cuyo nombre
            // ya existe (monomorphizaciones identicas de otro modulo).  Las
            // synteticas por-modulo (`__module_init_<mod>`) ya son unicas.
            if (!fn.name.empty() && !merged_fn_names.insert(fn.name).second)
                continue;
            fn.source_file = remap_source_file(file_remap, fn.source_file);
            /* Y los de dentro: una funcion que ya traia codigo inlinado lleva
             * el fichero de donde vino CADA trozo, que es de otro modulo. */
            for (ir::InlineSite &s : fn.inline_sites)
                s.source_file = remap_source_file(file_remap, s.source_file);
            merged.functions.push_back(std::move(fn));
        }
        // M.staticdata-pool: el storage canonico es ahora un pool unico
        // por modulo.  El merge usa @c append_raw_entries para concatenar
        // bytes + reescribir offsets en un solo paso O(total_bytes).
        merged.static_data.append_raw_entries(std::move(dep_ir.static_data));
        /* Los structs de la dependencia, con el origen de sus metodos: cada
         * modulo cuenta solo los que DEFINE, asi que no se repiten. */
        for (ir::IrStructType &st : dep_ir.struct_types)
            merged.struct_types.push_back(std::move(st));
        // Globals: merge insertando entradas del dep en el mapa del root.
        // Si una entrada ya existe en root, dep gana? No -- dep no
        // sobrescribe (root tiene prioridad).  En la practica los nombres
        // no colisionan en MVP.
        for (auto &gv : dep_ir.globals) {
            merged.globals.emplace(gv.first, gv.second);
        }
        // BugFix M.ni: native_imports cross-module.  Sin este merge,
        // los CALLN emitidos desde un dep (e.g. `vesta_io:vio_print` al
        // usar @c print desde un metodo de clase) generan un simbolo no
        // resuelto en el linker.  La funcion @c register_native_import
        // ya deduplica internamente, asi que llamarla directo es seguro.
        /* Con lo DECLARADO sobre cada nativa: si el dep dijo lo que hace, esa
         * es la unica copia que hay de ese dato, y perderla aqui devuelve la
         * funcion a "puede hacer cualquier cosa" en el modulo fusionado -- que
         * es el que se analiza. */
        for (auto &ni : dep_ir.native_imports) {
            merged.register_native_import(ni.lib, ni.name, ni.effects);
        }
    }

    // BugFix M.mi: injetar al inicio del @c __module_init del root un
    // CALL a cada @c __module_init_<dep> mergeado.  Sin esto, las
    // clases declaradas en deps nunca se registran (sus defclass viven
    // dentro de su propio @c __module_init , que el main no llama
    // directamente).  Se ejecutan en topo order (deps primero).
    {
        std::vector<std::string> dep_init_names;
        for (size_t i = 0; i + 1 < work.size(); ++i) {
            if (shaken_indices.count(i) != 0) continue;
            // Verifica que existe un @c __module_init_<dep> entre las
            // funciones mergeadas (algunos deps sin clases/globals no
            // tienen uno; saltar silente).
            const std::string nm = module_init_symbol(work[i].module_name.str());
            for (const auto &fn : merged.functions) {
                if (fn.name == nm) {
                    dep_init_names.push_back(nm);
                    break;
                }
            }
        }
        if (!dep_init_names.empty()) {
            bool found = false;
            for (auto &fn : merged.functions) {
                if (!ir::is_module_init(fn.name)) continue;
                if (fn.blocks.empty()) continue;
                auto &entry = fn.blocks.front();
                std::vector<ir::IrInstr> head;
                head.reserve(dep_init_names.size());
                for (const auto &dn : dep_init_names) {
                    ir::IrInstr c{};
                    c.op = ir::IrOp::CALL;
                    c.type = ir::IrType::I64;
                    c.dst = ir::IR_NO_VALUE;
                    c.func_name = dn;
                    head.push_back(std::move(c));
                }
                entry.instrs.insert(entry.instrs.begin(),
                                    std::make_move_iterator(head.begin()),
                                    std::make_move_iterator(head.end()));
                found = true;
                break;
            }
            // Si el root no genero su propio __module_init pero hay deps
            // con classes, creamos un stub que solo encadena llamadas.
            if (!found) {
                ir::IrFunction stub;
                stub.name = ir::kModuleInit;
                stub.ret_type = ir::IrType::I64;
                const ir::IrBlockId entry = stub.new_block("entry");
                for (const auto &dn : dep_init_names) {
                    ir::IrInstr c{};
                    c.op = ir::IrOp::CALL;
                    c.type = ir::IrType::I64;
                    c.dst = ir::IR_NO_VALUE;
                    c.func_name = dn;
                    stub.append(entry, std::move(c));
                }
                ir::IrInstr ret{};
                ret.op = ir::IrOp::RET;
                ret.type = ir::IrType::I64;
                ret.dst = ir::IR_NO_VALUE;
                stub.append(entry, std::move(ret));
                merged.functions.push_back(std::move(stub));
            }
        }
    }

    // CPU dispatch cross-module: los globals fp-table (__vx_*_fp,
    // __vx_cpu_features) son program-globales (unificados arriba por
    // shared_key), pero los `__vx_*_init` que los inicializan se preponen
    // a `main` SOLO en el modulo que baja `main` (Lowering::run).  Si el
    // ROOT no usa dispatch pero un DEP si (p.ej. el dep llama s.length()
    // -> __vx_strlen_fp), el init existe como funcion pero nunca se
    // llama -> el slot queda en 0 -> call a fp nulo -> SEGV.  Aqui, sobre
    // el modulo mergeado, prepondemos a `main` las CALLs a los inits que
    // existan y que main aun no invoque (idempotente: si el root ya las
    // prepuso, se detectan y no se duplican).  Orden de ejecucion:
    // __vx_cpu_init (cpuid) -> __vx_memcpy_init -> __vx_strdisp_init.
    {
        // Que inits existen tras el merge, por su posicion en el orden.
        bool has[ir::rt::kDispatchInitCount] = {};
        bool any = false;
        for (const auto &fn : merged.functions) {
            const int idx = ir::rt::dispatch_init_index(fn.name);
            if (idx < 0) continue;
            has[idx] = true;
            any = true;
        }
        if (any) {
            for (auto &fn : merged.functions) {
                if (fn.name != "main" || fn.blocks.empty()) continue;
                auto &ins = fn.blocks.front().instrs;
                // Detectar inits ya presentes (idempotencia).
                bool have[ir::rt::kDispatchInitCount] = {};
                for (const auto &x : ins) {
                    if (x.op != ir::IrOp::CALL) continue;
                    const int idx = ir::rt::dispatch_init_index(x.func_name);
                    if (idx >= 0) have[idx] = true;
                }
                // Se anteponen del ULTIMO al primero: insertar al principio
                // invierte el orden, asi que el que tiene que correr antes
                // queda delante.
                for (int i = ir::rt::kDispatchInitCount - 1; i >= 0; --i) {
                    if (!has[i] || have[i]) continue;
                    ir::IrInstr c{};
                    c.op = ir::IrOp::CALL;
                    c.type = ir::IrType::VOID;
                    c.dst = ir::IR_NO_VALUE;
                    c.func_name = ir::rt::kDispatchInits[i];
                    c.source_line = 0;
                    ins.insert(ins.begin(), std::move(c));
                }
                break;
            }
        }
    }

    // Dedup de las funciones synthetic del CPU dispatch (`__vx_*`): el
    // root y cada dep emiten su propio juego de helpers (__vx_strlen_base,
    // __vx_memcpy_init, etc.) con nombres identicos.  Tras el merge habria
    // colision de simbolos en el linker AOT.  Mantener la PRIMERA aparicion
    // (root primero, luego deps) y descartar las siguientes con el mismo
    // nombre.  Solo afecta a los synthetic `__vx_*` (los simbolos de
    // usuario ya estan mangled con prefijo de modulo, no colisionan).
    {
        // Primera pasada: detectar si hay duplicados (sin mover nada).
        std::unordered_set<std::string> seen_vx_fns;
        bool has_dup = false;
        for (const auto &fn : merged.functions) {
            if (ir::rt::is_runtime_symbol(fn.name)) {
                if (!seen_vx_fns.insert(fn.name).second) {
                    has_dup = true;
                    break;
                }
            }
        }
        // Segunda pasada: reconstruir solo si hubo duplicados (preserva la
        // PRIMERA aparicion; root primero, luego deps).
        if (has_dup) {
            seen_vx_fns.clear();
            std::vector<ir::IrFunction> kept;
            kept.reserve(merged.functions.size());
            for (auto &fn : merged.functions) {
                if (ir::rt::is_runtime_symbol(fn.name) &&
                    !seen_vx_fns.insert(fn.name).second) {
                    continue; // ya presente: descartar duplicado
                }
                kept.push_back(std::move(fn));
            }
            merged.functions = std::move(kept);
        }
    }

    // M.staticdata-pool full: dedup de @c static_data por content_hash
    // + remap de STR_LIT_ADDR.imm a los indices unificados.  Indices
    // estables y deterministicos: dos builds del mismo source producen el
    // mismo mapping (JIT cache friendly).  Tambien colapsa bytes
    // duplicados cuando dos modulos importan el mismo string.
    if (merged.static_data.size() >= 2) {
        // Construir mapping old_idx -> new_idx via first-occurrence
        // por content_hash.  Mantener orden estable de aparicion.
        const size_t N = merged.static_data.size();
        std::vector<uint64_t> remap(N);
        std::unordered_map<uint64_t, uint64_t> first_by_hash;
        // Globals de programa (CPU dispatch fp-table): un slot por
        // shared_key en TODO el binario, aunque sean NON_DEDUP.  El init
        // del root inicializa el slot unificado que leen las funciones de
        // los modulos dependientes.
        std::unordered_map<std::string, uint64_t> first_by_shared_key;
        ir::IrModule::StaticDataStore new_store;
        new_store.alignment_default = merged.static_data.alignment_default;
        new_store.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            auto [bp, bn] = merged.static_data.bytes_at(i);
            auto &m = merged.static_data.meta_at(i);
            // Compute hash si no esta (defensa: push_back ya lo calcula,
            // pero entries movidas del dep podrian tenerlo en 0 si el
            // dep no lo hizo).
            if (m.content_hash == 0 && bn != 0) {
                uint64_t h = 0xcbf29ce484222325ull;
                for (size_t k = 0; k < bn; ++k) {
                    h ^= static_cast<uint64_t>(bp[k]);
                    h *= 0x100000001b3ull;
                }
                m.content_hash = h;
            }
            // Dos entries colapsan SOLO si tienen el mismo hash + bytes
            // identicos (defensa contra colision hash) + mismas flags +
            // mismo alignment.  Diferencias en cualquiera de esos
            // campos preservan ambas entradas.
            //
            // SD_FLAG_NON_DEDUP: globals mutables zero-init colapsarian
            // entre si por bytes identicos {0,0,...,0} -- el flag los
            // marca como "siempre distintos" para preservar storage real.
            const uint64_t hkey = m.content_hash;
            bool merged_in = false;
            // Prioridad maxima: shared_key (global de programa).  Unifica
            // todas las entries con la misma clave en un solo slot,
            // independientemente de NON_DEDUP / bytes.
            if (!m.shared_key.empty()) {
                auto it = first_by_shared_key.find(m.shared_key);
                if (it != first_by_shared_key.end()) {
                    remap[i] = it->second;
                    merged_in = true;
                } else {
                    const uint64_t new_idx = new_store.push_back(bp, bn);
                    new_store.meta_at(new_idx) = m;
                    first_by_shared_key.emplace(m.shared_key, new_idx);
                    remap[i] = new_idx;
                    merged_in = true;
                }
            }
            const bool is_non_dedup =
                (m.flags & ir::IrModule::SD_FLAG_NON_DEDUP) != 0;
            if (!merged_in && !is_non_dedup) {
                auto it = first_by_hash.find(hkey);
                if (it != first_by_hash.end()) {
                    const uint64_t prev_idx = it->second;
                    if (new_store.equals(prev_idx, bp, bn) &&
                        new_store.meta_at(prev_idx).flags == m.flags &&
                        new_store.meta_at(prev_idx).alignment == m.alignment) {
                        remap[i] = prev_idx;
                        merged_in = true;
                    }
                }
            }
            if (!merged_in) {
                const uint64_t new_idx = new_store.push_back(bp, bn);
                new_store.meta_at(new_idx) = m; // copiar flags/section
                first_by_hash.emplace(hkey, new_idx);
                remap[i] = new_idx;
            }
        }
        // Aplicar remap a TODAS las instrucciones STR_LIT_ADDR + a las
        // referencias `code.s_<N>` dentro de RAW_ASM.
        if (new_store.size() < N) {
            for (auto &fn : merged.functions) {
                for (auto &bb : fn.blocks) {
                    for (auto &ins : bb.instrs) {
                        if (ins.op == ir::IrOp::STR_LIT_ADDR) {
                            if (ins.imm < remap.size()) {
                                ins.imm = remap[ins.imm];
                            }
                            continue;
                        }
                        if (ins.op == ir::IrOp::RAW_ASM &&
                            !ins.func_name.empty()) {
                            std::string &s = ins.func_name;
                            std::string out;
                            out.reserve(s.size());
                            size_t i = 0;
                            while (i < s.size()) {
                                const size_t p = s.find("code.s_", i);
                                if (p == std::string::npos) {
                                    out.append(s, i, s.size() - i);
                                    break;
                                }
                                out.append(s, i, p - i);
                                out.append("code.s_");
                                size_t j = p + 7;
                                uint64_t num = 0;
                                while (j < s.size() && s[j] >= '0' &&
                                       s[j] <= '9') {
                                    num = num * 10 +
                                          static_cast<uint64_t>(s[j] - '0');
                                    ++j;
                                }
                                const uint64_t nidx =
                                    (num < remap.size()) ? remap[num] : num;
                                out.append(std::to_string(nidx));
                                i = j;
                            }
                            s = std::move(out);
                        }
                    }
                }
            }
            merged.static_data = std::move(new_store);
        }
    }

    // 5.b. Verificar los CONTRATOS de huella (@pure/@nothrow/@nopanic/@alloc/
    // @stack) sobre el IR PRE-opt (snapshot antes de optimizar): ahi TODAS las
    // funciones existen (el inline/DCE aun no las elimino) -> enforcement
    // completo.  Semantica source-level (source<=N => efectivo<=N, sound).
    // Solo ERROR cuando la violacion es DEMOSTRABLE.  Parte del sistema de
    // tipos.
    {
        /* Los contratos -- de funcion y de TIPO, con la huella de cada tipo --
         * ya estan juntos: cada modulo los apunto al compilarse y los reunio
         * `gather_unit_results`.
         *
         * Y TODAS las comprobaciones previas a optimizar, por la puerta unica.
         *
         * Se miran sobre `merged`, que es la fusion de los modulos: ahi la misma
         * nativa declarada en dos de ellos llega junta por primera vez.
         *
         * Aqui habia una copia del criterio de los contratos, y este camino se
         * habia quedado SIN las otras comprobaciones que el de fichero suelto si
         * hacia -- entre ellas el error del desbordamiento entero --.  Como este
         * es el camino que toma todo programa real, esas reglas no corrian para
         * nadie y nada fallaba.  Lo que se comprueba y en que orden lo dice
         * `vx/module_checks.h`. */
        PreOptInput pre;
        pre.module = &merged;
        pre.file = &root_path;
        pre.contracts = &res.contracts;
        pre.type_contracts = &res.type_contracts;
        pre.type_fingerprints = &res.type_fingerprints;
        pre.measure_only = opts.emit_ir_preopt;
        if (!run_pre_opt_checks(pre, res.diagnostics)) {
            res.ok = false;
            return res;
        }
    }

    // Modo --analyze: capturar el IR PRE-optimizacion para que el analizador
    // contraste la complejidad del FUENTE con la del codigo final.  Sin esto la
    // ruta de proyecto (con imports) reportaba "PRE-opt: no disponible".
    //
    // Se pliegan las ramas comptime-constantes (const fold + unreachable, SIN
    // inline): `is_float<T>()` es una CONSTANTE para cada instanciacion, asi
    // que la rama muerta del template (el bucle CAS que solo toca el caso
    // float) no es parte del cuerpo de `fetch_add<i64>` -- su algoritmo real es
    // O(1).  Es resolucion de la monomorfizacion, no optimizacion.  NO se
    // inlinea: el parcial es propiedad del cuerpo escrito.
    /* Copia para el modulo CON inline, tomada ANTES de ese plegado.  Importa
     * que sea antes: el plegado se hace para medir el cuerpo escrito y modifica
     * `merged`, asi que partir de ahi describiria un programa que el compilador
     * nunca ve.  Desde aqui, la entrada es exactamente la de una compilacion
     * normal, que es lo unico que hace util al informe. */
    ir::IrModule para_inline;
    const bool quiere_inline = opts.emit_ir_inlined && opts.emit_ir_preopt;
    if (quiere_inline) para_inline = merged;

    if (opts.emit_ir_preopt) {
        for (auto &fn : merged.functions) {
            bool changed = true;
            while (changed) {
                changed = false;
                if (ir::applied(ir::ir_pass_const_fold(fn))) changed = true;
                if (ir::applied(ir::ir_pass_unreachable(fn))) changed = true;
            }
        }
    }

    /* La INSTANTANEA de antes de optimizar, y quien la pide.
     *
     * Iba dentro del bloque de arriba, o sea atada a un flag de VOLCADO: quien
     * queria los HECHOS del momento anterior no los tenia si no pedia ademas
     * el dump del intermedio.  Son dos cosas distintas -- una mira, la otra
     * imprime -- y atarlas dejaba al linter sin poder preguntar por lo que el
     * usuario ESCRIBIo.
     *
     * Lo que NO se mueve es la mutacion de `merged` de ahi arriba: de ese
     * modulo sale el codigo que se emite, y tocarlo por haber pedido analisis
     * haria que el compilador generase codigo distinto SEGUN SI LE PREGUNTAS.
     */
    if (opts.emit_ir_preopt ||
        wants_stage_(opts, analysis::asa::kStagePreOpt)) {
        /* Los locales a REGISTRO, sobre una COPIA.
         *
         * Es construccion de SSA, no optimizacion: no quita un bucle ni cambia
         * cuantas vueltas da, pone el programa en la forma en la que los
         * analisis hablan.  Sin ella "antes de optimizar" no es analizable --
         * el contador de un `for` vive en un `alloca` y se lee con un `load`
         * en cada vuelta, asi que no hay PHI, y sin PHI no hay variable de
         * induccion que encontrar --.
         *
         * Sobre una COPIA y no sobre `merged` porque de ahi sale el codigo que
         * se emite, y el orden en que corren los pases influye en lo que
         * deciden los siguientes: mutarlo aqui haria que el compilador
         * generase codigo distinto SEGUN SI LE PIDES ANaLISIS. */
        ir::IrModule pre_snapshot = merged;
        for (auto &fn : pre_snapshot.functions)
            /* Por `applied`, que es la unica puerta: aqui se estaba MUTANDO el
             * IR y tirando el resultado, con lo que la version no se movia y un
             * analisis cacheado de esta copia podia servirse despues -- y lo
             * que guarda son punteros a instrucciones.  Nadie lo habria visto:
             * lo saca la firma, no una revision. */
            (void)ir::applied(ir::ir_pass_sroa_stack_structs(fn));
        ir::emit_ir_module_cache(pre_snapshot,
                                 res.ir_module_cache_bytes_preopt.buf);

        /* Y los HECHOS de ese momento, si quien compila los pidio.  Por la
         * MISMA puerta que los de despues: es el mismo conocimiento sobre otro
         * codigo, no otro mecanismo.  Este camino solo miraba DESPUES, con lo
         * que la decision de que el ASA mire antes solo llegaba al camino de
         * fichero suelto -- y por aqui pasa todo lo que declara `namespace`,
         * que es la mayoria. */
        if (wants_stage_(opts, analysis::asa::kStagePreOpt) &&
            opts.asa.anything()) {
            /* Vacio hacia la puerta = TODOS, que es lo que pide quien vuelca.
             */
            const std::vector<const char *> asa_wanted = opts.asa.domain_list();
            /* Clave y fichero POR MOMENTO, como en el camino de fichero
             * suelto: los dos tienen que construirlas igual o la misma
             * pregunta acaba con dos respuestas segun como compiles. */
            const auto s = ensure_facts_impl_(
                pre_snapshot, facts, asa_wanted,
                root_facts_key != 0
                    ? asa_facts_path_for_stage(
                          vxfacts_path_for(root_path, std::string()),
                          analysis::asa::kStagePreOpt)
                    : std::string(),
                asa_facts_key(asa_module_id(root_path), opts,
                              analysis::asa::kStagePreOpt),
                analysis::asa::kStagePreOpt, root_path);
            res.asa_summaries.insert(res.asa_summaries.end(), s.begin(),
                                     s.end());
        }
    }

    // --vx-emit-ir: copia del IR PRE-opt (antes de optimizar) para el dump.
    // La ruta de proyecto (con imports) NO rellenaba res.ir_text -- solo lo
    // hacia compile_vx_source --, asi que `vm --vesta prog.vx --vx-emit-ir`
    // generaba un .ir VACIO en cuanto el fuente tenia un import.
    ir::IrModule ir_pre_dump;
    if (opts.dump_ir) ir_pre_dump = merged;

    cerrar_fase(res.times.modules_us, "vx.phase.optimize");

    // 5. Optimizar el IR mergeado.  En modo --analyze SIN inline: el coste
    //    PARCIAL es propiedad del cuerpo escrito -- si el inline lo alterase,
    //    dependeria del optimizador (`return this.swap(v)` es parcial O(1), no
    //    O(n) por el bucle de swap inyectado).  El coste TOTAL lo compone el
    //    analizador via el callgraph.  Fuera de --analyze, inline normal.
    // Multi-ISA: la rentabilidad de la if-conversion (SELECT vs branch) depende
    // de la microarquitectura destino (cmov ~2c x86, csel ~1c ARM64, sin cmov
    // nativo en RISC-V).  Ajustamos el modelo de coste al arch del TARGET
    // activo antes de optimizar; asi la decision horneada en el IR corresponde
    // a la ISA para la que se compila (host x86_64 por defecto).
    {
        std::string tisa_os, tisa_arch;
        vx::get_aot_condcomp_target(tisa_os, tisa_arch);
        if (tisa_arch == "arm64" || tisa_arch == "aarch64")
            ir::set_target_isa(ir::TargetIsa::ARM64);
        else if (tisa_arch == "riscv" || tisa_arch == "riscv64")
            ir::set_target_isa(ir::TargetIsa::RISCV);
        else
            ir::set_target_isa(ir::TargetIsa::X86_64);
    }

    /* ASA observa ANTES de optimizar: esta es la forma del programa tal como se
     * escribio.  Es una verdad distinta de la de despues, no una version peor
     * -- medido: de tres sacos escritos a mano, la escalarizacion se lleva dos
     * antes de que nadie los mire, asi que observar solo despues hace creer que
     * el programa no los tenia. */
    {
        // Medir es decision de QUIEN mide: el cronometro es una utilidad y no
        // sabe
        // bajo que bandera vive cada uno de sus usuarios.
        util::CronoTramo t_("phase:asa_dump_shapes",
                            util::flag_on(util::FlagId::Times));
        analysis::asa::volcar_formas(merged, "pre-opt");
    }

    /* El codigo que de verdad se construye, para quien ademas del coste del
     * cuerpo escrito necesita ver eso.  Sale de la misma bajada, asi que no hay
     * que compilar el fuente otra vez: basta optimizar la copia de arriba con
     * el inline puesto.  Aqui y no antes, porque la ISA del objetivo ya esta
     * fijada y el optimizador decide con ella. */
    if (quiere_inline) {
        /* Una SEGUNDA optimizacion completa, sobre la copia con inline.  Cae
         * en la misma ventana que la de verdad, asi que sin tramo propio se
         * suma al coste del optimizador sin que nadie sepa que son dos. */
        util::CronoTramo t_("phase:ir_optimize (inlined copy)",
                            util::flag_on(util::FlagId::Times));
        ir::ir_optimize(para_inline, ir::opt_level_from_int(opts.opt_level),
                        /*allow_inline=*/true);
        ir::emit_ir_module_cache(para_inline,
                                 res.ir_module_cache_bytes_inlined.buf);
    }

    /* El asignador del lenguaje queda DENTRO del modulo (sin tocar las
     * reservas): asi el selector del JIT puede llamarlo y compartir mecanismo
     * con el binario nativo, que es lo que permite depurar aquel desde aqui. */
    {
        util::CronoTramo t_("phase:language_allocator",
                            util::flag_on(util::FlagId::Times));
        traer_asignador_del_lenguaje(merged, opts, root_path);
    }

    /* La exclusividad de los prestamos, ANTES de optimizar y con su propia base
     * de ese momento.  Aqui corria DESPUES, compartiendo base con las cotas, y
     * eran dos fallos en uno: el inline se lleva por delante los sitios de
     * llamada que la demuestran -- asi que no encontraba nada, mientras que en
     * el camino de fichero suelto si --, y una misma base servia a dos
     * momentos, con lo que un analisis de antes de optimizar se reutilizaba
     * despues.
     *
     * La misma comprobacion no puede dar dos respuestas segun se compile un
     * fichero o un proyecto. */
    /* Se comprueba SIEMPRE.  Lo que decide la opcion es el peso del veredicto,
     * no si se mira: saltarse la comprobacion entera dejaba a `--analyze` sin
     * nada que ensenar, que es lo contrario de para lo que existe. */
    borrow::check_borrows_before_opt(merged, root_path,
                                    opts.violations_are_errors,
                                    res.diagnostics);

    /* El modulo FUSIONADO, antes de que nadie lo toque.  Este momento no se
     * verificaba: solo se miraba cada dependencia recien bajada, asi que lo que
     * la fusion pudiera dejar mal no lo veia nadie -- y fusionar es justo donde
     * se renumeran valores y se juntan bloques de procedencias distintas. */
    ir::ir_verify_if_asked(merged, "merged", root_path);

    {
        util::CronoTramo t_("phase:ir_optimize",
                            util::flag_on(util::FlagId::Times));
        /* Con el almacen si se pidio el momento de EN MEDIO.  Ver la nota en
         * el camino de fichero suelto. */
        ir::ir_optimize(merged, ir::opt_level_from_int(opts.opt_level),
                        /*allow_inline=*/!opts.emit_ir_preopt,
                        wants_stage_(opts, analysis::asa::kStageDuringOpt)
                            ? &facts
                            : nullptr);
    }

    /* Sobre el codigo que DE VERDAD se va a emitir: lo que el analisis puede
     * demostrar fuera de su region no puede quedarse en `--analyze`, tiene que
     * salir al compilar, que es cuando se lee. */
    /* La base de hechos de ESTA compilacion, una sola: ver la nota del camino
     * de fichero suelto.  Los dos entran por el mismo sitio a proposito. */
    /* EN SU BLOQUE, como la de antes de optimizar.  Sin las llaves la base
     * vivia hasta el final de la funcion -- o sea durante emitir y enlazar --
     * aunque su unico consumidor sea la linea de abajo.
     *
     * Y no es poca cosa: dentro lleva el gestor de analisis, que guarda lo
     * calculado de las 73.501 funciones.  Medido con el eje del tiempo del
     * comprobador, en el instante de mayor memoria sostiene 81 MiB de tablas
     * points-to mas 64 de la contabilidad que las indexa.  El propio
     * destructor del gestor ya lo decia: "un analisis solo hace falta mientras
     * se trabaja su funcion, y aqui sobreviven todos hasta el final".
     *
     * No se pierde conocimiento: los dominios que se cachean en disco vuelven
     * de ahi, y los que no, se derivan otra vez si alguien pregunta.  Lo que
     * se suelta es la COPIA en memoria. */
    {
        analysis::asa::FactBase fact_base(analysis::asa::kStagePostOpt);
        vx_report_bounds(merged, res.diagnostics, root_path, fact_base,
                         opts.violations_are_errors ? DiagLevel::ERR
                                                    : DiagLevel::WARN);
        /* Aprovechando la MISMA base: lo que este necesita -- def-use,
         * points-to, escape -- ya esta ahi o se pedira por la misma puerta.
         * Construir una propia rehace el escape del modulo entero. */
        vx_report_fn_addr_crossing(merged, fact_base);
    }
    /* Y de que memoria es el destino de cada llamada indirecta.  Aqui, sobre
     * `merged` ya optimizado, por lo mismo que el informe de limites: es el
     * codigo que de verdad se va a emitir.
     *
     * En ESTE camino y no en el de fichero suelto: todo programa pasa por
     * aqui -- basta que el manifiesto declare algo auto-importable, y lo
     * declara siempre --, asi que una comprobacion que solo viva en el otro no
     * corre nunca.  Ya hay una asi: la del desbordamiento entero. */
    vx_report_callind_memory(merged, res.diagnostics, root_path,
                             opts.violations_are_errors ? DiagLevel::ERR
                                                        : DiagLevel::WARN);
    /* La exclusividad de los prestamos NO se comprueba aqui: se hizo ANTES de
     * optimizar, que es donde todavia existen las llamadas que la demuestran.
     * Ver el comentario de alli. */
    /* Precondiciones del asm.  SIEMPRE, no bajo opcion: una instruccion cuya
     * exigencia no se cumple no da un resultado peor, hace caer el programa --
     * y callarselo ya costo descubrirlo ejecutando.
     *
     * Aqui esta TODO lo que se va a enlazar, asi que si hay un `main` lo que se
     * construye es el programa entero y no queda ningun fuera desde el que
     * llamar: entonces se puede afirmar de una funcion publica lo mismo que de
     * una privada.  Sin `main` esto es una libreria (o un modulo suelto) y lo
     * publico sigue siendo alcanzable por quien no se ve. */
    bool hay_main = false;
    for (const ir::IrFunction &f : merged.functions)
        if (f.name == "main") {
            hay_main = true;
            break;
        }
    {
        util::CronoTramo t_("phase:asm_preconditions",
                            util::flag_on(util::FlagId::Times));
        /* Antes de preguntar, que este lo que se va a preguntar -- venga de la
         * compilacion anterior o de producirlo ahora.  El consumidor no
         * distingue una cosa de la otra a proposito: si tuviera que decidir
         * cuando fiarse de la cache, esa decision acabaria escrita en tantos
         * sitios como consumidores haya, y bastaria que uno se separara para
         * que el mismo programa dijera cosas distintas segun quien preguntara.
         *
         * Sin identidad del modulo no se toca el disco: comprobar que lo
         * guardado es de ESTE programa es lo unico que hace sana una cache. */
        /* `asa.layout` porque lo consume el informe de precondiciones de aqui
         * abajo, y ademas lo que haya pedido quien compila: el linter y el
         * editor saben que dominios van a consultar, y producirlos AQUI -- con
         * el modulo en memoria y su identidad conocida -- deja el trabajo hecho
         * y guardado, en vez de que cada consumidor lo rehaga despues y lo
         * tire al terminar. */
        /* `asa.layout` SIEMPRE -- lo consume el informe de precondiciones de
         * aqui abajo --, y ademas los dominios que haya pedido quien compila,
         * pero solo si pidio ESTE momento: pedir hechos sin decir de que
         * codigo dejaba al consumidor preguntando por un momento y al
         * productor contestando por otro, con lo que no veia nada y ademas lo
         * recalculaba. */
        std::vector<const char *> wanted = {"asa.layout"};
        const bool all_domains =
            opts.asa.all_domains &&
            wants_stage_(opts, analysis::asa::kStagePostOpt);
        if (all_domains)
            wanted.clear(); // vacio hacia la puerta = TODOS
        else if (wants_stage_(opts, analysis::asa::kStagePostOpt))
            for (const char *d : opts.asa.domains)
                if (d != nullptr) wanted.push_back(d);
        /* Producir los hechos y sellarlos en disco corre DENTRO de la ventana
         * que el informe llama "optimizar", asi que sin medirlo se le atribuia
         * al optimizador un trabajo que no es suyo. */
        util::CronoTramo t_hechos_("phase:facts_store",
                                   util::flag_on(util::FlagId::Times));
        const auto s = ensure_facts_impl_(
            merged, facts, wanted,
            root_facts_key != 0
                ? asa_facts_path_for_stage(
                      vxfacts_path_for(root_path, std::string()),
                      analysis::asa::kStagePostOpt)
                : std::string(),
            asa_facts_key(asa_module_id(root_path), opts,
                          analysis::asa::kStagePostOpt),
            analysis::asa::kStagePostOpt, root_path);
        res.asa_summaries.insert(res.asa_summaries.end(), s.begin(), s.end());
        /* Y sale con el resultado, para que quien compilo pueda consultarlo sin
         * volver a producirlo.  Se MUEVE, y el informe de abajo lee ya de ahi.
         *
         * Copiarlo costaba dos veces: la memoria de un almacen entero -- que en
         * un modulo grande son cientos de miles de hechos -- y, como cada hecho
         * guarda punteros al arena de cadenas de SU almacen, una copia correcta
         * obliga ademas a reinternar todas.  Moviendo no hay ninguna de las
         * dos. */
        res.facts = std::move(facts);
        vx_report_asm_preconditions(merged, res.diagnostics, root_path,
                                    hay_main, opts.emit_ir_preopt,
                                    opts.native_poo ? "aot" : "vm", res.facts);
    }

    if (opts.dump_ir) {
        std::ostringstream ir_oss;
        ir_oss << "// ============================================\n";
        ir_oss << "// SSA IR pre-optimizacion (frontend output, mergeado)\n";
        ir_oss << "// ============================================\n";
        ir::ir_print(ir_pre_dump, ir_oss);
        ir_oss << "\n// ============================================\n";
        ir_oss << "// SSA IR post-optimizacion (opt_level=" << opts.opt_level
               << ")\n";
        ir_oss << "// ============================================\n";
        ir::ir_print(merged, ir_oss);
        res.ir_text = ir_oss.str();
    }

    /* TEMPORAL: cuanto tiraria un barrido de alcanzabilidad, SIN tirar nada.
     *
     * Nada en el compilador borra hoy una funcion que ya no puede alcanzar
     * nadie -- ni un pase, ni el emisor, ni el enlazador --, y eso no es solo
     * codigo que el fuente no usaba: inlinear una funcion en TODAS sus llamadas
     * la deja sin llamadores, y su cuerpo se queda, se vuelve a optimizar en la
     * ronda siguiente y se emite.
     *
     * Antes de escribir el barrido hay que saber cuanto vale, porque borrar
     * codigo es de las cosas que no se pueden equivocar a medias.  Esto solo
     * CUENTA.  Con `VX_DEAD_FN_COUNT=1`. */
    if (util::flag_on(util::FlagId::DeadFnCount))
        count_unreachable_functions(merged);

    /* LA MEMOIZACION DE RANGOS SE SUELTA AQUI, y el sitio es el punto.
     *
     * Quien la usa es el optimizador, y acaba de terminar; emitir no pregunta
     * rangos.  Soltarla mas tarde -- probado en la frontera del ensamblado --
     * no baja el pico: para entonces las paginas que se querian ahorrar ya
     * estan tocadas, y devolver memoria despues de eso no las devuelve.  Un
     * pico es paginas TOCADAS, asi que lo que cuenta es soltar ANTES de que
     * alguien vaya a pedir, no antes de acabar. */
    (void)analysis::release_range_memo();

    /* Y despues de optimizar, que es el momento mas delicado y el que a este
     * camino le faltaba: el optimizador opera SOBRE el grafo -- corta bloques,
     * los une, mueve instrucciones -- y una cirugia mal cerrada no se nota hasta
     * que el programa hace otra cosa.  El camino de fichero suelto si lo
     * verificaba; el que toma todo programa real, no. */
    ir::ir_verify_if_asked(merged, "post-opt", root_path);

    cerrar_fase(res.times.optimize_us, "vx.phase.emit");

    // 6. Emitir .vel desde el IR mergeado.
    ir::EmitOptions emit_opts;
    emit_opts.opt_level = ir::opt_level_from_int(opts.opt_level);
    emit_opts.emit_comments = true;
    emit_opts.emit_debug = opts.emit_debug;
    // emit_opts.emit_stackmaps queda en su default (true): VSMP siempre.
    emit_opts.module_name =
        opts.module_name.empty() ? work.back().module_name.str() : opts.module_name;
    /* `merged` sale ya optimizado de la fase anterior.  El emisor optimizaba su
     * copia otra vez -- el mismo trabajo, el mismo modulo -- y eso era la mayor
     * parte de lo que costaba emitir.
     *
     * Salvo con `emit_ir_preopt`, donde la optimizacion de arriba se hace SIN
     * inline a proposito (el coste que se mide es el del cuerpo escrito) y el
     * codigo que se emite si tiene que llevarlo. */
    emit_opts.ya_optimizado = !opts.emit_ir_preopt;
    /* Quien solo quiere el IR se salta esto.  Asignar registros y escribir el
     * texto `.vel` es el noventa por ciento del coste del frontend, y el
     * informe de `--analyze` no lee ni una linea de ese texto.  Lo que le falta
     * al modulo es en que registro quedo cada valor, que la pone el emisor por
     * ser quien lo decide; el resto sale igual.
     *
     * Se salta la EMISION, no lo que viene detras: ahi se decide si el modulo
     * necesita la maquina de compilacion, y volver antes la habria apagado
     * justo para quien mas la necesita. */
    ir::EmitResult eres;
    if (!opts.ir_only) {
        eres = ir::ir_emit_module(merged, emit_opts);

        /* PRUEBA: el artefacto comptime, FILTRANDO EL IR en vez de recortar el
         * fuente.  Aqui `merged` ya esta parseado, chequeado, bajado, fusionado
         * y optimizado, con los nombres resueltos: quedarse con unas funciones
         * es copiar el modulo y filtrar un vector.  El enfoque por texto tenia
         * que RECONSTRUIR eso, y el AST ni siquiera guarda el tramo de cada
         * decl (`SourceLoc::length` es la del token). */
        /* Valvula: `VESTA_NO_FILTRO_COMPTIME=1` vuelve al artefacto del
         * programa entero.  Sirve para separar en un fallo si la causa es el
         * filtro o cualquier otra cosa, sin recompilar el compilador. */
        if (!res.comptime_unit_names.empty() &&
            !util::flag_on(util::FlagId::NoFiltroComptime)) {
            /* Por IDENTIDAD: los nombres vienen internados, y el de cada
             * funcion se pregunta con `name_key()`, ya internado tambien. */
            const util::NamedSet<util::InternedName, scratch::ComptimeUnitNames,
                                 util::InternedNameHash>
                del_conjunto(res.comptime_unit_names.begin(),
                             res.comptime_unit_names.end());
            /* Indice por nombre para no recorrer el modulo por cada callee. */
            std::unordered_map<std::string, const ir::IrFunction *> por_nombre;
            por_nombre.reserve(merged.functions.size());
            for (const ir::IrFunction &f : merged.functions)
                por_nombre.emplace(f.name, &f);

            /* RAICES: lo que la ComptimeVM tiene que poder invocar.
             *
             * No basta con lo que el recolector nombra.  Hay dos familias
             * SINTETICAS que nadie declara y que se ejecutan al compilar igual:
             * los `__macro_<X>` (incluidos los constructores `comptime
             * T(expr)`, que bajan a `__macro_<T>__ctor_N`) y los `__ctblock_N`
             * de un bloque `comptime { }`.  Dejarlas fuera rompio
             * `comptime_literal_import` y `bfc311`. */
            std::vector<std::string> pendientes;
            std::unordered_set<std::string> dentro;
            for (const ir::IrFunction &f : merged.functions) {
                /* `__module_init` es raiz aunque nadie lo nombre: es lo que
                 * registra clases y macros al cargar el artefacto.  Sin el, la
                 * carga deja simbolos sin resolver y el comptime no se entera
                 * -- el programa compilaba y daba otro resultado. */
                const bool sintetica = ir::is_macro_symbol(f.name) ||
                                       ir::is_ctblock_symbol(f.name) ||
                                       ir::is_module_init(f.name);
                /* Una macro ya entra por `sintetica`: buscar tambien su nombre
                 * sin prefijo no cambiaba nada, y se quito. */
                if (sintetica ||
                    del_conjunto.count(
                        util::InternedName::from_interned(f.name_key())) != 0) {
                    if (dentro.insert(f.name).second)
                        pendientes.push_back(f.name);
                }
            }
            /* CLAUSURA sobre el grafo de llamadas del IR: lo que una raiz llama
             * tiene que viajar con ella o el artefacto no es auto-suficiente y
             * la invocacion falla EN COMPILACION.  Hacerlo aqui -- y no sobre
             * el fuente -- es lo que lo vuelve cerrado POR CONSTRUCCION: los
             * nombres ya estan resueltos y no hay que adivinar nada. */
            while (!pendientes.empty()) {
                const std::string actual = std::move(pendientes.back());
                pendientes.pop_back();
                const auto it = por_nombre.find(actual);
                if (it == por_nombre.end()) continue;
                for (const ir::IrBlock &b : it->second->blocks)
                    for (const ir::IrInstr &in : b.instrs) {
                        /* Llamarla no es la unica forma de necesitarla:
                         * TOMAR SU DIRECCION tambien.  `LABEL_ADDR` nombra una
                         * funcion que se usa como VALOR -- un puntero a
                         * funcion -- y esa funcion tiene que viajar igual.
                         *
                         * Sin esto se rompia de verdad, y con un fallo dificil:
                         * `std.syscall.windows` tiene
                         *
                         *     invoke_syscall invoke_method = invoke;
                         *     resolve_syscall resolve_method =
                         * resolve_ntdll_export;
                         *
                         * -- valores por defecto de campo --, asi que `invoke`
                         * y `resolve_ntdll_export` se referencian por nombre y
                         * NO se llaman.  El cierre no las veia, no viajaban al
                         * artefacto, y al ejecutarlo la llamada saltaba a
                         * `0x31`.  El destino se sabe: es un nombre. */
                        if (in.op != ir::IrOp::CALL &&
                            in.op != ir::IrOp::TAILCALL &&
                            in.op != ir::IrOp::LABEL_ADDR)
                            continue;
                        if (in.func_name.empty()) continue; // indirecta
                        if (dentro.insert(in.func_name).second)
                            pendientes.push_back(in.func_name);
                    }
            }

            /* `main` NO viaja.  Se probo incluirlo -- el artefacto se carga
             * como ejecutable y parecia necesitar punto de entrada -- pero sus
             * llamadas quedaban COLGANDO y el enlazado fallaba con "simbolo no
             * resuelto: code.copy_ok".  Cerrar sobre `main` traeria el programa
             * entero, que es justo lo que se evita.  Lo que de verdad faltaba
             * era `__module_init`, que si es raiz. */
            ir::IrModule solo_ct = merged; // cabecera: imports/globals/libs
            solo_ct.functions.clear();
            for (const ir::IrFunction &f : merged.functions)
                if (dentro.count(f.name)) solo_ct.functions.push_back(f);
            if (!solo_ct.functions.empty()) {
                ir::EmitOptions eo_ct = emit_opts;
                /* Ninguna de estas funciones es la entrada de un programa:
                 * todas se invocan por su PC y cualquiera puede llamar a otra,
                 * asi que todas tienen que poder VOLVER. */
                eo_ct.sin_punto_de_entrada = true;
                ir::EmitResult e_ct = ir::ir_emit_module(solo_ct, eo_ct);
                if (e_ct.ok) {
                    res.comptime_vel_text = std::move(e_ct.vel_text);
                    /* PUNTO DE ENTRADA: `__module_init`, no `main`.
                     *
                     * El artefacto comptime NO es un programa: es una
                     * biblioteca de funciones que la ComptimeVM invoca por su
                     * PC, resuelto desde la tabla de simbolos.  Nunca se entra
                     * por `main`. Pero se carga con `load_executable`, y un
                     * ejecutable tiene entrada: sin ninguna, el enlazador deja
                     * `start_pc = 0` -- avisa con "PC(0) por defecto" -- y
                     * usarlo salta a la direccion 0 (segfault).
                     *
                     * Incluir `main` para taparlo no vale: arrastra sus
                     * llamadas y el enlazado falla con simbolos sin resolver
                     * (`code.copy_ok`), y cerrar sobre el traeria el programa
                     * entero, que es lo que se esta evitando.
                     *
                     * La entrada correcta es `__module_init`, que es justo lo
                     * que debe ejecutarse al cargar el artefacto: registra las
                     * clases y los macros. */
                    bool tiene_init = false;
                    for (const ir::IrFunction &f : solo_ct.functions)
                        if (ir::is_module_init(f.name)) {
                            tiene_init = true;
                            break;
                        }
                    if (tiene_init)
                        /* ENTRE COMILLAS y AL FINAL.  Las dos cosas, y las dos
                         * costaron un fallo:
                         *
                         *  - sin comillas, el ensamblador se para en el `.` con
                         *    "se esperaba un )".  Es la convencion del `.vel`
                         *    para una etiqueta con punto
                         * (`@Absolute("code.X")`).
                         *  - al PRINCIPIO, las anotaciones se procesan en orden
                         *    y todavia no hay ninguna seccion definida, asi que
                         *    la etiqueta no se puede resolver ("no se ha
                         *    definido ninguna seccion ... code.__module_init").
                         *
                         * Nadie los habia visto porque el fallo se tragaba: el
                         * sitio que ensambla el artefacto tenia un `if (ok)`
                         * sin `else`, asi que la compilacion seguia sin
                         * artefacto y sin decir nada. */
                        res.comptime_vel_text += std::string("\n@InitPc(\"") +
                                                 ir::kModuleInit + "\")\n";
                    /* La seccion @ir del conjunto: la del programa entero no
                     * vale, describe otras funciones. */
                    util::byte_buffer_release(
                        res.comptime_ir_section_bytes.buf);
                }
                if (util::flag_on(util::FlagId::PruebaIrComptime)) {
                    /* QUE se queda fuera: es lo que hay que mirar cuando el
                     * artefacto filtrado se comporta distinto del entero. */
                    std::cerr << "[comptime-ir] fuera:";
                    for (const ir::IrFunction &f : merged.functions)
                        if (!dentro.count(f.name)) std::cerr << " " << f.name;
                    std::cerr << "\n";
                }
                if (util::flag_on(util::FlagId::PruebaIrComptime))
                    std::cerr
                        << "[comptime-ir] programa " << merged.functions.size()
                        << " fns / " << eres.vel_text.size() << " B .vel"
                        << "  ->  conjunto " << solo_ct.functions.size()
                        << " fns / " << res.comptime_vel_text.size()
                        << " B .vel" << (e_ct.ok ? "" : "  (FALLO)") << "\n";
            }
        }
        if (!eres.ok) {
            SourceLoc loc;
            loc.set_file(root_path);
            res.diagnostics.diag(std::move(loc), DiagLevel::ERR, "VX4017",
                                 {eres.error});
            res.ok = false;
            return res;
        }
    }
    res.vel_text = std::move(eres.vel_text);
    /* Y de donde salio, para quien pueda ensamblar sin releerlo. */
    res.vel_sink = eres.sink;

    /* Mapa del artefacto en el grafo de depuracion: uno solo con los simbolos
     * y los mapas de TODOS los modulos.  La misma pieza que el camino de
     * fichero suelto. */
    compose_vxdbg_artifact(work, opts, res);
    /* Donde dejo el asignador cada valor, antes de guardar el intermedio: es
     * lo que permite decir que `%8` es el `r1` de la instruccion maquina.  Se
     * estampa aqui, entre emitir y serializar, porque el emisor recibe el
     * modulo como solo-lectura y este es el punto en que ya se sabe. */
    for (auto &fn : merged.functions) {
        auto it = eres.value_regs.find(fn.name);
        if (it == eres.value_regs.end()) continue;
        const size_t n = std::min(fn.values.size(), it->second.size());
        for (size_t v = 0; v < n; ++v)
            fn.values[v].reg = it->second[v];
    }
    // El intermedio que viaja dentro del artefacto: sin artefacto, sobra.
    if (!opts.ir_only)
        ir::emit_ir_section(merged.functions, merged.source_files,
                            res.ir_section_bytes.buf);
    //  AOT multi-modulo: exponer el IR mergeado (functions + static_data
    // + globals) como module_cache para que el path -m aot lo consuma.  El
    // single-file lo rellena en compile_vx_source; aqui lo rellenamos desde
    // el modulo mergeado de todos los .vx del proyecto.
    ir::emit_ir_module_cache(merged, res.ir_module_cache_bytes.buf);

    /* Lo que queda del frontend, que NO es enlazar: se llamaba asi porque era
     * la ultima marca y se comia todo lo que viniera detras -- la cola de esta
     * funcion Y el ensamblado entero --.  Ahora el ensamblado abre la suya en
     * cuanto el frontend devuelve, y esta se queda con lo que de verdad cubre.
     */
    cerrar_fase(res.times.emit_us, "vx.phase.finish");

    // AOT.2.d: detectar @AllocatorOverride / @PanicHandler en el modulo ROOT,
    // igual que hace compile_vx_source.  Sin esto, un .vx que declara el
    // allocator NO podia tener imports: el driver -m aot lo compilaba como
    // fichero suelto porque por el camino de proyecto no le llegaban estos
    // simbolos, y abortaba con "no pude compilar el slab allocator (o no expone
    // @AllocatorOverride)".  O sea: la stdlib era la unica parte del lenguaje
    // que no podia importar.
    if (!work.empty() && work.back().ast) {
        for (auto &decl : work.back().ast->decls) {
            if (!decl || decl->kind != ast::NodeKind::FunctionDecl) continue;
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            /* `@Provides`: el rol sale del NOMBRE del builtin, y quien cubre
             * `malloc` se lleva tambien lo que el compilador emite por su
             * cuenta -- el `new`, la liberacion de RAII --.  Ver la nota en
             * `compiler.cpp`, que hace lo mismo para el fichero suelto. */
            switch (fd->provides_builtin) {
            case Builtin::Malloc: res.aot_alloc_sym = fd->name; break;
            case Builtin::Free: res.aot_free_sym = fd->name; break;
            case Builtin::Panic: res.aot_panic_sym = fd->name; break;
            default: break;
            }
        }
    }
    /* Y si quien provee es una PLANTILLA, manda su INSTANCIA.
     *
     * El bucle de arriba apunta el nombre de la declaracion, que para una
     * plantilla es el que NO existe como simbolo -- lo emiten sus instancias
     * --, asi que el enlazado acababa pidiendo `...__vx_free` a secas.  El
     * comprobador ya creo la instancia que trabaja en BYTES, porque reservar
     * sin escribir `malloc` es lo normal: un `new` lo hace. */
    if (!root_alloc_sym.empty()) res.aot_alloc_sym = root_alloc_sym;
    if (!root_free_sym.empty()) res.aot_free_sym = root_free_sym;
    /* El del PROGRAMA manda sobre el de la biblioteca, tambien para la maquina:
     * si alguien declara el suyo, se usa entero -- usarlo a medias, con el JIT
     * llamando a otro, seria peor que no usarlo. */
    if (!res.aot_alloc_sym.empty()) merged.alloc_sym = res.aot_alloc_sym;
    if (!res.aot_free_sym.empty()) merged.free_sym = res.aot_free_sym;
    /* Y un punto de entrada de nombre CONOCIDO que lleve hasta el.
     *
     * El backend tiene que saber que llamar sin suponer quien hay detras: si
     * buscara un nombre concreto quedaria atado al asignador de la biblioteca,
     * y un programa con el suyo acabaria usandolo a medias.  Asi busca siempre
     * lo mismo y quien conteste lo decide aqui.
     *
     * Es un puente de una linea, no una copia: llama al de verdad y devuelve lo
     * que devuelva. */
    /* has_lowerable_macros: gate del two- compile.  El single-file
     * (compile_vx_source) lo setea escaneando su irmod; en el path multi-modulo
     * hay que escanear el MODULO MERGEADO -- si cualquier funcion (root o dep)
     * es `is_macro_compiled` (un @Macro o una comptime fn ruteada a la
     * ComptimeVM, `__macro_<X>`), el proyecto necesita el two- para que
     * esos call sites se evaluen en la VM (si no, quedan deferidos -> codigo
     * vacio).  Sin esto, un proyecto CON imports que use comptime fns nunca
     * disparaba el segundo pase. */
    for (const auto &fn : merged.functions) {
        // Un constructor comptime tambien es codigo que hay que compilar y
        // cargar antes de poder invocarlo.  Se reconoce por el nombre porque
        // es lo que sobrevive al merge; sin esto la fase no se disparaba y el
        // constructor acababa resolviendose por el evaluador de AST -- que
        // solo funciona dentro del mismo fichero.
        if (fn.is_macro_compiled || ir::is_macro_symbol(fn.name)) {
            res.has_lowerable_macros = true;
            break;
        }
    }
    /* Y si a CUALQUIER modulo del camino le quedo un `inject(...)` pendiente,
     * su bloque asm salio vacio: hay que repetir.  Mirar solo el IR no basta,
     * porque una funcion comptime invocada UNICAMENTE desde un inject no deja
     * rastro en el IR cuando el inject no llega a expandirse. */
    /* El conjunto ENTERO, ya concatenado, a un fichero.  Es lo que de verdad
     * habria que compilar para tener la maquina de compilacion antes que los
     * modulos, asi que es lo que hay que poder leer cuando no compila: los
     * volcados por modulo no lo enseñan, porque el `namespace` de cada uno se
     * añade AL JUNTARLOS. */
    if (!util::flag_text(util::FlagId::VolcarUnidad).empty() &&
        !res.comptime_unit_source.empty()) {
        const std::string &d = util::flag_text(util::FlagId::VolcarUnidad);
        std::error_code tec;
        std::filesystem::create_directories(d, tec);
        /* Con el numero de modulos en el nombre.  Una misma orden compila
         * VARIAS veces -- se han visto tres, de 2, 11 y 2 modulos -- y con un
         * nombre fijo la ultima pisaba a las demas: el fichero parecia decir
         * que el conjunto traia un modulo de siete cuando lo que pasaba es que
         * se estaba mirando otra compilacion. */
        std::ofstream f(d + "/_conjunto_" + std::to_string(work.size()) +
                        "mods.vx");
        if (f) f << res.comptime_unit_source;
    }
    /* Del resumen, no del comprobador de tipos: se le pregunto al acabar cada
     * modulo, que es cuando existia.  Lo que se guardaba para poder hacer esta
     * pregunta era un objeto de decenas de megabytes por modulo. */
    for (const auto &pm : work) {
        if (pm.inject_pending) {
            res.has_lowerable_macros = true;
            res.unresolved_inject = true;
            res.unresolved_inject_code = pm.inject_code;
            res.unresolved_inject_arg = pm.inject_arg;
            break;
        }
    }
    /* Y ADEMAS, mirando lo que se va a emitir.
     *
     * Preguntarle a cada modulo si le quedo algo pendiente solo funciona con
     * los que se acaban de compilar: los que se sirven del cache no traen type
     * checker al que preguntar, asi que un cuerpo vacio heredado de una pasada
     * anterior pasaba invisible -- que es justo el caso que hay que cazar.
     *
     * El IR fusionado si lo dice, venga de donde venga: el cuerpo lleva la
     * marca que dejo quien no pudo expandirlo.  Cuesta un recorrido del modulo
     * ya construido y no depende de cuantas pasadas haya habido. */
    if (!res.unresolved_inject) {
        for (const ir::IrFunction &fn : merged.functions) {
            for (const ir::IrBlock &b : fn.blocks) {
                for (const ir::IrInstr &in : b.instrs) {
                    if (in.op != ir::IrOp::INLINE_ASM) continue;
                    const size_t at =
                        in.func_name.find(ir::kAsmBodyPendingMark);
                    if (at == std::string::npos) continue;
                    res.unresolved_inject = true;
                    /* Y el motivo que la marca lleva detras: codigo del
                     * catalogo y argumento.  Es la UNICA via para un modulo
                     * servido del cache, que no trae comprobador al que
                     * preguntar. */
                    if (res.unresolved_inject_code.empty()) {
                        std::istringstream tail(in.func_name.substr(
                            at + std::strlen(ir::kAsmBodyPendingMark)));
                        tail >> res.unresolved_inject_code >>
                            res.unresolved_inject_arg;
                    }
                    break;
                }
                if (res.unresolved_inject) break;
            }
            if (res.unresolved_inject) break;
        }
    }
    res.ok = !res.diagnostics.has_errors();

    // Diagramas del IR mergeado y del .vel final.  Los del AST y los tipos
    // del raiz ya los saco `compile_unit`, en el mismo momento que el camino
    // de fichero suelto.  Los IR pre y post-opt se diagraman desde @c merged
    // (sin distincion de pre vs post porque @c ir_optimize ya corrio sobre
    // merged; el diagrama "pre" es identico al "post" en project compile --
    // esto es una limitacion documentada del modelo merge IR).
    if (opts.dump_mermaid_ir_pre || opts.dump_mermaid_ir_post) {
        std::string ir_text =
            mermaid_from_ir_module(merged, "IR (merged + optimized)");
        if (opts.dump_mermaid_ir_pre) res.mermaid_ir_pre = ir_text;
        if (opts.dump_mermaid_ir_post) res.mermaid_ir_post = ir_text;
    }
    if (opts.dump_graphviz_ir_pre || opts.dump_graphviz_ir_post) {
        std::string ir_text =
            graphviz_from_ir_module(merged, "IR (merged + optimized)");
        if (opts.dump_graphviz_ir_pre) res.graphviz_ir_pre = ir_text;
        if (opts.dump_graphviz_ir_post) res.graphviz_ir_post = ir_text;
    }
    if (opts.dump_html_ir_pre || opts.dump_html_ir_post) {
        // Proyecto multi-fichero: el IR ya esta merged + optimizado, asi que
        // pre y post comparten el mismo grafo (igual que Mermaid/Graphviz).
        std::string html =
            html_from_ir_module(merged, "SSA IR (merged + optimized)");
        if (opts.dump_html_ir_pre) res.html_ir_pre = html;
        if (opts.dump_html_ir_post) res.html_ir_post = html;
    }
    if (opts.dump_mermaid_vel) {
        res.mermaid_vel = mermaid_from_vel_text(res.vel_text);
    }
    if (opts.dump_graphviz_vel) {
        res.graphviz_vel = graphviz_from_vel_text(res.vel_text);
    }
    if (opts.dump_html_vel) {
        res.html_vel = html_from_vel_text(res.vel_text);
    }

    //  M5.B: poblar dep_paths con los paths canonicos de TODOS los
    // modulos compilados (incluido root).  main.cpp persistira el project
    // cache asociando (paths, hashes, .velb final).
    res.dep_paths.reserve(work.size());
    for (const auto &pm : work) {
        res.dep_paths.push_back(pm.canonical_path.str());
    }
    return res;
}

/**
 * @brief Aparece la palabra @p kw en el texto, fuera de comentarios y cadenas?
 *
 * Escaner deliberadamente simple, y permisivo por diseno: si dice que si y el
 * parser luego no la encuentra, no pasa nada.  Se busca por palabra COMPLETA
 * para que `importante` o `namespaced` no cuenten.
 */
static bool contiene_palabra(const std::string &source, const char *kw) {
    const size_t klen = std::strlen(kw);
    enum {
        NORMAL,
        IN_LINE_COMMENT,
        IN_BLOCK_COMMENT,
        IN_STRING
    } state = NORMAL;
    size_t i = 0;
    auto at_word_boundary = [&](size_t k) {
        if (k > 0) {
            char p = source[k - 1];
            if ((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z') ||
                (p >= '0' && p <= '9') || p == '_')
                return false;
        }
        if (k + klen > source.size()) return false;
        if (source.compare(k, klen, kw) != 0) return false;
        if (k + klen < source.size()) {
            char n = source[k + klen];
            if ((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') ||
                (n >= '0' && n <= '9') || n == '_')
                return false;
        }
        return true;
    };
    while (i < source.size()) {
        char c = source[i];
        switch (state) {
        case NORMAL:
            if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
                state = IN_LINE_COMMENT;
                i += 2;
                continue;
            }
            if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
                state = IN_BLOCK_COMMENT;
                i += 2;
                continue;
            }
            if (c == '"') {
                state = IN_STRING;
                ++i;
                continue;
            }
            if (at_word_boundary(i)) return true;
            break;
        case IN_LINE_COMMENT:
            if (c == '\n') state = NORMAL;
            break;
        case IN_BLOCK_COMMENT:
            if (c == '*' && i + 1 < source.size() && source[i + 1] == '/') {
                state = NORMAL;
                i += 2;
                continue;
            }
            break;
        case IN_STRING:
            if (c == '\\' && i + 1 < source.size()) {
                i += 2;
                continue;
            }
            if (c == '"') state = NORMAL;
            break;
        }
        ++i;
    }
    return false;
}

/**
 * @brief Ver la declaracion en compiler.h.
 *
 * El texto sale del catalogo multi-idioma: aqui solo viajan DATOS (que region,
 * que tramo, que hueco).  La prueba va DENTRO del mensaje, porque un "acceso
 * fuera de region" sin su derivacion obliga a reconstruirla a mano.
 */

/**
 * @brief Ver la declaracion en compiler.h.
 */
/**
 * @brief Avisa de un `call_site` que un bloque de ensamblador deja sin sentido.
 *
 * Pedir `call_site` lee la direccion de retorno de la PILA, asi que un bloque
 * que mueva @c rsp o @c rbp la deja donde no esta y el gancho recibe lo que
 * hubiera en esa posicion.
 *
 * Se puede DECIR, y ahi esta la diferencia con tratar el asm como una caja
 * negra: la tabla de efectos ya declara que @c push y @c pop escriben @c rsp
 * -- "se dice POR DONDE", como reza ahi --, asi que basta preguntarlo.  Otro
 * compilador devolveria el numero igual y el usuario mediria mal sin saberlo.
 *
 * SOLO se llama si el modulo pidio la direccion de retorno en alguna parte
 * (@c IrModule::usa_return_addr, que pone el propio emisor al bajarla).  Sin
 * eso habria que recorrer el IR ENTERO de cada programa para descubrir algo
 * que quien lo emitio ya sabia -- y el precio lo pagarian todas las
 * compilaciones, incluidas las que no usan ganchos.
 *
 * @param mod   Modulo ya bajado.
 * @param diags Donde dejar el aviso.
 * @param file  Fichero al que apuntar.
 */
static void vx_warn_call_site_with_asm(const ir::IrModule &mod,
                                       Diagnostics &diags,
                                       const std::string &file) {
    for (const ir::IrFunction &fn : mod.functions) {
        // Una sola pasada por funcion, y se corta en cuanto se sabe la
        // respuesta: en cuanto hay las dos cosas ya no queda nada que mirar.
        bool wants_return_addr = false;
        const ir::IrInstr *asm_block = nullptr;
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &in : b.instrs) {
                if (in.op == ir::IrOp::RETURN_ADDR)
                    wants_return_addr = true;
                else if (in.op == ir::IrOp::INLINE_ASM && asm_block == nullptr)
                    asm_block = &in;
                if (wants_return_addr && asm_block != nullptr) break;
            }
            if (wants_return_addr && asm_block != nullptr) break;
        }
        if (!wants_return_addr || asm_block == nullptr) continue;

        // Solo ahora se analiza el bloque, que es lo caro de todo esto.
        const vx::AsmBlockEffects e = vx::asm_analyze_block_no_classes(
            asm_block->func_name, vx::asm_arch_actual());
        /* Los nombres llegan CANONICOS por arquitectura: en x86 los cuatro
         * anchos de la pila (`rsp`/`esp`/`sp`/`spl`) se normalizan a `rsp` y
         * los del marco a `rbp`, y en arm64 salen `sp`, `x29` y `x30`.  Por
         * eso la lista no enumera anchos ni alias: eso ya lo resolvio quien
         * mejor lo sabe.
         *
         * `x30` entra porque en arm64 la vuelta viaja en el registro de
         * enlace, no en la pila: pisarlo rompe lo mismo que mover `rsp` en
         * x86, aunque la pila quede intacta. */
        static const char *const kReturnPathRegs[] = {
            "rsp", "rbp", "sp", "x29", "x30", "r13", "r11", "r14"};
        std::string touched;
        for (const std::string &r : e.escritos) {
            bool afecta = false;
            for (const char *c : kReturnPathRegs)
                if (r == c) {
                    afecta = true;
                    break;
                }
            if (!afecta) continue;
            if (!touched.empty()) touched += ", ";
            touched += r;
        }
        if (touched.empty()) continue;
        diags.diag(
            SourceLoc{util::intern_name(file), asm_block->source_line, 1},
            DiagLevel::WARN, "VXW934", {fn.name, touched});
    }
}

void vx_report_asm_preconditions(const ir::IrModule &mod, Diagnostics &diags,
                                 const std::string &file, bool programa_cerrado,
                                 bool decir_lo_no_acotado, const char *backend,
                                 analysis::asa::FactStore &facts) {
    /* Con QUE garantia se coloca la seccion de datos NO es una propiedad del
     * programa: depende de donde acabe corriendo.  Por eso se pregunta con el
     * destino en la mano, en vez de llevar un numero dentro.
     *
     * Y se PREGUNTA AL ASA en vez de recalcularlo.  Antes esta linea llamaba
     * directamente a `alineacion_seccion_datos(backend)`, que es el mismo
     * calculo que el dominio `disposicion` ya hace y deposita como hecho: dos
     * sitios respondiendo lo mismo, que es justo lo que el primer invariante
     * -- un hecho, un productor -- existe para impedir.  El dia que la garantia
     * cambie, con dos calculos uno de los dos se queda viejo y nadie se entera.
     *
     * Se produce SOLO ese dominio: en la medicion costaba 0 us, mientras que
     * producirlo todo se iba a ~544 us por modulo, y el 80% eran los rangos que
     * aqui no se consultan.
     *
     * Si el hecho no aparece, se cae al calculo directo.  No es un respaldo
     * silencioso: `find` dice si habia hechos con ese codigo fuera de alcance,
     * y eso distingue "aun no se produce" de "se produce y no vale aqui". */
    /* POST-optimizacion: aqui se habla del codigo que de verdad se va a
     * emitir, que es lo que este informe tiene que juzgar. */
    analysis::asa::produce(mod, facts, {"asa.layout"},
                           analysis::asa::kStagePostOpt);
    analysis::asa::Scope here;
    here.backend = backend;
    /* Y se pregunta por el mismo momento en el que se produjo: preguntar sin
     * decirlo no es "cualquiera", es no casar con ninguno. */
    here.stage = analysis::asa::kStagePostOpt;
    /* El bytecode es una ISA mas, y el hecho del cargador se sella por ella. */
    if (backend != nullptr &&
        (std::strcmp(backend, analysis::asa::kBackendVm) == 0 ||
         std::strcmp(backend, analysis::asa::kBackendJit) == 0))
        here.isa = analysis::asa::kIsaVelb;
    const auto found = facts.find("layout.section_alignment", here);
    const uint32_t computed = analysis::alineacion_seccion_datos(backend);
    const uint32_t garantia =
        found ? static_cast<uint32_t>(found.fact->what.a) : computed;
    /* Y SI LAS DOS FUENTES NO DICEN LO MISMO, eso no es una duda del analisis:
     * es un FALLO DEL COMPILADOR, y tiene que gritar.
     *
     * Mientras el respaldo exista hay dos caminos que contestan a la misma
     * pregunta, y el dia que uno se quede viejo el otro lo TAPA en silencio --
     * que es exactamente como este hecho estuvo meses mudo.  Comparar cuesta
     * una instruccion y convierte un fallo invisible en uno ruidoso.
     *
     * Solo se compara cuando el hecho VALE aqui: si no vale, el respaldo es la
     * respuesta buena y no hay nada que contrastar. */
    if (found && static_cast<uint32_t>(found.fact->what.a) != computed) {
        SourceLoc loc;
        loc.set_file(file);
        diags.diag(loc, DiagLevel::WARN,
                   analysis::asa::unknown_reason_code(
                       analysis::asa::UnknownReason::SourcesDisagree),
                   {"layout.section_alignment",
                    std::to_string(found.fact->what.a),
                    std::to_string(computed)});
    }
    const uint32_t cabecera = analysis::alineacion_payload_reserva(backend);
    /* Lo que le llega a cada funcion desde sus sitios de llamada.  Sin esto,
     * un parametro no vale nada y la comprobacion se queda en la frontera --
     * que es justo donde NO esta el asm: quien exige alineacion suele recibir
     * el destino, no reservarlo. */
    const analysis::AlignmentSummaries resumen =
        analysis::compute_alignment_summaries(mod, programa_cerrado);

    /**
     * @brief Lo que se pudo decir de UNA instruccion que exige alineacion.
     *
     * El orden importa: es el de menos a mas fuerte.  Una misma instruccion del
     * fuente puede verse VARIAS veces -- el inline trae una copia a cada sitio
     * donde se llamo --, y cada copia sabe lo suyo: donde se ve la direccion
     * concreta se puede demostrar, y en la funcion original quiza no.  Son la
     * misma instruccion, asi que el usuario tiene que recibir UN veredicto.
     */
    enum class Veredicto : uint8_t {
        SinAncho = 0,  ///< no se pudo determinar cuanto exige.
        SinPrueba = 1, ///< se sabe cuanto exige, no si se cumple.
        Cumple = 2,    ///< demostrado que cumple.
        Falla = 3,     ///< demostrado que NO cumple.
    };
    /// Una instruccion del fuente y lo mejor que se pudo decir de ella.
    struct Sitio {
        uint32_t line = 0;
        uint32_t column = 0;
        std::string mnemonic;
        uint16_t bytes = 0;
        std::string operando;
        uint32_t resto = 0;  ///< la prueba, cuando @c Falla.
        uint32_t modulo = 0; ///< la prueba, cuando @c Falla.
        Veredicto v = Veredicto::SinAncho;
    };
    std::vector<Sitio> sitios;

    /**
     * @brief Lo que una funcion EXIGE de quien la llama.
     *
     * Dentro de la funcion, la alineacion de un parametro no se puede saber: la
     * pone el llamante.  Asi que la exigencia que el asm impone sobre un
     * parametro deja de morir en "sin prueba" y viaja HACIA FUERA, para
     * cruzarla con lo que se sabe del argumento en cada sitio de llamada.
     *
     * Es el simetrico de los resumenes de rango, que hacen el viaje contrario
     * (lo que los llamantes pasan, hacia dentro).  Y es DERIVADA: sale de lo
     * que la instruccion exige segun la base de datos, no de una anotacion.
     */
    struct ExigenciaFrontera {
        std::string funcion;
        uint32_t param = 0;
        uint32_t bytes = 0;   ///< de cuanto tiene que ser multiplo.
        std::string mnemonic; ///< quien lo exige, para la prueba.
    };
    std::vector<ExigenciaFrontera> exigencias;
    /// La alineacion de cada funcion, calculada UNA vez: la usan el recorrido
    /// del asm y despues el de los sitios de llamada.
    std::unordered_map<std::string, analysis::AlignmentFacts> alineacion_de;

    for (const ir::IrFunction &fn : mod.functions) {
        if (fn.blocks.empty()) continue;
        /* Una funcion de la que se ha visto TODO lo que podria llamarla, y no
         * habia nada, no se ejecuta nunca.  Eso no es ignorancia -- es la
         * ausencia demostrada --, y comprobar el cuerpo de algo que nadie llama
         * solo produce ruido: es lo que pasa con la copia que el inline deja
         * huerfana. */
        if (resumen.universo_de(fn.name) ==
            analysis::Universo::CerradoSinLlamantes)
            continue;
        (void)exigencias; // se llena mas abajo y se cruza tras el recorrido
        /* De que valor habla cada operando del asm.  Lo responde el mismo sitio
         * que se lo responde al modelo de efectos y al eliminador de escrituras
         * muertas: el camino marcador -> ligadura -> hueco -> contenido se
         * recorre UNA vez y en un solo sitio. */
        const analysis::AsmBindingFacts lig =
            analysis::compute_asm_bindings(fn);
        /* Y el diccionario que le falta al analisis del texto: tras la
         * sustitucion, `movdqa [$0], $1` no dice que `$1` mida 128 bits.  Lo
         * dice la CLASE con la que se declaro, que es lo que escribio el
         * programador, y viene hecho con las ligaduras. */
        const vx::AsmOperandClasses &clases = lig.operand_classes;

        /* Se guarda para el segundo recorrido (el de los sitios de llamada) en
         * vez de volver a calcularla: es el mismo hecho sobre la misma funcion,
         * y recalcularlo seria pagar dos veces por saber lo mismo. */
        const analysis::AlignmentFacts &alin =
            alineacion_de
                .emplace(fn.name, analysis::compute_alignment(
                                      fn, &resumen, &mod, garantia, cabecera))
                .first->second;
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &in : b.instrs) {
                /* Tambien el asm ELEVADO.
                 *
                 * Esto solo miraba los bloques opacos, asi que entender mejor
                 * un bloque hacia que se comprobara MENOS: en cuanto el elevado
                 * aprendia a pasarlo a IR, la comprobacion de alineacion
                 * desaparecia y un programa que revienta al ejecutarse pasaba
                 * el compilador.  Justo al reves de lo que tiene que pasar.
                 *
                 * Un micro asm lleva su instruccion en la plantilla, asi que el
                 * analisis del texto es el mismo; lo que cambia es de donde
                 * sale la direccion: en el opaco la dicen las ligaduras del
                 * bloque y aqui la lleva el propio operando. */
                if (in.op != ir::IrOp::INLINE_ASM &&
                    in.op != ir::IrOp::ASM_MICRO)
                    continue;
                const ir::AsmMicro *am = (in.op == ir::IrOp::ASM_MICRO &&
                                          in.imm < fn.asm_micros.size())
                                             ? &fn.asm_micros[in.imm]
                                             : nullptr;
                if (in.op == ir::IrOp::ASM_MICRO && am == nullptr) continue;
                /* Cuanto MIDE cada operando.  En el bloque opaco lo dice la
                 * clase que escribio el programador; en el elevado lo lleva la
                 * ficha, que para eso guarda el ancho de cada operando.  Sin
                 * esto la comprobacion se queda a medias: sabe que la
                 * instruccion exige alineacion pero no de cuantos bytes. */
                vx::AsmOperandClasses clases_micro;
                if (am != nullptr) {
                    clases_micro.reserve(am->operands.size());
                    for (size_t oi = 0; oi < am->operands.size(); ++oi) {
                        const ir::AsmMicroOperand &o = am->operands[oi];
                        const char *c = "reg";
                        if (o.regclass == vx::ASM_RC_VEC ||
                            o.regclass == vx::ASM_RC_FP)
                            c = o.width == 512   ? "zmm"
                                : o.width == 256 ? "ymm"
                                                 : "xmm";
                        clases_micro.push_back(vx::asm_operand_class(
                            "$" + std::to_string(oi), c));
                    }
                }
                const std::string texto =
                    am != nullptr ? vx::asm_micro_texto_con_forma(*am)
                                  : in.func_name;
                const vx::AsmInferResult inf = vx::asm_infer_clobbers(
                    texto, {}, am != nullptr ? clases_micro : clases);
                for (const vx::AsmAlignReq &req : inf.align_reqs) {
                    Sitio s;
                    s.line = in.source_line;
                    s.column = in.source_column;
                    s.mnemonic = req.mnemonic;
                    s.bytes = req.bytes;
                    s.operando = req.operando.empty() ? "?" : req.operando;
                    if (req.bytes != 0) {
                        /* Que valor es la direccion.  El analisis del texto ya
                         * dijo por que operando se llega (@c req.base); las
                         * ligaduras dicen de que valor habla ese operando.
                         *
                         * Puede haber VARIAS candidatas (dos variables en el
                         * mismo registro).  Entonces no se sabe cual es, pero
                         * eso no obliga a callarse: si TODAS cumplen, cumple
                         * sea cual sea; si TODAS fallan, revienta sea cual sea.
                         * Solo cuando discrepan no hay nada demostrado -- y ahi
                         * se avisa, que no es lo mismo que dar por bueno ni que
                         * dar por malo. */
                        s.v = Veredicto::SinPrueba;
                        /* En el micro asm la base es un operando de la propia
                         * instruccion, y su valor viaja con ella: no hay que
                         * preguntarle a nadie de que variable se trata. */
                        std::vector<analysis::LigaduraAsm> cands_micro;
                        if (am != nullptr && req.base.size() >= 2 &&
                            req.base[0] == '$') {
                            const size_t k =
                                (size_t)std::atoi(req.base.c_str() + 1);
                            size_t nv = 0;
                            for (size_t oi = 0; oi < am->operands.size();
                                 ++oi) {
                                if (am->operands[oi].value == ir::IR_NO_VALUE)
                                    continue;
                                if (oi == k && nv < in.operands.size()) {
                                    analysis::LigaduraAsm l;
                                    l.valor = in.operands[nv];
                                    cands_micro.push_back(l);
                                    break;
                                }
                                ++nv;
                            }
                        }
                        analysis::AsmBindingFacts::Candidatas cands;
                        if (am != nullptr) {
                            cands.datos = cands_micro.data();
                            cands.n = cands_micro.size();
                        } else {
                            cands = lig.candidatas(req.base);
                        }
                        if (!cands.empty()) {
                            bool todas_cumplen = true, todas_fallan = true;
                            for (const analysis::LigaduraAsm &l : cands) {
                                const ir::IrValueId dir = l.valor;
                                const bool cumple =
                                    dir != ir::IR_NO_VALUE &&
                                    alin.multiplo_de(dir, req.bytes);
                                const bool falla =
                                    dir != ir::IR_NO_VALUE &&
                                    alin.seguro_no_multiplo_de(dir, req.bytes);
                                todas_cumplen = todas_cumplen && cumple;
                                todas_fallan = todas_fallan && falla;
                                if (falla) {
                                    // La prueba: la primera que falla vale para
                                    // explicarlo, y si fallan todas dan la
                                    // misma razon.
                                    s.resto = alin.resto_de(dir);
                                    s.modulo = alin.de(dir);
                                }
                            }
                            if (todas_cumplen)
                                s.v = Veredicto::Cumple;
                            else if (todas_fallan)
                                s.v = Veredicto::Falla;
                            /* Y si la direccion es un PARaMETRO, aqui no se
                             * puede saber: la alineacion la pone QUIEN LLAMA.
                             * En vez de morir en "sin prueba", la exigencia
                             * sale de la funcion y se apunta como propiedad
                             * suya -- "el parametro N debe ser multiplo de K"
                             * --, para cruzarla despues en cada sitio de
                             * llamada.
                             *
                             * Derivada, no declarada: sale de lo que la
                             * instruccion EXIGE (base de datos) y de por donde
                             * le llega la direccion.  Nadie la escribe. */
                            if (s.v == Veredicto::SinPrueba) {
                                for (const analysis::LigaduraAsm &l : cands) {
                                    for (size_t pi = 0; pi < fn.params.size();
                                         ++pi) {
                                        if (fn.params[pi] != l.valor) continue;
                                        ExigenciaFrontera e;
                                        e.funcion = fn.name;
                                        e.param = static_cast<uint32_t>(pi);
                                        e.bytes = req.bytes;
                                        e.mnemonic = req.mnemonic;
                                        exigencias.push_back(std::move(e));
                                    }
                                }
                            }
                        }
                    }
                    /* Se acumula en vez de emitirse: el veredicto es del SITIO
                     * del fuente, y hasta haber visto todas sus copias no se
                     * sabe cual es el mejor que se pudo dar. */
                    bool fusionado = false;
                    for (Sitio &y : sitios) {
                        if (y.line != s.line || y.column != s.column ||
                            y.mnemonic != s.mnemonic)
                            continue;
                        fusionado = true;
                        if (s.v > y.v) {
                            const uint32_t l0 = y.line, c0 = y.column;
                            y = s;
                            y.line = l0;
                            y.column = c0;
                        }
                        break;
                    }
                    if (!fusionado) sitios.push_back(std::move(s));
                }
            }
        }
    }

    /* Y ahora UN veredicto por sitio.  La regla es la de siempre: una prueba no
     * la borra una copia que no pudo demostrar nada, y no poder demostrar que
     * algo es seguro no es demostrar que es inseguro.  Demostrado-que-falla
     * manda sobre todo: basta con que UN camino reviente. */
    for (const Sitio &s : sitios) {
        SourceLoc loc;
        loc.line = s.line;
        loc.column = s.column;
        loc.set_file(file);
        switch (s.v) {
        case Veredicto::Cumple: break; // demostrado: no hay nada que decir.
        case Veredicto::Falla:
            diags.diag(loc, DiagLevel::ERR, "VXA013",
                       {s.mnemonic, std::to_string(s.bytes),
                        std::to_string(s.resto), std::to_string(s.modulo)});
            break;
        case Veredicto::SinPrueba:
            diags.diag(loc, DiagLevel::WARN, "VXA011",
                       {s.mnemonic, s.operando, std::to_string(s.bytes)});
            break;
        case Veredicto::SinAncho:
            diags.diag(loc, DiagLevel::WARN, "VXA012", {s.mnemonic});
            break;
        }
    }

    /* Y AHORA lo que la funcion exige, cruzado con lo que se sabe del argumento
     * EN CADA SITIO DE LLAMADA.
     *
     * Es la otra mitad: dentro de la funcion la alineacion de un parametro no
     * se puede saber, asi que la exigencia salio hacia fuera; aqui se encuentra
     * con quien si lo sabe.  Sin esto, una precondicion que el propio asm
     * declara se quedaba sin comprobar en el unico sitio donde era comprobable.
     *
     * Indexado por funcion llamada: un vector recorrido por cada llamada seria
     * llamadas x exigencias, y de eso ya se ha aprendido bastante hoy. */
    if (!exigencias.empty()) {
        std::unordered_map<std::string, std::vector<const ExigenciaFrontera *>>
            por_llamada;
        for (const ExigenciaFrontera &e : exigencias)
            por_llamada[e.funcion].push_back(&e);
        /* Un sitio del fuente se dice UNA vez.  La misma llamada aparece tantas
         * veces como copias tenga el codigo (inline, monomorfizacion, la propia
         * funcion analizada en dos modulos), y repetir el mismo aviso cinco
         * veces no informa cinco veces: entierra los demas.  La clave es el
         * SITIO y el argumento, que es de lo que se habla. */
        std::unordered_set<uint64_t> ya_dicho;
        for (const ir::IrFunction &fn : mod.functions) {
            if (fn.is_native) continue;
            /* La alineacion del LLAMANTE, ya calculada en el recorrido de
             * arriba.  Si esta funcion no llego a analizarse alli, no se
             * inventa: se deja sin comprobar. */
            auto ia = alineacion_de.find(fn.name);
            if (ia == alineacion_de.end()) continue;
            const analysis::AlignmentFacts &alin_caller = ia->second;
            for (const ir::IrBlock &b : fn.blocks) {
                for (const ir::IrInstr &in : b.instrs) {
                    if (in.op != ir::IrOp::CALL || in.func_name.empty())
                        continue;
                    auto it = por_llamada.find(in.func_name);
                    if (it == por_llamada.end()) continue;
                    for (const ExigenciaFrontera *e : it->second) {
                        if (e->param >= in.operands.size()) continue;
                        const ir::IrValueId arg = in.operands[e->param];
                        if (arg == ir::IR_NO_VALUE) continue;
                        if (alin_caller.multiplo_de(arg, e->bytes))
                            continue; // demostrado que cumple: nada que decir
                        const uint64_t sitio =
                            (static_cast<uint64_t>(in.source_line) << 32) ^
                            (static_cast<uint64_t>(in.source_column) << 8) ^
                            e->param;
                        if (!ya_dicho.insert(sitio).second)
                            continue; // ya se dijo de este sitio
                        SourceLoc loc;
                        loc.line = in.source_line;
                        loc.column = in.source_column;
                        loc.set_file(file);
                        const std::vector<std::string> args = {
                            in.func_name, std::to_string(e->param),
                            std::to_string(e->bytes), e->mnemonic};
                        if (alin_caller.seguro_no_multiplo_de(arg, e->bytes))
                            diags.diag(loc, DiagLevel::ERR, "VXA040", args);
                        else
                            diags.diag(loc, DiagLevel::WARN, "VXA041", args);
                    }
                }
            }
        }
    }

    /* Y lo que NO se pudo acotar, dicho.
     *
     * Un acceso cuya extension no se determina vale "toca el objeto entero", y
     * un objeto entero NUNCA se sale de si mismo: la comprobacion de limites
     * mira ese acceso y calla.  O sea que la unica senal de que el analisis no
     * llego era... ninguna.  Eso ya costo caro: un `asm` que escribia mas alla
     * del buffer del llamante pasaba sin un aviso y corrompia la variable de al
     * lado, y el fallo aparecia despues, lejos y disfrazado de fallo del motor.
     *
     * No saber es un resultado, y se cuenta como tal.  Con el MOTIVO, que es lo
     * unico accionable: quien lee no puede hacer nada con "no se pudo", pero si
     * con "el bloque salta" o "ese operando no dice cuantos bytes mide". */
    /* Un `@Hook` que pide `call_site` lee la direccion de retorno de la PILA,
     * asi que un bloque de ensamblador que mueva `rsp` o `rbp` la deja donde
     * no esta y el gancho recibe lo que hubiera en esa posicion.
     *
     * Esto se puede DECIR, y es la diferencia con tratar el asm como una caja
     * negra: la tabla de efectos ya declara que `push`/`pop` escriben `rsp`
     * -- "se dice POR DONDE", como reza ahi --, asi que basta preguntarlo.
     * Otro compilador devolveria el numero igual, y el usuario mediria mal sin
     * enterarse.
     *
     * Corre SIEMPRE, no bajo el flag de lo no acotado: no es un detalle de
     * cuanto se sabe del bloque, es un valor que va a salir mal.
     */
    if (mod.usa_return_addr) vx_warn_call_site_with_asm(mod, diags, file);

    if (!decir_lo_no_acotado) return;
    for (const ir::IrFunction &fn : mod.functions) {
        const analysis::AsmBindingFacts lig =
            analysis::compute_asm_bindings(fn);
        const vx::AsmOperandClasses &clases = lig.operand_classes;
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &in : b.instrs) {
                if (in.op != ir::IrOp::INLINE_ASM) continue;
                const vx::AsmBlockEffects e = vx::asm_analyze_block(
                    in.func_name, vx::asm_arch_actual(), clases);
                // Solo importa lo que ESCRIBE: leer de mas no corrompe a nadie.
                const char *motivo = nullptr;
                for (const vx::AsmBlockEffects::Acceso &a : e.accesos) {
                    if (!a.escribe) continue;
                    if (!a.valida) {
                        motivo = "VXA016";
                        break;
                    }
                    if (!a.extension.ancho_conocido()) {
                        motivo = "VXA015";
                        break;
                    }
                    if (lig.candidatas(a.base).size() != 1) {
                        motivo = "VXA017";
                        break;
                    }
                }
                if (motivo == nullptr && !e.accesos_incompletos) continue;
                if (motivo == nullptr) motivo = "VXA016";
                SourceLoc loc;
                loc.line = in.source_line;
                loc.column = in.source_column;
                loc.set_file(file);
                const std::string razon =
                    vx::diag::format(motivo, {std::string()});
                diags.diag(loc, DiagLevel::WARN, "VXA014", {razon});
            }
        }
    }
}

/* `vx_report_borrow_across_calls` vivia aqui, dentro del fichero del camino de
 * proyecto, que es de todo menos el sitio de una comprobacion de prestamos.
 * Ahora es `borrow::report_exclusive_across_calls`, en su modulo, junto a la
 * que AVERIGUA lo que ella CUENTA. */

void vx_report_bounds(const ir::IrModule &mod, Diagnostics &diags,
                      const std::string &file, analysis::asa::FactBase &base,
                      DiagLevel level) {
    // Medicion del dominio de FORMA, apagada salvo que se pida explicitamente.
    // Todavia no la consume nadie: primero hay que saber si distingue algo.
    analysis::asa::volcar_formas(mod, "post-opt");
    /* El motor sale de la BASE, no de aqui.  Construir uno propio rehace el
     * escape y los resumenes del modulo entero, y quien ya lo hubiera pedido
     * para este mismo momento lo habria pagado dos veces. */
    analysis::effects::EffectAnalysis &ea =
        base.effects(mod, analysis::asa::kStagePostOpt);
    /* Y los resumenes de frontera tambien: son los mismos con los que se
     * construyo ese motor, asi que el comprobador no rehace nada. */
    const analysis::RangeSummaries &boundary_summaries =
        base.boundary(mod, analysis::asa::kStagePostOpt);
    for (const analysis::effects::BoundsViolation &v :
         analysis::effects::check_region_bounds(mod, &ea,
                                                &boundary_summaries)) {
        SourceLoc loc;
        loc.line = v.line;
        loc.set_file(file);
        diags.diag(loc, level, "VX3001",
                   {vx::diag::format(v.write ? "VX3002" : "VX3003", {}),
                    std::to_string(v.width), v.region, std::to_string(v.limite),
                    std::to_string(v.off), std::to_string(v.off + v.width)});
        /* COMO se detecto -- no solo que pasa.  Quien lee un error tiene que
         * poder juzgar si se lo cree, y para eso necesita saber de donde sale
         * cada mitad del veredicto. */
        /* EN QUE funcion.  El analisis siempre lo supo (@c BoundsViolation
         * lleva su nombre) y el informe lo tiraba, con lo que un error en una
         * funcion de la stdlib -- fusionada en el modulo -- se leia como si
         * estuviera en el fichero del usuario.  Peor aun: la linea es la del
         * modulo de origen y el fichero el del raiz, asi que un programa de
         * quince lineas recibia errores en la linea 414.  Hasta que el informe
         * sepa de que fichero viene cada funcion, al menos se dice de quien es
         * la linea para que nadie la busque donde no esta. */
        if (!v.function.empty())
            diags.note(loc, vx::diag::format("VX3006", {v.function}));
        diags.note(loc, vx::diag::format("VX3004", {std::to_string(v.objeto)}));
        /* Y QUE hacer.  El analisis conoce las dos salidas: agrandar el objeto
         * hasta donde llega el acceso, o no pasar de donde llega el objeto. */
        diags.note(loc,
                   vx::diag::format("VX3005", {std::to_string(v.off + v.width),
                                               std::to_string(v.limite)}));
    }
}

/**
 * @brief De donde saca el informe de direcciones sus tres entradas.
 *
 * Con NOMBRE y no capturado en una lambda: quien contesta sale en el perfil, y
 * aqui contestar cuesta -- detras de cada una hay un analisis del modulo.
 */
struct FnAddrReportCtx {
    analysis::asa::FactBase *base = nullptr;
    const ir::IrModule *mod = nullptr;
    /// El escape de cada funcion del modulo, por su posicion.
    const analysis::ModuleEscape *escape = nullptr;
    /// Lo que se contesta de una funcion sin cuerpo (nativa): no hay nada suyo
    /// que se pueda escapar DESDE aqui.
    analysis::EscapeInfo none;
};

/// Def-use de @p fn, por la base.
static const analysis::IrFacts &fn_addr_facts_of(void *ctx,
                                                 const ir::IrFunction &fn) {
    auto *c = static_cast<FnAddrReportCtx *>(ctx);
    return c->base->structure(fn, analysis::asa::kStagePostOpt);
}

/// Points-to de @p fn, por la base.
static const analysis::PointsTo &
fn_addr_points_to_of(void *ctx, const ir::IrFunction &fn) {
    auto *c = static_cast<FnAddrReportCtx *>(ctx);
    return c->base->memory(fn, analysis::asa::kStagePostOpt);
}

/// Escape de @p fn, del cierre de modulo que la base ya hizo.
static const analysis::EscapeInfo &fn_addr_escape_of(void *ctx,
                                                     const ir::IrFunction &fn) {
    auto *c = static_cast<FnAddrReportCtx *>(ctx);
    const analysis::EscapeInfo *e = c->escape->find(*c->mod, fn);
    return e == nullptr ? c->none : *e;
}

void vx_report_fn_addr_crossing(const ir::IrModule &mod,
                                analysis::asa::FactBase &base) {
    /* Detras de su interruptor: hoy solo MIDE.  Antes de cambiar como se
     * emite una direccion de funcion hay que saber si el fallo ocurre de
     * verdad y cuanto hay de cada clase -- decidirlo sin el numero es
     * decidirlo a ciegas. */
    if (!util::flag_on(util::FlagId::FnAddrReport)) return;

    FnAddrReportCtx ctx;
    ctx.base = &base;
    ctx.mod = &mod;
    ctx.escape = &base.escape(mod);

    analysis::FnAddrInputs in;
    in.facts = &fn_addr_facts_of;
    in.escape = &fn_addr_escape_of;
    in.points_to = &fn_addr_points_to_of;
    in.ctx = &ctx;
    const analysis::FnAddrEscapeModule report =
        analysis::compute_fn_addr_escape_module(mod, in);

    /* Cuantas hay y cuantas CRUZAN.  La diferencia es lo que se puede
     * abaratar, que es el numero que decide si emitir la nativa por defecto
     * sale a cuenta. */
    size_t crossing = 0;
    size_t seen = 0;
    size_t by_reason[6] = {0, 0, 0, 0, 0, 0};
    for (const analysis::FnAddrEscapeOfFunction &entry : report.by_function) {
        /* El total lo trae el propio analisis: volver a contarlo aqui seria
         * recorrer el modulo entero otra vez y, peor, volver a decidir cuales
         * son de funcion -- que es internar un nombre por instruccion. */
        seen += entry.escape.fn_addr_count;
        for (const analysis::FnAddrSite &site : entry.escape.sites) {
            ++crossing;
            const size_t idx = static_cast<size_t>(site.crossing);
            if (idx < 6) ++by_reason[idx];
        }
    }
    /* El texto sale del CATALOGO, aunque esto sea una medicion y no un
     * diagnostico: que lo lea una persona es lo que decide que tenga que estar
     * en su idioma.  Escribirlo aqui seria dejar castellano clavado en el
     * compilador. */
    std::fprintf(stderr, "%s\n",
                 vx::diag::format("VX2134", {std::to_string(seen),
                                             std::to_string(crossing),
                                             std::to_string(by_reason[1]),
                                             std::to_string(by_reason[2]),
                                             std::to_string(by_reason[3]),
                                             std::to_string(by_reason[4]),
                                             std::to_string(by_reason[5])})
                     .c_str());
}

void vx_report_callind_memory(const ir::IrModule &mod, Diagnostics &diags,
                              const std::string &file, DiagLevel level) {
    /* Detras de su interruptor mientras se mide.  La regla es la acordada --
     * donde no se pueda deducir de que memoria es una direccion, error -- pero
     * encenderla sin saber a cuantos sitios alcanza seria decidir a ciegas. */
    if (!util::flag_on(util::FlagId::CallindStrict)) return;
    /* UNA base para todo el modulo: su memoizacion vale mientras viva, asi
     * que crearla por funcion no cachearia nada.  DESPUES de optimizar, que
     * es cuando esto corre: lo que se le reprocha al usuario tiene que ser lo
     * que de verdad queda, no lo que el optimizador iba a resolver. */
    analysis::asa::FactBase base(analysis::asa::kStagePostOpt);
    for (const ir::IrFunction &fn : mod.functions) {
        for (const ir::CallTargetSite &s :
             ir::ir_callind_target_memory(fn, base)) {
            if (s.memory != ir::CallTargetMemory::Unknown) continue;
            SourceLoc loc;
            loc.line = s.line;
            loc.set_file(file);
            diags.diag(loc, level, "VX2128", {});
            /* EN QUE funcion.  La stdlib va FUSIONADA en el modulo, asi que
             * sin esto un sitio suyo se lee como si estuviera en el fichero
             * del usuario, y encima con una linea que alli no existe.  Es la
             * misma cautela que ya tomo el informe de limites. */
            if (!fn.name.empty())
                diags.note(loc, vx::diag::format("VX3006", {fn.name}));
            /* Y POR QUE no se supo, que no es un adorno: lo que llega por un
             * parametro no lo puede saber nunca quien lo recibe -- solo quien
             * lo pasa --, mientras que lo que viene de memoria si se deduce
             * mirando quien escribe ahi.  Se arreglan de forma OPUESTA, asi
             * que un mensaje que no los separe manda al sitio equivocado.
             *
             * Un codigo por motivo, y el texto del CATALOGO: el analisis
             * devuelve un dato, nunca una frase. */
            const char *why_code = nullptr;
            switch (s.why) {
            case ir::CallTargetUnknown::Parameter: why_code = "VX2129"; break;
            case ir::CallTargetUnknown::FromMemory: why_code = "VX2130"; break;
            case ir::CallTargetUnknown::Computed: why_code = "VX2131"; break;
            case ir::CallTargetUnknown::Disagree: why_code = "VX2132"; break;
            case ir::CallTargetUnknown::TooDeep: why_code = "VX2133"; break;
            case ir::CallTargetUnknown::None: break;
            }
            if (why_code != nullptr)
                diags.note(loc, vx::diag::format(why_code, {}));
        }
    }
}

bool vx_source_has_imports(const std::string &source) {
    return contiene_palabra(source, "import");
}

bool vx_source_declara_namespace(const std::string &source) {
    return contiene_palabra(source, "namespace");
}

bool vx_source_needs_project(const std::string &source) {
    if (contiene_palabra(source, "import")) return true;
    if (contiene_palabra(source, "namespace")) return true;
    /* Y lo que el manifiesto declare auto-importable, aunque el fuente no lo
     * escriba: esa es justamente la razon de que exista la auto-importacion --
     * reservar memoria se escribe `new`, no `import` --.  Decidirlo por lo
     * ESCRITO mandaba al camino de fichero suelto a un programa que SI tiene
     * dependencias, y alli no hay grafo de modulos donde traerlas. */
    return !auto_import_modules(stdlib_manifest_path()).empty();
}

} // namespace vx
