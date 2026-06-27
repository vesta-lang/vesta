/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <csignal>
#include <openssl/sha.h>

#include "cxxopts.hpp"

#include "cli/cli.h"
#include "cli/vsh.h"
#include "analyze/bigo.h" // Subsistema de coste: modo --analyze (Big-O)
#include "ir/ir_emitter.h"
#include "ir/ssa_ir_serialize.h" // Phase AOT: parse_ir_section (round-trip del @ir)
#include "aot/aot_analyze.h" // Phase AOT.1: analisis de compatibilidad nativa
#include "aot/aot_lower.h" // Phase AOT.2: re-bajada RAW_ALLOC/FREE/PANIC -> CALL
#include "aot/object_writer.h" // Phase AOT.4: emisor PE/ELF (ObjectWriter)
#include "aot/aot_native.h"    // Phase AOT.3 Paso 2: _start arch-portable
#include "aot/linker.h"        // Phase AOT.5: linker propio (enlaza .o)
#include "jit/vreg_pipeline.h" // Phase AOT.3 Paso 2: vreg_compile_native (HOST_LEAF)
#include "jit/vec_isa.h" // ancho SIMD del target (--float-isa)
#include "jit/auto_jit.h"
#include "jit/keystone_asm_backend.h" // Phase AS inc.4b: registrar backend asm
#include "jit/inline_asm_trampoline.h" // Phase AS inc.6: helper runner inline-asm
#include "runtime/profile.h"           // Sprint D.6 (2026-06-03)
#include "pkg/cli.h"
#include "runtime/proceso_runtime.h"
#include "cli/runtime_api_commands.h"
#include "util/assembler_multiprocess.h"
#include "vex/compiler.h"
// Forward-decl (evita incluir vex/parser.h, que arrastra cabeceras Windows que
// desbalancean el push/pop_macro(VOID) de ssa_ir.h).  @Target target-aware AOT.
namespace vex {
void set_aot_condcomp_target(const std::string &os,
                             const std::string &arch) noexcept;
}
#include "vex/comptime_vm.h"    /* Phase MC.4 probe del ComptimeRuntime */
#include "vex/project_cache.h"  /* Phase M5.B project-level cache */
#include "vex/velb_signature.h" /* Phase M.L28: firmas digitales */
#include "util/sqlite_singleton.h"
#include "util/fs_utils.h"
#include "runtime/manager_runtime.h"
#include "gc/gc_heap.h"
#include "loader/loader.h"
#include "profiler/timer.h"
#include "distrib/dist_runtime.h"
#include "distrib/dist_debug.h"
#include "distrib/node_registry.h"
#include "debug/debugger.h"
#include "debug/auth.h"
#include "install/install.h"

#include <filesystem>
#include <map>           // Phase MC.16: per-macro manifest map
#include <unordered_map> // Phase AOT.3 2b-ii: layout name->offset
#include <set>           // Phase MC.16: manifest diff seen-set
#include <cctype>        // Phase MC.16: isalnum en find_macro_ranges_with_names
#include <openssl/rand.h>

#ifdef VESTA_HAS_PREPROCESSOR
#include "preprocessor/preprocessor.h"
#endif

// flag global para el modo --dist-server; SIGINT lo pone a false
static std::atomic<bool> g_server_running{true};
static void on_dist_sigint(int) {
    g_server_running.store(false);
}

/**
 * @brief Construye la NodeAuthConfig a partir de los flags de autenticacion.
 *
 * Calcula SHA-256 del token en texto plano si se proporciono --dist-token.
 * Copia las rutas TLS si se activo --dist-tls.
 *
 * @param token    Token en texto plano (puede estar vacio).
 * @param use_tls  true si se activo --dist-tls.
 * @param cert     Ruta al certificado PEM del cliente.
 * @param key      Ruta a la clave privada PEM del cliente.
 * @param ca       Ruta al CA bundle PEM para verificar pares.
 * @return NodeAuthConfig relleno.
 */
static distrib::NodeAuthConfig
build_node_auth(const std::string &token, bool use_tls, const std::string &cert,
                const std::string &key, const std::string &ca) {
    distrib::NodeAuthConfig auth{};
    if (!token.empty()) {
        auth.use_token = true;
        SHA256(reinterpret_cast<const unsigned char *>(token.c_str()),
               token.size(), auth.token_hash);
    }
    if (use_tls) {
        auth.use_tls = true;
        std::snprintf(auth.cert_path, sizeof(auth.cert_path), "%s",
                      cert.c_str());
        std::snprintf(auth.key_path, sizeof(auth.key_path), "%s", key.c_str());
        std::snprintf(auth.ca_path, sizeof(auth.ca_path), "%s", ca.c_str());
    }
    return auth;
}

/**
 * @brief Aplica la configuracion distribuida a una instancia VM.
 *
 * Reemplaza el dist_runtime minimo creado en VM::VM() por uno completamente
 * configurado segun los flags --dist-* de la linea de comandos.
 * Si --dist-port > 0 o --dist-discover esta activo, llama a start() para
 * abrir el servidor VDP y/o el hilo de descubrimiento UDP.
 * Registra cualquier nodo estatico indicado con --dist-add-node (formato
 * IP:PUERTO).
 *
 * @param vm     Instancia VM sobre la que se aplica la configuracion.
 * @param result Resultado del parseo de cxxopts con todos los flags.
 */
static void apply_dist_config(runtime::VM *vm,
                              const cxxopts::ParseResult &result) {
    const std::string token = result["dist-token"].as<std::string>();
    const bool use_tls = result.count("dist-tls") > 0;
    const std::string cert = result["dist-cert"].as<std::string>();
    const std::string key = result["dist-key"].as<std::string>();
    const std::string ca = result["dist-ca"].as<std::string>();

    // construir configuracion del DistRuntime
    distrib::DistRuntimeConfig cfg{};
    cfg.local_node_id = result["dist-node-id"].as<uint64_t>();
    cfg.vdp_listen_port = result["dist-port"].as<uint16_t>();
    cfg.discover_port = result["dist-discover-port"].as<uint16_t>();
    cfg.enable_discovery = result.count("dist-discover") > 0;

    std::string name = result["dist-name"].as<std::string>();
    if (!name.empty())
        std::snprintf(cfg.local_node_name, sizeof(cfg.local_node_name), "%s",
                      name.c_str());
    else
        std::snprintf(cfg.local_node_name, sizeof(cfg.local_node_name),
                      "vm-%llu", static_cast<unsigned long long>(vm->id));

    cfg.server_auth = build_node_auth(token, use_tls, cert, key, ca);

    // reemplazar el dist_runtime minimal por uno completamente configurado
    vm->dist_runtime = std::make_unique<distrib::DistRuntime>(*vm, cfg);

    // arrancar el servidor VDP y/o el descubrimiento si alguno esta habilitado
    if (cfg.vdp_listen_port > 0 || cfg.enable_discovery) {
        if (vm->dist_runtime->start()) {
            std::string msg = "[dist] Servidor VDP iniciado";
            if (cfg.vdp_listen_port)
                msg += " en puerto " + std::to_string(cfg.vdp_listen_port);
            if (cfg.enable_discovery)
                msg += " con descubrimiento UDP (puerto " +
                       std::to_string(cfg.discover_port) + ")";
            vesta::scout() << msg << "\n";
        } else {
            std::cerr << "[dist] Error al iniciar el servidor VDP\n";
        }
    }

    // registrar nodos estaticos proporcionados con --dist-add-node IP:PUERTO
    if (result.count("dist-add-node")) {
        distrib::NodeAuthConfig node_auth =
            build_node_auth(token, use_tls, cert, key, ca);
        for (auto &spec :
             result["dist-add-node"].as<std::vector<std::string>>()) {
            auto colon = spec.rfind(':');
            if (colon == std::string::npos) {
                std::cerr << "[dist] Formato invalido (esperado IP:PUERTO): "
                          << spec << "\n";
                continue;
            }
            std::string node_ip = spec.substr(0, colon);
            uint16_t node_port = 0;
            try {
                node_port =
                    static_cast<uint16_t>(std::stoul(spec.substr(colon + 1)));
            } catch (...) {
                std::cerr << "[dist] Puerto invalido en: " << spec << "\n";
                continue;
            }

            uint32_t idx = vm->dist_runtime->add_node(
                node_ip.c_str(), node_port, node_auth, node_ip.c_str());
            vesta::scout() << "[dist] Nodo registrado: " << node_ip << ":"
                           << node_port << " (idx=" << idx << ")\n";
        }
    }
}

/* forzar registro de virtual fns runtime (callbacks Vex->C). */
extern "C" void runtime_ensure_vex_callback_registered(void);

int main(int argc, char *argv[]) {
#if defined(WIN32) || defined(_WIN32) ||                                       \
    defined(__WIN32) && !defined(__CYGWIN__)
    asm_multi_process::run_and_capture("chcp 65001");
#endif
    runtime_ensure_vex_callback_registered();
    // Phase AS inc.4b: registrar el backend de ensamblado Keystone para que el
    // frontend Vex valide la sintaxis del inline asm en compile-time.
    jit::register_keystone_asm_backend();
    // Phase AS inc.6: registrar el helper nativo vrt:inline_asm_exec que el
    // interprete (modo -m vm, sin JIT) invoca por cada bloque inline-asm.
    jit::register_inline_asm_runner();

    // ------------------------------------------------------------------
    // Subcomando especial: @c vm pkg <subcmd> ...
    // Despachamos ANTES del parser cxxopts porque el package manager
    // tiene su propio sistema de flags y subcomandos.
    // ------------------------------------------------------------------
    if (argc >= 2 && std::string(argv[1]) == "pkg") {
        // Pasamos argv shifted: argv[0]="vm", argv[1]="pkg",
        // queremos que pkg::cli reciba argv[1]=primer subcomando.
        // Construimos un argc/argv nuevo skipeando el "pkg" literal.
        std::vector<char *> sub;
        sub.push_back(argv[0]);
        for (int i = 2; i < argc; ++i)
            sub.push_back(argv[i]);
        return pkg::cli::run(static_cast<int>(sub.size()), sub.data());
    }

    /* registrar el hook auto-JIT en el runtime.
     * El runtime (vesta_rt) tiene un function pointer nulo por defecto;
     * el main aqui lo apunta al JIT trigger.  Si el env var
     * VESTA_JIT_THRESHOLD esta presente, el trigger compila funciones
     * hot a codigo nativo.  Sin env var, el
     * threshold queda en UINT32_MAX y el hook es no-op aunque se llame. */
    runtime::g_callvirt_post_hook = &jit::maybe_compile_method;

    cxxopts::Options options("VMProject", "Virtual Machine Example");

    options.add_options()("h,help", "Mostrar ayuda")(
        "o,output", "Archivo de salida (sin extensión o completo)",
        cxxopts::value<std::string>())(
        "driver", "Compilar un directorio completo en paralelo",
        cxxopts::value<std::string>())(
        "worker", "Compilar un único archivo (modo interno)",
        cxxopts::value<std::string>())(
        "j,threads", "Número de hilos para el driver",
        cxxopts::value<int>()->default_value("0"))("v,version",
                                                   "Mostrar versión")(
        "m,mode",
        "Modo de ejecucion/compilacion: vm (interprete) | jit (JIT en "
        "caliente) | aot (compilacion nativa standalone, requiere --vex)",
        cxxopts::value<std::string>()->default_value("vm"))(
        "list-arch", "Imprimir arquitecturas soportadas")(
        "asm-file", "Archivo ASM a ensamblar", cxxopts::value<std::string>())(
        "disasm-file", "Archivo binario a desensamblar",
        cxxopts::value<std::string>())(
        "arch", "Arquitectura para ensamblar/desensamblar",
        cxxopts::value<std::string>())(
        "save-output", "Guardar código ensamblado/desensamblado en archivo")(
        "output-prefix", "Prefijo/nombre base para archivos de salida",
        cxxopts::value<std::string>()->default_value("out"))(
        "run", "Ejecutar un archivo .velb en la VM",
        cxxopts::value<std::string>())("build",
                                       "Compilar un archivo .vel a .velb",
                                       cxxopts::value<std::string>())(
        "schedulers", "Número de schedulers para el comando run",
        cxxopts::value<size_t>()->default_value("1"))(
        "install",
        "Ejecutar el instalador interactivo (o con flags adicionales)")(
        "uninstall", "Desinstalar Vesta usando el manifest")(
        "repair", "Reparar la instalacion existente")(
        "silent", "Modo silencioso para install/uninstall")(
        "per-user", "Forzar instalacion per-user")(
        "system-wide", "Forzar instalacion system-wide")(
        "prefix", "Directorio destino", cxxopts::value<std::string>())(
        "manifest", "Ruta a install_manifest.json",
        cxxopts::value<std::string>())(
        "stats",
        "Mostrar estadísticas de ejecución al finalizar (tiempo, MIPS)")
        // ---- opciones de JIT ----
        ("jit-threshold",
         "Umbral de invocaciones de un metodo para disparar JIT (default: "
         "UINT32_MAX = JIT off; sugerido para test: 1)",
         cxxopts::value<uint32_t>())(
            "jit-warn", "Imprimir warnings cada vez que el Selector encuentra "
                        "una IR op no soportada (dedup por op+linea)")(
            "jit-stats", "Imprimir snapshot final de counters del JIT: "
                         "compiled/unsupported/no_ir + threshold")(
            "jit-disasm", "Volcar hex bytes + disasm (Capstone) de cada "
                          "funcion JIT-compilada a stderr")
        // ---- opciones de profiling (D.6 PGO) ----
        ("profile",
         "Generar @c .vprof con branch/type/alloc counters al exit (PGO para "
         "C2). Path opcional; default: 'program.vprof'.",
         cxxopts::value<std::string>()->implicit_value("program.vprof"))
        // ---- opciones de runtime distribuido ----
        ("dist-port",
         "Puerto VDP del servidor distribuido (0 = sin servidor TCP)",
         cxxopts::value<uint16_t>()->default_value("0"))(
            "dist-discover", "Activar descubrimiento UDP de nodos en la LAN")(
            "dist-discover-port", "Puerto UDP para descubrimiento de nodos",
            cxxopts::value<uint16_t>()->default_value("7790"))(
            "dist-name", "Nombre del nodo local (cadena identificativa)",
            cxxopts::value<std::string>()->default_value(""))(
            "dist-node-id",
            "ID de 64 bits del nodo (0 = generar automaticamente)",
            cxxopts::value<uint64_t>()->default_value("0"))(
            "dist-add-node",
            "Nodo estatico a registrar y conectar (formato IP:PUERTO, "
            "repetible)",
            cxxopts::value<std::vector<std::string>>())(
            "dist-token",
            "Token de autenticacion en texto plano (se almacena como SHA-256)",
            cxxopts::value<std::string>()->default_value(""))(
            "dist-tls", "Usar TLS en las conexiones VDP salientes y entrantes")(
            "dist-cert", "Ruta al certificado TLS del nodo local (PEM)",
            cxxopts::value<std::string>()->default_value(""))(
            "dist-key", "Ruta a la clave privada TLS del nodo local (PEM)",
            cxxopts::value<std::string>()->default_value(""))(
            "dist-ca", "Ruta al CA bundle TLS para verificar pares (PEM)",
            cxxopts::value<std::string>()->default_value(""))(
            "dist-server", "Modo servidor distribuido puro: espera conexiones "
                           "VDP sin ejecutar bytecode")(
            "dist-debug", "Activar trazas de depuracion del subsistema "
                          "distribuido (RSPAWN, HALT, FUTURE_FULFILL)")(
            "script", "Ejecutar un fichero VestaShell (.vsh) y salir",
            cxxopts::value<std::string>())(
            "interprete",
            "Abrir el interprete interactivo VestaShell (REPL .vsh)")(
            "ir-file",
            "Compilar archivo .ir (SSA IR) a .vel y opcionalmente a .velb",
            cxxopts::value<std::string>())(
            "ir-opt",
            "Nivel de optimizacion IR: 0=O0, 1=O1, 2=O2, 3=O3 (defecto: 1)",
            cxxopts::value<int>()->default_value("1"))(
            "ir-emit-only", "Solo emitir el texto .vel; no compilar a .velb")(
            "vex", "Compilar archivo .vex (lenguaje Vex) a .velb",
            cxxopts::value<std::string>())(
            "vex-emit-only",
            "Solo emitir el .vel intermedio del .vex; no compilar a .velb")(
            "vex-emit-ir",
            "Emitir el SSA IR del .vex (pre y post optimizacion) en "
            "<output>.ir; util para debug del frontend")(
            "analyze",
            "Subsistema de coste: analiza la complejidad algoritmica (Big-O) "
            "estatica de cada funcion de un .vex y la imprime; valida el "
            "contrato @complexity si esta presente. Cero impacto en codegen.",
            cxxopts::value<std::string>())(
            "analyze-json",
            "Con --analyze: emite el coste por funcion como JSON (para "
            "consumir desde un renderer de diagramas) en vez de texto legible.")
        // Phase AOT: con -m aot, target de compilacion nativa.
        ("target",
         "Tier de compilacion nativa AOT (-m aot): bare|embed|full (default "
         "bare).",
         cxxopts::value<std::string>()->default_value("bare"))(
            "freestanding",
            "AOT bare sin libc (kernels/bootloaders): RAW_ALLOC/FREE/PANIC "
            "requieren hooks @AllocatorOverride/@PanicHandler.")(
            "no-exceptions",
            "AOT (-m aot): DESACTIVA el mecanismo de excepciones nativo "
            "(setjmp/longjmp).  Un try/catch/throw da error.  Para "
            "kernels/freestanding sin runtime de excepciones.")(
            "no-io",
            "AOT (-m aot): NO auto-incluye el runtime de I/O "
            "(stdlib/vex/vex_io.vex).  El usuario aporta __vex_write y los "
            "__vex_print_* (p.ej. enlazar vesta_io_bare.o, o freestanding).")(
            "no-mem",
            "AOT (-m aot): NO auto-incluye el slab allocator "
            "(stdlib/vex/vex_mem.vex).  El allocator usa libc malloc/free (o el "
            "@AllocatorOverride del usuario).")(
            "format",
            "Formato del ejecutable AOT (-m aot): pe|elf (default: PE en "
            "Windows, ELF en el resto).",
            cxxopts::value<std::string>())(
            "no-pie", "AOT: refs a datos absolutas (mov reg,imm64; requiere "
                      "base de imagen fija) en vez de RIP-relativas (default, "
                      "position-independent), analogo a gcc/clang -no-pie.")(
            "emit",
            "AOT: tipo de artefacto: exe (ejecutable standalone, default) | "
            "obj (objeto relocatable linkable: ELF .o o COFF .obj segun "
            "--format) | shared (libreria compartida ELF .so que exporta sus "
            "funciones; dlopen/dlsym) | bin (binario plano sin cabecera: "
            "bootloader/ROM/firmware, entry en offset 0).",
            cxxopts::value<std::string>())(
            "bin-base",
            "AOT --emit bin: base de carga del binario plano (hex, e.g. "
            "0x7C00); afecta solo a refs absolutas (--no-pie). Default 0.",
            cxxopts::value<std::string>())(
            "aot-arch",
            "AOT: arquitectura objetivo: x86-64 (default) | x86-32 (modo "
            "protegido, kernels: 8 GP eax-edi, regparm(3), subset entero de "
            "32-bit).",
            cxxopts::value<std::string>()->default_value("x86-64"))(
            "float-isa",
            "AOT: backend de punto flotante / ancho SIMD del vectorizador: "
            "sse2 (default, 128b, corre en CUALQUIER x86-64) | x87 (legacy) | "
            "avx (AVX2 256b, requiere AVX2 en la CPU) | avx512f (512b, requiere "
            "AVX-512) | auto (multiversion: emite las 3 variantes y elige la "
            "optima en runtime por CPUID; lo mejor para distribuir un solo "
            "binario). Nota: un binario avx/avx512f FIJO da SIGILL en una CPU "
            "sin ese soporte; usa auto para portabilidad.",
            cxxopts::value<std::string>()->default_value("sse2"))(
            "vex-base",
            "VA base address para el modulo (hex, e.g. 0x10000000). Usado para "
            "plugins cargados via loadmodule, evita solapamiento con el caller "
            "(default 0x0).",
            cxxopts::value<std::string>()->default_value("0x0"))
        // Phase M.sandbox: restringe las capabilities del modulo
        // principal al subset listado.  Default vacio = ALL granted
        // (zero overhead, backward compat).  Sintaxis: 'fs:read,net,
        // ffi:call=kernel32.dll;user32.dll' etc.  Ver include/loader/
        // sandbox.h para tabla completa de caps + sintaxis.
        ("vex-caps",
         "Phase M.sandbox: restringe caps del modulo principal. Sintaxis: "
         "'fs:read,net,ffi:call=kernel32.dll;user32.dll'. Vacio = ALL granted "
         "(default). 'none' = sandbox total.",
         cxxopts::value<std::string>()->default_value(""))
        // Diagramas para debug y traceo del pipeline Vex.  Tres formatos
        // seleccionables via --diagram-format:
        //   mermaid (default): escribe .mmd con bloque ```mermaid```;
        //                      listo para VS Code / GitHub / mermaid.live.
        //   graphviz:          escribe .dot con `digraph G { ... }`;
        //                      listo para `dot -Tsvg foo.dot -o foo.svg`.
        //   html:              escribe .html interactivo AUTOCONTENIDO
        //                      (CSS+JS embebidos, sin CDN): pan/zoom, panel
        //                      de detalle por nodo, busqueda, filtros de
        //                      aristas.  Se abre directo en el navegador.
        //   both:              mermaid + graphviz.   all: los tres.
        // El contenido es paralelo entre formatos: misma topologia, misma
        // info por nodo (Graphviz/HTML llevan extra via tooltips/detalle).
        // --diagram-all genera las 4 vistas (AST, IR pre, IR post, VEL)
        // en el formato escogido.
        ("diagram-vex",
         "Generar diagrama del AST Vex post type-check (.ast.<ext>)")(
            "diagram-ir",
            "Generar diagrama del SSA IR pre-optimizacion (.ir.pre.<ext>)")(
            "diagram-ir-opt",
            "Generar diagrama del SSA IR post-optimizacion (.ir.post.<ext>)")(
            "diagram-vel",
            "Generar diagrama del bytecode .vel final (.vel.<ext>)")(
            "diagram-all", "Generar las 4 vistas (vex, ir pre, ir post, vel) "
                           "con sufijos correspondientes")(
            "diagram-format",
            "Formato: mermaid | graphviz | html | both | all (default: "
            "mermaid). html produce paginas interactivas autocontenidas "
            "(.html); both=mermaid+graphviz; all=los tres.",
            cxxopts::value<std::string>()->default_value("mermaid"))(
            "diagram-cost",
            "Anotar cada nodo-funcion de los diagramas IR (pre y post) con su "
            "coste Big-O (parcial + total) calculado por analyze::bigo. Aplica "
            "a --diagram-ir / --diagram-ir-opt / --diagram-all.")(
            "gc-debug",
            "Activar trazas de debug del Garbage Collector a stderr (minor_gc, "
            "major_gc, sweep, release_handle, evacuate). Util para "
            "diagnosticar use-after-free o objetos colectados prematuramente. "
            "Alt: env VESTA_GC_DEBUG=1.")(
            "gc-debug-buffered",
            "Activar modo BUFFERED del GC debug: ~100x mas rapido (buffer "
            "thread-local de 64KB) pero pierde las ultimas trazas en crash. "
            "Implica --gc-debug. Alt: env VESTA_GC_DEBUG_BUFFERED=1.")(
            "debug-port",
            "Activar el servidor de depuracion TCP en el puerto N (default "
            "9229 si N=0). Soporta multiples clientes simultaneos y JSON via "
            "length-prefix framing. El servidor permanece disponible mientras "
            "la VM ejecuta; los clientes (e.g. tools/dbg_client.vsh) se "
            "conectan via tcp_connect, envian comandos JSON y reciben eventos "
            "asincronos (break/exit/exception/spawned). Sin este flag, el "
            "debugger NO se instancia y el coste runtime es exactamente cero. "
            "Cuando esta presente, el proceso main arranca PAUSADO en su "
            "primera instruccion para dar tiempo al cliente a conectarse y "
            "poner breakpoints; el cliente debe enviar 'continue 0' para "
            "arrancar la ejecucion.",
            cxxopts::value<uint16_t>()->default_value("0"))(
            "server-mode",
            "Iniciar la VM como un servidor persistente de depuracion. Implica "
            "--debug-port (default 9229). No requiere --run: la VM se queda "
            "viva esperando comandos del debugger para cargar/ejecutar/matar "
            "procesos remotamente. Compatible con --schedulers, --dist-port, "
            "etc. Termina con SIGINT (Ctrl+C) o con el comando "
            "'server_shutdown' del cliente. Comandos disponibles: load_velb, "
            "load_velb_bytes, kill_proc, server_info, server_shutdown, "
            "auth_login/logout/whoami, "
            "auth_create_user/delete_user/list_users/change_pass, "
            "fs_read/write/list/stat/delete/mkdir/rename.")(
            "server-port",
            "Puerto del servidor persistente (sinonimo de --debug-port en modo "
            "--server-mode). Default 9229.",
            cxxopts::value<uint16_t>()->default_value("0"))(
            "server-root",
            "Directorio raiz del sandbox de filesystem en modo --server-mode. "
            "Todas las rutas usadas por fs_* y load_velb se resuelven contra "
            "este directorio; cualquier intento de salir (con '..' o rutas "
            "absolutas) se rechaza. Sin este flag NO hay sandbox (modo "
            "back-compat: cualquier ruta absoluta del servidor es accesible). "
            "Recomendado en despliegues compartidos.",
            cxxopts::value<std::string>()->default_value(""))(
            "auth-db",
            "Ruta al fichero SQLite con la tabla vm_users (usuarios, hashes "
            "PBKDF2 y roles) en modo --server-mode. Sin este flag la "
            "autenticacion esta desactivada y cualquier cliente puede invocar "
            "cualquier comando (modo desarrollo).",
            cxxopts::value<std::string>()->default_value(""))(
            "admin-user",
            "Nombre del usuario admin a crear automaticamente cuando la base "
            "de datos --auth-db esta vacia. Default 'admin'.",
            cxxopts::value<std::string>()->default_value("admin"))(
            "admin-password",
            "Contrasenya del usuario admin a crear automaticamente. Si se "
            "omite y --auth-db apunta a una BD vacia, se genera una "
            "contrasenya aleatoria de 24 caracteres y se imprime en stdout UNA "
            "UNICA VEZ.",
            cxxopts::value<std::string>()->default_value(""))(
            "no-repl",
            "Desactivar el REPL interactivo local en modo --server-mode (modo "
            "headless). Sin este flag, el operador local mantiene un prompt "
            "vesta> que comparte el mismo runtime con los clientes remotos; "
            "con el, el proceso solo escucha el socket TCP.")(
            "vex-debug",
            "Emitir comentarios `// @line N` en el .vel intermedio del "
            "compilador Vex y, cuando se integre la pipeline completa de debug "
            "section (Phase 2), embeber la tabla bytecode_offset -> (file, "
            "line) en el .velb final.  Sin este flag, el .vel/.velb no "
            "contienen info de debug -> el ejecutable es mas pequeno y el "
            "frontend NO genera datos extra.  Con el flag, el cliente del "
            "debugger puede setear breakpoints por linea Vex (`b file.vex:42`) "
            "en lugar de solo por addr.")(
            "port",
            "Transpilar el IR a codigo fuente del lenguaje destino y escribir "
            "a <output>.<ext> (e.g. .c).  Valores actuales: 'c'.  Futuro: "
            "'java', 'js', 'rust'.  Con --port=c se genera codigo C99 portable "
            "listo para compilar con gcc/clang -O3 -std=c11 SIN dependencias "
            "de VestaVM (a menos que --port-gc=vesta).  Implica --vex (se "
            "aplica al pipeline Vex post-optimizacion).",
            cxxopts::value<std::string>())(
            "port-gc",
            "Modelo de memoria del codigo portado: none|vesta|boehm.  none "
            "(default): malloc/free + sin GC.  Las IR ops de objetos GC "
            "(NEWOBJ/strings) emiten stub.  vesta: enlazar contra "
            "vesta_rt.lib.  boehm: enlazar contra libgc (placeholder).",
            cxxopts::value<std::string>()->default_value("none"))(
            "port-exc",
            "Manejo de excepciones del codigo portado: none|setjmp|returncode. "
            " none: throw -> abort.  setjmp (default): longjmp portable.  "
            "returncode: codigo de retorno + propagacion explicita (no impl "
            "v1).",
            cxxopts::value<std::string>()->default_value("setjmp"))(
            "port-types",
            "Estilo de tipos del codigo portado: stdint (default) usa "
            "int8_t/uint64_t/etc de stdint.h.  builtin usa char/int/long del C "
            "clasico (menos portable).",
            cxxopts::value<std::string>()->default_value("stdint"))(
            "port-strings",
            "Modelo de strings: raw (const char* solo literales) | managed "
            "(default: VexString con tracking per-thread).",
            cxxopts::value<std::string>()->default_value("managed"))(
            "port-freestanding",
            "Generar C freestanding (sin includes automaticos de "
            "stdio/stdlib/math).  El usuario debe proveer vex_throw + "
            "vex_panic_with_str.  Para bootloaders, kernels, firmware.")(
            "port-stdlib-dir",
            "Path al directorio stdlib/port/c con los snippets .v.c "
            "(autodetect si vacio).",
            cxxopts::value<std::string>()->default_value(""))(
            "port-arch",
            "Target CPU: '' (portable, default) | native | x86-64-v2 | "
            "x86-64-v3 | x86-64-v4.  Emite #pragma GCC target con flags "
            "agresivas (AVX2/FMA/BMI2 segun nivel).",
            cxxopts::value<std::string>()->default_value(""))(
            "port-no-aggressive",
            "Desactivar atributos agresivos (const/cold/restrict/always_inline "
            "+ __builtin_expect/unreachable).  Default: activado.")(
            "instrument",
            "Instrumentacion en el IR Vex (heredada por bytecode VM, JIT, port "
            "C, port futuros): none (default) | trace (calls a "
            "vex_trace:enter/exit por funcion) | profile (timing per-funcion).",
            cxxopts::value<std::string>()->default_value("none"))
#ifdef VESTA_HAS_PREPROCESSOR
            ("preprocess-only",
             "Solo preprocesar un .vel y mostrar/guardar el resultado (debug)",
             cxxopts::value<std::string>())
#endif
                ("keep-labels",
                 "Mantener los nombres de label en el .velb (por defecto se "
                 "eliminan: el loader no los usa y reducen ~9% el tamano)")(
                    "emit-map", "Generar archivo .velb-map con info de "
                                "simbolos y secciones (debug). Off por "
                                "defecto: cuesta ~60% del tiempo del linker")
        // Phase M.L28: firmas digitales del .velb.
        ("sign-velb",
         "Firmar un .velb con clave privada PEM. Uso: --sign-velb input.velb "
         "--sign-key priv.pem -o output.signed.velb",
         cxxopts::value<std::string>())(
            "sign-key", "Path al PEM con la clave privada para --sign-velb",
            cxxopts::value<std::string>())(
            "verify-velb",
            "Verificar la firma digital de un .velb. Uso: --verify-velb "
            "file.velb --verify-key pub.pem",
            cxxopts::value<std::string>())(
            "verify-key", "Path al PEM con la clave publica para --verify-velb",
            cxxopts::value<std::string>())
        // Phase AOT.5: linker propio (enlaza .o sin ld/gcc).
        ("link",
         "Phase AOT.5: enlaza objetos relocatables (ELF64 o COFF AMD64, "
         "auto-detectados; los de --emit obj o de gcc/MSVC) en un ejecutable "
         "nativo SIN ld/gcc. Uso: vm --link a.o b.o -o prog [--format elf|pe] "
         "[--entry sym] [--link-base 0xADDR]. Con --entry usa ese simbolo "
         "como entrada SIN stub (kernel/bootloader); sin el, sintetiza "
         "_start->main (ejecutable hosted).")(
            "entry",
            "Con --link: simbolo de entrada del ejecutable (e.g. _kstart). "
            "Vacio => _start sintetico que llama a main.",
            cxxopts::value<std::string>()->default_value(""))(
            "link-base",
            "Con --link: base de carga del ejecutable (hex, e.g. 0x100000 para "
            "un kernel). Default segun el formato.",
            cxxopts::value<std::string>()->default_value(""))(
            "link-script",
            "Con --link: script de enlace ESCRITO EN VEX (un .vex con 'fn "
            "link()' que llama a builtins base/entry/stack/section/"
            "section_size/align_up/debug_build). El linker lo compila y ejecuta "
            "para leer la configuracion. Los CLI --link-base/--entry tienen "
            "prioridad sobre el script.",
            cxxopts::value<std::string>()->default_value(""))(
            "link-debug",
            "Con --link --link-script: hace que el builtin debug_build() del "
            "script devuelva true.",
            cxxopts::value<bool>()->default_value("false"));

    // BUG FIX: Args posicionales y allow_unrecognised DEBEN configurarse
    // ANTES de @c options.parse(...).  El bug anterior registraba el
    // option `positional` y @c parse_positional DESPUES del parse, asi
    // que cualquier `vm --script foo.vsh arg1 arg2` perdia arg1/arg2 y
    // ARGV del script quedaba con solo el path del script.  Se mueve
    // toda la configuracion arriba; esto es prerequisito de @c parse.
    options.add_options()("positional", "Argumentos posicionales",
                          cxxopts::value<std::vector<std::string>>());
    options.parse_positional({"positional"});
    options.positional_help("[args...]");
    options
        .allow_unrecognised_options(); // para flags sin que aun hay que parsear

    auto result = options.parse(argc, argv);

    // activar trazas de depuracion del subsistema distribuido si se paso
    // --dist-debug
    if (result.count("dist-debug")) distrib::set_dist_debug(true);

    // activar trazas del GC si se paso --gc-debug.  Tambien respeta env
    // VESTA_GC_DEBUG=1 (la inicializacion estatica corre antes de main,
    // no se pierde si el flag CLI esta o no).
    if (result.count("gc-debug")) gc::set_gc_debug(true);
    // --gc-debug-buffered implica --gc-debug ademas de buffered mode.
    if (result.count("gc-debug-buffered")) {
        gc::set_gc_debug(true);
        gc::set_gc_debug_buffered(true);
    }

    // ---- Configuracion del JIT ----
    //
    // Orden de prioridad (mayor primero):
    //   1. --jit-threshold N   (CLI explicito)
    //   2. -m jit              (preset = threshold 1, util para tests)
    //   3. VESTA_JIT_THRESHOLD env var (default ya lazy-leida en auto_jit)
    //
    // --jit-warn activa el output detallado de IR ops no soportadas.
    // --jit-stats imprime el snapshot final al RET de main (independiente).
    /* Prioridad: --jit-threshold > -m jit/-m interp explicito > env var
     * VESTA_JIT_THRESHOLD > default (UINT32_MAX = JIT off).
     *
     * cxxopts retorna count("mode") == 0 cuando no se paso `-m` aunque
     * la opcion tenga default_value("vm").  Usamos eso para distinguir
     * "el usuario eligio -m algo" vs "default; mirar env var".
     *
     * Importante: leer el env var ANTES del Loader para que la
     * eager-compile pass se dispare con el threshold correcto.  Sin esto
     * el threshold se inicializa lazy en el primer maybe_compile_*, ya
     * tarde para eager compile. */
    // Phase AOT: modo de compilacion nativa standalone (-m aot).  Se resuelve
    // aqui (junto al resto de modos) y se consume en el bloque --vex mas abajo.
    bool aot_mode = false;
    aot::Tier aot_tier = aot::Tier::BARE;
    bool aot_freestanding = result.count("freestanding") > 0;
    bool aot_no_exceptions = result.count("no-exceptions") > 0;
    bool aot_no_io = result.count("no-io") > 0;
    bool aot_no_mem = result.count("no-mem") > 0;
    {
        const std::string &tname = result["target"].as<std::string>();
        if (tname == "bare")
            aot_tier = aot::Tier::BARE;
        else if (tname == "embed")
            aot_tier = aot::Tier::EMBED;
        else if (tname == "full")
            aot_tier = aot::Tier::FULL;
        else {
            std::cerr << "[aot] --target invalido: '" << tname
                      << "' (usa bare|embed|full)\n";
            return EXIT_FAILURE;
        }
    }

    if (result.count("jit-threshold")) {
        jit::set_jit_threshold(result["jit-threshold"].as<uint32_t>());
    } else if (result.count("mode")) {
        const std::string m = result["mode"].as<std::string>();
        if (m == "jit") {
            jit::set_jit_threshold(1);
        } else if (m == "vm" || m == "interp") {
            jit::set_jit_threshold(UINT32_MAX);
        } else if (m == "aot") {
            // Modo AOT: no se ejecuta nada en la VM; el JIT runtime queda off.
            aot_mode = true;
            jit::set_jit_threshold(UINT32_MAX);
            // @Target target-aware: los atomos os:/arch: de @Target se evaluan
            // contra el TARGET del binario AOT (cross-compile), no el host de
            // build, para que las variantes por plataforma del runtime/usuario
            // se seleccionen segun --format / --aot-arch.
            std::string fmt_s;
            if (result.count("format"))
                fmt_s = result["format"].as<std::string>();
            else
#if defined(_WIN32)
                fmt_s = "pe";
#else
                fmt_s = "elf";
#endif
            const std::string arch_s =
                result.count("aot-arch")
                    ? result["aot-arch"].as<std::string>()
                    : std::string("x86-64");
            const bool is32 = (arch_s == "x86-32" || arch_s == "x86_32" ||
                               arch_s == "i386");
            vex::set_aot_condcomp_target(fmt_s == "pe" ? "windows" : "linux",
                                         is32 ? "x86" : "x86_64");
        }
    } else {
        /* Sin flags CLI explicitos -> consultar env var ahora.  Lo
         * hacemos via la API publica que delega en el mismo
         * std::call_once que el path lazy interno, asi futuras
         * inicializaciones lazy son no-op (idempotente). */
        jit::init_threshold_from_env_now();
    }
    if (result.count("jit-warn")) {
        jit::g_jit_warn_unsupported = true;
    }
    if (result.count("jit-disasm")) {
        jit::g_jit_disasm = true;
    }
    /* Sprint D.6 (2026-06-03): activar profile counters runtime.  El
     * --profile [path] inicializa el collector y registra el atexit
     * handler para dump automatico al finalizar el proceso.  Tambien
     * se respeta la env var VESTA_PROFILE_DUMP cuando el flag no esta. */
    {
        std::string profile_path;
        if (result.count("profile")) {
            profile_path = result["profile"].as<std::string>();
        } else if (const char *env = std::getenv("VESTA_PROFILE_DUMP")) {
            profile_path = env;
        }
        if (!profile_path.empty()) {
            runtime::profile::profile_init(profile_path);
        }
    }
    const bool jit_stats_requested = result.count("jit-stats") > 0;
    /* Sprint string-perf-6: --stats o --jit-stats activan el counter MIPS
     * per-block en JIT.  Sin estos flags el JIT corre sin overhead de
     * instrumentacion (35-50% mas rapido en hot loops con muchos bloques). */
    if (result.count("stats") || jit_stats_requested) {
        jit::g_jit_emit_instr_counter = true;
    }

    if (result.count("help")) {
        vesta::scout() << options.help() << std::endl;
        return 0;
    }

    std::string out_prefix;
    if (result.count("output")) {
        out_prefix = result["output"].as<std::string>();
    } else {
        out_prefix = result["output-prefix"].as<std::string>();
    }

    if (result.count("version")) {
        vesta::scout() << "Vesta v0.1.0" << std::endl;
        return 0;
    }

    if (result.count("list-arch")) {
        const ArchSupport &archs = get_available_architectures();
        vesta::scout() << "Capstone supported architectures:\n";
        for (auto &a : archs.capstone)
            vesta::scout() << "  " << a << "\n";
        vesta::scout() << "Keystone supported architectures:\n";
        for (auto &a : archs.keystone)
            vesta::scout() << "  " << a << "\n";
        return 0;
    }

    // === Validacion temprana de combinaciones de flags ===
    // Sin esto, flags incompatibles o mal escritas se ignoraban en silencio
    // por el orden de dispatch.  Ejemplos reales detectados:
    //   - `--run x.velb -m aot --emit exe --format exe`: el dispatch de --run
    //     ganaba y ejecutaba el .velb, ignorando -m aot/--emit/--format.
    //   - `--vex x.vex --emit exe` (sin -m aot): --emit se ignoraba en silencio.
    // Falla cerrado con mensaje claro en vez de hacer algo distinto a lo pedido.
    {
        // (1) Acciones primarias mutuamente excluyentes: solo una a la vez.
        static const char *const kPrimaryActions[] = {
            "run",      "worker",       "driver", "build",
            "vex",      "asm-file",     "disasm-file", "script"};
        std::vector<std::string> present;
        for (const char *a : kPrimaryActions)
            if (result.count(a)) present.emplace_back(std::string("--") + a);
        if (present.size() > 1) {
            std::cerr << "[cli] acciones incompatibles: ";
            for (size_t i = 0; i < present.size(); ++i)
                std::cerr << (i ? ", " : "") << present[i];
            std::cerr << " -- elige solo una.\n";
            return EXIT_FAILURE;
        }

        // (2) -m aot solo tiene sentido compilando un .vex a binario nativo.
        if (aot_mode && !result.count("vex")) {
            std::cerr << "[cli] -m aot requiere --vex <archivo.vex> "
                         "(compilacion nativa desde fuente Vex).\n";
            if (result.count("run"))
                std::cerr << "[cli]   nota: --run ejecuta un .velb en la VM; "
                             "es incompatible con -m aot.\n";
            return EXIT_FAILURE;
        }

        // (3) --emit / --format solo aplican a la compilacion AOT (-m aot) o,
        //     en el caso de --format, al linker propio (--link).  Fuera de esos
        //     contextos se ignoraban sin avisar.
        const bool link_mode = result.count("link") > 0;
        if (result.count("emit") && !aot_mode) {
            std::cerr << "[cli] --emit solo aplica con -m aot "
                         "(compilacion nativa standalone).\n";
            return EXIT_FAILURE;
        }
        if (result.count("format") && !aot_mode && !link_mode) {
            std::cerr << "[cli] --format solo aplica con -m aot o --link.\n";
            return EXIT_FAILURE;
        }
    }

    // Phase M.L28: firmas digitales del .velb (independientes del flujo
    // de compile / run / etc.).  Ambos comandos retornan al usuario al
    // terminar; no continuan al resto del flow.
    if (result.count("sign-velb")) {
        const std::string in_path = result["sign-velb"].as<std::string>();
        if (!result.count("sign-key")) {
            std::cerr << "error: --sign-velb requiere --sign-key <priv.pem>\n";
            return EXIT_FAILURE;
        }
        const std::string key_path = result["sign-key"].as<std::string>();
        const std::string out_path = result.count("output")
                                         ? result["output"].as<std::string>()
                                         : (in_path + ".signed");
        std::ifstream f(in_path, std::ios::binary | std::ios::ate);
        if (!f) {
            std::cerr << "error: no se puede abrir " << in_path << "\n";
            return EXIT_FAILURE;
        }
        const std::streamsize sz = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(sz < 0 ? 0 : sz));
        if (sz > 0) f.read(reinterpret_cast<char *>(bytes.data()), sz);
        f.close();
        std::vector<uint8_t> signed_bytes;
        std::string err;
        if (!vex::velb_sign(bytes, key_path, vex::VsigAlgo::RSA_SHA256,
                            signed_bytes, err)) {
            std::cerr << "error: " << err << "\n";
            return EXIT_FAILURE;
        }
        std::ofstream of(out_path, std::ios::binary);
        if (!of) {
            std::cerr << "error: no se puede escribir " << out_path << "\n";
            return EXIT_FAILURE;
        }
        of.write(reinterpret_cast<const char *>(signed_bytes.data()),
                 static_cast<std::streamsize>(signed_bytes.size()));
        std::cerr << "[sign] " << in_path << " (" << bytes.size() << " B) -> "
                  << out_path << " (" << signed_bytes.size() << " B, +"
                  << (signed_bytes.size() - bytes.size())
                  << " B footer VSIG)\n";
        return EXIT_SUCCESS;
    }

    if (result.count("verify-velb")) {
        const std::string in_path = result["verify-velb"].as<std::string>();
        if (!result.count("verify-key")) {
            std::cerr
                << "error: --verify-velb requiere --verify-key <pub.pem>\n";
            return EXIT_FAILURE;
        }
        const std::string key_path = result["verify-key"].as<std::string>();
        std::ifstream f(in_path, std::ios::binary | std::ios::ate);
        if (!f) {
            std::cerr << "error: no se puede abrir " << in_path << "\n";
            return EXIT_FAILURE;
        }
        const std::streamsize sz = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(sz < 0 ? 0 : sz));
        if (sz > 0) f.read(reinterpret_cast<char *>(bytes.data()), sz);
        f.close();
        auto vr = vex::velb_verify_signature(bytes, key_path);
        if (vr.ok) {
            std::cerr << "[verify] " << in_path << ": firma VALIDA\n";
            return EXIT_SUCCESS;
        }
        std::cerr << "[verify] " << in_path << ": " << vr.error << "\n";
        return EXIT_FAILURE;
    }

    // Phase AOT.5: linker propio -- enlaza objetos .o en un ejecutable nativo
    // sin depender de ld/gcc.  Uso: vm --link a.o b.o -o prog [--format elf]
    // [--entry sym] [--link-base 0xADDR].
    if (result.count("link")) {
        std::vector<std::string> inputs;
        if (result.count("positional"))
            inputs = result["positional"].as<std::vector<std::string>>();
        if (inputs.empty()) {
            std::cerr << "error: --link requiere al menos un objeto .o "
                         "(vm --link a.o [b.o ...] -o salida)\n";
            return EXIT_FAILURE;
        }
        aot::LinkOptions lopts;
        // Formato: --format pe|elf (default ELF; slice 1 = ELF).
        if (result.count("format")) {
            const std::string fmt = result["format"].as<std::string>();
            if (fmt == "pe")
                lopts.fmt = aot::ObjFormat::PE;
            else if (fmt == "elf")
                lopts.fmt = aot::ObjFormat::ELF;
            else {
                std::cerr << "error: --format invalido para --link: " << fmt
                          << " (pe|elf)\n";
                return EXIT_FAILURE;
            }
        } else {
            lopts.fmt = aot::ObjFormat::ELF;
        }
        lopts.entry = result["entry"].as<std::string>();
        const std::string lbase = result["link-base"].as<std::string>();
        if (!lbase.empty())
            lopts.image_base =
                std::strtoull(lbase.c_str(), nullptr, 0); // 0x.. o decimal
        lopts.link_script = result["link-script"].as<std::string>();
        lopts.debug = result.count("link-debug") > 0;
        std::string out_path = result["output"].as<std::string>();
        std::string lerr;
        if (!aot::aot_link(inputs, out_path, lopts, lerr)) {
            std::cerr << "[link] error: " << lerr << "\n";
            return EXIT_FAILURE;
        }
        std::string entry_note =
            !lopts.entry.empty()
                ? (", entry=" + lopts.entry)
                : (lopts.link_script.empty() ? std::string(", _start->main")
                                             : std::string(", entry via "
                                                           "link-script"));
        std::cerr << "[link] ejecutable '" << out_path << "' escrito ("
                  << inputs.size() << " objeto(s)" << entry_note << ").\n";
        return EXIT_SUCCESS;
    }

#ifdef VESTA_HAS_PREPROCESSOR
    // Solo preprocesar: expandir macros y directivas sin ensamblar
    // vm.exe --preprocess-only src/main.vel [-o salida.vel]
    if (result.count("preprocess-only")) {
        const std::string &src_path =
            result["preprocess-only"].as<std::string>();

        // leer el archivo fuente
        std::ifstream ifs(src_path, std::ios::binary);
        if (!ifs) {
            std::cerr << "error: no se puede abrir: " << src_path << "\n";
            return EXIT_FAILURE;
        }
        std::string source((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());

        // configurar el preprocesador igual que run_worker
        vpp::Preprocessor pp;
        std::string source_dir =
            std::filesystem::path(src_path).parent_path().string();
        if (source_dir.empty()) source_dir = ".";
        pp.options().include_paths.push_back(source_dir);
        std::string exe_dir = std::filesystem::path(fs::get_executable_path())
                                  .parent_path()
                                  .string();
        pp.options().import_paths.push_back(exe_dir +
                                            "/preprocessor/include_lib");
        pp.options().import_paths.push_back(exe_dir + "/include_lib");
        pp.options().import_paths.push_back(source_dir);
#ifdef _WIN32
        pp.options().predefines.push_back("__VPP_WINDOWS__");
#elif defined(__linux__)
        pp.options().predefines.push_back("__VPP_LINUX__");
#elif defined(__APPLE__)
        pp.options().predefines.push_back("__VPP_MACOS__");
#endif
#if defined(__x86_64__) || defined(_M_X64)
        pp.options().predefines.push_back("__VPP_X86_64__");
#elif defined(__i386__) || defined(_M_IX86)
        pp.options().predefines.push_back("__VPP_X86_32__");
#elif defined(__aarch64__) || defined(_M_ARM64)
        pp.options().predefines.push_back("__VPP_AARCH64__");
#endif

        std::string processed = pp.process(source, src_path);

        // imprimir todos los diagnosticos
        for (const auto &d : pp.diagnostics().diagnostics()) {
            std::cerr << d.loc.file << ":" << d.loc.line << ":" << d.loc.col
                      << ": "
                      << (d.level >= vpp::DiagLevel::ERR ? "error: "
                                                         : "warning: ")
                      << d.message << "\n";
        }
        if (pp.diagnostics().has_errors()) return EXIT_FAILURE;

        // escribir a archivo si se especifico -o, si no a stdout
        if (!out_prefix.empty() && out_prefix != "out") {
            // determinar nombre del archivo de salida
            std::string out_file = out_prefix;
            if (out_file.find('.') == std::string::npos) out_file += ".vel";
            std::ofstream ofs(out_file);
            if (!ofs) {
                std::cerr << "error: no se puede escribir: " << out_file
                          << "\n";
                return EXIT_FAILURE;
            }
            ofs << processed;
            vesta::scout() << "[preprocess-only] -> " << out_file << "\n";
        } else {
            // sin -o: imprimir a stdout para inspeccion rapida
            std::cout << processed;
        }
        return EXIT_SUCCESS;
    }
#endif

    // -----------------------------------------------------------------------
    // Modo servidor distribuido puro (sin ejecutar bytecode)
    // vm.exe --dist-server --dist-port 7789 [--dist-discover] [--dist-name
    // nodo1]
    //        [--dist-add-node 192.168.1.100:7789] [--dist-token secreto]
    //        [--dist-tls --dist-cert cert.pem --dist-key key.pem --dist-ca
    //        ca.pem]
    // -----------------------------------------------------------------------
    if (result.count("dist-server")) {
        try {
            runtime::ManageVM dist_mgr(nullptr, 0);
            runtime::VM *vm = dist_mgr.loader.create_vm_instance(1);
            if (!vm) {
                std::cerr << "[dist-server] Error: no se pudo crear la "
                             "instancia VM\n";
                return EXIT_FAILURE;
            }

            apply_dist_config(vm, result);

            // activar modo persistente para que el scheduler no termine al no
            // haber procesos; los procesos remotos llegan via rspawn despues
            // del arranque
            vm->vm_persistent = true;
            vm->start();

            vesta::scout() << "[dist-server] Nodo distribuido activo. "
                              "Pulse Ctrl+C para detener.\n";

            // esperar hasta Ctrl+C
            std::signal(SIGINT, on_dist_sigint);
            while (g_server_running.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

            vm->dist_runtime->stop();
            vm->stop();
            vesta::scout() << "[dist-server] Nodo detenido.\n";
        } catch (const std::exception &e) {
            std::cerr << "[dist-server] Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    // -----------------------------------------------------------------------
    // Modo servidor persistente de depuracion (sin ejecutar un .velb inicial).
    //
    // vm.exe --server-mode [--server-port N | --debug-port N] [--schedulers M]
    //        [--dist-port ...] [--vex-debug]
    //
    // La VM se queda viva indefinidamente atendiendo comandos del debugger
    // (load_velb, kill_proc, server_info, server_shutdown, etc.).  Los
    // clientes (tools/dbg_client.vsh o el IDE Electron) pueden conectarse,
    // cargar .velb desde el filesystem de la VM, lanzarlos como procesos y
    // gestionarlos via los comandos normales del protocolo de depuracion.
    //
    // Termina con SIGINT (Ctrl+C) o con el comando server_shutdown del cliente.
    // -----------------------------------------------------------------------
    if (result.count("server-mode")) {
        try {
            size_t num_schedulers = result["schedulers"].as<size_t>();
            runtime::ManageVM mgr(nullptr, 0);
            runtime::VM *vm = mgr.loader.create_vm_instance(num_schedulers);
            if (!vm) {
                std::cerr << "[server-mode] Error: no se pudo crear la "
                             "instancia VM\n";
                return EXIT_FAILURE;
            }

            // Aplicar configuracion distribuida si el usuario paso flags
            // --dist-*
            bool has_dist = result.count("dist-port") > 0 ||
                            result.count("dist-discover") > 0 ||
                            result.count("dist-add-node") > 0 ||
                            result.count("dist-tls") > 0 ||
                            result.count("dist-name") > 0 ||
                            result.count("dist-token") > 0 ||
                            result.count("dist-node-id") > 0;
            if (has_dist) apply_dist_config(vm, result);

            // Resolver puerto: --server-port > --debug-port > default 9229
            uint16_t srv_port = 0;
            if (result.count("server-port"))
                srv_port = result["server-port"].as<uint16_t>();
            if (srv_port == 0 && result.count("debug-port"))
                srv_port = result["debug-port"].as<uint16_t>();
            if (srv_port == 0) srv_port = debug::DBG_DEFAULT_PORT;

            // Configuracion del sandbox de filesystem.
            std::string srv_root = result["server-root"].as<std::string>();
            if (!srv_root.empty()) {
                std::error_code rec;
                std::filesystem::create_directories(srv_root, rec);
            }

            // Configuracion del AuthManager.
            std::string auth_db = result["auth-db"].as<std::string>();
            bool auth_enabled = !auth_db.empty();
            if (auth_enabled) {
                if (!debug::AuthManager::instance().init(auth_db)) {
                    std::cerr
                        << "[server-mode] Error: "
                        << debug::AuthManager::instance().last_init_error()
                        << "\n";
                    std::cerr << "[server-mode] Sugerencia: usa una ruta "
                                 "absoluta a un directorio escribible, "
                                 "p.ej. --auth-db \"C:\\Users\\TuUsuario\\"
                                 "VestaVM\\users.db\".\n";
                    return EXIT_FAILURE;
                }
                // Bootstrap del admin si la BD esta vacia.
                if (!debug::AuthManager::instance().has_any_user()) {
                    std::string admin_user =
                        result["admin-user"].as<std::string>();
                    std::string admin_pass =
                        result["admin-password"].as<std::string>();
                    if (admin_pass.empty()) {
                        // Generar contrasenya aleatoria de 24 chars base64.
                        uint8_t raw[18];
                        if (RAND_bytes(raw, sizeof(raw)) != 1) {
                            std::cerr
                                << "[server-mode] Error: RAND_bytes "
                                   "fallo al generar la contrasenya admin\n";
                            return EXIT_FAILURE;
                        }
                        static const char *alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ"
                                                      "abcdefghijkmnpqrstuvwxyz"
                                                      "23456789";
                        admin_pass.reserve(24);
                        for (size_t i = 0; i < 24; ++i)
                            admin_pass.push_back(alphabet[raw[i % 18] % 62]);
                    }
                    std::string e;
                    if (!debug::AuthManager::instance().create_user(
                            admin_user, admin_pass, debug::Role::ADMIN, &e)) {
                        std::cerr << "[server-mode] Error: no se pudo crear "
                                     "el admin: "
                                  << e << "\n";
                        return EXIT_FAILURE;
                    }
                    vesta::scout() << "[server-mode] Usuario admin creado: '"
                                   << admin_user << "'\n";
                    vesta::scout()
                        << "[server-mode] Contrasenya admin (SOLO se "
                           "muestra una vez): "
                        << admin_pass << "\n";
                }
            }

            // Arrancar el servidor de depuracion ANTES de iniciar la VM.
            auto dbg = std::make_unique<debug::Debugger>(*vm);
            dbg->set_server_root(srv_root);
            dbg->set_auth_required(auth_enabled);
            if (!dbg->start(srv_port)) {
                std::cerr << "[server-mode] Error: no se pudo iniciar el "
                             "servidor de depuracion en puerto "
                          << srv_port << "\n";
                return EXIT_FAILURE;
            }
            vm->debugger = dbg.get();
            // NO activamos `sched->has_hooks=true` aqui: el debugger lo
            // hara automaticamente via @c refresh_scheduler_hooks cuando
            // el primer breakpoint / step / watch llegue.  Si no hay
            // depuracion activa, los schedulers se quedan en el fast
            // path threaded computed-goto y el coste runtime es 0%.

            // Modo persistente: el scheduler espera nuevos procesos en
            // lugar de terminar al quedarse sin trabajo.  Cuando un cliente
            // hace load_velb, el proceso resultante entra en READY y el
            // scheduler lo recoge sin necesidad de reiniciar nada.
            vm->vm_persistent = true;
            vm->start();

            vesta::scout() << "[server-mode] VM persistente activa.\n";
            vesta::scout() << "[server-mode] Debugger TCP escuchando en "
                              "puerto "
                           << srv_port << ".\n";
            vesta::scout() << "[server-mode] Schedulers: " << num_schedulers
                           << ".\n";
            if (!srv_root.empty()) {
                vesta::scout()
                    << "[server-mode] Sandbox de filesystem: " << srv_root
                    << "\n";
            } else {
                vesta::scout() << "[server-mode] Sin sandbox de filesystem "
                                  "(use --server-root para restringir).\n";
            }
            if (auth_enabled) {
                vesta::scout() << "[server-mode] Auth: ACTIVO (BD: " << auth_db
                               << ").  Los clientes deben "
                                  "invocar auth_login.\n";
            } else {
                vesta::scout() << "[server-mode] Auth: DESACTIVADO (modo "
                                  "desarrollo; use --auth-db <ruta> para "
                                  "exigir login).\n";
            }
            // Conectar el flag global con el debugger para que el comando
            // server_shutdown remoto lo pueda mover.
            std::signal(SIGINT, on_dist_sigint);
            debug::set_server_shutdown_flag(&g_server_running);

            bool no_repl = result.count("no-repl") > 0;
            if (no_repl) {
                // -- Modo headless (sin REPL local) --
                vesta::scout() << "[server-mode] Pulse Ctrl+C o envie "
                                  "'server_shutdown' para detener.\n";
                while (g_server_running.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                // -- REPL local compartiendo runtime con clientes remotos --
                //
                // El REPL (VestaViewManager) corre en ESTE thread porque
                // posee stdin (readline necesita TTY).  Un thread watcher
                // observa g_server_running: si el flag pasa a false desde
                // fuera (SIGINT o server_shutdown remoto), invoca
                // VestaViewManager::stop() para cerrar el bucle del REPL
                // -- pero como readline bloquea en stdin, el operador
                // debera pulsar Enter UNA vez para liberarlo.  Se imprime
                // un mensaje para que sepa que hacer.
                //
                // Cuando el operador local teclea 'exit' o EOF en el
                // REPL, vm.run() retorna; pasamos g_server_running a
                // false aqui mismo para que el watcher salga.
                vesta::scout() << "[server-mode] REPL local activo "
                                  "(teclea 'exit' o pulsa Ctrl+D para "
                                  "salir).  Tambien aceptamos "
                                  "'server_shutdown' del cliente.\n";

                cli::Config cfg_srv;
                cfg_srv.history_file = "my_vm_history.txt";
                cfg_srv.history_max = 1000;
                cfg_srv.prompt = "vesta(server)> ";
                cfg_srv.multiline_end = ";;";

                cli::VestaViewManager view_mgr(cfg_srv);
                view_mgr.set_execute_callback([](const std::string &cmd) {
                    // mismo comportamiento del REPL normal: ejecucion
                    // asincrona via run_command_async; cuando el future
                    // resuelve, se imprime el output.
                    auto fut = runtime::run_command_async(cmd);
                    std::thread([f = std::move(fut)]() mutable {
                        try {
                            auto out = f.get();
                            if (!out.empty())
                                vesta::scout() << out << std::endl;
                        } catch (const std::exception &e) {
                            std::cerr << "Runtime error: " << e.what()
                                      << std::endl;
                        }
                    }).detach();
                });

                std::atomic<bool> watcher_done{false};
                std::thread watcher([&]() {
                    while (g_server_running.load() && !watcher_done.load())
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(100));
                    if (watcher_done.load()) return;
                    // shutdown remoto / SIGINT: detener el REPL.  El
                    // readline poll-loop chequea I.running cada ~50 ms,
                    // asi que view_mgr.stop() liberara el prompt en
                    // ~50 ms sin requerir entrada del operador.
                    vesta::scout() << "\n[server-mode] Apagado recibido; "
                                      "cerrando REPL local.\n";
                    view_mgr.stop();
                });

                // Bucle del REPL (bloqueante en stdin).
                view_mgr.run();

                // El REPL salio (exit/EOF) o fue parado por el watcher;
                // marcar global y joinar el thread.
                g_server_running.store(false);
                watcher_done.store(true);
                if (watcher.joinable()) watcher.join();
            }

            vesta::scout() << "[server-mode] Senial de parada recibida; "
                              "deteniendo VM...\n";
            if (vm->dist_runtime) vm->dist_runtime->stop();
            dbg->stop();
            // Desconectar el hook ANTES de destruir la VM para que un
            // comando server_shutdown tardio no escriba a memoria liberada.
            debug::set_server_shutdown_flag(nullptr);
            vm->stop();
            vesta::scout() << "[server-mode] Servidor detenido.\n";
        } catch (const std::exception &e) {
            std::cerr << "[server-mode] Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (result.count("install") || result.count("uninstall") ||
        result.count("repair")) {
        // Reconstruir argv "limpio" para el parser interno del instalador
        std::vector<std::string> args;
        if (result.count("uninstall"))
            args.push_back("uninstall");
        else if (result.count("repair"))
            args.push_back("repair");
        else
            args.push_back("install");

        if (result.count("silent")) args.push_back("--silent");
        if (result.count("per-user")) args.push_back("--per-user");
        if (result.count("system-wide")) args.push_back("--system-wide");
        if (result.count("prefix"))
            args.push_back("--prefix=" + result["prefix"].as<std::string>());
        if (result.count("manifest"))
            args.push_back("--manifest=" +
                           result["manifest"].as<std::string>());

        std::vector<char *> argv2;
        argv2.push_back(argv[0]);
        for (auto &s : args)
            argv2.push_back(s.data());
        return install::run_install_cli((int)argv2.size(), argv2.data());
    }

    // Compilar un archivo como worker
    // vm.exe --worker src/main.vel -o main.velb
    if (result.count("worker")) {
        return asm_multi_process::run_worker(
            result["worker"].as<std::string>(), out_prefix,
            /*skip_preprocessor=*/false,
            /*keep_labels=*/(result.count("keep-labels") > 0),
            /*ir_section_bytes=*/nullptr,
            /*emit_map=*/(result.count("emit-map") > 0));
    }

    // Compilar un proyecto entero en paralelo
    // vm.exe --driver src/ -j 8 -o program.velb
    // Compilar con número automático de hilos
    // vm.exe --driver src/ -j 0 -o program.velb
    if (result.count("driver")) {
        int threads = result["threads"].as<int>();
        return asm_multi_process::run_driver(result["driver"].as<std::string>(),
                                             threads, out_prefix);
    }

    bool save_output = result.count("save-output") > 0;

    // ensamblar un solo archivo (modo clásico)
    // vm.exe --asm-file src/main.asm --arch x86_64
    if (result.count("asm-file")) {
        return assemble_file(result["asm-file"].as<std::string>(),
                             result["arch"].as<std::string>(), save_output,
                             out_prefix)
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }

    if (result.count("disasm-file")) {
        if (!result.count("arch")) {
            std::cerr << "--arch es requerido para desensamblar\n";
            return EXIT_FAILURE;
        }
        return disassemble_file(result["disasm-file"].as<std::string>(),
                                result["arch"].as<std::string>(), save_output,
                                out_prefix)
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }

    // Compilar un archivo .ir (SSA IR) a .vel y opcionalmente a .velb
    // vm.exe --ir-file program.ir --ir-opt 2 -o program.velb
    if (result.count("ir-file")) {
        const std::string &ir_path = result["ir-file"].as<std::string>();
        int opt_n = result["ir-opt"].as<int>();
        bool emit_only = result.count("ir-emit-only") > 0;

        // Leer el archivo .ir
        std::ifstream ifs(ir_path);
        if (!ifs.is_open()) {
            std::cerr << "[ir] No se puede abrir: " << ir_path << "\n";
            return EXIT_FAILURE;
        }
        std::string ir_text((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());

        // Emitir .vel
        ir::EmitOptions eopts;
        eopts.opt_level = ir::opt_level_from_int(opt_n);
        eopts.emit_comments = true;
        eopts.export_all = true;

        ir::EmitResult er = ir::ir_emit_text(ir_text, eopts);
        if (!er.ok) {
            std::cerr << "[ir] Error de emision: " << er.error << "\n";
            return EXIT_FAILURE;
        }

        // Determinar nombre del archivo .vel de salida
        std::string vel_path =
            out_prefix.empty() ? "out.vel" : out_prefix + ".vel";
        {
            std::ofstream ofs(vel_path);
            if (!ofs.is_open()) {
                std::cerr << "[ir] No se puede escribir: " << vel_path << "\n";
                return EXIT_FAILURE;
            }
            ofs << er.vel_text;
        }
        vesta::scout() << "[ir] .vel generado: " << vel_path << "\n";

        if (emit_only) return EXIT_SUCCESS;

        // Compilar el .vel generado a .velb usando el pipeline existente
        return asm_multi_process::run_worker(
            vel_path, out_prefix,
            /*skip_preprocessor=*/false,
            /*keep_labels=*/(result.count("keep-labels") > 0),
            /*ir_section_bytes=*/nullptr,
            /*emit_map=*/(result.count("emit-map") > 0));
    }

    // Compilar un archivo .vex (lenguaje Vex) a .velb.
    // Pipeline:
    //   .vex source
    //     -> [VPP opcional]    (metaprogramacion compartida con .vel)
    //     -> Vex frontend      (lex + parse + tipos + lowering)
    //     -> ir::IrModule
    //     -> ir_emit_module    (texto .vel)
    //     -> run_worker(.vel, skip_preprocessor=true)
    //     -> .velb
    //
    // -----------------------------------------------------------------
    // Subsistema de coste (MODO ANALISIS): vm --analyze <archivo.vex>
    //
    // Rama APARTE del path de compilacion normal.  Compila el .vex hasta
    // el SSA IR (sin emitir .velb), corre el analizador estatico de
    // complejidad (analyze::bigo) sobre cada funcion del modulo OPTIMIZADO
    // (O2), e imprime el coste Big-O.  Si una funcion declara @complexity,
    // valida el contrato de forma conservadora (solo avisa si esta
    // CONFIADO de la discrepancia).  Cero impacto en el codegen.
    //
    //   vm --analyze prog.vex            -> salida legible
    //   vm --analyze prog.vex --analyze-json -> JSON (para diagramas)
    // -----------------------------------------------------------------
    if (result.count("analyze")) {
        const std::string &vex_path = result["analyze"].as<std::string>();
        const bool want_json = result.count("analyze-json") > 0;

        std::ifstream ifs(vex_path);
        if (!ifs.is_open()) {
            std::cerr << "[analyze] No se puede abrir: " << vex_path << "\n";
            return EXIT_FAILURE;
        }
        std::string vex_source((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
        ifs.close();

        // Compilar hasta el IR.  Reutilizamos compile_vex_source que ya
        // rellena ir_module_cache_bytes (modulo POST-O2) y, con
        // @c emit_ir_preopt, tambien ir_module_cache_bytes_preopt
        // (modulo PRE-opt: la complejidad algoritmica del fuente).
        vex::CompileOptions copts;
        copts.module_name = "main";
        copts.opt_level = 2;
        copts.emit_ir_preopt = true;
        vex::CompileResult cr =
            vex::compile_vex_source(vex_source, vex_path, copts);
        // Volcar diagnosticos (errores/warnings) del frontend.
        for (const auto &d : cr.diagnostics.all())
            vex::print_diagnostic(std::cerr, d);
        if (!cr.ok) {
            std::cerr << "[analyze] la compilacion fallo; no hay IR que "
                         "analizar.\n";
            return EXIT_FAILURE;
        }
        if (cr.ir_module_cache_bytes.empty()) {
            std::cerr << "[analyze] el modulo no produjo IR.\n";
            return EXIT_FAILURE;
        }

        // Deserializar el modulo POST-opt (complejidad efectiva del codigo
        // final, tras inline/loop-elim/unroll) + correr el analisis +
        // composicion interprocedural (call-graph bottom-up -> coste TOTAL).
        ir::IrModule amod_post;
        if (!ir::parse_ir_module_cache(cr.ir_module_cache_bytes, amod_post)) {
            std::cerr << "[analyze] no se pudo deserializar el IR.\n";
            return EXIT_FAILURE;
        }
        analyze::ModuleCost mc_post = analyze::analyze_module(amod_post);
        analyze::compose_interproc(mc_post);

        // Deserializar el modulo PRE-opt (complejidad algoritmica del fuente
        // tal como se escribio).  Si por alguna razon no esta disponible,
        // caemos al post (mejor mostrar algo que fallar).
        bool have_pre = !cr.ir_module_cache_bytes_preopt.empty();
        ir::IrModule amod_pre;
        analyze::ModuleCost mc_pre;
        if (have_pre &&
            ir::parse_ir_module_cache(cr.ir_module_cache_bytes_preopt,
                                      amod_pre)) {
            mc_pre = analyze::analyze_module(amod_pre);
            analyze::compose_interproc(mc_pre);
        } else {
            have_pre = false;
        }

        // Helper: localizar el CostResult de una funcion por nombre en un
        // ModuleCost (el orden pre/post puede diferir tras la optimizacion).
        auto find_fn = [](const analyze::ModuleCost &m,
                          const std::string &name)
            -> const analyze::CostResult * {
            for (const auto &f : m.functions)
                if (f.function == name) return &f;
            return nullptr;
        };

        // Helper: validar UNA dimension declarada contra su clase inferida.
        // CONSERVADOR: solo senala discrepancia si HAY contrato, la
        // inferencia es EXACT, ambas clases son conocidas (no O(?)) y
        // difieren.  Devuelve true si hay discrepancia confirmada.  Si la
        // dimension no se declara (decl vacio) devuelve false sin tocar nada.
        auto validate_dim = [](const std::string &decl,
                               analyze::CostClass inferred,
                               analyze::Confidence conf) -> bool {
            if (decl.empty()) return false;
            analyze::CostClass dc = analyze::parse_cost_class(decl);
            if (conf != analyze::Confidence::EXACT) return false;
            if (inferred == analyze::CostClass::O_UNKNOWN) return false;
            if (dc == analyze::CostClass::O_UNKNOWN) return false;
            return dc != inferred;
        };

        if (want_json) {
            // JSON con dos arrays: "pre" y "post".  Cada CostResult expone
            // ya su "partial" (big_o) y "total" + calls[] para diagramas.
            std::ostringstream js;
            js << "{\"pre\":"
               << (have_pre ? analyze::module_cost_to_json(mc_pre)
                            : std::string("[]"))
               << ",\"post\":" << analyze::module_cost_to_json(mc_post) << "}";
            std::cout << js.str() << "\n";
            return EXIT_SUCCESS;
        }

        // Salida legible: por cada funcion (orden del modulo POST-opt),
        // mostrar los 4 costes: PRE/POST x PARCIAL/TOTAL.
        std::cout << "Analisis de coste (Big-O) -- " << vex_path << "\n";
        std::cout << "Niveles: PRE-opt (fuente) y POST-opt (codigo final O2);"
                     " PARCIAL (cuerpo, calls=O(1)) y TOTAL (interprocedural)."
                     "\n";
        std::cout << "Contrato @complexity: cada dimension declarada "
                     "(partial_pre/partial_post/total_pre/total_post) se valida"
                     " contra su coste inferido.  @complexity(O(...)) = azucar"
                     " de total_post.\n";
        std::cout
            << "=================================================="
               "===========\n";
        int mismatches = 0;
        for (const auto &rp : mc_post.functions) {
            const analyze::CostResult *pre = have_pre
                ? find_fn(mc_pre, rp.function)
                : nullptr;

            std::cout << "  " << rp.function << "\n";
            // PRE-opt.
            if (pre) {
                std::cout << "      PRE-opt : parcial "
                          << analyze::cost_class_str(pre->big_o)
                          << "   total "
                          << analyze::cost_class_str(pre->total_class)
                          << "   [" << pre->detail << "]\n";
            } else {
                std::cout << "      PRE-opt : (no disponible)\n";
            }
            // POST-opt.
            std::cout << "      POST-opt: parcial "
                      << analyze::cost_class_str(rp.big_o) << "   total "
                      << analyze::cost_class_str(rp.total_class) << "   ["
                      << rp.detail << "]\n";

            // Resaltar cuando PRE difiere de POST (el optimizer simplifico).
            if (pre && pre->total_class != rp.total_class) {
                std::cout << "      >> el optimizer cambio el coste TOTAL: "
                          << analyze::cost_class_str(pre->total_class)
                          << " (fuente) -> "
                          << analyze::cost_class_str(rp.total_class)
                          << " (efectivo)\n";
            }
            // Resaltar cuando PARCIAL difiere de TOTAL (callees elevan).
            if (rp.big_o != rp.total_class) {
                std::cout << "      >> callees elevan el coste: parcial "
                          << analyze::cost_class_str(rp.big_o) << " -> total "
                          << analyze::cost_class_str(rp.total_class) << "\n";
            }

            // Contrato @complexity: validar CADA dimension declarada contra
            // su coste inferido correspondiente.  Cada una es independiente.
            const bool has_any_contract =
                !rp.decl_partial_pre.empty() || !rp.decl_partial_post.empty() ||
                !rp.decl_total_pre.empty() || !rp.decl_total_post.empty();
            if (has_any_contract) {
                std::cout << "      @complexity declarada:\n";
                // Tabla: (etiqueta, decl, inferida, confianza).  Para PRE
                // usamos el CostResult PRE si existe.
                struct DimRow {
                    const char *label;
                    const std::string *decl;
                    analyze::CostClass inferred;
                    analyze::Confidence conf;
                    bool have;
                };
                std::vector<DimRow> rows = {
                    {"partial_pre ", &rp.decl_partial_pre,
                     pre ? pre->big_o : analyze::CostClass::O_UNKNOWN,
                     pre ? pre->confidence : analyze::Confidence::UNKNOWN,
                     pre != nullptr},
                    {"partial_post", &rp.decl_partial_post, rp.big_o,
                     rp.confidence, true},
                    {"total_pre   ", &rp.decl_total_pre,
                     pre ? pre->total_class : analyze::CostClass::O_UNKNOWN,
                     pre ? pre->total_confidence
                         : analyze::Confidence::UNKNOWN,
                     pre != nullptr},
                    {"total_post  ", &rp.decl_total_post, rp.total_class,
                     rp.total_confidence, true},
                };
                for (const auto &row : rows) {
                    if (row.decl->empty()) continue;
                    std::cout << "        " << row.label << ": " << *row.decl
                              << " -> "
                              << analyze::cost_class_str(
                                     analyze::parse_cost_class(*row.decl));
                    if (!row.have) {
                        std::cout << "  (dimension no disponible; no validada)";
                    } else if (validate_dim(*row.decl, row.inferred,
                                            row.conf)) {
                        std::cout << "  ** DISCREPANCIA: inferida "
                                  << analyze::cost_class_str(row.inferred)
                                  << " **";
                        ++mismatches;
                    } else {
                        std::cout << "  (ok, inferida "
                                  << analyze::cost_class_str(row.inferred)
                                  << ")";
                    }
                    std::cout << "\n";
                }
            }
        }
        std::cout
            << "=================================================="
               "===========\n";
        std::cout << "Funciones analizadas: " << mc_post.functions.size()
                  << "; contratos con discrepancia: " << mismatches << "\n";
        return EXIT_SUCCESS;
    }

    // Ejemplo: vm.exe --vex src/main.vex -o main.velb
    if (result.count("vex")) {
        const std::string &vex_path = result["vex"].as<std::string>();
        bool emit_only = result.count("vex-emit-only") > 0;
        bool emit_ir = result.count("vex-emit-ir") > 0;

        // Flags de diagramas (Mermaid y/o Graphviz).  --diagram-all activa
        // los 4 diagramas; --diagram-format elige el formato de salida.
        const bool diag_all = result.count("diagram-all") > 0;
        const bool diag_vex = diag_all || result.count("diagram-vex") > 0;
        const bool diag_ir_pre = diag_all || result.count("diagram-ir") > 0;
        const bool diag_ir_post =
            diag_all || result.count("diagram-ir-opt") > 0;
        const bool diag_vel = diag_all || result.count("diagram-vel") > 0;

        // Parsear --diagram-format: mermaid | graphviz | both.
        // Lower-case para tolerar "Mermaid", "GraphViz", "BOTH", etc.
        std::string diag_fmt = result.count("diagram-format") > 0
                                   ? result["diagram-format"].as<std::string>()
                                   : std::string("mermaid");
        for (auto &c : diag_fmt)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // Formatos: mermaid | graphviz/dot | html | both (=mermaid+graphviz) |
        // all (=los tres).  "html" produce paginas interactivas autocontenidas.
        bool emit_mermaid =
            (diag_fmt == "mermaid" || diag_fmt == "both" || diag_fmt == "all");
        bool emit_graphviz = (diag_fmt == "graphviz" || diag_fmt == "dot" ||
                              diag_fmt == "both" || diag_fmt == "all");
        bool emit_html = (diag_fmt == "html" || diag_fmt == "all");
        if (!emit_mermaid && !emit_graphviz && !emit_html) {
            std::cerr << "[diagram] Formato desconocido: '" << diag_fmt
                      << "' (usar mermaid | graphviz | html | both | all). "
                         "Defaulting a mermaid.\n";
            emit_mermaid = true;
        }

        // parsear --vex-base (VA base en hex).  0x0 = comportamiento
        // por defecto (caller).  Para plugins cargados via loadmodule usar
        // un valor distinto (ej. 0x10000000) para evitar solapamiento con
        // el caller cuyo code section vive en 0x0..N.
        uint64_t vex_base_addr = 0;
        if (result.count("vex-base")) {
            const std::string &s = result["vex-base"].as<std::string>();
            try {
                vex_base_addr =
                    std::stoull(s, nullptr, 0); // base 0 = autodetect 0x prefix
            } catch (...) {
                std::cerr << "[vex] --vex-base invalido: " << s << "\n";
                return EXIT_FAILURE;
            }
        }

        // 1 Leer el .vex.
        std::ifstream ifs(vex_path);
        if (!ifs.is_open()) {
            std::cerr << "[vex] No se puede abrir: " << vex_path << "\n";
            return EXIT_FAILURE;
        }
        std::string vex_source((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());

        // 2 Aplicar VPP (mismo pipeline que run_worker).  Esto es
        // best-effort: si una macro genera sintaxis no soportada por Vex,
        // el frontend reportara el error con la posicion preprocesada.
#ifdef VESTA_HAS_PREPROCESSOR
        {
            vpp::Preprocessor pp;
            std::string source_dir =
                std::filesystem::path(vex_path).parent_path().string();
            pp.options().include_paths.push_back(source_dir);
            std::string exe_dir =
                std::filesystem::path(fs::get_executable_path())
                    .parent_path()
                    .string();
            pp.options().import_paths.push_back(exe_dir +
                                                "/preprocessor/include_lib");
            pp.options().import_paths.push_back(exe_dir + "/include_lib");
            pp.options().import_paths.push_back(source_dir);
#ifdef _WIN32
            pp.options().predefines.push_back("__VPP_WINDOWS__");
#elif defined(__linux__)
            pp.options().predefines.push_back("__VPP_LINUX__");
#elif defined(__APPLE__)
            pp.options().predefines.push_back("__VPP_MACOS__");
#endif
            std::string processed = pp.process(vex_source, vex_path);
            if (pp.diagnostics().has_errors()) {
                for (const auto &d : pp.diagnostics().diagnostics()) {
                    std::cerr << d.loc.file << ":" << d.loc.line << ": "
                              << (d.level == vpp::DiagLevel::ERR ? "error: "
                                                                 : "warning: ")
                              << d.message << "\n";
                }
                return EXIT_FAILURE;
            }
            vex_source = std::move(processed);
        }
#endif

        // 3 Frontend Vex: source -> IR -> .vel.
        // Sanitizar el nombre del modulo: el parser .vel rechaza
        // identificadores que empiezan con digito o que contienen caracteres no
        // [A-Za-z0-9_], pero los nombres de fichero pueden tener cualquier
        // cosa.  Aplicamos dos transformaciones: (a) si empieza por digito,
        // anteponer "m_"; (b) sustituir cualquier byte no alfanumerico por '_'.
        std::string raw_name = std::filesystem::path(vex_path).stem().string();
        std::string mod_name;
        mod_name.reserve(raw_name.size() + 2);
        if (!raw_name.empty() && (raw_name[0] >= '0' && raw_name[0] <= '9')) {
            mod_name = "m_";
        }
        for (char c : raw_name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_';
            mod_name.push_back(ok ? c : '_');
        }
        if (mod_name.empty()) mod_name = "main";

        vex::CompileOptions copts;
        copts.module_name = mod_name;
        copts.opt_level = 2;
        copts.dump_ir = emit_ir;     // habilita CompileResult::ir_text
        copts.native_poo = aot_mode; // Phase AOT.2.b: clases nativas en -m aot
        copts.exceptions_enabled = !aot_no_exceptions; // C3: configurable
        // Bits del target para el inline-asm @Naked (validacion compile-time):
        // --aot-arch x86-32 -> 32 (si no, `jmp ecx` y demas fallan en mode64).
        if (aot_mode) {
            const std::string aa = result.count("aot-arch")
                                       ? result["aot-arch"].as<std::string>()
                                       : std::string("x86-64");
            copts.asm_target_bits =
                (aa == "x86-32" || aa == "x86_32" || aa == "i386") ? 32 : 64;
            // Ancho SIMD del TARGET para el matcher del vectorizador (mismo
            // mapeo que el codegen vreg deriva de FloatIsa): el binario AOT usa
            // el ancho de --float-isa, no el del host de build.  AUTO -> host
            // del build como estimacion (el multiversion-cpuid es futuro).
            const std::string fi = result.count("float-isa")
                                       ? result["float-isa"].as<std::string>()
                                       : std::string("sse2");
            if (fi == "avx")
                copts.aot_vec_width = 32;
            else if (fi == "avx512")
                copts.aot_vec_width = 64;
            else if (fi == "auto") {
                // AUTO: multiversion por cpuid en runtime.  El matcher hornea el
                // chunk con estrategia dual (element-wise 64, reduccion 16) para
                // que UN IR compile a las 3 variantes; el driver las compila 3x.
                copts.aot_auto_vec = true;
                copts.aot_vec_width = 64; // element-wise max (la reduccion usa 16)
            } else
                copts.aot_vec_width = 16; // sse2 / x87
        }
        // Instrumentacion: aplica al IR independientemente del target
        // (bytecode VM, JIT, port C, etc.).  Validar valor aqui mismo.
        copts.instrument_mode = result["instrument"].as<std::string>();
        if (copts.instrument_mode != "none" &&
            copts.instrument_mode != "trace" &&
            copts.instrument_mode != "profile") {
            std::cerr << "[vex] --instrument invalido: "
                      << copts.instrument_mode
                      << " (valores: none|trace|profile)\n";
            return 2;
        }
        // --vex-debug: emite `// @line N` en el .vel y genera la
        // seccion debug en el .velb final.  Por defecto OFF: el ejecutable
        // queda mas pequeno y la compilacion mas rapida.
        copts.emit_debug = (result.count("vex-debug") > 0);
        // Flags de diagramas: cada uno habilita la generacion del diagrama
        // correspondiente en CompileResult, segun el formato elegido por
        // --diagram-format.  Se escriben a archivos al final del bloque.
        copts.dump_mermaid_ast = emit_mermaid && diag_vex;
        copts.dump_mermaid_ir_pre = emit_mermaid && diag_ir_pre;
        copts.dump_mermaid_ir_post = emit_mermaid && diag_ir_post;
        copts.dump_mermaid_vel = emit_mermaid && diag_vel;
        copts.dump_graphviz_ast = emit_graphviz && diag_vex;
        copts.dump_graphviz_ir_pre = emit_graphviz && diag_ir_pre;
        copts.dump_graphviz_ir_post = emit_graphviz && diag_ir_post;
        copts.dump_graphviz_vel = emit_graphviz && diag_vel;
        copts.dump_html_ast = emit_html && diag_vex;
        copts.dump_html_ir_pre = emit_html && diag_ir_pre;
        copts.dump_html_ir_post = emit_html && diag_ir_post;
        copts.dump_html_vel = emit_html && diag_vel;
        // --diagram-cost: anotar el coste Big-O en los diagramas IR.
        copts.annotate_cost = result.count("diagram-cost") > 0;

        // Flag --port=<lang>: si presente, configurar el transpiler IR ->
        // codigo. El frontend Vex llama al port::Transpiler tras la fase de
        // optimizacion del IR; el resultado queda en cr.port_text para que aqui
        // lo escribamos a archivo con la extension correspondiente.
        if (result.count("port")) {
            copts.port_target = result["port"].as<std::string>();
            // Validacion temprana del target: solo 'c' en v1.
            if (copts.port_target != "c") {
                std::cerr << "[port] Target desconocido: '" << copts.port_target
                          << "'.  Soportados: c.\n";
                return EXIT_FAILURE;
            }
            // Parsear los flags --port-gc / --port-exc / --port-types.
            const std::string &gc_s = result["port-gc"].as<std::string>();
            if (!port::parse_gc_mode(gc_s, copts.port_options.gc)) {
                std::cerr << "[port] --port-gc invalido: " << gc_s
                          << " (valores: none|vesta|boehm)\n";
                return EXIT_FAILURE;
            }
            const std::string &exc_s = result["port-exc"].as<std::string>();
            if (!port::parse_exc_mode(exc_s, copts.port_options.exc)) {
                std::cerr << "[port] --port-exc invalido: " << exc_s
                          << " (valores: none|setjmp|returncode)\n";
                return EXIT_FAILURE;
            }
            const std::string &str_s = result["port-strings"].as<std::string>();
            if (!port::parse_string_mode(str_s, copts.port_options.strings)) {
                std::cerr << "[port] --port-strings invalido: " << str_s
                          << " (valores: raw|managed)\n";
                return 2;
            }
            const std::string &ty_s = result["port-types"].as<std::string>();
            if (!port::parse_type_style(ty_s, copts.port_options.types)) {
                std::cerr << "[port] --port-types invalido: " << ty_s
                          << " (valores: stdint|builtin)\n";
                return EXIT_FAILURE;
            }
            copts.port_options.freestanding =
                result.count("port-freestanding") > 0;
            copts.port_options.stdlib_port_c_dir =
                result["port-stdlib-dir"].as<std::string>();
            copts.port_options.arch_target =
                result["port-arch"].as<std::string>();
            copts.port_options.aggressive_opt =
                (result.count("port-no-aggressive") == 0);
            copts.port_options.module_name = mod_name;
            copts.port_options.source_path = vex_path;
        }

        /* === Phase MC.12: cache persistente para @Macros ===
         *
         * Cache dir: `./.cache/vex/` (cwd-relative).  Key = FNV-1a 64
         * sobre (cache_format_version + vex_path + vex_source).  File =
         * `.cache/vex/<hex_key>.velb`.
         *
         * Flow:
         *   1. Compute key.
         *   2. Si el cache hit existe, setear @c VESTA_MC_PREBUILT antes
         *      de la primera invocacion de @c compile_vex_source.  Una
         *      sola compilacion + VM eval directo.
         *   3. Si miss + el modulo tiene @Macros, ejecutar pase 1 (AST
         *      eval), persistir el .velb resultante al cache_path, luego
         *      pase 2 con @c VESTA_MC_PREBUILT seteado.  Dos compiles
         *      en frio.  Las siguientes corridas son cache-hit -> 1 compile.
         *
         * Cache invalidation: implicita.  Cualquier cambio en el byte
         * stream del fuente (incluso un comentario o whitespace) genera
         * un key distinto y por tanto miss.  Sin sweeper automatico --
         * el usuario puede borrar `.cache/vex/` para limpiar manualmente.
         */
        const uint8_t cache_format_version =
            2; /* Bump por MC.14 macro-scoped key */

        /* Phase MC.14: macro-scoped cache key.  Hashea SOLO los rangos
         * source que contienen declaraciones `@Macro` (incluyendo
         * anotaciones precedentes como @Pure/@Inline).  Cambios en
         * codigo NO-macro (main, helpers no marcados, comentarios
         * fuera de macros) NO invalidan el cache.
         *
         * Implementacion text-scan: detecta `@Macro`, scanea atras al
         * inicio de linea + lineas de anotacion previas, scanea adelante
         * a la llave de cierre balanceada (saltando strings y comentarios
         * line-style).  Heuristica suficiente para 99% de codigo Vex
         * estandar.  Edge cases (comentarios de bloque con `@Macro`
         * dentro, etc.) producen cache miss falso pero no incorrectness.
         *
         * Si NO hay @Macros en el fuente, fallback a hash full-source
         * (el cache no se usa en ese caso de todos modos). */
        const auto find_macro_ranges = [](const std::string &src)
            -> std::vector<std::pair<size_t, size_t>> {
            std::vector<std::pair<size_t, size_t>> ranges;
            size_t pos = 0;
            while ((pos = src.find("@Macro", pos)) != std::string::npos) {
                /* Skip si @Macro aparece dentro de un literal o comentario.
                 * Heuristica simple: si los 2 chars previos son "//" o "/ *",
                 * salta.  Mejorable; v1 acepta falsos positivos. */
                /* Scan back al inicio de linea + lineas de anotacion. */
                size_t line_start = pos;
                while (line_start > 0 && src[line_start - 1] != '\n')
                    --line_start;
                /* Incluir anotaciones precedentes (@Pure, @Inline, etc.) */
                while (line_start > 0) {
                    size_t prev_end = line_start - 1;
                    if (prev_end == 0 || src[prev_end] != '\n') break;
                    size_t prev_start = prev_end;
                    while (prev_start > 0 && src[prev_start - 1] != '\n')
                        --prev_start;
                    size_t k = prev_start;
                    while (k < prev_end && (src[k] == ' ' || src[k] == '\t'))
                        ++k;
                    if (k < prev_end && src[k] == '@') {
                        line_start = prev_start;
                    } else {
                        break;
                    }
                }
                /* Avanzar hasta llave de apertura del body. */
                size_t bs = src.find('{', pos + 6);
                if (bs == std::string::npos) break;
                /* Scan balanced close, skipping strings/line-comments. */
                int depth = 1;
                size_t i = bs + 1;
                while (i < src.size() && depth > 0) {
                    char c = src[i];
                    if (c == '"') {
                        ++i;
                        while (i < src.size() && src[i] != '"') {
                            if (src[i] == '\\' && i + 1 < src.size()) ++i;
                            ++i;
                        }
                        ++i;
                        continue;
                    }
                    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
                        while (i < src.size() && src[i] != '\n')
                            ++i;
                        continue;
                    }
                    if (c == '{')
                        ++depth;
                    else if (c == '}')
                        --depth;
                    ++i;
                }
                ranges.emplace_back(line_start, i);
                pos = i;
            }
            return ranges;
        };

        /* Phase MC.16: per-macro manifest diagnostico.  Computa hash
         * por macro y guarda un manifest junto al .velb cacheado.  En
         * un cache miss, compara con el manifest anterior (si existe)
         * para reportar QUE macros cambiaron.
         *
         * Foundational para true per-macro relink: cuando el linker
         * soporte compilacion incremental, esta info dira que macros
         * recompilar.  Por ahora solo diagnostico via VESTA_MC_VERBOSE. */
        const auto find_macro_ranges_with_names = [&](const std::string &src)
            -> std::vector<std::tuple<std::string, size_t, size_t>> {
            std::vector<std::tuple<std::string, size_t, size_t>> ranges;
            size_t pos = 0;
            while ((pos = src.find("@Macro", pos)) != std::string::npos) {
                size_t line_start = pos;
                while (line_start > 0 && src[line_start - 1] != '\n')
                    --line_start;
                while (line_start > 0) {
                    size_t prev_end = line_start - 1;
                    if (prev_end == 0 || src[prev_end] != '\n') break;
                    size_t prev_start = prev_end;
                    while (prev_start > 0 && src[prev_start - 1] != '\n')
                        --prev_start;
                    size_t k = prev_start;
                    while (k < prev_end && (src[k] == ' ' || src[k] == '\t'))
                        ++k;
                    if (k < prev_end && src[k] == '@') {
                        line_start = prev_start;
                    } else {
                        break;
                    }
                }
                size_t bs = src.find('{', pos + 6);
                if (bs == std::string::npos) break;
                /* Extraer nombre: tras @Macro hay typically:
                 *   @Macro\ncomptime string NAME(args) { ... }
                 * El nombre es el identifier ANTES del '('. */
                std::string macro_name;
                {
                    size_t paren = src.find('(', pos + 6);
                    if (paren != std::string::npos && paren < bs) {
                        size_t name_end = paren;
                        while (name_end > pos + 6 &&
                               (src[name_end - 1] == ' ' ||
                                src[name_end - 1] == '\t' ||
                                src[name_end - 1] == '\n')) {
                            --name_end;
                        }
                        size_t name_start = name_end;
                        while (name_start > pos + 6 &&
                               (std::isalnum(static_cast<unsigned char>(
                                    src[name_start - 1])) ||
                                src[name_start - 1] == '_')) {
                            --name_start;
                        }
                        if (name_end > name_start) {
                            macro_name =
                                src.substr(name_start, name_end - name_start);
                        }
                    }
                }
                if (macro_name.empty()) macro_name = "<anon>";

                int depth = 1;
                size_t i = bs + 1;
                while (i < src.size() && depth > 0) {
                    char c = src[i];
                    if (c == '"') {
                        ++i;
                        while (i < src.size() && src[i] != '"') {
                            if (src[i] == '\\' && i + 1 < src.size()) ++i;
                            ++i;
                        }
                        ++i;
                        continue;
                    }
                    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
                        while (i < src.size() && src[i] != '\n')
                            ++i;
                        continue;
                    }
                    if (c == '{')
                        ++depth;
                    else if (c == '}')
                        --depth;
                    ++i;
                }
                ranges.emplace_back(std::move(macro_name), line_start, i);
                pos = i;
            }
            return ranges;
        };

        const uint64_t cache_key = [&]() -> uint64_t {
            constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
            constexpr uint64_t FNV_PRIME = 1099511628211ULL;
            uint64_t h = FNV_OFFSET;
            h ^= cache_format_version;
            h *= FNV_PRIME;
            for (char c : vex_path) {
                h ^= static_cast<uint8_t>(c);
                h *= FNV_PRIME;
            }
            h ^= 0xFFu;
            h *= FNV_PRIME;
            const auto ranges = find_macro_ranges(vex_source);
            if (ranges.empty()) {
                /* Sin macros: hash full-source.  El cache no se usa
                 * (has_lowerable_macros=false impedira el populate),
                 * pero un key estable previene colisiones si el usuario
                 * compila el mismo path con/sin macros sobre el mismo
                 * archivo. */
                for (char c : vex_source) {
                    h ^= static_cast<uint8_t>(c);
                    h *= FNV_PRIME;
                }
            } else {
                /* Hash solo los rangos macro + separadores. */
                for (const auto &r : ranges) {
                    const size_t end = (r.second < vex_source.size())
                                           ? r.second
                                           : vex_source.size();
                    for (size_t i = r.first; i < end; ++i) {
                        h ^= static_cast<uint8_t>(vex_source[i]);
                        h *= FNV_PRIME;
                    }
                    h ^= 0xFEu; /* separador entre macros distintos. */
                    h *= FNV_PRIME;
                }
            }
            return h;
        }();
        char cache_key_hex[17];
        std::snprintf(cache_key_hex, sizeof(cache_key_hex), "%016llx",
                      static_cast<unsigned long long>(cache_key));
        const std::string cache_dir = ".cache/vex";
        const std::string cache_prefix = cache_dir + "/" + cache_key_hex;
        const std::string cache_path = cache_prefix + ".velb";
        const bool cache_hit = std::filesystem::exists(cache_path);
        const bool user_already_set_prebuilt =
            (std::getenv("VESTA_MC_PREBUILT") != nullptr);
        const bool verbose_mc = (std::getenv("VESTA_MC_VERBOSE") != nullptr &&
                                 std::getenv("VESTA_MC_VERBOSE")[0] == '1');

        /* CACHE HIT path: setear env var ANTES del primer compile_vex_source.
         * Solo 1 invocacion de compile, con VM eval activo desde el inicio. */
        if (cache_hit && !user_already_set_prebuilt) {
#if defined(_WIN32)
            _putenv_s("VESTA_MC_PREBUILT", cache_path.c_str());
#else
            setenv("VESTA_MC_PREBUILT", cache_path.c_str(), 1);
#endif
            if (verbose_mc) {
                std::cerr << "[mc-cache] hit: " << cache_path << "\n";
            }
        }

        // Phase M5.B: PROJECT CACHE LOOKUP.  Si el source tiene imports,
        // intentar cache hit a nivel @c .velb final.  Si todos los hashes
        // de root + deps recursivos coinciden con los cacheados, copiar
        // el @c .velb cacheado al output y SALTAR todo el compile +
        // link.  Desactivable via @c VEX_NO_PROJECT_CACHE=1.
        const bool project_cache_enabled = []() {
            const char *v = std::getenv("VEX_NO_PROJECT_CACHE");
            return !(v && v[0] == '1');
        }();
        const bool project_cache_verbose = []() {
            const char *v = std::getenv("VEX_VERBOSE_PROJECT_CACHE");
            return v && v[0] == '1';
        }();
        const bool has_imports = vex::vex_source_has_imports(vex_source);

        vex::ProjectCacheKey pck;
        pck.opt_level = copts.opt_level;
        pck.emit_debug = copts.emit_debug;
        pck.vex_base = 0; // no usado por compile_vex_project; queda 0
        pck.instrument_mode = copts.instrument_mode;
        pck.port_target = copts.port_target;
        const uint32_t opts_hash = vex::project_cache_opts_hash(pck);

        // Path canonico del root para el cache key.
        std::string canonical_root;
        try {
            canonical_root =
                std::filesystem::weakly_canonical(vex_path).string();
        } catch (...) {
            canonical_root = vex_path;
        }
        const std::string pc_dir = vex::default_project_cache_dir();
        const std::string pc_path =
            vex::project_cache_path(canonical_root, pc_dir);

        // Artefactos que SOLO se producen recorriendo el pipeline completo
        // (no estan en el .velb cacheado): diagramas (mmd/dot/html), dump de
        // IR (--vex-emit-ir) y transpile (--port).  Si el usuario los pide,
        // saltamos el hit del project-cache para forzar la compilacion y que
        // se generen; de lo contrario el cache hit los omitiria en silencio.
        const bool wants_pipeline_artifacts =
            diag_vex || diag_ir_pre || diag_ir_post || diag_vel || emit_ir ||
            !copts.port_target.empty();

        if (project_cache_enabled && has_imports && !wants_pipeline_artifacts) {
            uint32_t cached_opts_hash = 0;
            std::vector<vex::ProjectCacheDep> cached_deps;
            std::vector<uint8_t> cached_velb;
            if (vex::project_cache_load(pc_path, cached_opts_hash, cached_deps,
                                        cached_velb) &&
                cached_opts_hash == opts_hash && !cached_deps.empty() &&
                !cached_velb.empty() &&
                vex::project_cache_validate(cached_deps)) {
                // HIT: escribir el .velb cacheado al output y salir.
                const std::string out_velb = out_prefix + ".velb";
                std::ofstream f(out_velb, std::ios::binary);
                if (f) {
                    f.write(reinterpret_cast<const char *>(cached_velb.data()),
                            static_cast<std::streamsize>(cached_velb.size()));
                    if (f.good()) {
                        if (project_cache_verbose) {
                            std::cerr << "[vex-project-cache] hit: " << pc_path
                                      << " -> " << out_velb << " ("
                                      << cached_velb.size() << " bytes)\n";
                        }
                        return EXIT_SUCCESS;
                    }
                }
                // Si fallo escribir, caemos al compile normal (no es fatal).
                if (project_cache_verbose) {
                    std::cerr << "[vex-project-cache] hit pero write fallo, "
                                 "recompile\n";
                }
            } else if (project_cache_verbose) {
                std::cerr << "[vex-project-cache] miss: " << pc_path << "\n";
            }
        }

        // Phase M.2.e: si el source tiene `import`, dispatch al
        // compilador multi-modulo.  Este construye el dep graph,
        // compila cada dep en topo order, inyecta .vexi, y mergea
        // todas las funciones en un solo .vel.
        vex::CompileResult cr =
            has_imports ? vex::compile_vex_project(vex_path, copts)
                        : vex::compile_vex_source(vex_source, vex_path, copts);

        /* Phase MC.16+: diagnostico de macros que el lowering rechazo
         * por usar builtins comptime-only no aliasables (comptime_compile,
         * static_assert, etc.) o comptime globals.  Estos macros caen al
         * AST evaluator y NO se benefician del cache/VM/memo.  Imprimir
         * via VESTA_MC_VERBOSE para que el usuario sepa por que. */
        if (verbose_mc && cr.ok && !cr.macro_skip_reasons.empty()) {
            for (const auto &sk : cr.macro_skip_reasons) {
                std::cerr << "[mc-lower] " << sk.first << ": AST-only ("
                          << sk.second << ")\n";
            }
        }

        /* Limpiar env var si lo seteamos arriba (cache hit). */
        if (cache_hit && !user_already_set_prebuilt) {
#if defined(_WIN32)
            _putenv_s("VESTA_MC_PREBUILT", "");
#else
            unsetenv("VESTA_MC_PREBUILT");
#endif
        }

        if (!cr.ok) {
            for (const auto &d : cr.diagnostics.all()) {
                vex::print_diagnostic(std::cerr, d);
            }
            return EXIT_FAILURE;
        }

        // ------------------------------------------------------------------
        // Phase AOT (Paso 1): modo -m aot.  Analiza la compatibilidad nativa
        // del modulo (sin emitir binario aun -- eso es el Paso 2: codegen
        // HOST_LEAF -> ObjectWriter).  El IR optimizado viaja en
        // cr.ir_section_bytes (mismo @ir que consume el JIT); se deserializa
        // y se pasa al analizador AOT.1.
        // ------------------------------------------------------------------
        if (aot_mode) {
            aot::AotTarget tgt;
            tgt.tier = aot_tier;
            tgt.freestanding = aot_freestanding;
            // AOT.2.d: roles cubiertos por @AllocatorOverride / @PanicHandler
            // -> admiten su LIBC_MAPPED tambien en --freestanding.
            tgt.alloc_provided = !cr.aot_alloc_sym.empty();
            tgt.free_provided = !cr.aot_free_sym.empty();
            tgt.panic_provided = !cr.aot_panic_sym.empty();
            tgt.exceptions_enabled = !aot_no_exceptions; // C3: configurable

            const char *tier_name = (aot_tier == aot::Tier::BARE)    ? "bare"
                                    : (aot_tier == aot::Tier::EMBED) ? "embed"
                                                                     : "full";

            if (cr.ir_module_cache_bytes.empty()) {
                std::cerr
                    << "[aot] el modulo no produjo IR; nada que compilar a "
                       "nativo.\n";
                return EXIT_FAILURE;
            }

            // Modulo COMPLETO (functions + static_data + globals): el codegen
            // AOT necesita el static_data para materializar los literales en
            // .rodata.
            ir::IrModule aot_mod;
            if (!ir::parse_ir_module_cache(cr.ir_module_cache_bytes, aot_mod)) {
                std::cerr
                    << "[aot] no se pudo deserializar el IR del modulo.\n";
                return EXIT_FAILURE;
            }

            // ----------------------------------------------------------------
            // Auto-bundle del runtime de excepciones (stdlib/vex/vex_exc.vex).
            // Si el modulo usa try/catch/throw (THROW o CALL __vex_setjmp en el
            // IR) y no define el runtime el mismo, lo compilamos inline (mismo
            // native_poo + el @Target ya seleccionado para el target AOT) y
            // FUSIONAMOS sus funciones + la seccion .vexexc en aot_mod -> el .o
            // queda autocontenido (no hay que enlazar vex_exc.o a mano).  El
            // dead-elim posterior conserva solo las __vex_* realmente usadas.
            // Removible con --no-exceptions (exceptions_enabled=false).
            // ----------------------------------------------------------------
            if (!aot_no_exceptions) {
                bool uses_exc = false, defines_exc = false;
                for (const auto &af : aot_mod.functions) {
                    if (af.name == "__vex_setjmp") defines_exc = true;
                    for (const auto &b : af.blocks)
                        for (const auto &ins : b.instrs)
                            if (ins.op == ir::IrOp::THROW ||
                                (ins.op == ir::IrOp::CALL &&
                                 ins.func_name == "__vex_setjmp"))
                                uses_exc = true;
                }
                if (uses_exc && !defines_exc) {
                    // Localizar vex_exc.vex: junto al exe (instalacion) o en el
                    // repo (dev: build_dir/../stdlib/vex) o cwd.
                    const std::string exe_dir =
                        std::filesystem::path(fs::get_executable_path())
                            .parent_path()
                            .string();
                    const std::vector<std::string> cands = {
                        exe_dir + "/stdlib/vex/vex_exc.vex",
                        exe_dir + "/../stdlib/vex/vex_exc.vex",
                        "stdlib/vex/vex_exc.vex"};
                    std::string ve_path;
                    for (const auto &c : cands)
                        if (std::filesystem::exists(c)) {
                            ve_path = c;
                            break;
                        }
                    if (ve_path.empty()) {
                        std::cerr << "[aot] usa excepciones pero no encuentro "
                                     "stdlib/vex/vex_exc.vex (enlazalo a mano o "
                                     "compila con --no-exceptions).\n";
                        return EXIT_FAILURE;
                    }
                    std::ifstream vef(ve_path);
                    std::string ve_src((std::istreambuf_iterator<char>(vef)),
                                       std::istreambuf_iterator<char>());
                    vex::CompileOptions ve_opts;
                    ve_opts.module_name = "vex_exc";
                    ve_opts.opt_level = 2;
                    ve_opts.native_poo = true;
                    ve_opts.exceptions_enabled = true;
                    // Mismo target bits que el modulo principal (el @Naked
                    // setjmp/longjmp x86-32 debe ensamblarse en mode32).
                    ve_opts.asm_target_bits = copts.asm_target_bits;
                    vex::CompileResult ve_cr =
                        vex::compile_vex_source(ve_src, ve_path, ve_opts);
                    ir::IrModule ve_mod;
                    if (!ve_cr.ok || ve_cr.ir_module_cache_bytes.empty() ||
                        !ir::parse_ir_module_cache(ve_cr.ir_module_cache_bytes,
                                                   ve_mod)) {
                        std::cerr << "[aot] no pude compilar el runtime de "
                                     "excepciones vex_exc.vex.\n";
                        return EXIT_FAILURE;
                    }
                    // Merge (mismo patron que compiler_project): remap de
                    // STR_LIT_ADDR/code.s_N por el offset del static_data, luego
                    // append de funciones (las que no existan ya) + static_data
                    // + globals + native_imports.  vex_exc no tiene literales,
                    // pero el remap es correcto en general (defensa).
                    const uint64_t sd_off =
                        static_cast<uint64_t>(aot_mod.static_data.size());
                    std::unordered_set<std::string> have;
                    for (const auto &af : aot_mod.functions)
                        have.insert(af.name);
                    for (auto &fn : ve_mod.functions) {
                        if (sd_off != 0)
                            for (auto &bb : fn.blocks)
                                for (auto &ins : bb.instrs)
                                    if (ins.op == ir::IrOp::STR_LIT_ADDR)
                                        ins.imm += sd_off;
                        if (!have.count(fn.name))
                            aot_mod.functions.push_back(std::move(fn));
                    }
                    aot_mod.static_data.append_raw_entries(
                        std::move(ve_mod.static_data));
                    for (auto &gv : ve_mod.globals)
                        aot_mod.globals.emplace(gv.first, gv.second);
                    for (auto &ni : ve_mod.native_imports)
                        aot_mod.register_native_import(ni.lib, ni.name);
                    std::cout << "[aot] runtime de excepciones "
                                 "(stdlib/vex/vex_exc.vex) incluido en el "
                                 "objeto.\n";
                }
            }

            // ----------------------------------------------------------------
            // Auto-bundle del runtime de monitores (stdlib/vex/vex_sync.vex).
            // Si el modulo usa `synchronized`/`monitor`, el lowering native_poo
            // emite CALL __vex_monenter/__vex_monexit (monitor reentrante inline
            // en el objeto, palabra en obj+16) en vez de las IR ops MONENTER/
            // MONEXIT.  Fusionamos vex_sync.vex (atomic CAS + tid nativos) ->
            // el .o queda autocontenido.  El dead-elim conserva solo lo usado.
            // ----------------------------------------------------------------
            {
                bool uses_sync = false, defines_sync = false;
                for (const auto &af : aot_mod.functions) {
                    if (af.name == "__vex_monenter") defines_sync = true;
                    for (const auto &b : af.blocks)
                        for (const auto &ins : b.instrs)
                            if (ins.op == ir::IrOp::CALL &&
                                (ins.func_name == "__vex_monenter" ||
                                 ins.func_name == "__vex_monexit"))
                                uses_sync = true;
                }
                if (uses_sync && !defines_sync) {
                    const std::string exe_dir =
                        std::filesystem::path(fs::get_executable_path())
                            .parent_path()
                            .string();
                    const std::vector<std::string> cands = {
                        exe_dir + "/stdlib/vex/vex_sync.vex",
                        exe_dir + "/../stdlib/vex/vex_sync.vex",
                        "stdlib/vex/vex_sync.vex"};
                    std::string vs_path;
                    for (const auto &c : cands)
                        if (std::filesystem::exists(c)) {
                            vs_path = c;
                            break;
                        }
                    if (vs_path.empty()) {
                        std::cerr << "[aot] usa synchronized pero no encuentro "
                                     "stdlib/vex/vex_sync.vex (enlazalo a mano).\n";
                        return EXIT_FAILURE;
                    }
                    std::ifstream vsf(vs_path);
                    std::string vs_src((std::istreambuf_iterator<char>(vsf)),
                                       std::istreambuf_iterator<char>());
                    vex::CompileOptions vs_opts;
                    vs_opts.module_name = "vex_sync";
                    vs_opts.opt_level = 2;
                    vs_opts.native_poo = true;
                    vs_opts.asm_target_bits = copts.asm_target_bits;
                    vex::CompileResult vs_cr =
                        vex::compile_vex_source(vs_src, vs_path, vs_opts);
                    ir::IrModule vs_mod;
                    if (!vs_cr.ok || vs_cr.ir_module_cache_bytes.empty() ||
                        !ir::parse_ir_module_cache(vs_cr.ir_module_cache_bytes,
                                                   vs_mod)) {
                        std::cerr << "[aot] no pude compilar el runtime de "
                                     "monitores vex_sync.vex.\n";
                        return EXIT_FAILURE;
                    }
                    const uint64_t sd_off =
                        static_cast<uint64_t>(aot_mod.static_data.size());
                    std::unordered_set<std::string> have;
                    for (const auto &af : aot_mod.functions)
                        have.insert(af.name);
                    for (auto &fn : vs_mod.functions) {
                        if (sd_off != 0)
                            for (auto &bb : fn.blocks)
                                for (auto &ins : bb.instrs)
                                    if (ins.op == ir::IrOp::STR_LIT_ADDR)
                                        ins.imm += sd_off;
                        if (!have.count(fn.name))
                            aot_mod.functions.push_back(std::move(fn));
                    }
                    aot_mod.static_data.append_raw_entries(
                        std::move(vs_mod.static_data));
                    for (auto &gv : vs_mod.globals)
                        aot_mod.globals.emplace(gv.first, gv.second);
                    for (auto &ni : vs_mod.native_imports)
                        aot_mod.register_native_import(ni.lib, ni.name);
                    std::cout << "[aot] runtime de monitores "
                                 "(stdlib/vex/vex_sync.vex) incluido en el "
                                 "objeto.\n";
                }
            }

            // ----------------------------------------------------------------
            // Auto-bundle del runtime de asincronia (stdlib/vex/vex_async.vex).
            // Si el modulo usa spawn/future/await/fulfill, el lowering
            // native_poo emite CALL __vex_spawn/__vex_future_new/__vex_await/
            // __vex_fulfill (scheduler cooperativo, no hilos del SO).
            // Fusionamos vex_async.vex -> .o autocontenido.
            // ----------------------------------------------------------------
            {
                bool uses_async = false, defines_async = false;
                for (const auto &af : aot_mod.functions) {
                    if (af.name == "__vex_spawn") defines_async = true;
                    for (const auto &b : af.blocks)
                        for (const auto &ins : b.instrs)
                            if (ins.op == ir::IrOp::CALL &&
                                (ins.func_name == "__vex_spawn" ||
                                 ins.func_name == "__vex_future_new" ||
                                 ins.func_name == "__vex_await" ||
                                 ins.func_name == "__vex_fulfill" ||
                                 ins.func_name == "__vex_msgsend" ||
                                 ins.func_name == "__vex_msgrecv"))
                                uses_async = true;
                }
                if (uses_async && !defines_async) {
                    const std::string exe_dir =
                        std::filesystem::path(fs::get_executable_path())
                            .parent_path()
                            .string();
                    const std::vector<std::string> cands = {
                        exe_dir + "/stdlib/vex/vex_async.vex",
                        exe_dir + "/../stdlib/vex/vex_async.vex",
                        "stdlib/vex/vex_async.vex"};
                    std::string va_path;
                    for (const auto &c : cands)
                        if (std::filesystem::exists(c)) {
                            va_path = c;
                            break;
                        }
                    if (va_path.empty()) {
                        std::cerr << "[aot] usa spawn/async pero no encuentro "
                                     "stdlib/vex/vex_async.vex (enlazalo a mano).\n";
                        return EXIT_FAILURE;
                    }
                    std::ifstream vaf(va_path);
                    std::string va_src((std::istreambuf_iterator<char>(vaf)),
                                       std::istreambuf_iterator<char>());
                    vex::CompileOptions va_opts;
                    va_opts.module_name = "vex_async";
                    va_opts.opt_level = 2;
                    va_opts.native_poo = true;
                    va_opts.asm_target_bits = copts.asm_target_bits;
                    vex::CompileResult va_cr =
                        vex::compile_vex_source(va_src, va_path, va_opts);
                    ir::IrModule va_mod;
                    if (!va_cr.ok || va_cr.ir_module_cache_bytes.empty() ||
                        !ir::parse_ir_module_cache(va_cr.ir_module_cache_bytes,
                                                   va_mod)) {
                        std::cerr << "[aot] no pude compilar el runtime de "
                                     "asincronia vex_async.vex.\n";
                        return EXIT_FAILURE;
                    }
                    const uint64_t sd_off =
                        static_cast<uint64_t>(aot_mod.static_data.size());
                    std::unordered_set<std::string> have;
                    for (const auto &af : aot_mod.functions)
                        have.insert(af.name);
                    for (auto &fn : va_mod.functions) {
                        if (sd_off != 0)
                            for (auto &bb : fn.blocks)
                                for (auto &ins : bb.instrs)
                                    if (ins.op == ir::IrOp::STR_LIT_ADDR)
                                        ins.imm += sd_off;
                        if (!have.count(fn.name))
                            aot_mod.functions.push_back(std::move(fn));
                    }
                    aot_mod.static_data.append_raw_entries(
                        std::move(va_mod.static_data));
                    for (auto &gv : va_mod.globals)
                        aot_mod.globals.emplace(gv.first, gv.second);
                    for (auto &ni : va_mod.native_imports)
                        aot_mod.register_native_import(ni.lib, ni.name);
                    std::cout << "[aot] runtime de asincronia "
                                 "(stdlib/vex/vex_async.vex) incluido en el "
                                 "objeto.\n";
                }
            }

            // ----------------------------------------------------------------
            // Auto-bundle del runtime de I/O (stdlib/vex/vex_io.vex).
            // Si el modulo usa print/println, el lowering native_poo emite
            // CALLN `vex_bare_io:__vex_*` (write + formateadores).  En vez de
            // exigir enlazar vesta_io_bare.o (libc/printf), fusionamos un
            // runtime Vex puro que escribe via FFI a write/_write (fd 1, sin
            // printf -> mas rapido) y formatea los numeros en Vex.  Tras el
            // merge reescribimos esas CALLN a CALL plano (__vex_*) -> resuelven
            // a las funciones bundle-adas (no quedan como import externo).  El
            // usuario puede REDEFINIR cualquier __vex_* en su modulo: si lo
            // hace, el lowering ya emitio CALL a la suya y no detectamos la
            // CALLN -> no se bundle-a.  Removible con --freestanding (el
            // usuario aporta los __vex_*) o --no-io.
            // ----------------------------------------------------------------
            if (!aot_no_io && !aot_freestanding) {
                const std::string io_pfx = "vex_bare_io:";
                bool uses_io = false, defines_io = false;
                for (const auto &af : aot_mod.functions) {
                    if (af.name == "__vex_write") defines_io = true;
                    for (const auto &b : af.blocks)
                        for (const auto &ins : b.instrs)
                            if (ins.op == ir::IrOp::CALLN &&
                                ins.func_name.rfind(io_pfx, 0) == 0)
                                uses_io = true;
                }
                if (uses_io && !defines_io) {
                    const std::string exe_dir =
                        std::filesystem::path(fs::get_executable_path())
                            .parent_path()
                            .string();
                    const std::vector<std::string> cands = {
                        exe_dir + "/stdlib/vex/vex_io.vex",
                        exe_dir + "/../stdlib/vex/vex_io.vex",
                        "stdlib/vex/vex_io.vex"};
                    std::string io_path;
                    for (const auto &c : cands)
                        if (std::filesystem::exists(c)) {
                            io_path = c;
                            break;
                        }
                    if (io_path.empty()) {
                        std::cerr << "[aot] usa print/println pero no encuentro "
                                     "stdlib/vex/vex_io.vex (enlazalo a mano o "
                                     "compila con --freestanding y aporta "
                                     "__vex_write).\n";
                        return EXIT_FAILURE;
                    }
                    std::ifstream iof(io_path);
                    std::string io_src((std::istreambuf_iterator<char>(iof)),
                                       std::istreambuf_iterator<char>());
                    vex::CompileOptions io_opts;
                    io_opts.module_name = "vex_io";
                    io_opts.opt_level = 2;
                    io_opts.native_poo = true;
                    io_opts.asm_target_bits = copts.asm_target_bits;
                    vex::CompileResult io_cr =
                        vex::compile_vex_source(io_src, io_path, io_opts);
                    ir::IrModule io_mod;
                    if (!io_cr.ok || io_cr.ir_module_cache_bytes.empty() ||
                        !ir::parse_ir_module_cache(io_cr.ir_module_cache_bytes,
                                                   io_mod)) {
                        std::cerr << "[aot] no pude compilar el runtime de I/O "
                                     "vex_io.vex.\n";
                        return EXIT_FAILURE;
                    }
                    // Merge (mismo patron que vex_exc): remap de STR_LIT_ADDR por
                    // el offset del static_data + append de funciones nuevas +
                    // static_data + globals + native_imports (write/_write/abort).
                    const uint64_t sd_off =
                        static_cast<uint64_t>(aot_mod.static_data.size());
                    std::unordered_set<std::string> have;
                    for (const auto &af : aot_mod.functions)
                        have.insert(af.name);
                    for (auto &fn : io_mod.functions) {
                        if (sd_off != 0)
                            for (auto &bb : fn.blocks)
                                for (auto &ins : bb.instrs)
                                    if (ins.op == ir::IrOp::STR_LIT_ADDR)
                                        ins.imm += sd_off;
                        if (!have.count(fn.name))
                            aot_mod.functions.push_back(std::move(fn));
                    }
                    aot_mod.static_data.append_raw_entries(
                        std::move(io_mod.static_data));
                    for (auto &gv : io_mod.globals)
                        aot_mod.globals.emplace(gv.first, gv.second);
                    for (auto &ni : io_mod.native_imports)
                        aot_mod.register_native_import(ni.lib, ni.name);
                    // Reescribir CALLN `vex_bare_io:__vex_*` -> CALL `__vex_*`
                    // (las funciones ahora viven en el modulo; resolucion
                    // intra-imagen, sin import externo).
                    for (auto &af : aot_mod.functions)
                        for (auto &b : af.blocks)
                            for (auto &ins : b.instrs)
                                if (ins.op == ir::IrOp::CALLN &&
                                    ins.func_name.rfind(io_pfx, 0) == 0) {
                                    ins.op = ir::IrOp::CALL;
                                    ins.func_name =
                                        ins.func_name.substr(io_pfx.size());
                                }
                    // El I/O es block-buffered: inyectar CALL __vex_flush antes
                    // de cada RET de main para volcar al salir (el _start nativo
                    // no corre atexit).  Asi nada se pierde y el buffering vale
                    // (1 syscall por 4 KiB en vez de 1 por write).
                    for (auto &af : aot_mod.functions) {
                        if (af.name != "main") continue;
                        for (auto &b : af.blocks) {
                            std::vector<ir::IrInstr> ni;
                            ni.reserve(b.instrs.size() + 1);
                            for (auto &ins : b.instrs) {
                                if (ins.op == ir::IrOp::RET) {
                                    ir::IrInstr fl{};
                                    fl.op = ir::IrOp::CALL;
                                    // type por defecto = VOID (0); evitamos la
                                    // macro VOID de windef.h (restaurada tras
                                    // incluir ssa_ir.h).
                                    fl.dst = ir::IR_NO_VALUE;
                                    fl.func_name = "__vex_flush";
                                    fl.source_line = ins.source_line;
                                    ni.push_back(std::move(fl));
                                }
                                ni.push_back(std::move(ins));
                            }
                            b.instrs = std::move(ni);
                        }
                    }
                    std::cout << "[aot] runtime de I/O (stdlib/vex/vex_io.vex) "
                                 "incluido en el objeto.\n";
                }
            }

            // AOT: eliminar funciones MUERTAS (no alcanzables) antes de
            // analizar/compilar.  Sin esto, una factoria-de-closure inlineada
            // en su caller queda como copia standalone no usada y su GC_ALLOC
            // bloquearia la compilacion bare.  Cierre transitivo desde main +
            // funciones @section, siguiendo CALL/TAILCALL/LABEL_ADDR y los
            // sym_refs de las vtablas (static_data).  Conservador: ante la duda
            // se conserva (un drop erroneo daria un error de enlace ruidoso,
            // nunca corrupcion).
            {
                std::unordered_set<std::string> live;
                std::vector<std::string> work;
                auto add_live = [&](const std::string &n) {
                    if (!n.empty() && live.insert(n).second) work.push_back(n);
                };
                std::unordered_map<std::string, const ir::IrFunction *> by_name;
                for (auto &f : aot_mod.functions) by_name[f.name] = &f;
                // Modo LIBRERIA: un modulo sin `main` es una libreria (.o/.so);
                // TODAS sus funciones son raices (son la API publica; no se sabe
                // quien las llamara desde fuera).  El linker final hace el
                // dead-strip del ejecutable (--gc-sections), asi que esto NO
                // infla el .exe: con `main`, la poda desde main mantiene el exe
                // lean (una funcion no alcanzable se elimina).  Sin esto, una
                // libreria Vex compilaba a 0 funciones.
                const bool is_library = (by_name.count("main") == 0);
                add_live("main");
                add_live("__module_init");
                // AUTO multiversion (--float-isa auto): el dispatch (auto_init)
                // referencia las VARIANTES por nombre derivado (NAME$sse2/...),
                // no la funcion base NAME -> la poda no la veria.  Mantener viva
                // toda funcion con ops VEC (la base de la que el driver deriva
                // las 3 variantes); su recorrido arrastra sus callees (p.ej.
                // sum_f64).  Espejo de la siembra del BFS de codegen.
                const bool auto_keep_vec =
                    (result.count("float-isa") &&
                     result["float-isa"].as<std::string>() == "auto");
                auto has_vec = [](const ir::IrFunction &f) -> bool {
                    for (const auto &b : f.blocks)
                        for (const auto &in : b.instrs) {
                            const auto op = in.op;
                            if (op == ir::IrOp::VEC_BINOP ||
                                op == ir::IrOp::VEC_UNOP ||
                                op == ir::IrOp::VEC_FMA ||
                                op == ir::IrOp::VEC_BINOP_S ||
                                op == ir::IrOp::VEC_BCAST ||
                                op == ir::IrOp::VEC_ACC_ZERO ||
                                op == ir::IrOp::VEC_ACC_ADD ||
                                op == ir::IrOp::VEC_ACC_FMA ||
                                op == ir::IrOp::VEC_ACC_STORE ||
                                op == ir::IrOp::VEC_ACC_COMBINE)
                                return true;
                        }
                    return false;
                };
                for (auto &f : aot_mod.functions) {
                    if (!f.section.empty() || f.is_naked || is_library ||
                        (auto_keep_vec && has_vec(f)))
                        add_live(f.name);
                }
                // Raices por vtablas/datos: nombres referenciados en sym_refs.
                for (size_t si = 0; si < aot_mod.static_data.size(); ++si)
                    for (const auto &sr : aot_mod.static_data.meta_at(si).sym_refs)
                        add_live(sr.sym);
                while (!work.empty()) {
                    const std::string cur = work.back();
                    work.pop_back();
                    auto it = by_name.find(cur);
                    if (it == by_name.end()) continue;
                    for (const auto &b : it->second->blocks)
                        for (const auto &ins : b.instrs) {
                            if ((ins.op == ir::IrOp::CALL ||
                                 ins.op == ir::IrOp::TAILCALL ||
                                 ins.op == ir::IrOp::LABEL_ADDR) &&
                                !ins.func_name.empty())
                                add_live(ins.func_name);
                            // THROW baja a CALL __vex_throw en el backend
                            // (no es un CALL en el IR) -> referencia implicita
                            // al runtime de excepciones auto-hospedado.
                            if (ins.op == ir::IrOp::THROW)
                                add_live("__vex_throw");
                            // INLINE_ASM: el cuerpo (func_name) puede referenciar
                            // funciones del modulo via tokens `__vxf_<label>` que
                            // el lowering inserto (inline-asm accede simbolos
                            // propios).  La poda NO ve estas refs porque el asm es
                            // texto opaco -> escanearlas explicitamente para que la
                            // funcion referenciada sobreviva y se compile.  Los
                            // `__vxg_<slot>` son globales (rodata), no funciones.
                            if (ins.op == ir::IrOp::INLINE_ASM &&
                                !ins.func_name.empty()) {
                                const std::string &body = ins.func_name;
                                const std::string tag = "__vxf_";
                                size_t p = 0;
                                while ((p = body.find(tag, p)) !=
                                       std::string::npos) {
                                    size_t s = p + tag.size();
                                    size_t e = s;
                                    while (e < body.size() &&
                                           (std::isalnum((unsigned char)body[e]) ||
                                            body[e] == '_' || body[e] == '$'))
                                        ++e;
                                    if (e > s)
                                        add_live(body.substr(s, e - s));
                                    p = e;
                                }
                            }
                        }
                }
                std::vector<ir::IrFunction> kept;
                kept.reserve(aot_mod.functions.size());
                for (auto &f : aot_mod.functions)
                    if (live.count(f.name)) kept.push_back(std::move(f));
                aot_mod.functions = std::move(kept);
            }

            // AOT: promover envs de closure de heap a stack cuando no escapan
            // (closure-aware escape analysis).  Tras esto, los que no se
            // pudieron promover quedan GC_ALLOC -> aot_analyze los rechaza
            // limpio (nunca heap sin liberar).  Corre solo en el path AOT.
            for (auto &afn : aot_mod.functions)
                (void)ir::ir_pass_promote_closure_env(afn);

            // AOT opcion 1: las closures que escapan CROSS-FUNCTION (env creado
            // en una factoria y retornado; no inlinable -> promote no lo pudo
            // poner en stack) se liberan con RAW_FREE determinista en el dueno
            // terminal.  Lo que no tenga dueno limpio se revierte a GC_ALLOC
            // (aot_analyze lo rechaza -> nunca leak).  Corre tras promote.
            (void)ir::ir_pass_own_closure_envs(aot_mod);

            if (std::getenv("VESTA_AOT_DUMP_IR")) {
                std::cerr << "===== AOT native_poo IR =====\n";
                ir::ir_print(aot_mod, std::cerr);
                std::cerr << "=============================\n";
            }
            aot::AotCompatReport rep = aot::aot_analyze_module(aot_mod, tgt);
            std::cout << "[aot] target=" << tier_name
                      << (aot_freestanding ? " --freestanding" : "") << ": "
                      << aot_mod.functions.size() << " funcion(es), "
                      << rep.ok_functions.size()
                      << " compilable(s) a nativo.\n";


            if (!rep.compatible) {
                std::cerr << rep.render();
                std::cerr
                    << "[aot] modulo NO compilable a nativo en este target "
                       "(ver incompatibilidades arriba).\n";
                return EXIT_FAILURE;
            }

            // Phase AOT.2: re-bajar las ops sintetizadas (RAW_ALLOC/RAW_FREE/
            // PANIC) a CALL a simbolos externos (convencion libc; los resuelve
            // el linker -> el .o NO depende de libc).  Tras esto el selector ve
            // solo CALL.  Se ejecuta DESPUES del gate de analyze para que el
            // chequeo freestanding sobre RAW_ALLOC siga aplicando.
            // AOT.2.d: nombres de simbolo segun
            // @AllocatorOverride/@PanicHandler (vacio = convencion C
            // malloc/free/abort).
            aot::AotLowerConfig lcfg;
            // Detectar si el modulo usa el allocator (RAW_ALLOC/RAW_FREE o el
            // calloc del `new`) para decidir el auto-bundle del slab Vex.
            bool aot_uses_alloc = false;
            for (const auto &af : aot_mod.functions)
                for (const auto &b : af.blocks)
                    for (const auto &in : b.instrs)
                        if (in.op == ir::IrOp::RAW_ALLOC ||
                            in.op == ir::IrOp::RAW_FREE ||
                            ((in.op == ir::IrOp::CALL ||
                              in.op == ir::IrOp::TAILCALL) &&
                             in.func_name == "calloc"))
                            aot_uses_alloc = true;
            bool bundle_mem = false;
            ir::IrModule mem_mod; // poblado si bundle_mem (merge tras aot_lower)
            if (!cr.aot_alloc_sym.empty()) {
                // @AllocatorOverride del usuario: respetarlo (no bundle).
                lcfg.alloc_sym = cr.aot_alloc_sym;
                lcfg.has_alloc_override =
                    true; // __new calloc -> alloc_sym(size)
            } else if (aot_uses_alloc && !aot_no_mem && !aot_freestanding) {
                // Sin @AllocatorOverride del usuario -> el slab Vex
                // (stdlib/vex/vex_mem.vex) es el allocator por DEFECTO, via el
                // MISMO mecanismo @AllocatorOverride (reciclamos la sintaxis):
                // compilamos vex_mem y leemos sus simbolos override
                // (__vex_malloc / __vex_free) genericamente, no hardcoded.  Sin
                // libc malloc/free.  El usuario sustituye con su propio
                // @AllocatorOverride, o lo desactiva con --no-mem.
                const std::string exe_dir =
                    std::filesystem::path(fs::get_executable_path())
                        .parent_path()
                        .string();
                const std::vector<std::string> cands = {
                    exe_dir + "/stdlib/vex/vex_mem.vex",
                    exe_dir + "/../stdlib/vex/vex_mem.vex",
                    "stdlib/vex/vex_mem.vex"};
                std::string mem_path;
                for (const auto &c : cands)
                    if (std::filesystem::exists(c)) {
                        mem_path = c;
                        break;
                    }
                if (mem_path.empty()) {
                    std::cerr << "[aot] usa el allocator pero no encuentro "
                                 "stdlib/vex/vex_mem.vex (enlazalo a mano, usa "
                                 "@AllocatorOverride o compila con --no-mem).\n";
                    return EXIT_FAILURE;
                }
                std::ifstream mf(mem_path);
                std::string mem_src((std::istreambuf_iterator<char>(mf)),
                                    std::istreambuf_iterator<char>());
                vex::CompileOptions mem_opts;
                mem_opts.module_name = "vex_mem";
                mem_opts.opt_level = 2;
                mem_opts.native_poo = true;
                mem_opts.asm_target_bits = copts.asm_target_bits;
                vex::CompileResult mem_cr =
                    vex::compile_vex_source(mem_src, mem_path, mem_opts);
                if (!mem_cr.ok || mem_cr.ir_module_cache_bytes.empty() ||
                    !ir::parse_ir_module_cache(mem_cr.ir_module_cache_bytes,
                                               mem_mod) ||
                    mem_cr.aot_alloc_sym.empty() ||
                    mem_cr.aot_free_sym.empty()) {
                    std::cerr << "[aot] no pude compilar el slab allocator "
                                 "vex_mem.vex (o no expone @AllocatorOverride).\n";
                    return EXIT_FAILURE;
                }
                // Override por defecto = los simbolos que vex_mem declaro con
                // @AllocatorOverride (mismo trato que un override del usuario).
                lcfg.alloc_sym = mem_cr.aot_alloc_sym;
                lcfg.free_sym = mem_cr.aot_free_sym;
                lcfg.has_alloc_override = true; // __new calloc -> alloc_sym(size)
                bundle_mem = true;
            }
            if (!cr.aot_free_sym.empty()) lcfg.free_sym = cr.aot_free_sym;
            if (!cr.aot_panic_sym.empty()) {
                lcfg.panic_sym = cr.aot_panic_sym;
                lcfg.panic_takes_msg = true; // @PanicHandler(msg_addr, len)
            }
            aot::aot_lower_runtime(aot_mod, lcfg);

            // Merge del slab (stdlib/vex/vex_mem.vex, ya compilado en mem_mod)
            // DESPUES de aot_lower: ahora RAW_ALLOC/calloc/RAW_FREE ya son CALL
            // __vex_malloc/__vex_free, asi que el codegen BFS los alcanza desde
            // main.  Mismo patron de merge que vex_io/vex_exc.
            if (bundle_mem) {
                const uint64_t sd_off =
                    static_cast<uint64_t>(aot_mod.static_data.size());
                std::unordered_set<std::string> have;
                for (const auto &af : aot_mod.functions)
                    have.insert(af.name);
                for (auto &fn : mem_mod.functions) {
                    if (sd_off != 0)
                        for (auto &bb : fn.blocks)
                            for (auto &ins : bb.instrs)
                                if (ins.op == ir::IrOp::STR_LIT_ADDR)
                                    ins.imm += sd_off;
                    if (!have.count(fn.name))
                        aot_mod.functions.push_back(std::move(fn));
                }
                aot_mod.static_data.append_raw_entries(
                    std::move(mem_mod.static_data));
                for (auto &gv : mem_mod.globals)
                    aot_mod.globals.emplace(gv.first, gv.second);
                for (auto &ni : mem_mod.native_imports)
                    aot_mod.register_native_import(ni.lib, ni.name);
                std::cout << "[aot] slab allocator (stdlib/vex/vex_mem.vex) "
                             "incluido en el objeto.\n";
            }

            // Bare AOT: NO hay VM stack (rbx no es un ProcessVM*).  Forzar
            // TODAS las ALLOCAs a la pila nativa (host_alloca), incluso las
            // que "escapan" a un CALL -- en nativo el host stack ES
            // addressable cross-call.  Sin esto, un local cuya direccion se
            // pasa a una funcion (p.ej. un buffer para itoa) se aloca con
            // ALLOCA_VM ([rbx+0x40]) -> direccion basura -> SIGSEGV.
            for (auto &afn : aot_mod.functions)
                ir::ir_pass_promote_local_allocas(afn, /*force_all=*/true);

            // ------------------------------------------------------------------
            // Paso 2: codegen nativo (HOST_LEAF) + emision del ejecutable.
            //
            // Hito minimo: compila SOLO `main` (sin CALL ni datos -> bytes
            // position-independent, sin relocations) y sintetiza un _start
            // arch+formato-especifico que llama a main y termina el proceso con
            // su codigo de retorno.  Todo el codegen pasa por el path vreg
            // (TargetRegInfo + selector + encoder), portable a otras arch.
            // ------------------------------------------------------------------

            // Arquitectura objetivo: x86-64 (default) o x86-32 (modo protegido,
            // kernels): 8 GP eax-edi, sin REX, operando 32-bit, regparm(3);
            // subset entero de 32-bit (i32/u32/ptr32).
            bool aot_mode32 = false;
            {
                const std::string a = result["aot-arch"].as<std::string>();
                if (a == "x86-32" || a == "x86_32" || a == "i386")
                    aot_mode32 = true;
                else if (a == "x86-64" || a == "x86_64" || a == "amd64")
                    aot_mode32 = false;
                else {
                    std::cerr << "[aot] --aot-arch desconocido: '" << a
                              << "' (use x86-64 | x86-32).\n";
                    return EXIT_FAILURE;
                }
            }
            const aot::AotArch arch =
                aot_mode32 ? aot::AotArch::X86_32 : aot::AotArch::X86_64;

            // Backend de punto flotante (--float-isa).  Hoy el codegen float
            // (FP-regalloc en XMM, packing-ready) esta en construccion; el flag
            // queda cableado para que el selector elija el backend cuando llegue.
            jit::FloatIsa aot_fisa = jit::FloatIsa::SSE2;
            {
                const std::string f = result["float-isa"].as<std::string>();
                if (f == "sse2" || f == "sse")
                    aot_fisa = jit::FloatIsa::SSE2;
                else if (f == "x87" || f == "fpu")
                    aot_fisa = jit::FloatIsa::X87;
                else if (f == "avx")
                    aot_fisa = jit::FloatIsa::AVX;
                else if (f == "avx512f" || f == "avx512")
                    aot_fisa = jit::FloatIsa::AVX512F;
                else if (f == "auto")
                    aot_fisa = jit::FloatIsa::AUTO;
                else {
                    std::cerr << "[aot] --float-isa desconocido: '" << f
                              << "' (use sse2 | x87 | avx | avx512f | auto).\n";
                    return EXIT_FAILURE;
                }
            }

            // Formato de salida: --format pe|elf, o default por host.
            aot::ObjFormat fmt =
#if defined(_WIN32)
                aot::ObjFormat::PE;
#else
                aot::ObjFormat::ELF;
#endif
            if (result.count("format")) {
                const std::string f = result["format"].as<std::string>();
                if (f == "pe" || f == "PE")
                    fmt = aot::ObjFormat::PE;
                else if (f == "elf" || f == "ELF")
                    fmt = aot::ObjFormat::ELF;
                else {
                    std::cerr << "[aot] --format desconocido: '" << f
                              << "' (use pe|elf).\n";
                    return EXIT_FAILURE;
                }
            }

            // Referencias a datos: PIC (RIP-relativo, default) vs absoluto
            // (--no-pie, requiere base de imagen fija).  Analogo gcc/clang.
            const bool aot_pic = (result.count("no-pie") == 0);

            // --emit exe|obj|shared.
            //   EXEC   : ejecutable standalone con _start (requiere main).
            //   OBJECT : .o/.obj relocatable (sin _start; main global; relocs
            //   como
            //            registros; linkable con ld/gcc/link).
            //   SHARED : .so/.dll (sin _start; exporta TODAS las funciones;
            //   PIC).
            bool emit_obj = false, emit_shared = false, emit_bin = false;
            if (result.count("emit")) {
                const std::string em = result["emit"].as<std::string>();
                if (em == "exe" || em == "exec") {
                } else if (em == "obj" || em == "o")
                    emit_obj = true;
                else if (em == "shared" || em == "dll" || em == "so")
                    emit_shared = true;
                else if (em == "bin" || em == "flat")
                    emit_bin = true;
                else {
                    std::cerr << "[aot] --emit desconocido: '" << em
                              << "' (use exe|obj|shared|bin).\n";
                    return EXIT_FAILURE;
                }
            }
            // --emit shared: ELF (.so) y PE (.dll).
            if (emit_shared && !aot_pic) {
                std::cerr << "[aot] --emit shared requiere PIC; --no-pie no es "
                             "compatible con .so.\n";
                return EXIT_FAILURE;
            }
            // base de carga del binario plano (.bin) -- solo afecta refs
            // absolutas.
            uint64_t bin_base = 0;
            if (result.count("bin-base")) {
                bin_base = std::strtoull(
                    result["bin-base"].as<std::string>().c_str(), nullptr, 0);
            }
            // OBJECT/SHARED/BIN no llevan _start (lo aporta el
            // crt/host/loader).
            const bool no_stub = emit_obj || emit_shared || emit_bin;

            // x86-32: soporta --emit bin (flat), --emit exe (ELF32/PE32) y
            // --emit obj (.o ELF32 -- COFF32 .obj es follow-up).  .so/.dll de
            // 32-bit pendientes.  El objeto conserva la extension .o (linkable
            // con gcc -m32 / ld).
            if (aot_mode32) {
                // --emit obj: .o ELF32 (--format elf) o .obj COFF i386
                // (--format pe), ambos conservando la extension para linkers
                // externos (gcc -m32 / ld / link.exe).
                const bool ok32 = emit_bin || emit_obj ||
                                  (!no_stub /* EXEC: ELF32 o PE32 */);
                if (!ok32) {
                    std::cerr
                        << "[aot] --aot-arch x86-32: soporta --emit bin, "
                           "--emit exe (ELF32 / PE32) o --emit obj (.o ELF32 / "
                           ".obj COFF i386); .so/.dll de 32-bit son "
                           "follow-ups.\n";
                    return EXIT_FAILURE;
                }
            }

            // main: requerido para EXEC y OBJECT; OPCIONAL para SHARED
            // (libreria).
            const ir::IrFunction *main_fn = nullptr;
            for (const auto &fn : aot_mod.functions)
                if (fn.name == "main") {
                    main_fn = &fn;
                    break;
                }
            if (!main_fn && !emit_shared && !emit_bin && !emit_obj) {
                std::cerr
                    << "[aot] no se encontro la funcion 'main' en el modulo.\n";
                return EXIT_FAILURE;
            }

            // _start (arch+formato): solo para EXEC.  OBJECT/SHARED/BIN no
            // llevan _start (lo aporta el crt del linker / el host / el loader
            // externo).
            aot::StartStub stub{};
            if (!no_stub) stub = aot::aot_make_start_stub(arch, fmt);
            if (!no_stub && !stub.ok) {
                std::cerr << "[aot] " << stub.err << "\n";
                return EXIT_FAILURE;
            }

            // Indice nombre -> IrFunction* del modulo (para resolver CALLs).
            std::unordered_map<std::string, const ir::IrFunction *> fn_by_name;
            for (const auto &f : aot_mod.functions)
                fn_by_name[f.name] = &f;

            // Codigo compilado de cada funcion + sus relocations + su seccion.
            struct AotFn {
                std::string name;
                std::vector<uint8_t> bytes;
                std::vector<jit::NativeReloc> relocs;
                std::string section;       // @section ("" = .text)
                std::string section_perms; // "rwx" explicito ("" = convencion)
                int64_t section_at = -1;   // @at(N)
                int32_t section_order = 0x7fffffff; // @order(N)
            };
            std::vector<AotFn> compiled;
            std::unordered_map<std::string, size_t>
                compiled_idx; // name -> compiled[]

            // BFS desde main: compila cada funcion alcanzada por un CALL.
            // Ademas se SIEMBRAN las funciones con @section explicito (el
            // usuario las coloco a proposito; pueden referenciarse SOLO via
            // section_start/end o asm, sin CALL directo -> no dead-strip).  En
            // SHARED (.so) se siembran TODAS las funciones: la libreria las
            // EXPORTA todas.
            std::vector<std::string> work;
            std::unordered_map<std::string, bool> queued;
            if (main_fn) {
                work.push_back("main");
                queued["main"] = true;
            }
            for (const auto &fn : aot_mod.functions) {
                // SHARED siembra todo (exporta todo).  OBJECT sin main es una
                // libreria -> tambien siembra todo (compila todas sus funciones
                // para que un linker las pueda usar).  Con main, OBJECT mantiene
                // el BFS desde main (no regresa programas con funciones
                // inalcanzables no-compilables).  @section siempre se siembra.
                // Phase NR @Naked: un ISR/stub se referencia desde la IDT/GDT
                // o por asm externo, NUNCA por un CALL visible -> sembrarlo
                // siempre para que no lo elimine el dead-strip del BFS.
                if ((emit_shared || (emit_obj && !main_fn) ||
                     !fn.section.empty() || fn.is_naked) &&
                    !queued.count(fn.name)) {
                    queued[fn.name] = true;
                    work.push_back(fn.name);
                }
            }
            // Sembrar las funciones referenciadas por bloques `bytes` (`dq
            // foo`) para que se compilen aunque main no las alcance por CALL.
            for (const auto &e : aot_mod.static_data.entries) {
                for (const auto &sr : e.meta.sym_refs) {
                    if (fn_by_name.count(sr.sym) && !queued.count(sr.sym)) {
                        queued[sr.sym] = true;
                        work.push_back(sr.sym);
                    }
                }
            }
            bool aot_codegen_ok = true;
            // AUTO (--float-isa auto): multiversion por cpuid.  Las funciones
            // con ops VEC_* (vectorizadas) se compilan 3x (sse2/avx2/avx512); el
            // IR es UNO (chunk dual: element-wise 64, reduccion 16 -> cada
            // variante decompone a su ancho).  El dispatch (fp+init+trampolin) va
            // despues; aqui solo emitimos las variantes fn$sse2/avx2/avx512.
            const bool aot_auto = (aot_fisa == jit::FloatIsa::AUTO);
            auto fn_has_vec_ops = [](const ir::IrFunction &f) -> bool {
                for (const auto &b : f.blocks)
                    for (const auto &in : b.instrs) {
                        const auto op = in.op;
                        if (op == ir::IrOp::VEC_BINOP || op == ir::IrOp::VEC_UNOP ||
                            op == ir::IrOp::VEC_FMA || op == ir::IrOp::VEC_BINOP_S ||
                            op == ir::IrOp::VEC_BCAST ||
                            op == ir::IrOp::VEC_ACC_ZERO ||
                            op == ir::IrOp::VEC_ACC_ADD ||
                            op == ir::IrOp::VEC_ACC_FMA ||
                            op == ir::IrOp::VEC_ACC_STORE ||
                            op == ir::IrOp::VEC_ACC_COMBINE)
                            return true;
                    }
                return false;
            };
            // Nombres de las variantes multiversionadas (los consume el dispatch).
            std::vector<std::string> mv_funcs; // funciones multiversionadas
            // AUTO: sembrar toda funcion con ops VEC.  __vex_main_body (el main
            // del usuario renombrado por la lowering) NO se alcanza por CALL
            // directo (el main sintetico hace CALLIND via fp), asi que hay que
            // encolarlo explicitamente para que se dequeue -> compile sus 3
            // variantes.  __vex_auto_init si se alcanza (main lo CALL-prepende).
            if (aot_auto) {
                for (const auto &fn : aot_mod.functions) {
                    if (fn_has_vec_ops(fn) && !queued.count(fn.name)) {
                        queued[fn.name] = true;
                        work.push_back(fn.name);
                    }
                }
            }
            while (!work.empty()) {
                const std::string nm = work.back();
                work.pop_back();
                auto itf = fn_by_name.find(nm);
                if (itf == fn_by_name.end()) {
                    std::cerr << "[aot] simbolo no resuelto: la funcion '" << nm
                              << "' (referenciada por un CALL) no existe en el "
                                 "modulo.\n";
                    aot_codegen_ok = false;
                    break;
                }
                AotFn af;
                af.name = nm;
                af.section = itf->second->section;
                af.section_perms = itf->second->section_perms;
                af.section_at = itf->second->section_at;
                af.section_order = itf->second->section_order;
                af.bytes = jit::vreg_compile_native(
                    *itf->second, {}, {}, {}, {}, &af.relocs, aot_pic,
                    /*target_sysv=*/fmt == aot::ObjFormat::ELF,
                    /*mode32=*/aot_mode32, /*fisa=*/aot_fisa);
                if (af.bytes.empty()) {
                    std::cerr
                        << "[aot] el selector vreg no soporta la funcion '"
                        << nm << "' todavia (op fuera del subset nativo).\n";
                    aot_codegen_ok = false;
                    break;
                }
                compiled_idx[nm] = compiled.size();
                compiled.push_back(std::move(af));

                // AUTO: si la funcion tiene ops VEC, emitir las 3 variantes de
                // ancho compilando el MISMO IR con fisa sse2/avx2/avx512.  El
                // dispatch (mas abajo) las cablea via fp+init+trampolin.  La
                // copia `nm` (compilada arriba con AUTO=host) sirve de baseline
                // hasta que el trampolin la reemplace.
                if (aot_auto && fn_has_vec_ops(*itf->second)) {
                    const std::pair<const char *, jit::FloatIsa> variants[] = {
                        {"$sse2", jit::FloatIsa::SSE2},
                        {"$avx2", jit::FloatIsa::AVX},
                        {"$avx512", jit::FloatIsa::AVX512F}};
                    bool ok = true;
                    for (const auto &v : variants) {
                        AotFn vf;
                        vf.name = nm + v.first;
                        vf.bytes = jit::vreg_compile_native(
                            *itf->second, {}, {}, {}, {}, &vf.relocs, aot_pic,
                            /*target_sysv=*/fmt == aot::ObjFormat::ELF,
                            /*mode32=*/aot_mode32, /*fisa=*/v.second);
                        if (vf.bytes.empty()) {
                            std::cerr << "[aot] variante " << vf.name
                                      << " no compilable.\n";
                            ok = false;
                            break;
                        }
                        // Las CALL de la variante encolan callees igual que el
                        // baseline (se hace abajo en el loop de relocs comun? no:
                        // las variantes no pasan por ese loop -> encolar aqui).
                        for (const jit::NativeReloc &r : vf.relocs)
                            if (r.kind == jit::NativeReloc::Kind::CALL_REL32 &&
                                fn_by_name.count(r.symbol) &&
                                !queued.count(r.symbol)) {
                                queued[r.symbol] = true;
                                work.push_back(r.symbol);
                            }
                        compiled_idx[vf.name] = compiled.size();
                        compiled.push_back(std::move(vf));
                    }
                    if (!ok) {
                        aot_codegen_ok = false;
                        break;
                    }
                    mv_funcs.push_back(nm);
                }
                // Encolar los callees (relocs CALL_REL32 a nombres de funcion).
                // Los simbolos que NO son funciones del modulo son EXTERNOS
                // (libc/runtime, resueltos por el linker): no se encolan, se
                // emiten como relocs externas en PASS 2.
                for (const jit::NativeReloc &r : compiled.back().relocs) {
                    // CALL directo a un callee del modulo -> encolar.
                    if (r.kind == jit::NativeReloc::Kind::CALL_REL32) {
                        if (queued.count(r.symbol)) continue;
                        if (!fn_by_name.count(r.symbol)) continue; // externo
                        queued[r.symbol] = true;
                        work.push_back(r.symbol);
                        continue;
                    }
                    // Referencia a la DIRECCION de una funcion ("fnsym:<name>",
                    // de LABEL_ADDR: puntero de funcion para CALLIND, p.ej. el
                    // despacho de helpers multi-versionados o as_native_callback)
                    // -> encolar el target tambien (no llega por CALL directo).
                    if (r.symbol.rfind("fnsym:", 0) == 0) {
                        const std::string tgt = r.symbol.substr(6);
                        if (queued.count(tgt)) continue;
                        if (!fn_by_name.count(tgt)) continue; // externo
                        queued[tgt] = true;
                        work.push_back(tgt);
                    }
                }
            }
            if (!aot_codegen_ok) return EXIT_FAILURE;

            // ------------------------------------------------------------------
            // Layout MULTI-SECCION (2b, dev OS): el usuario decide en que
            // seccion vive cada funcion (@section) / dato.  Construimos un
            // buffer por seccion; .text (indice 0) es la de entrada (el _start
            // stub va en su offset 0).  TODAS las refs (stub->main, llamadas,
            // datos) se declaran al ObjectWriter, que las resuelve tras el
            // layout (unica entidad que conoce la VA de cada seccion) ->
            // CALL/JMP cross-seccion "just work".
            // ------------------------------------------------------------------
            struct SecAccum {
                std::string name;
                bool is_code = true;
                std::string perms; // "" = por convencion del nombre
                std::vector<uint8_t> bytes;
                int64_t at = -1;            // @at(N) (.bin)
                int32_t order = 0x7fffffff; // @order(N) (.bin)
            };
            std::vector<SecAccum> secs;
            std::unordered_map<std::string, int> sec_index;
            // Recoge perms/at/order de cualquier fn/dato que toque la seccion.
            // El primer @at no-default gana; el menor @order gana.  Conflictos
            // de @at distintos en la misma seccion se reportan.
            auto get_sec = [&](const std::string &name, bool is_code,
                               const std::string &perms, int64_t at = -1,
                               int32_t order = 0x7fffffff) -> int {
                auto it = sec_index.find(name);
                if (it != sec_index.end()) {
                    SecAccum &s = secs[it->second];
                    if (!perms.empty() && s.perms.empty()) s.perms = perms;
                    if (at >= 0) {
                        if (s.at >= 0 && s.at != at)
                            std::cerr
                                << "[aot] @at en conflicto para la seccion '"
                                << name << "' (" << s.at << " vs " << at
                                << ").\n";
                        else
                            s.at = at;
                    }
                    if (order < s.order) s.order = order;
                    return it->second;
                }
                const int idx = static_cast<int>(secs.size());
                SecAccum s;
                s.name = name;
                s.is_code = is_code;
                s.perms = perms;
                s.at = at;
                s.order = order;
                secs.push_back(std::move(s));
                sec_index[name] = idx;
                return idx;
            };
            // .text es SIEMPRE la seccion 0 (entry); el stub arranca en su off
            // 0.
            const int text_sec = get_sec(".text", true, "");
            secs[text_sec].bytes = stub.bytes;

            // Colocar cada funcion en su seccion (default .text).  main primero
            // dentro de su seccion (determinismo).
            struct FnLoc {
                int sec;
                uint32_t off;
            };
            std::unordered_map<std::string, FnLoc> fn_loc;
            auto place_fn = [&](size_t ci) {
                AotFn &af = compiled[ci];
                const std::string &fsec =
                    af.section.empty() ? ".text" : af.section;
                const int si = get_sec(fsec, /*is_code=*/true, af.section_perms,
                                       af.section_at, af.section_order);
                const uint32_t off =
                    static_cast<uint32_t>(secs[si].bytes.size());
                fn_loc[af.name] = {si, off};
                secs[si].bytes.insert(secs[si].bytes.end(), af.bytes.begin(),
                                      af.bytes.end());
            };
            // main primero (si existe; en SHARED puede no haber).
            if (main_fn) place_fn(compiled_idx["main"]);
            for (size_t ci = 0; ci < compiled.size(); ++ci)
                if (!main_fn || compiled[ci].name != "main") place_fn(ci);

            // ------------------------------------------------------------------
            // AOT.2.exec (PE-IAT): EXEC/SHARED standalone que llaman a simbolos
            // EXTERNOS (libc malloc/free/calloc/abort, o un FFI extern).  El
            // codigo emitio `call <sym>` (E8 rel32 directo) pero el simbolo NO
            // esta en la imagen -> hay que pasar por la IAT.  Como un `call
            // [rip+IAT]` (FF 15) mide 6 bytes y el E8 ya emitido mide 5, NO se
            // puede parchear in-situ.  Solucion estandar (como un linker): un
            // THUNK de import por simbolo en .text -- `FF 25 disp32` (jmp
            // [rip+IAT], 6 bytes) -- y el `call <sym>` apunta al thunk (REL32
            // normal intra-.text).  El `FF 25` del thunk se parchea via el
            // mecanismo de imports (mismo que ExitProcess del _start).  El
            // loader rellena la IAT con la direccion real -> el thunk salta
            // ahi. Solo PE EXEC por ahora (ELF EXEC = PLT-GOT, slice 2;
            // SHARED/.dll = follow-up; .o sigue emitiendo relocs externas que
            // resuelve gcc/ld).
            struct PeThunkImport {
                std::string dll, func;
                uint64_t off;
            };
            std::vector<PeThunkImport> pe_thunk_imports;
            // EXEC (PE o ELF) que llama a externos -> thunks de import.  PE los
            // resuelve por IAT (slice 1); ELF por eager-GOT como PIE dinamico
            // (slice 2).  El mismo thunk FF 25 sirve a ambos; difiere solo la
            // metadata (idata vs dynamic) que pone el emisor.
            const bool exec_native = !no_stub;
            if (exec_native) {
                // Recolectar externos (CALL_REL32 a un nombre que NO es funcion
                // del modulo), en orden estable y deduplicado.
                std::vector<std::string> ext_syms;
                std::unordered_set<std::string> ext_seen;
                for (const AotFn &af : compiled)
                    for (const jit::NativeReloc &r : af.relocs)
                        if (r.kind == jit::NativeReloc::Kind::CALL_REL32 &&
                            !fn_by_name.count(r.symbol) &&
                            !ext_seen.count(r.symbol)) {
                            ext_seen.insert(r.symbol);
                            ext_syms.push_back(r.symbol);
                        }
                // Mapa simbolo -> DLL.  PE: libc (MinGW/Windows) = msvcrt.dll;
                // los FFI extern de DLL del usuario llegan como "sym" (el lib
                // se perdio en el selector) -> default msvcrt.dll (follow-up:
                // cablear el DLL real desde el CompileResult).  ELF: el campo
                // se ignora (todo va a libc.so.6 via DT_NEEDED).
                const bool is_pe = (fmt == aot::ObjFormat::PE);
                // DLL real de cada simbolo externo via el mecanismo FFI del
                // lenguaje: `extern "kernel32.dll" { fn WriteFile(...); }`
                // registra ("kernel32.dll", "WriteFile") en native_imports (ver
                // lower_call FFI declarativo).  Construimos symbol -> DLL desde
                // ahi, en vez de una tabla hardcodeada en el compilador.  PE: si
                // un simbolo no esta declarado en ningun extern (libc implicito:
                // malloc/free/abort de msvcrt) -> msvcrt.dll.  ELF: todo va a
                // libc.so.6 via DT_NEEDED (el SONAME no se usa para resolver).
                std::unordered_map<std::string, std::string> sym2dll;
                for (const auto &ni : aot_mod.native_imports) {
                    // Solo nombres de DLL reales (FFI extern a sistema); las
                    // libs-plugin (rutas tipo stdlib/native/...) no son DLLs
                    // resolubles por IAT y en native_poo no llegan a reloc.
                    if (ni.lib.size() >= 4 &&
                        (ni.lib.rfind(".dll") == ni.lib.size() - 4 ||
                         ni.lib.rfind(".DLL") == ni.lib.size() - 4))
                        sym2dll[ni.name] = ni.lib;
                }
                auto dll_for = [is_pe, &sym2dll](const std::string &sym)
                    -> std::string {
                    if (!is_pe) return "libc.so.6";
                    auto it = sym2dll.find(sym);
                    if (it != sym2dll.end()) return it->second;
                    return "msvcrt.dll";
                };
                for (const std::string &sym : ext_syms) {
                    const uint32_t toff =
                        static_cast<uint32_t>(secs[text_sec].bytes.size());
                    // FF 25 00 00 00 00 -> jmp [rip+disp32]; disp32 a parchear.
                    const uint8_t thunk[6] = {0xFF, 0x25, 0x00,
                                              0x00, 0x00, 0x00};
                    secs[text_sec].bytes.insert(secs[text_sec].bytes.end(),
                                                thunk, thunk + 6);
                    fn_loc[sym] = {text_sec, toff}; // el call <sym> -> el thunk
                    pe_thunk_imports.push_back({dll_for(sym), sym, toff});
                }
            }

            // PASADA 1: colocar (lazy, una vez por entry) los datos
            // referenciados en su seccion (default .rodata) -> completa `secs`
            // ANTES de crearlas en el writer.  Devuelve (sec, off) del entry N.
            const auto &sd = aot_mod.static_data;
            std::unordered_map<uint32_t, std::pair<int, uint64_t>> data_loc;
            auto place_data = [&](uint32_t N) -> std::pair<int, uint64_t> {
                auto it = data_loc.find(N);
                if (it != data_loc.end()) return it->second;
                const auto &e = sd.entries[N];
                const std::string dsec = e.meta.section_name.empty()
                                             ? ".rodata"
                                             : e.meta.section_name;
                const int si =
                    get_sec(dsec, /*is_code=*/false, e.meta.section_perms,
                            e.meta.section_at, e.meta.section_order);
                const uint64_t off = secs[si].bytes.size();
                const uint8_t *p = sd.bytes.data() + e.byte_offset;
                secs[si].bytes.insert(secs[si].bytes.end(), p, p + e.byte_len);
                std::pair<int, uint64_t> loc{si, off};
                data_loc[N] = loc;
                return loc;
            };
            for (const AotFn &af : compiled) {
                for (const jit::NativeReloc &r : af.relocs) {
                    if (r.kind == jit::NativeReloc::Kind::CALL_REL32) continue;
                    if (r.symbol.rfind("secsym:", 0) == 0)
                        continue; // simbolo de seccion (pass 2)
                    if (r.symbol.rfind("fnsym:", 0) == 0)
                        continue; // direccion de funcion (pass 2, sin dato)
                    if (r.symbol.rfind("rodata.", 0) != 0) {
                        std::cerr
                            << "[aot] reloc de dato con simbolo inesperado: '"
                            << r.symbol << "'.\n";
                        return EXIT_FAILURE;
                    }
                    const uint32_t N = static_cast<uint32_t>(
                        std::strtoul(r.symbol.c_str() + 7, nullptr, 10));
                    if (N >= sd.size()) {
                        std::cerr
                            << "[aot] reloc a static_data fuera de rango: N="
                            << N << ".\n";
                        return EXIT_FAILURE;
                    }
                    place_data(N);
                }
            }
            // FORCE_EMIT: bloques `bytes` y datos en @section que deben
            // emitirse aunque ningun reloc los referencie (firmas, tablas,
            // boot sectors).  Se colocan en orden de aparicion.
            for (uint32_t N = 0; N < sd.size(); ++N) {
                if (sd.entries[N].meta.flags & ir::IrModule::SD_FLAG_FORCE_EMIT)
                    place_data(N);
            }

            // Phase NR / dev-OS: nombre de bloque (asm/bytes) -> su ubicacion.
            // Permite que OTROS bloques lo referencien por simbolo (un `jmp
            // other_block` o un `dd gdt` cross-block).  Los bloques con
            // symbol_name son FORCE_EMIT -> ya estan colocados (loop de
            // arriba); ademas place_data() es idempotente.
            std::unordered_map<std::string, std::pair<int, uint64_t>>
                data_sym_loc;
            for (uint32_t N = 0; N < sd.size(); ++N) {
                const std::string &snm = sd.entries[N].meta.symbol_name;
                if (snm.empty()) continue;
                data_sym_loc[snm] = place_data(N);
            }

            // Crear el writer + TODAS las secciones (writer idx == secs idx,
            // mismo orden; `secs` ya esta completa tras la pasada 1).
            aot::ObjectWriter w(fmt);
            w.set_mode32(aot_mode32); // x86-32 EXEC -> contenedor ELF32
            for (const SecAccum &s : secs) {
                // Permisos: explicitos (@section(".x","rwx")), o por convencion
                // del nombre (.text*->rx, .rodata*->r, .data*/.bss*->rw).
                std::string p = s.perms;
                if (p.empty()) {
                    if (s.name.rfind(".text", 0) == 0)
                        p = "rx";
                    else if (s.name.rfind(".rodata", 0) == 0)
                        p = "r";
                    else if (s.name.rfind(".data", 0) == 0)
                        p = "rw";
                    else if (s.name.rfind(".bss", 0) == 0)
                        p = "rw";
                    else
                        p = s.is_code ? "rx" : "r";
                }
                uint32_t flags = 0;
                if (p.find('r') != std::string::npos)
                    flags |= aot::SecFlag::READ;
                if (p.find('w') != std::string::npos)
                    flags |= aot::SecFlag::WRITE;
                const bool exec = (p.find('x') != std::string::npos);
                if (exec)
                    flags |= aot::SecFlag::EXEC | aot::SecFlag::CODE;
                else
                    flags |= aot::SecFlag::DATA;
                aot::WriterSection ws;
                ws.name = s.name;
                ws.flags = flags;
                ws.data = s.bytes;
                ws.at = s.at;
                ws.order = s.order; // ubicacion/orden (.bin)
                w.add_section(std::move(ws));
            }
            if (emit_shared) {
                // Libreria compartida: exporta TODAS las funciones como
                // simbolos globales (dlsym).  Sin _start ni entry.
                w.set_output_kind(aot::OutputKind::SHARED);
                for (const AotFn &af : compiled) {
                    const FnLoc &fl = fn_loc[af.name];
                    w.add_symbol(af.name, fl.sec, fl.off, /*is_func=*/true);
                }
            } else if (emit_obj) {
                // Objeto relocatable: main es un simbolo GLOBAL (lo invoca el
                // crt del linker externo); sin _start ni entry.
                w.set_output_kind(aot::OutputKind::OBJECT);
                // Exporta como GLOBAL todas las funciones de USUARIO (no
                // empiezan por "__").  Los helpers internos (__vex_*/__new_*/
                // __module_init/...) quedan LOCALES -> no colisionan al enlazar
                // varios .o Vex (cada .o lleva su propia copia, referenciada via
                // relocs de seccion).  Asi una libreria .o (sin main) expone sus
                // funciones y otro .o las resuelve cross-file con el linker.
                for (const AotFn &af : compiled) {
                    // Los inits de programa del CPU-dispatch (cpu/memcpy/strdisp)
                    // se EXPORTAN como globales aunque empiecen por "__": el
                    // linker los recolecta de CADA .o y los ejecuta antes de main
                    // (cada .o tiene sus propios slots fp; basta correr su init).
                    const bool is_init = (af.name == "__vex_cpu_init" ||
                                          af.name == "__vex_memcpy_init" ||
                                          af.name == "__vex_strdisp_init");
                    // En un EJECUTABLE (hay main) los helpers __-prefijados
                    // quedan LOCALES (program-internos; evita colisiones al
                    // enlazar varios .o).  En una LIBRERIA (sin main) son la
                    // API publica -> se exportan globales (p.ej. el runtime
                    // __vex_setjmp/__vex_throw/... que otro .o resuelve).
                    if (main_fn && af.name.rfind("__", 0) == 0 && !is_init)
                        continue; // helper interno del ejecutable -> local
                    const FnLoc &fl2 = fn_loc[af.name];
                    w.add_symbol(af.name, fl2.sec, fl2.off, /*is_func=*/true);
                }
            } else if (emit_bin) {
                // Binario plano: sin cabecera ni _start; entry = offset 0 (la
                // primera seccion .text, donde va main si existe).  Las refs
                // absolutas se resuelven contra --bin-base.
                w.set_output_kind(aot::OutputKind::FLAT_BIN);
                w.set_flat_base(bin_base);
            } else {
                w.set_entry(text_sec, 0); // _start en .text offset 0
                // stub->main: rel32 a la VA real de main (resuelta por el
                // writer).
                const FnLoc &ml = fn_loc["main"];
                w.add_reloc(text_sec, stub.main_call_off,
                            aot::RelocTarget::addr(ml.sec, ml.off),
                            aot::RelocKind::REL32);
            }

            // PASADA 2: declarar las relocs de cada funcion (llamadas + datos +
            // simbolos de seccion).  El writer las resuelve (EXEC) o las emite
            // como registros (OBJECT) tras el layout.
            for (const AotFn &af : compiled) {
                const FnLoc &fl = fn_loc[af.name];
                for (const jit::NativeReloc &r : af.relocs) {
                    const uint64_t site =
                        static_cast<uint64_t>(fl.off) + r.offset;
                    if (r.kind == jit::NativeReloc::Kind::CALL_REL32) {
                        auto it = fn_loc.find(r.symbol);
                        if (it == fn_loc.end()) {
                            // Simbolo EXTERNO (libc/runtime:
                            // malloc/free/abort...). Solo en OBJECT (.o/.obj):
                            // el linker del sistema lo resuelve.
                            // EXEC/SHARED/BIN necesitarian IAT/PLT-GOT
                            // (AOT.2.exec, futuro).
                            if (!emit_obj) {
                                // PE EXEC ya resolvio sus externos via thunks
                                // de IAT (el simbolo ESTA en fn_loc -> no llega
                                // aqui). Quedan: ELF EXEC/SHARED (PLT-GOT,
                                // slice 2), PE SHARED (.dll, follow-up) y .bin.
                                std::cerr
                                    << "[aot] llamada a simbolo externo '"
                                    << r.symbol
                                    << "' aun no soportada para este target ("
                                    << (fmt == aot::ObjFormat::ELF
                                            ? "ELF EXEC/shared: "
                                              "PLT-GOT pendiente"
                                            : "PE shared/.bin")
                                    << "); usa --emit obj (enlazar con gcc/ld) "
                                       "o, en "
                                       "Windows, --emit exe (PE-IAT).\n";
                                return EXIT_FAILURE;
                            }
                            w.add_reloc(fl.sec, site,
                                        aot::RelocTarget::extern_sym(r.symbol),
                                        aot::RelocKind::REL32);
                        } else {
                            w.add_reloc(fl.sec, site,
                                        aot::RelocTarget::addr(it->second.sec,
                                                               it->second.off),
                                        aot::RelocKind::REL32);
                        }
                    } else if (r.symbol.rfind("secsym:", 0) == 0) {
                        // Simbolo de seccion "secsym:<k>:<name>" (dev OS):
                        // s=start (base), e=end (base+size), z=size (tamano).
                        const char kc = r.symbol.size() > 7 ? r.symbol[7] : 's';
                        const std::string sname = r.symbol.substr(9);
                        auto si = sec_index.find(sname);
                        if (si == sec_index.end()) {
                            std::cerr << "[aot] section_"
                                      << (kc == 'z'   ? "size"
                                          : kc == 'e' ? "end"
                                                      : "start")
                                      << "(\"" << sname
                                      << "\"): la seccion no existe "
                                         "(ningun codigo/dato la usa).\n";
                            return EXIT_FAILURE;
                        }
                        const int tsi = si->second;
                        // REL32 si la ref fue RIP-relativa (DATA_REL32), ABS64
                        // si absoluta (--no-pie).  SIZE es siempre un inmediato
                        // (IMM64).
                        const bool rel =
                            (r.kind == jit::NativeReloc::Kind::DATA_REL32);
                        if (kc == 'z') {
                            w.add_reloc(fl.sec, site,
                                        aot::RelocTarget::size(tsi),
                                        aot::RelocKind::IMM64);
                        } else if (kc == 'e') {
                            w.add_reloc(fl.sec, site,
                                        aot::RelocTarget::end(tsi),
                                        rel ? aot::RelocKind::REL32
                                            : aot::RelocKind::ABS64);
                        } else {
                            w.add_reloc(fl.sec, site,
                                        aot::RelocTarget::addr(tsi, 0),
                                        rel ? aot::RelocKind::REL32
                                            : aot::RelocKind::ABS64);
                        }
                    } else if (r.symbol.rfind("fnsym:", 0) == 0) {
                        // Direccion de una FUNCION del modulo ("fnsym:<name>",
                        // de LABEL_ADDR): puntero de funcion para CALLIND.  Se
                        // resuelve contra el offset de la funcion en su seccion
                        // (REL32 si RIP-rel, ABS64 si --no-pie).
                        const std::string tgt = r.symbol.substr(6);
                        auto fit = fn_loc.find(tgt);
                        if (fit == fn_loc.end()) {
                            std::cerr
                                << "[aot] direccion de funcion no resuelta: '"
                                << tgt << "' (referenciada como puntero).\n";
                            return EXIT_FAILURE;
                        }
                        const aot::RelocKind k =
                            (r.kind == jit::NativeReloc::Kind::DATA_REL32)
                                ? aot::RelocKind::REL32
                                : aot::RelocKind::ABS64;
                        w.add_reloc(fl.sec, site,
                                    aot::RelocTarget::addr(fit->second.sec,
                                                           fit->second.off),
                                    k);
                    } else {
                        const uint32_t N = static_cast<uint32_t>(
                            std::strtoul(r.symbol.c_str() + 7, nullptr, 10));
                        const std::pair<int, uint64_t> loc = place_data(N);
                        const aot::RelocKind k =
                            (r.kind == jit::NativeReloc::Kind::DATA_REL32)
                                ? aot::RelocKind::REL32
                                : aot::RelocKind::ABS64;
                        w.add_reloc(
                            fl.sec, site,
                            aot::RelocTarget::addr(loc.first, loc.second), k);
                    }
                }
            }

            // Relocs de los bloques `bytes` con referencias a simbolos
            // (`dq main`): el campo (placeholder 0) se parchea con la direccion
            // del simbolo.  Solo se resuelven contra FUNCIONES (fn_loc); una
            // ref a otro dato/seccion es trabajo futuro.
            for (uint32_t N = 0; N < sd.size(); ++N) {
                const auto &meta = sd.entries[N].meta;
                if (meta.sym_refs.empty()) continue;
                auto dit = data_loc.find(N);
                if (dit == data_loc.end()) continue; // no colocada (no deberia)
                const int dsec = dit->second.first;
                const uint64_t doff = dit->second.second;
                for (const auto &sr : meta.sym_refs) {
                    // Resolver contra una FUNCION (fn_loc) o, si no, contra
                    // otro BLOQUE asm/bytes nombrado (data_sym_loc): un dev-OS
                    // hace `jmp pm32` / `dd gdt` cross-block.
                    int tsec;
                    uint64_t toff;
                    auto fit = fn_loc.find(sr.sym);
                    if (fit != fn_loc.end()) {
                        tsec = fit->second.sec;
                        toff = fit->second.off;
                    } else {
                        auto dsit = data_sym_loc.find(sr.sym);
                        if (dsit == data_sym_loc.end()) {
                            std::cerr << "[aot] referencia a simbolo no "
                                         "resuelto '"
                                      << sr.sym
                                      << "' (ni funcion ni bloque asm/bytes).\n";
                            return EXIT_FAILURE;
                        }
                        tsec = dsit->second.first;
                        toff = dsit->second.second;
                    }
                    // Kind segun is_rel + ancho: REL32 (jmp/call near), IMM32
                    // (dd -> VA absoluta de 32 bits) o ABS64 (dq -> 64 bits).
                    const aot::RelocKind k =
                        sr.is_rel ? aot::RelocKind::REL32
                                  : (sr.width == 4 ? aot::RelocKind::IMM32
                                                   : aot::RelocKind::ABS64);
                    w.add_reloc(dsec, doff + sr.offset,
                                aot::RelocTarget::addr(tsec, toff), k);
                }
            }

            if (!no_stub && stub.has_import_call) {
                w.add_import_call(aot::ImportCall{stub.import_dll,
                                                  stub.import_func, text_sec,
                                                  stub.import_call_off});
            }
            // AOT.2.exec: registrar el import de cada thunk (FF 25 -> IAT).  Se
            // agrupan por DLL junto al ExitProcess (kernel32) en el emisor PE.
            for (const PeThunkImport &ti : pe_thunk_imports) {
                w.add_import_call(
                    aot::ImportCall{ti.dll, ti.func, text_sec, ti.off});
            }

            std::string werr;
            if (!w.write(out_prefix, werr)) {
                std::cerr << "[aot] error al escribir '" << out_prefix
                          << "': " << werr << "\n";
                return EXIT_FAILURE;
            }

            const char *fmt_name = (fmt == aot::ObjFormat::PE) ? "PE" : "ELF";
            if (emit_shared)
                std::cout << "[aot] libreria compartida " << fmt_name
                          << (fmt == aot::ObjFormat::PE ? " (.dll)" : " (.so)")
                          << " escrita en '" << out_prefix << "' ("
                          << compiled.size() << " simbolo(s) exportado(s); "
                          << (fmt == aot::ObjFormat::PE
                                  ? "LoadLibrary/GetProcAddress"
                                  : "dlopen/dlsym")
                          << ").\n";
            else if (emit_obj)
                std::cout << "[aot] objeto relocatable " << fmt_name
                          << (fmt == aot::ObjFormat::PE ? " (.obj)" : " (.o)")
                          << " escrito en '" << out_prefix
                          << "' (main GLOBAL; linkable con "
                          << (fmt == aot::ObjFormat::PE ? "link.exe/gcc-mingw"
                                                        : "ld/gcc")
                          << ").\n";
            else if (emit_bin)
                std::cout
                    << "[aot] binario plano (.bin) escrito en '" << out_prefix
                    << "' (sin cabecera; entry en offset 0; base de carga 0x"
                    << std::hex << bin_base << std::dec << ").\n";
            else
                std::cout << "[aot] ejecutable nativo " << fmt_name
                          << " escrito en '" << out_prefix
                          << "' (entry _start -> main -> exit, return "
                             "de main como exit-code).\n";
            return EXIT_SUCCESS;
        }

        /* CACHE MISS + @Macros presentes: hacer two-phase y persistir
         * el resultado a `.cache/vex/<key>.velb` para futuras corridas. */
        if (!cache_hit && cr.has_lowerable_macros &&
            !user_already_set_prebuilt) {
            std::error_code ec;
            std::filesystem::create_directories(cache_dir, ec);

            /* Phase MC.16: per-macro manifest diagnostico.  Computamos
             * hash por macro del fuente actual y comparamos con manifest
             * anterior (si existe).  Reportamos via VESTA_MC_VERBOSE que
             * macros cambiaron -- foundation para futuro per-macro relink. */
            if (verbose_mc) {
                const auto current_ranges =
                    find_macro_ranges_with_names(vex_source);
                std::vector<std::pair<std::string, std::string>> current_hashes;
                current_hashes.reserve(current_ranges.size());
                for (const auto &r : current_ranges) {
                    const std::string &mname = std::get<0>(r);
                    const size_t start = std::get<1>(r);
                    const size_t end = std::get<2>(r);
                    constexpr uint64_t FNV_O = 14695981039346656037ULL;
                    constexpr uint64_t FNV_P = 1099511628211ULL;
                    uint64_t h = FNV_O;
                    for (size_t i = start; i < end && i < vex_source.size();
                         ++i) {
                        h ^= static_cast<uint8_t>(vex_source[i]);
                        h *= FNV_P;
                    }
                    char buf[17];
                    std::snprintf(buf, sizeof(buf), "%016llx",
                                  static_cast<unsigned long long>(h));
                    current_hashes.emplace_back(mname, buf);
                }
                /* Cargar manifest anterior si existe (key viejo = el del
                 * cache que acabamos de invalidar; lo buscamos por scan). */
                std::map<std::string, std::string> previous;
                bool had_previous = false;
                {
                    std::error_code sec;
                    for (const auto &entry :
                         std::filesystem::directory_iterator(cache_dir, sec)) {
                        if (!entry.is_regular_file()) continue;
                        const std::string p = entry.path().string();
                        if (p.size() > 9 &&
                            p.substr(p.size() - 9) == ".manifest") {
                            std::ifstream ifs(p);
                            std::string line;
                            while (std::getline(ifs, line)) {
                                const auto tab = line.find('\t');
                                if (tab != std::string::npos) {
                                    previous[line.substr(0, tab)] =
                                        line.substr(tab + 1);
                                }
                            }
                            had_previous = !previous.empty();
                            break; /* solo el primero, suficiente para diag */
                        }
                    }
                }
                if (had_previous) {
                    size_t changed = 0, added = 0;
                    std::set<std::string> seen;
                    for (const auto &cur : current_hashes) {
                        seen.insert(cur.first);
                        auto it = previous.find(cur.first);
                        if (it == previous.end()) {
                            std::cerr << "[mc-manifest] añadido: " << cur.first
                                      << "\n";
                            ++added;
                        } else if (it->second != cur.second) {
                            std::cerr << "[mc-manifest] cambio: " << cur.first
                                      << "\n";
                            ++changed;
                        }
                    }
                    for (const auto &prev : previous) {
                        if (!seen.count(prev.first)) {
                            std::cerr
                                << "[mc-manifest] eliminado: " << prev.first
                                << "\n";
                        }
                    }
                    std::cerr << "[mc-manifest] total=" << current_hashes.size()
                              << " cambios=" << changed
                              << " añadidos=" << added << "\n";
                }
                /* Escribir nuevo manifest. */
                const std::string manifest_path = cache_prefix + ".manifest";
                std::ofstream mfs(manifest_path);
                if (mfs) {
                    for (const auto &p : current_hashes) {
                        mfs << p.first << '\t' << p.second << '\n';
                    }
                }
            }

            /* Phase MC.14: cache sweeper TTL-based.  Antes de poblar el
             * nuevo cache file, escanea @c .cache/vex/ y borra archivos
             * cuyo mtime sea anterior a @c TTL dias (default 30, override
             * via env var @c VESTA_MC_CACHE_TTL_DAYS).  Solo corre en el
             * path de miss para evitar sobrecargar el path comun de hit.
             * Cost ~50us para directorios con <100 archivos -- despreciable
             * cuando ya estamos en miss path (que ya cuesta 7-10ms).
             *
             * Default agresivo (30 dias) para que el cache no crezca
             * indefinidamente; el usuario que use el cache regularmente
             * verá hits constantemente y los archivos no expiran.  Los
             * archivos de proyectos abandonados se limpian solos. */
            {
                uint64_t ttl_days = 30;
                if (const char *e = std::getenv("VESTA_MC_CACHE_TTL_DAYS")) {
                    char *end = nullptr;
                    const unsigned long v = std::strtoul(e, &end, 10);
                    if (end != e && v <= 3650) ttl_days = v; /* cap 10 anyos */
                }
                if (ttl_days > 0) {
                    const auto now_tp =
                        std::filesystem::file_time_type::clock::now();
                    const auto ttl = std::chrono::hours(24 * ttl_days);
                    size_t swept = 0;
                    std::error_code swec;
                    for (const auto &entry :
                         std::filesystem::directory_iterator(cache_dir, swec)) {
                        if (!entry.is_regular_file()) continue;
                        std::error_code mtec;
                        const auto mt = entry.last_write_time(mtec);
                        if (mtec) continue;
                        if (now_tp - mt > ttl) {
                            std::error_code rmec;
                            std::filesystem::remove(entry.path(), rmec);
                            if (!rmec) ++swept;
                        }
                    }
                    if (verbose_mc && swept > 0) {
                        std::cerr
                            << "[mc-cache] sweeper: " << swept
                            << " archivo(s) expirado(s) eliminado(s) (TTL "
                            << ttl_days << " dias)\n";
                    }
                }
            }

            const std::string tmp_vel_path = cache_prefix + ".vel.tmp";
            {
                std::ofstream tmp(tmp_vel_path, std::ios::binary);
                if (tmp) {
                    if (copts.emit_debug) {
                        tmp << "// @file " << vex_path << "\n";
                    }
                    tmp << cr.vel_text;
                }
            }
            /* Linker -> .velb persistente en .cache/vex/.  Reusa el
             * mismo @c run_worker que produce el .velb final. */
            const int tmp_rc = asm_multi_process::run_worker(
                tmp_vel_path, cache_prefix,
                /*skip_preprocessor=*/true,
                /*keep_labels=*/false,
                /*ir_section_bytes=*/&cr.ir_section_bytes,
                /*emit_map=*/false);
            if (tmp_rc == EXIT_SUCCESS) {
#if defined(_WIN32)
                _putenv_s("VESTA_MC_PREBUILT", cache_path.c_str());
#else
                setenv("VESTA_MC_PREBUILT", cache_path.c_str(), 1);
#endif
                // Phase M.2.e: same dispatch en el path two-phase del macro
                // cache.  Si el source tiene imports, usar compile_vex_project.
                vex::CompileResult cr2 =
                    vex::vex_source_has_imports(vex_source)
                        ? vex::compile_vex_project(vex_path, copts)
                        : vex::compile_vex_source(vex_source, vex_path, copts);
#if defined(_WIN32)
                _putenv_s("VESTA_MC_PREBUILT", "");
#else
                unsetenv("VESTA_MC_PREBUILT");
#endif
                if (cr2.ok) {
                    cr = std::move(cr2);
                }
                if (verbose_mc) {
                    std::cerr << "[mc-cache] miss + populated: " << cache_path
                              << "\n";
                }
                /* Cleanup del .vel temporal y del .velb-map (no de cache_path,
                 * que es el archivo cacheado y debe persistir). */
                std::remove(tmp_vel_path.c_str());
                const std::string cache_velb_map = cache_path + "-map";
                std::remove(cache_velb_map.c_str());
            }
        }

        // Mostrar warnings (cr.ok no impide los warnings).
        for (const auto &d : cr.diagnostics.all()) {
            if (d.level != vex::DiagLevel::ERR)
                vex::print_diagnostic(std::cerr, d);
        }

        // si --vex-base fue especificado y es != 0, parchear el
        // texto .vel para reemplazar el @IniAddress(0x0000000000000000)
        // generado por defecto por el ir_emitter por @IniAddress(<base>).
        // Esto desplaza todo el code section a la VA solicitada, evitando
        // solapamiento con el caller cuando este modulo se carga via
        // loadmodule.  Es una solucion tactica; idealmente el ir_emitter
        // tomaria una opcion de base address directamente.
        //
        // ADEMAS: insertar `@InitPc(main)` antes del @Module(...) para que
        // el linker compute start_pc = absolute_addr_of_main = base +
        // offset(main) = base + 0 (main es siempre el primer label en
        // codigo Vex).  Sin esto, start_pc queda en 0 y loadmodule ejecuta
        // codigo del caller en vez del plugin.
        if (vex_base_addr != 0) {
            const std::string from = "@IniAddress(0x0000000000000000)";
            char buf[64];
            std::snprintf(buf, sizeof(buf), "@IniAddress(0x%016llX)",
                          static_cast<unsigned long long>(vex_base_addr));
            std::string to = buf;
            size_t pos = cr.vel_text.find(from);
            if (pos != std::string::npos) {
                cr.vel_text.replace(pos, from.size(), to);
                vesta::scout() << "[vex] @IniAddress patched -> " << to << "\n";
            } else {
                std::cerr << "[vex] aviso: no se encontro @IniAddress(0x0...) "
                             "para parchear con --vex-base\n";
            }
            // Insertar @InitPc(<base>) NUMERICO antes del @Module(...).  El
            // assembler procesa anotaciones single-pass y `main` no esta
            // definido todavia cuando @InitPc se evalua, asi que usamos el
            // valor absoluto (= base, ya que main es siempre el primer label
            // en codigo Vex y la seccion code tiene @Align(0x1000) que se
            // alinea con la base hex que pasa el usuario).
            const std::string mod_marker = "@Module(";
            size_t mod_pos = cr.vel_text.find(mod_marker);
            if (mod_pos != std::string::npos) {
                char ipbuf[64];
                std::snprintf(ipbuf, sizeof(ipbuf), "@InitPc(0x%llX)\n\n",
                              static_cast<unsigned long long>(vex_base_addr));
                cr.vel_text.insert(mod_pos, ipbuf);
                vesta::scout()
                    << "[vex] @InitPc(0x" << std::hex << vex_base_addr
                    << std::dec << ") insertado (start_pc = base address)\n";
            } else {
                std::cerr << "[vex] aviso: no se encontro @Module(...) para "
                             "insertar @InitPc\n";
            }
            // Convertir el `hlt` final del main del modulo en `ret` para que
            // sea LLAMABLE via callvm desde loadmod del caller.  Por defecto
            // main de Vex termina con `leave\nhlt` (convencion entry-point);
            // un plugin necesita main RET-able para que el push de return
            // address en loadmod resulte en flujo de vuelta al caller.
            // El standalone execution del plugin no funciona tras esta
            // conversion (RET pop'ea garbage del stack).  Aceptamos esta
            // limitacion: los plugins no se ejecutan standalone.
            const std::string main_ret_marker =
                "main_ret:\n    leave\n    hlt\n";
            const std::string main_ret_repl = "main_ret:\n    leave\n    ret\n";
            size_t hlt_pos = cr.vel_text.find(main_ret_marker);
            if (hlt_pos != std::string::npos) {
                cr.vel_text.replace(hlt_pos, main_ret_marker.size(),
                                    main_ret_repl);
                vesta::scout() << "[vex] main_ret hlt -> ret (modo plugin: "
                                  "callable via loadmod)\n";
            } else {
                std::cerr << "[vex] aviso: no se encontro 'main_ret: leave "
                             "hlt' para convertir a ret\n";
            }
        }

        // Diagramas: cada flag activo produce un archivo .mmd o .dot segun
        // el formato escogido.  Mermaid reconocido por VS Code / GitHub /
        // mermaid.live.  Graphviz se renderiza con `dot -Tsvg foo.dot -o
        // foo.svg`.
        auto write_diagram = [&](const std::string &content,
                                 const std::string &suffix) -> bool {
            if (content.empty()) return true; // no se solicito; no es error
            std::string base =
                out_prefix.empty() ? copts.module_name : out_prefix;
            std::string path = base + suffix;
            std::ofstream ofs(path);
            if (!ofs.is_open()) {
                std::cerr << "[diagram] No se puede escribir: " << path << "\n";
                return false;
            }
            ofs << content;
            vesta::scout() << "[diagram] generado: " << path << "\n";
            return true;
        };
        if (emit_mermaid) {
            if (diag_vex && !write_diagram(cr.mermaid_ast, ".ast.mmd"))
                return EXIT_FAILURE;
            if (diag_ir_pre && !write_diagram(cr.mermaid_ir_pre, ".ir.pre.mmd"))
                return EXIT_FAILURE;
            if (diag_ir_post &&
                !write_diagram(cr.mermaid_ir_post, ".ir.post.mmd"))
                return EXIT_FAILURE;
            if (diag_vel && !write_diagram(cr.mermaid_vel, ".vel.mmd"))
                return EXIT_FAILURE;
        }
        if (emit_graphviz) {
            if (diag_vex && !write_diagram(cr.graphviz_ast, ".ast.dot"))
                return EXIT_FAILURE;
            if (diag_ir_pre &&
                !write_diagram(cr.graphviz_ir_pre, ".ir.pre.dot"))
                return EXIT_FAILURE;
            if (diag_ir_post &&
                !write_diagram(cr.graphviz_ir_post, ".ir.post.dot"))
                return EXIT_FAILURE;
            if (diag_vel && !write_diagram(cr.graphviz_vel, ".vel.dot"))
                return EXIT_FAILURE;
        }
        if (emit_html) {
            // Paginas HTML interactivas autocontenidas: abrir en el navegador.
            if (diag_vex && !write_diagram(cr.html_ast, ".ast.html"))
                return EXIT_FAILURE;
            if (diag_ir_pre && !write_diagram(cr.html_ir_pre, ".ir.pre.html"))
                return EXIT_FAILURE;
            if (diag_ir_post &&
                !write_diagram(cr.html_ir_post, ".ir.post.html"))
                return EXIT_FAILURE;
            if (diag_vel && !write_diagram(cr.html_vel, ".vel.html"))
                return EXIT_FAILURE;
        }

        // Si --vex-emit-ir esta activo, escribir el dump del SSA IR
        // (pre y post optimizacion) en <out>.ir y salir.  Util para
        // debug del frontend sin tocar el .vel ni el linker.  No se
        // compila a .velb en este modo.
        if (emit_ir) {
            std::string ir_path = out_prefix.empty()
                                      ? (copts.module_name + ".ir")
                                      : (out_prefix + ".ir");
            std::ofstream ofs_ir(ir_path);
            if (!ofs_ir.is_open()) {
                std::cerr << "[vex] No se puede escribir: " << ir_path << "\n";
                return EXIT_FAILURE;
            }
            ofs_ir << cr.ir_text;
            vesta::scout() << "[vex] .ir generado: " << ir_path << "\n";
            return EXIT_SUCCESS;
        }

        // Si --port=<lang> esta activo, escribir el codigo fuente generado
        // y terminar (no compilar a .velb).  El usuario lo compila con su
        // toolchain nativa (gcc/clang/javac/node/etc).  La extension se
        // deriva del target.
        if (!copts.port_target.empty()) {
            std::string ext = ".c"; // default; futuros: .java, .js
            if (copts.port_target == "c") ext = ".c";
            // (mas casos cuando agreguemos backends)
            std::string port_path = out_prefix.empty()
                                        ? (copts.module_name + ext)
                                        : (out_prefix + ext);
            std::ofstream ofs_port(port_path);
            if (!ofs_port.is_open()) {
                std::cerr << "[port] No se puede escribir: " << port_path
                          << "\n";
                return EXIT_FAILURE;
            }
            ofs_port << cr.port_text;
            vesta::scout() << "[port] " << copts.port_target
                           << " generado: " << port_path << " ("
                           << cr.port_text.size() << " bytes)\n";
            for (const auto &w : cr.port_warnings) {
                std::cerr << "[port] warning: " << w << "\n";
            }
            return EXIT_SUCCESS;
        }

        // 5 Escribir el .vel intermedio.
        std::string vel_path = out_prefix.empty() ? (copts.module_name + ".vel")
                                                  : (out_prefix + ".vel");
        {
            std::ofstream ofs(vel_path);
            if (!ofs.is_open()) {
                std::cerr << "[vex] No se puede escribir: " << vel_path << "\n";
                return EXIT_FAILURE;
            }
            // Si --vex-debug esta activo, prepend `// @file <vex_path>`
            // al .vel para que el lexer del .vel-to-.velb pase la info
            // al linker (Context::debug_source_file).
            if (copts.emit_debug) {
                ofs << "// @file " << vex_path << "\n";
            }
            ofs << cr.vel_text;
        }
        vesta::scout() << "[vex] .vel generado: " << vel_path << "\n";

        if (emit_only) return EXIT_SUCCESS;

        // Compilar .vel -> .velb saltando VPP
        // Opcion W: pasar el IR pre-serializado al linker via run_worker
        // para que se embeba en la seccion @c @ir del .velb v3.
        int rc = asm_multi_process::run_worker(
            vel_path, out_prefix,
            /*skip_preprocessor=*/true,
            /*keep_labels=*/(result.count("keep-labels") > 0),
            /*ir_section_bytes=*/&cr.ir_section_bytes,
            /*emit_map=*/(result.count("emit-map") > 0));

        // Phase M5.B: persistir el .velb final al project cache si
        // (a) el compile usa imports (compile_vex_project tiene
        // dep_paths populated), (b) el link fue exitoso, y (c) el cache
        // esta enabled.
        if (rc == EXIT_SUCCESS && project_cache_enabled && has_imports &&
            !cr.dep_paths.empty()) {
            const std::string velb_path = out_prefix + ".velb";
            std::ifstream vf(velb_path, std::ios::binary | std::ios::ate);
            if (vf.is_open()) {
                const std::streamsize sz = vf.tellg();
                vf.seekg(0, std::ios::beg);
                std::vector<uint8_t> velb_bytes(
                    static_cast<size_t>(sz < 0 ? 0 : sz));
                if (sz > 0) {
                    vf.read(reinterpret_cast<char *>(velb_bytes.data()), sz);
                }
                vf.close();
                if (!velb_bytes.empty()) {
                    std::vector<vex::ProjectCacheDep> deps;
                    deps.reserve(cr.dep_paths.size());
                    for (const auto &p : cr.dep_paths) {
                        std::ifstream df(p, std::ios::binary | std::ios::ate);
                        if (!df.is_open()) continue;
                        const std::streamsize dsz = df.tellg();
                        df.seekg(0, std::ios::beg);
                        std::vector<uint8_t> dbytes(
                            static_cast<size_t>(dsz < 0 ? 0 : dsz));
                        if (dsz > 0) {
                            df.read(reinterpret_cast<char *>(dbytes.data()),
                                    dsz);
                        }
                        vex::ProjectCacheDep d;
                        d.path = p;
                        d.source_hash =
                            vex::fnv1a64_bytes(dbytes.data(), dbytes.size());
                        deps.push_back(std::move(d));
                    }
                    const bool saved = vex::project_cache_save(
                        pc_path, opts_hash, deps, velb_bytes);
                    if (project_cache_verbose) {
                        std::cerr << "[vex-project-cache] "
                                  << (saved ? "saved" : "save_failed") << ": "
                                  << pc_path << " (" << velb_bytes.size()
                                  << " bytes, " << deps.size() << " deps)\n";
                    }
                }
            }
        }

        /* Phase MC.4: probe del ComptimeRuntime (activable via env var
         * VESTA_COMPTIME_PROBE=1).  Tras producir el .velb, lo carga
         * en una @c ComptimeRuntime para validar que el pipeline
         * bytecode-in-memory -> Loader -> symbol_table -> macro_entry_pc
         * funciona end-to-end.  Imprime stats y los nombres de macros
         * resueltos.  Cero impacto si el env var no esta seteado. */
        if (rc == EXIT_SUCCESS) {
            const char *probe_env = std::getenv("VESTA_COMPTIME_PROBE");
            if (probe_env && probe_env[0] == '1') {
                const std::string velb_path = out_prefix + ".velb";
                std::ifstream f(velb_path, std::ios::binary);
                if (f) {
                    std::vector<uint8_t> bytes(
                        (std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
                    f.close();
                    vex::ComptimeRuntime ctr;
                    const bool loaded =
                        ctr.load_macros_from_bytes(std::move(bytes));
                    std::cerr << "[mc-probe] load_macros_from_bytes -> "
                              << (loaded ? "OK" : "FAIL") << "\n";
                    std::cerr << "[mc-probe] is_initialized -> "
                              << (ctr.is_initialized() ? "true" : "false")
                              << "\n";
                    std::cerr << "[mc-probe] registered_macro_count -> "
                              << ctr.registered_macro_count() << "\n";
                    /* Listar nombres + PCs para demo. */
                    auto names = ctr.list_registered_macros();
                    for (const auto &p : names) {
                        std::cerr << "[mc-probe]   " << p.first << " @ 0x"
                                  << std::hex << p.second << std::dec << "\n";
                    }
                    /* Phase MC.5: intentar invocar el PRIMER macro a
                     * nivel funcion (filtrar sufijos internos del
                     * lowering: `_entry_`, `_if_`, `_ret`, `_while_`,
                     * `_for_`, `_then_`, `_else_`, `_merge_`). */
                    const std::pair<std::string, uint64_t> *first_fn = nullptr;
                    auto is_internal_label = [](const std::string &n) {
                        static const char *suffixes[] = {
                            "_entry_", "_if_",   "_ret",    "_while_", "_for_",
                            "_then_",  "_else_", "_merge_", "_loop_"};
                        for (const char *suf : suffixes) {
                            if (n.find(suf) != std::string::npos) return true;
                        }
                        return false;
                    };
                    for (const auto &p : names) {
                        if (p.first.find("__macro_") != 0) continue;
                        if (is_internal_label(p.first)) continue;
                        if (!first_fn) first_fn = &p;
                    }
                    if (!first_fn) {
                        std::cerr << "[mc-probe] no se encontro macro "
                                     "top-level invocable\n";
                    } else {
                        std::cerr
                            << "[mc-probe] invoke target: " << first_fn->first
                            << " @ 0x" << std::hex << first_fn->second
                            << std::dec << "\n";
                        /* MC.7: invocar el macro varias veces como
                         * @c string y mostrar el contenido devuelto. */
                        for (int iter = 1; iter <= 3; ++iter) {
                            std::string s_out;
                            const bool ok = ctr.invoke_string_macro(
                                first_fn->first, /*args=*/{}, s_out);
                            std::cerr << "[mc-probe] invoke#" << iter << " -> "
                                      << (ok ? "OK" : "FAIL") << "  string=\""
                                      << s_out << "\"\n";
                        }
                        std::cerr << "[mc-probe] call_count -> "
                                  << ctr.call_count() << "\n";
                    }

                    /* Phase MC.8: shadow_validate -- replay las
                     * expectaciones que el TypeChecker capturo durante
                     * la evaluacion AST de cada @Macro, invocando via
                     * VM, y comparando los strings.  Cualquier mismatch
                     * indica un bug en lowering del macro a IR (o en el
                     * AST evaluator).  Cero coste si no hay expectacioes. */
                    if (!cr.macro_expectations.empty()) {
                        for (const auto &e : cr.macro_expectations) {
                            ctr.record_expectation(e.macro_name, e.args,
                                                   e.expected_str, e.src_loc);
                        }
                        std::vector<vex::ComptimeRuntime::ShadowMismatch>
                            report;
                        const size_t mismatches = ctr.shadow_validate(report);
                        std::cerr << "[mc-shadow] expectations="
                                  << cr.macro_expectations.size()
                                  << " validated=" << report.size()
                                  << " mismatches=" << mismatches << "\n";
                        for (const auto &m : report) {
                            std::cerr << "[mc-shadow]   "
                                      << (m.match ? "OK   " : "FAIL ")
                                      << m.macro_name << " @ " << m.src_loc
                                      << "  (" << m.reason << ")";
                            if (!m.match) {
                                std::cerr << "  expected=\"" << m.expected
                                          << "\"  got=\"" << m.got << "\"";
                            }
                            std::cerr << "\n";
                        }
                    } else {
                        std::cerr << "[mc-shadow] sin expectaciones (no hay "
                                     "@Macros con args literales)\n";
                    }
                }
            }
        }

        return rc;
    }

    // Compilar un archivo .vel a .velb
    // vm.exe --build src/main.vel -o main.velb
    if (result.count("build")) {
        return asm_multi_process::run_worker(
            result["build"].as<std::string>(), out_prefix,
            /*skip_preprocessor=*/false,
            /*keep_labels=*/(result.count("keep-labels") > 0),
            /*ir_section_bytes=*/nullptr,
            /*emit_map=*/(result.count("emit-map") > 0));
    }

    // Abrir el REPL interactivo VestaShell (--interprete)
    if (result.count("interprete")) {
        vsh::VshInterpreter interp;
        // ARGV vacio o con [""]
        std::vector<std::string> empty_argv = {""};
        interp.set_argv(empty_argv);
        interp.run_interactive();
        return EXIT_SUCCESS;
    }

    // Ejecutar un fichero VestaShell directamente sin abrir el REPL
    // vm.exe --script mi_script.vsh [args extra para el script...]
    if (result.count("script")) {
        const std::string &vsh_path = result["script"].as<std::string>();

        // Construir ARGV: [vsh_path, args_posicionales_extra...]
        std::vector<std::string> script_args;
        script_args.push_back(vsh_path);
        if (result.count("positional")) {
            for (const auto &a :
                 result["positional"].as<std::vector<std::string>>()) {
                script_args.push_back(a);
            }
        }

        try {
            vsh::VshInterpreter interp;   // sin callback REPL
            interp.set_argv(script_args); // <--- AQUI lo importante
            interp.exec_file(vsh_path);
        } catch (const vsh::VshRuntimeError &e) {
            std::cerr << "[script] Error en " << vsh_path;
            if (e.line > 0) std::cerr << ":" << e.line;
            std::cerr << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        } catch (const vsh::VshParseError &e) {
            std::cerr << "[script] Error de sintaxis en " << vsh_path;
            if (e.line > 0) std::cerr << ":" << e.line;
            std::cerr << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        } catch (const std::exception &e) {
            std::cerr << "[script] " << e.what() << "\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    // Ejecutar un archivo .velb en la VM
    // vm.exe --run program.velb
    if (result.count("run")) {
        const std::string &velb_path = result["run"].as<std::string>();
        size_t num_schedulers = result["schedulers"].as<size_t>();

        try {
            Timer t_total_run;
            Timer t_construct;
            runtime::ManageVM mgr(nullptr, 0);
            runtime::VM *vm = mgr.loader.create_vm_instance(num_schedulers);
            if (!vm) {
                std::cerr << "Error: no se pudo crear la instancia de VM\n";
                return EXIT_FAILURE;
            }
            const long long ns_construct = t_construct.ns();

            // aplicar configuracion distribuida si el usuario paso algun flag
            // --dist-*
            bool has_dist = result.count("dist-port") > 0 ||
                            result.count("dist-discover") > 0 ||
                            result.count("dist-add-node") > 0 ||
                            result.count("dist-tls") > 0 ||
                            result.count("dist-name") > 0 ||
                            result.count("dist-token") > 0 ||
                            result.count("dist-node-id") > 0;
            if (has_dist) apply_dist_config(vm, result);

            // Phase M.sandbox (orden critico): pre-activar `sandbox_active`
            // ANTES de load_executable si las caps seran restringidas.
            // load_executable hace el eager-compile de main, cuyo guard de
            // seguridad consulta sandbox_active; si el flag se seteara solo
            // DESPUES (al aplicar exe.caps mas abajo), el eager-compile veria
            // sandbox_active=false -> JIT-compilaria main saltandose el check
            // de capabilities -> bypass del sandbox bajo JIT.
            if (result.count("vex-caps")) {
                const std::string cs = result["vex-caps"].as<std::string>();
                if (!cs.empty() && !::loader::parse_caps(cs).unrestricted()) {
                    mgr.loader.sandbox_active = true;
                }
            }

            Timer t_load;
            runtime::ProcessVM *proc =
                mgr.loader.load_executable(*vm, velb_path);
            if (!proc) {
                std::cerr << "Error: no se pudo cargar el ejecutable\n";
                return EXIT_FAILURE;
            }
            // Phase M.sandbox: aplicar @c --vex-caps al modulo cargado.
            // El primer Executable del pool es el que acabamos de cargar.
            if (result.count("vex-caps") && !mgr.loader.executables.empty()) {
                const std::string caps_str =
                    result["vex-caps"].as<std::string>();
                if (!caps_str.empty()) {
                    auto &exe = *mgr.loader.executables.front();
                    exe.caps = ::loader::parse_caps(caps_str);
                    // Activar el sandbox solo si las caps son restringidas;
                    // asi check_cap_at_pc paga overhead unicamente cuando hay
                    // un sandbox real en juego.
                    if (!exe.caps.unrestricted()) {
                        mgr.loader.sandbox_active = true;
                    }
                    std::cerr << "[sandbox] modulo principal con caps: "
                              << ::loader::caps_to_string(exe.caps) << "\n";
                }
            }
            const long long ns_load = t_load.ns();

            // Argumentos del script Vex.  Convencion: el path del .velb es
            // el "argv[0]" implicito del programa; los positionals que el
            // usuario pasa tras `--run prog.velb` son args[0..N-1] desde el
            // punto de vista del programa Vex (mismo modelo que VSH).  Los
            // builtins Vex `args_count()` y `args_get(i)` los exponen via
            // los opcodes getargc/getarg.
            if (result.count("positional")) {
                vm->script_args =
                    result["positional"].as<std::vector<std::string>>();
            }

            vm->make_ready(proc->pid);

            // Servidor de depuracion opcional (--debug-port).  Se
            // instancia ANTES de vm.start() para que los clientes ya
            // puedan conectarse antes de que el primer proceso comience.
            // El Debugger es heap-allocado y se libera al exit del scope
            // (RAII via unique_ptr).  Pone has_hooks=true en cada
            // scheduler para activar el slow path con FSM completa que
            // invoca on_before_exec antes de cada instruccion.
            //
            // Sin --debug-port, el unique_ptr queda vacio y el coste
            // runtime es exactamente cero: ningun scheduler tiene
            // has_hooks activo y el fast path se ejecuta sin checks.
            std::unique_ptr<debug::Debugger> dbg;
            if (result.count("debug-port")) {
                uint16_t dbg_port = result["debug-port"].as<uint16_t>();
                if (dbg_port == 0) dbg_port = debug::DBG_DEFAULT_PORT;
                dbg = std::make_unique<debug::Debugger>(*vm);
                if (!dbg->start(dbg_port)) {
                    std::cerr << "Warning: no se pudo iniciar el servidor "
                                 "de depuracion en puerto "
                              << dbg_port << "; continuando sin debugger.\n";
                    dbg.reset();
                } else {
                    vm->debugger = dbg.get();
                    for (auto &sched : vm->schedulers) {
                        sched->has_hooks = true;
                    }
                    // Pausa el proceso main ANTES de su primera
                    // instruccion: sin esto, programas cortos podrian
                    // terminar antes de que el cliente conecte.  El
                    // usuario tipicamente conecta el cliente VSH, hace
                    // `attach 0`, pone breakpoints (si los necesita), y
                    // emite `continue 0` para arrancar la ejecucion.
                    dbg->pause_at_start(proc->pid.local_pid);
                    vesta::scout() << "[debugger] servidor TCP escuchando en "
                                   << "puerto " << dbg_port << "\n";
                    vesta::scout() << "[debugger] proceso main pausado al "
                                   << "inicio (pid=" << proc->pid.local_pid
                                   << "); conecta el cliente y emite "
                                   << "'continue " << proc->pid.local_pid
                                   << "' para arrancar.\n";
                }
            }

            Timer t_run;
            Timer t_start_phase;
            vm->start();
            const long long ns_start = t_start_phase.ns();

            Timer t_poll;
            // en lugar de polling con sleep_for(1ms)
            // (granularity ~15.6ms en Windows), bloquear con condition
            // variable.  El ultimo scheduler que ponga vm_running=false
            // notifica done_cv y main desbloquea inmediatamente.
            {
                std::unique_lock<std::mutex> lk(vm->done_mtx);
                vm->done_cv.wait(lk, [&] {
                    return !vm->vm_running.load(std::memory_order_acquire);
                });
            }
            const long long ns_poll = t_poll.ns();

            long long elapsed_ns = t_run.ns();

            Timer t_stop;
            vm->stop();
            const long long ns_stop = t_stop.ns();
            const long long ns_total_run = t_total_run.ns();

            if (result.count("stats")) {
                long long elapsed_ms = elapsed_ns / 1'000'000;
                long long elapsed_us = elapsed_ns / 1'000;

                uint64_t total_instrs = 0;
                uint64_t total_jit_instrs = 0;
                uint64_t active_time_ns = 0;
                uint64_t jit_time_ns = 0;
                for (const auto &sched : vm->schedulers) {
                    total_instrs += sched->profiler_instr_counter;
                    /* JIT instrs: cada metodo JIT-eated incrementa el contador
                     * con N IR-instrs al entrar (proxy de bytecode instrs).
                     * Combinarlo con interp counter da MIPS reales. */
                    total_jit_instrs += sched->profiler_jit_instr_counter;
                    active_time_ns += sched->time_exec + sched->time_decode;
                    jit_time_ns += sched->time_jit;
                }
                /* active_time incluye el JIT time tambien (solo cuenta cuando
                 * el JIT-method retorna).  Si JIT corre durante interp
                 * dispatch, el time_jit ya esta incluido en time_exec via el
                 * wrapper. */

                for (auto &sched : vm->schedulers) {
                    vesta::scout()
                        << "[Scheduler " << sched->id_scheduler
                        << "] Estados de procesos: "
                        << sched->ready_queue.size() << " "
                        << sched->processes.size() << " " << sched->is_waiting
                        << " " << sched->should_kill // indica si la instancia
                                                     // debe morir.
                        << std::endl;
                    vesta::scout() << sched->to_string() << std::endl;
                    for (auto &p : sched->processes) {
                        vesta::scout() << "\t[Process " << p->pid.local_pid
                                       << "] Estados de procesos: "
                                       << runtime::vm_state_to_str(p->state)
                                       << " " << std::endl;
                        vesta::scout() << p->to_string() << std::endl;
                    }
                }

                /* TOTAL instructions = interp + JIT.  El interp counter
                 * se incrementa por instr ejecutada (profiler_instr_counter
                 * sin multiplicar - antes habia un fudge de *256 que ya no
                 * aplica).  El JIT counter se incrementa por N (IR instrs
                 * del metodo) al ENTRY de cada invocacion JIT-eated. */
                const uint64_t total_combined = total_instrs + total_jit_instrs;
                // sin hooks time_decode/time_exec son 0; usar wall time como
                // base
                uint64_t mips_base_ns =
                    active_time_ns > 0 ? active_time_ns : (uint64_t)elapsed_ns;
                double mips = total_combined > 0 && mips_base_ns > 0
                                  ? (total_combined * 1000.0) / mips_base_ns
                                  : 0.0;

                vesta::scout() << "\n=== RUN STATS ===\n";
                vesta::scout()
                    << "Wall time:     " << elapsed_ns << " ns  (" << elapsed_us
                    << " us, " << elapsed_ms << " ms)\n";
                if (active_time_ns > 0) {
                    vesta::scout()
                        << "Tiempo activo: " << active_time_ns << " ns  ("
                        << active_time_ns / 1000 << " us, "
                        << active_time_ns / 1'000'000 << " ms)\n";
                }
                vesta::scout() << "Instrucciones: " << total_combined
                               << "  (interp=" << total_instrs
                               << ", JIT=" << total_jit_instrs << ")\n";
                if (jit_time_ns > 0) {
                    vesta::scout() << "JIT time:      " << jit_time_ns
                                   << " ns  (" << jit_time_ns / 1000 << " us, "
                                   << jit_time_ns / 1'000'000 << " ms)\n";
                    if (total_jit_instrs > 0) {
                        const double jit_mips =
                            (total_jit_instrs * 1000.0) / jit_time_ns;
                        vesta::scout() << "JIT MIPS:      " << jit_mips << "\n";
                    }
                }
                vesta::scout()
                    << "MIPS:          " << mips
                    << (active_time_ns > 0 ? "" : "  (wall time)") << "\n";
                vesta::scout() << "\n=== OVERHEAD BREAKDOWN ===\n";
                vesta::scout()
                    << "VM construct:    " << ns_construct / 1000 << " us\n";
                vesta::scout()
                    << "load_executable: " << ns_load / 1000 << " us\n";
                vesta::scout() << "vm.start:        " << ns_start / 1000
                               << " us  (lanzar threads)\n";
                vesta::scout() << "wait until done: " << ns_poll / 1000
                               << " us  (cv wait)\n";
                vesta::scout() << "vm.stop:         " << ns_stop / 1000
                               << " us  (join threads)\n";
                vesta::scout()
                    << "Total --run:     " << ns_total_run / 1000 << " us\n";
            }

            /* --jit-stats: imprimir contadores del JIT al final.
             * Independiente de --stats; util para auditar % de funciones
             * JIT-eatables en programas reales. */
            if (jit_stats_requested) {
                vesta::scout() << "\n=== JIT STATS ===\n";
                vesta::scout() << jit::get_jit_stats_summary() << "\n";
            }
        } catch (const std::exception &e) {
            std::cerr << "Error al ejecutar " << velb_path << ": " << e.what()
                      << "\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    cli::Config cfg;
    cfg.history_file = "my_vm_history.txt";
    cfg.history_max = 1000;
    cfg.prompt = "vesta> ";
    cfg.multiline_end = ";;";

    cli::VestaViewManager vm(cfg);

    vm.set_execute_callback([](const std::string &cmd) {
        auto fut = runtime::run_command_async(cmd);
        std::thread([f = std::move(fut)]() mutable {
            try {
                auto out = f.get();
                if (!out.empty()) vesta::scout() << out << std::endl;
            } catch (const std::exception &e) {
                std::cerr << "Runtime error: " << e.what() << std::endl;
            }
        }).detach();
    });

    vm.run();

    return EXIT_SUCCESS;
}
