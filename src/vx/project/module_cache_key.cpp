/**
 * @file module_cache_key.cpp
 * @brief La identidad en cache del artefacto de un modulo.
 * @see vx/project/module_cache_key.h
 */
#include "vx/project/module_cache_key.h"

#include "util/env_flags.h"
#include "util/fnv.h"      // la semilla, el primo y la mezcla, en UN sitio
#include "util/fs_utils.h" // fs::get_executable_path()
#include "util/name_pool.h"
#include "vx/compiler.h"
#include "vx/module/vxi_format.h" // vxi_fnv1a, VXI_FORMAT_VERSION
#include "vx/source_hash.h"       // la identidad de un fuente son sus tokens

#include <filesystem>
#include <system_error>

namespace vx {

namespace {

/**
 * @brief Calcula la huella del compilador; ver @ref compiler_fingerprint.
 * @return La huella.
 */
uint64_t compute_compiler_fingerprint() {
    // Valvula para depurar: al recompilar el compilador (p.ej. para anadir una
    // traza) la huella cambia, los artefactos se invalidan y se regeneran
    // limpios -- con lo que el escenario que se queria observar desaparece
    // justo al ir a mirarlo.  Con VX_CACHE_FINGERPRINT la huella queda fija en
    // el valor que se le pase, asi que se puede instrumentar sin perder la
    // cache que reproduce el fallo.
    const std::string &fixed = util::flag_text(util::FlagId::CacheFingerprint);
    if (!fixed.empty()) return vxi_fnv1a(fixed);
    std::error_code ec;
    const std::string self = ::fs::get_executable_path();
    uint64_t h = util::kFnvOffset;
    if (!self.empty()) {
        const std::filesystem::path p(self);
        const auto sz = std::filesystem::file_size(p, ec);
        if (!ec) h = util::fnv_mix(h, static_cast<uint64_t>(sz));
        const auto tm = std::filesystem::last_write_time(p, ec);
        if (!ec)
            h = util::fnv_mix(
                h, static_cast<uint64_t>(tm.time_since_epoch().count()));
    }
    // Respaldo por si no se pudo mirar el ejecutable: al menos el formato de
    // interfaz, que ya cambia con las modificaciones de fondo.
    return util::fnv_mix(h, VXI_FORMAT_VERSION);
}

} // namespace

uint64_t compiler_fingerprint() {
    static const uint64_t fp = compute_compiler_fingerprint();
    return fp;
}

ModuleCacheKey module_cache_key(const ModuleCacheKeyInput &in) {
    const CompileOptions &opts = *in.opts;
    ModuleCacheKey key;
    /* La identidad del modulo es lo que DICE, no como esta escrito: se keyea
     * por sus tokens y no por los bytes del fichero.  Anadir un comentario o
     * reindentar obligaba a recompilar un modulo identico -- medido en el
     * banco, tocar un comentario salia mas caro que cambiar el cuerpo de una
     * funcion.
     *
     * Con informacion de depuracion SI cuenta la linea de cada token: el
     * artefacto lleva dentro donde esta cada cosa, y un comentario metido en
     * medio las desplaza todas. */
    uint64_t h = hash_de_tokens(*in.source, opts.emit_debug);
    // El COMPILADOR forma parte de lo que produjo el artefacto: un mismo fuente
    // compilado por dos versiones distintas da IR distinto.  Sin esto, arreglar
    // un bug de codegen no invalidaba nada y se seguian sirviendo artefactos
    // generados por la version anterior -- el fallo parecia seguir vivo, o
    // revivia al repoblarse la cache, y no habia forma de distinguirlo de un
    // bug real.
    h = util::hash_combine(h, compiler_fingerprint());
    /* Y los mandos del entorno que cambian lo EMITIDO: el `.vxir` de un modulo
     * compilado con un pase apagado no vale para uno compilado con el puesto.
     * Vale cero cuando no hay ninguno -- el caso normal --, asi que no
     * invalida nada de lo ya guardado. */
    if (const uint64_t env_fp = util::emitted_fingerprint())
        h = util::hash_combine(h, env_fp);
    // El modo de instrumentacion (y cualquier opcion futura que afecte a la
    // emision): sin esto, builds con cache de un modo distinto producian
    // `.vel` con relocations sin resolver -> SEGV silente en runtime.
    if (!opts.instrument_mode.empty() && opts.instrument_mode != "none")
        h = util::hash_combine(h, vxi_fnv1a(opts.instrument_mode));
    /* Y SI HABIA maquina de compilacion, que es lo que decide si el cuerpo de
     * un bloque `asm` generado por una funcion comptime sale ESCRITO o VACIO.
     * Son dos artefactos distintos del mismo fuente.
     *
     * Sin esto, la segunda pasada -- que existe precisamente para rehacer el
     * modulo con la maquina ya cargada -- se servia del que guardo la primera,
     * que se compilo SIN ella, y heredaba el cuerpo vacio.  Solo el modulo raiz
     * se rehacia; los demas entraban al binario con el bloque en blanco y el
     * programa daba otro valor, sin error. */
    if (in.comptime_machine)
        h = util::hash_combine(h, vxi_fnv1a("comptime-machine"));
    /* Y los `@Hook` del raiz, por la misma razon y con mas motivo: tejen
     * LLAMADAS en el IR de este modulo, cuyo fuente no ha cambiado por ello.
     * Sin esto, compilar un programa con `@Hook(enter, "std.*")` dejaba la
     * stdlib guardada CON el gancho dentro, y el siguiente programa que la
     * usara moria al enlazar con "simbolo no resuelto".
     *
     * No basta con meterlo en la clave del CAS: los artefactos que viven en la
     * cache por ruta (`math.vxir`) se reutilizan por esta clave, no por
     * aquella. */
    if (in.hooks_source_fp != 0) h = util::hash_combine(h, in.hooks_source_fp);
    // El IR de una dependencia depende del MODO de POO con que se baja: en
    // Full/VM las clases usan GC (newobj + gc_deref); en AOT (`native_poo`)
    // stack/heap nativo (calloc + dtor RAII).  Son IR distintos para el mismo
    // fuente, y sin esto un `.vxir` de `-m vm` se reusaria en `-m aot`.
    if (opts.native_poo) h = util::hash_combine(h, 0xA07A07A07A07A07AULL);
    key.source_hash = h;

    // Un modulo SOLO es especifico del objetivo si usa @Target (que descarta
    // declaraciones distintas segun os/arch al parsear).  Para NO recompilar al
    // alternar de objetivo, su cache se separa por FICHERO (no por clave, que
    // sobrescribiria el unico `.vxir` y forzaria recompilar en cada cambio):
    // asi persisten `mod.<os>-<arch>.vxir` para cada objetivo y alternar
    // PE<->ELF es acierto.  Los modulos SIN @Target comparten el fichero unico.
    const std::string &os = *in.target_os;
    const std::string &arch = *in.target_arch;
    std::string suffix;
    if ((!os.empty() || !arch.empty()) &&
        in.source->find("@Target") != std::string::npos)
        suffix = "." + os + "-" + arch;
    /* Compilar con la maquina de compilacion cargada da un resultado DISTINTO
     * al de compilar sin ella: sin la maquina, una funcion comptime no se puede
     * ejecutar y su valor sale vacio.  Los dos resultados no pueden compartir
     * artefacto, o la pasada buena se encuentra el de la provisional y lo da
     * por valido.
     *
     * Se marca la pasada CON maquina y no la de sin ella, que es la unica que
     * existe cuando no hay codigo de compilacion de por medio: asi el caso
     * normal conserva sus artefactos de siempre. */
    if (!util::flag_text(util::FlagId::McPrebuilt).empty()) suffix += ".mc";
    /* Internado una vez por modulo: es la identidad con que se piden sus rutas
     * de cache. */
    key.target_suffix = util::InternedName::intern(suffix);
    return key;
}

uint64_t module_content_key(uint64_t source_hash,
                            const DepAbiHashes &dep_hashes,
                            const std::string &target_suffix,
                            uint64_t config_fp) {
    uint64_t h = util::fnv_mix(util::kFnvOffset,
                               0x5641434B4559ull); // dominio "CAS module key"
    // Build del compilador: no reusar lo de otra version.
    h = util::fnv_mix(h, vxi_compiler_version_hash());
    h = util::fnv_mix(h, source_hash); // contenido (y todo lo de arriba)
    // Configuracion que afecta al IR antes de optimizar (asm_target_bits,
    // native_poo, excepciones, instrumentacion): cierra el hueco de un `asm`
    // que baja distinto en 32 y en 64 bits con el almacen compartido.
    h = util::fnv_mix(h, config_fp);
    /* Si la maquina de compilacion estaba cargada o no.  Es un eje REAL del
     * resultado: sin ella, una funcion comptime no se puede ejecutar y su
     * valor sale vacio, asi que el modulo compilado en esa pasada es
     * PROVISIONAL.  Sin distinguirlo, la pasada buena reutilizaba el modulo
     * provisional de la anterior y el arreglo no llegaba nunca. */
    h = util::fnv_mix(
        h, util::flag_text(util::FlagId::McPrebuilt).empty() ? 0ull : 1ull);
    if (!target_suffix.empty())
        h = util::fnv_mix(h, vxi_fnv1a(target_suffix)); // @Target
    for (uint64_t d : dep_hashes)
        h = util::fnv_mix(h, d);
    return h;
}

} // namespace vx
