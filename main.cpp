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
#include "cli/runtime_api_commands.h"
#include "util/assembler_multiprocess.h"
#include "util/sqlite_singleton.h"
#include "util/fs_utils.h"
#include "runtime/manager_runtime.h"
#include "loader/loader.h"
#include "profiler/timer.h"
#include "distrib/dist_runtime.h"
#include "distrib/dist_debug.h"
#include "distrib/node_registry.h"
#ifdef VESTA_HAS_PREPROCESSOR
    #include "preprocessor/preprocessor.h"
#endif

// flag global para el modo --dist-server; SIGINT lo pone a false
static std::atomic<bool> g_server_running{true};
static void on_dist_sigint(int) { g_server_running.store(false); }

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
static distrib::NodeAuthConfig build_node_auth(
    const std::string &token,
    bool               use_tls,
    const std::string &cert,
    const std::string &key,
    const std::string &ca)
{
    distrib::NodeAuthConfig auth{};
    if (!token.empty()) {
        auth.use_token = true;
        SHA256(reinterpret_cast<const unsigned char *>(token.c_str()),
               token.size(), auth.token_hash);
    }
    if (use_tls) {
        auth.use_tls = true;
        std::snprintf(auth.cert_path, sizeof(auth.cert_path), "%s", cert.c_str());
        std::snprintf(auth.key_path,  sizeof(auth.key_path),  "%s", key.c_str());
        std::snprintf(auth.ca_path,   sizeof(auth.ca_path),   "%s", ca.c_str());
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
 * Registra cualquier nodo estatico indicado con --dist-add-node (formato IP:PUERTO).
 *
 * @param vm     Instancia VM sobre la que se aplica la configuracion.
 * @param result Resultado del parseo de cxxopts con todos los flags.
 */
static void apply_dist_config(runtime::VM *vm, const cxxopts::ParseResult &result)
{
    const std::string token   = result["dist-token"].as<std::string>();
    const bool        use_tls = result.count("dist-tls") > 0;
    const std::string cert    = result["dist-cert"].as<std::string>();
    const std::string key     = result["dist-key"].as<std::string>();
    const std::string ca      = result["dist-ca"].as<std::string>();

    // construir configuracion del DistRuntime
    distrib::DistRuntimeConfig cfg{};
    cfg.local_node_id    = result["dist-node-id"].as<uint64_t>();
    cfg.vdp_listen_port  = result["dist-port"].as<uint16_t>();
    cfg.discover_port    = result["dist-discover-port"].as<uint16_t>();
    cfg.enable_discovery = result.count("dist-discover") > 0;

    std::string name = result["dist-name"].as<std::string>();
    if (!name.empty())
        std::snprintf(cfg.local_node_name, sizeof(cfg.local_node_name), "%s", name.c_str());
    else
        std::snprintf(cfg.local_node_name, sizeof(cfg.local_node_name), "vm-%llu",
                      static_cast<unsigned long long>(vm->id));

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
                msg += " con descubrimiento UDP (puerto " + std::to_string(cfg.discover_port) + ")";
            vesta::scout() << msg << "\n";
        } else {
            std::cerr << "[dist] Error al iniciar el servidor VDP\n";
        }
    }

    // registrar nodos estaticos proporcionados con --dist-add-node IP:PUERTO
    if (result.count("dist-add-node")) {
        distrib::NodeAuthConfig node_auth = build_node_auth(token, use_tls, cert, key, ca);
        for (auto &spec : result["dist-add-node"].as<std::vector<std::string>>()) {
            auto colon = spec.rfind(':');
            if (colon == std::string::npos) {
                std::cerr << "[dist] Formato invalido (esperado IP:PUERTO): " << spec << "\n";
                continue;
            }
            std::string node_ip   = spec.substr(0, colon);
            uint16_t    node_port = 0;
            try { node_port = static_cast<uint16_t>(std::stoul(spec.substr(colon + 1))); }
            catch (...) { std::cerr << "[dist] Puerto invalido en: " << spec << "\n"; continue; }

            uint32_t idx = vm->dist_runtime->add_node(
                node_ip.c_str(), node_port, node_auth, node_ip.c_str());
            vesta::scout() << "[dist] Nodo registrado: " << node_ip << ":"
                           << node_port << " (idx=" << idx << ")\n";
        }
    }
}


int main(int argc, char *argv[]) {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
    asm_multi_process::run_and_capture("chcp 65001");
#endif

    cxxopts::Options options("VMProject", "Virtual Machine Example");

    options.add_options()
            ("h,help", "Mostrar ayuda")
            ("o,output", "Archivo de salida (sin extensión o completo)", cxxopts::value<std::string>())
            ("driver", "Compilar un directorio completo en paralelo", cxxopts::value<std::string>())
            ("worker", "Compilar un único archivo (modo interno)", cxxopts::value<std::string>())
            ("j,threads", "Número de hilos para el driver", cxxopts::value<int>()->default_value("0"))
            ("v,version", "Mostrar versión")
            ("m,mode", "Modo de ejecución (vm/jit)", cxxopts::value<std::string>()->default_value("vm"))
            ("list-arch", "Imprimir arquitecturas soportadas")
            ("asm-file", "Archivo ASM a ensamblar", cxxopts::value<std::string>())
            ("disasm-file", "Archivo binario a desensamblar", cxxopts::value<std::string>())
            ("arch", "Arquitectura para ensamblar/desensamblar", cxxopts::value<std::string>())
            ("save-output", "Guardar código ensamblado/desensamblado en archivo")
            ("output-prefix", "Prefijo/nombre base para archivos de salida",
             cxxopts::value<std::string>()->default_value("out"))
            ("run", "Ejecutar un archivo .velb en la VM", cxxopts::value<std::string>())
            ("build", "Compilar un archivo .vel a .velb", cxxopts::value<std::string>())
            ("schedulers", "Número de schedulers para el comando run", cxxopts::value<size_t>()->default_value("1"))
            ("stats", "Mostrar estadísticas de ejecución al finalizar (tiempo, MIPS)")
            // ---- opciones de runtime distribuido ----
            ("dist-port",         "Puerto VDP del servidor distribuido (0 = sin servidor TCP)",
                cxxopts::value<uint16_t>()->default_value("0"))
            ("dist-discover",     "Activar descubrimiento UDP de nodos en la LAN")
            ("dist-discover-port","Puerto UDP para descubrimiento de nodos",
                cxxopts::value<uint16_t>()->default_value("7790"))
            ("dist-name",         "Nombre del nodo local (cadena identificativa)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-node-id",      "ID de 64 bits del nodo (0 = generar automaticamente)",
                cxxopts::value<uint64_t>()->default_value("0"))
            ("dist-add-node",     "Nodo estatico a registrar y conectar (formato IP:PUERTO, repetible)",
                cxxopts::value<std::vector<std::string>>())
            ("dist-token",        "Token de autenticacion en texto plano (se almacena como SHA-256)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-tls",          "Usar TLS en las conexiones VDP salientes y entrantes")
            ("dist-cert",         "Ruta al certificado TLS del nodo local (PEM)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-key",          "Ruta a la clave privada TLS del nodo local (PEM)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-ca",           "Ruta al CA bundle TLS para verificar pares (PEM)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-server",       "Modo servidor distribuido puro: espera conexiones VDP sin ejecutar bytecode")
            ("dist-debug",        "Activar trazas de depuracion del subsistema distribuido (RSPAWN, HALT, FUTURE_FULFILL)")
            ("script",            "Ejecutar un fichero VestaShell (.vsh) y salir", cxxopts::value<std::string>())
            ("interprete",        "Abrir el interprete interactivo VestaShell (REPL .vsh)")
#ifdef VESTA_HAS_PREPROCESSOR
            ("preprocess-only", "Solo preprocesar un .vel y mostrar/guardar el resultado (debug)", cxxopts::value<std::string>())
#endif
            ;

    auto result = options.parse(argc, argv);

    // activar trazas de depuracion del subsistema distribuido si se paso --dist-debug
    if (result.count("dist-debug"))
        distrib::set_dist_debug(true);

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
        for (auto &a: archs.capstone) vesta::scout() << "  " << a << "\n";
        vesta::scout() << "Keystone supported architectures:\n";
        for (auto &a: archs.keystone) vesta::scout() << "  " << a << "\n";
        return 0;
    }

#ifdef VESTA_HAS_PREPROCESSOR
    // Solo preprocesar: expandir macros y directivas sin ensamblar
    // vm.exe --preprocess-only src/main.vel [-o salida.vel]
    if (result.count("preprocess-only")) {
        const std::string& src_path = result["preprocess-only"].as<std::string>();

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
        std::string exe_dir =
            std::filesystem::path(fs::get_executable_path()).parent_path().string();
        pp.options().import_paths.push_back(exe_dir + "/preprocessor/include_lib");
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
        for (const auto& d : pp.diagnostics().diagnostics()) {
            std::cerr << d.loc.file << ":" << d.loc.line << ":"
                      << d.loc.col  << ": "
                      << (d.level >= vpp::DiagLevel::ERR ? "error: " : "warning: ")
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
                std::cerr << "error: no se puede escribir: " << out_file << "\n";
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
    // vm.exe --dist-server --dist-port 7789 [--dist-discover] [--dist-name nodo1]
    //        [--dist-add-node 192.168.1.100:7789] [--dist-token secreto]
    //        [--dist-tls --dist-cert cert.pem --dist-key key.pem --dist-ca ca.pem]
    // -----------------------------------------------------------------------
    if (result.count("dist-server")) {
        try {
            runtime::ManageVM dist_mgr(nullptr, 0);
            runtime::VM *vm = dist_mgr.loader.create_vm_instance(1);
            if (!vm) {
                std::cerr << "[dist-server] Error: no se pudo crear la instancia VM\n";
                return EXIT_FAILURE;
            }

            apply_dist_config(vm, result);

            // activar modo persistente para que el scheduler no termine al no haber procesos;
            // los procesos remotos llegan via rspawn despues del arranque
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

    // Compilar un archivo como worker
    // vm.exe --worker src/main.vel -o main.velb
    if (result.count("worker")) {
        return asm_multi_process::run_worker(
            result["worker"].as<std::string>(),
            out_prefix
        );
    }

    // Compilar un proyecto entero en paralelo
    // vm.exe --driver src/ -j 8 -o program.velb
    // Compilar con número automático de hilos
    // vm.exe --driver src/ -j 0 -o program.velb
    if (result.count("driver")) {
        int threads = result["threads"].as<int>();
        return asm_multi_process::run_driver(
            result["driver"].as<std::string>(),
            threads,
            out_prefix
        );
    }


    bool save_output = result.count("save-output") > 0;

    // ensamblar un solo archivo (modo clásico)
    // vm.exe --asm-file src/main.asm --arch x86_64
    if (result.count("asm-file")) {
        return assemble_file(
                   result["asm-file"].as<std::string>(),
                   result["arch"].as<std::string>(),
                   save_output,
                   out_prefix
               )
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }

    if (result.count("disasm-file")) {
        if (!result.count("arch")) {
            std::cerr << "--arch es requerido para desensamblar\n";
            return EXIT_FAILURE;
        }
        return disassemble_file(
                   result["disasm-file"].as<std::string>(),
                   result["arch"].as<std::string>(),
                   save_output,
                   out_prefix
               )
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }

    // Compilar un archivo .vel a .velb
    // vm.exe --build src/main.vel -o main.velb
    if (result.count("build")) {
        return asm_multi_process::run_worker(
            result["build"].as<std::string>(),
            out_prefix
        );
    }

    // Abrir el REPL interactivo VestaShell (--interprete)
    if (result.count("interprete")) {
        vsh::VshInterpreter interp;
        interp.run_interactive();
        return EXIT_SUCCESS;
    }

    // Ejecutar un fichero VestaShell directamente sin abrir el REPL
    // vm.exe --script mi_script.vsh
    if (result.count("script")) {
        const std::string &vsh_path = result["script"].as<std::string>();
        try {
            vsh::VshInterpreter interp; // sin callback REPL
            interp.exec_file(vsh_path);
        } catch (const vsh::VshRuntimeError &e) {
            std::cerr << "[script] Error en " << vsh_path << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        } catch (const vsh::VshParseError &e) {
            std::cerr << "[script] Error de sintaxis en " << vsh_path << ": " << e.what() << "\n";
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
        const std::string &velb_path      = result["run"].as<std::string>();
        size_t             num_schedulers = result["schedulers"].as<size_t>();

        try {
            runtime::ManageVM mgr(nullptr, 0);
            runtime::VM *     vm = mgr.loader.create_vm_instance(num_schedulers);
            if (!vm) {
                std::cerr << "Error: no se pudo crear la instancia de VM\n";
                return EXIT_FAILURE;
            }
            // aplicar configuracion distribuida si el usuario paso algun flag --dist-*
            bool has_dist = result.count("dist-port")        > 0 ||
                            result.count("dist-discover")    > 0 ||
                            result.count("dist-add-node")    > 0 ||
                            result.count("dist-tls")         > 0 ||
                            result.count("dist-name")        > 0 ||
                            result.count("dist-token")       > 0 ||
                            result.count("dist-node-id")     > 0;
            if (has_dist) apply_dist_config(vm, result);

            runtime::ProcessVM *proc = mgr.loader.load_executable(*vm, velb_path);
            if (!proc) {
                std::cerr << "Error: no se pudo cargar el ejecutable\n";
                return EXIT_FAILURE;
            }
            vm->make_ready(proc->pid);

            Timer t_run;
            vm->start();
            while (vm->has_alive_processes()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            long long elapsed_ns = t_run.ns();
            vm->stop();

            if (result.count("stats")) {
                long long elapsed_ms = elapsed_ns / 1'000'000;
                long long elapsed_us = elapsed_ns / 1'000;

                uint64_t total_instrs   = 0;
                uint64_t active_time_ns = 0;
                for (const auto &sched: vm->schedulers) {
                    total_instrs += sched->profiler_instr_counter;
                    active_time_ns += sched->time_exec + sched->time_decode;
                }


                for (auto &sched: vm->schedulers) {
                    vesta::scout()
                            << "[Scheduler " << sched->id_scheduler
                            << "] Estados de procesos: " << sched->ready_queue.size()
                            << " "
                            << sched->processes.size()
                            << " "
                            << sched->is_waiting
                            << " "
                            << sched->should_kill // indica si la instancia debe morir.
                            << std::endl;
                    vesta::scout() << sched->to_string() << std::endl;
                    for (auto &p: sched->processes) {
                        vesta::scout()
                                << "\t[Process " << p->pid.local_pid
                                << "] Estados de procesos: " << runtime::vm_state_to_str(p->state)
                                << " "
                                << std::endl;
                        vesta::scout() << p->to_string() << std::endl;
                    }
                }

                // sin hooks time_decode/time_exec son 0; usar wall time como base
                uint64_t mips_base_ns = active_time_ns > 0 ? active_time_ns : (uint64_t) elapsed_ns;
                double   mips         = total_instrs > 0 && mips_base_ns > 0
                                  ? (total_instrs * 1000.0) / mips_base_ns
                                  : 0.0;

                vesta::scout() << "\n=== RUN STATS ===\n";
                vesta::scout() << "Wall time:     " << elapsed_ns << " ns  ("
                        << elapsed_us << " us, " << elapsed_ms << " ms)\n";
                if (active_time_ns > 0) {
                    vesta::scout() << "Tiempo activo: " << active_time_ns << " ns  ("
                            << active_time_ns / 1000 << " us, "
                            << active_time_ns / 1'000'000 << " ms)\n";
                }
                vesta::scout() << "Instrucciones: " << total_instrs << "\n";
                vesta::scout() << "MIPS:          " << mips
                        << (active_time_ns > 0 ? "" : "  (wall time)") << "\n";
            }
        } catch (const std::exception &e) {
            std::cerr << "Error al ejecutar " << velb_path << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    cli::Config cfg;
    cfg.history_file  = "my_vm_history.txt";
    cfg.history_max   = 1000;
    cfg.prompt        = "vesta> ";
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
