/*
* VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file cli.cpp
 * @brief Implementacion del interprete interactivo (REPL) de VestaVM.
 *
 * Implementa @c cli::VestaViewManager y @c cli::VestaInterprete:
 * lectura de linea con historial, despacho de comandos mediante tabla de
 * registros (@c CmdEntry), callbacks y parseo de parametros con comillas.
 *
 * Para anadir un nuevo comando basta con:
 *   1. Escribir una funcion @c static void mi_cmd(const std::string &args).
 *   2. Anadir una entrada @c CmdEntry en @c cmd_table con el nombre, uso, descripcion y handler.
 * No se necesita modificar el bucle principal de @c VestaViewManager::run().
 */
#include "cli/cli.h"

#include <cstdio>

#include "cli/cli_init_manager_and_server.h"
#include "cli/sync_io.h"
#include "emmit/parser_to_bytecode.h"
#include "util/assembler_multiprocess.h"
#include "assembly/assembly.h"
#include "runtime/decode_table.h"
#include "runtime/proceso_runtime.h"
#include "disasm/disasm.h"
#include "util/ansi.h"
#include "util/fs_utils.h"
#include <cstring>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <fstream>
#include <filesystem>
#ifdef VESTA_HAS_PREPROCESSOR
#  include "preprocessor/preprocessor.h"
#endif

#if defined(_WIN32)
#  include <conio.h>
#else
#  include <termios.h>
#  include <unistd.h>
#endif

namespace cli {
    static ManagerOfManagersAndServer mgr = ManagerOfManagersAndServer();

    using namespace Assembly::Bytecode;

    /**
     * @brief Estado interno del REPL: configuracion, historial, cola de comandos y worker.
     */
    struct Impl {
        Config cfg;

        // estado
        std::atomic<bool> running{false};
        std::atomic<bool> interrupted_flag{false};

        // historial
        std::vector<std::string> history;
        std::mutex               hist_m;
        int                      history_cursor = -1;

        // cola de comandos
        std::deque<std::string> cmd_queue;
        std::mutex              q_m;
        std::condition_variable q_cv;
        bool                    q_closed = false;

        // worker
        std::thread worker_thread;

        // callback
        std::function<void(const std::string &)> vm_execute_cb;
        std::mutex                               cb_m;

        Impl(Config c) : cfg(std::move(c)) {}
    };

    // ---- utilidades generales ----

    /**
     * @brief Divide una cadena en palabras separadas por espacios.
     * @param s Cadena de entrada.
     * @return Vector de palabras.
     */
    static std::vector<std::string> split_words(const std::string &s) {
        std::istringstream       iss(s);
        std::vector<std::string> out;
        std::string              w;
        while (iss >> w) out.push_back(w);
        return out;
    }

    /**
     * @brief Expande "~" al directorio HOME y devuelve un std::filesystem::path.
     * @param p Cadena de ruta, opcionalmente comenzando con "~".
     * @return Ruta expandida; directorio actual si @p p esta vacio.
     */
    static std::filesystem::path expand_path(const std::string &p) {
        if (p.empty()) return std::filesystem::current_path();
        if (p[0] == '~') {
            const char *home = std::getenv(
#if defined(_WIN32)
                "USERPROFILE"
#else
                "HOME"
#endif
            );
            std::string home_s = home ? home : "";
            if (p.size() == 1)      return std::filesystem::path(home_s);
            if (!home_s.empty())    return std::filesystem::path(home_s) / p.substr(2);
            return std::filesystem::path(p.substr(1));
        }
        return std::filesystem::path(p);
    }

    /**
     * @brief Limpia la pantalla de la consola de forma portatil.
     *
     * En POSIX usa la secuencia ANSI ESC[2J ESC[H.
     * En Windows intenta habilitar Virtual Terminal Processing y, si falla,
     * recurre a system("cls").
     */
    static void clear_screen() {
#if defined(_WIN32)
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD dwMode = 0;
            if (GetConsoleMode(hOut, &dwMode)) {
                if ((dwMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
                    if (SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                        std::cout << "\x1B[2J\x1B[H" << std::flush;
                        return;
                    }
                } else {
                    std::cout << "\x1B[2J\x1B[H" << std::flush;
                    return;
                }
            }
        }
        std::system("cls");
#else
        std::cout << "\x1B[2J\x1B[H" << std::flush;
#endif
    }

    /**
     * @brief Ejecuta un comando de shell y devuelve su salida completa (stdout+stderr).
     * @param cmd Comando a ejecutar.
     * @return Salida combinada del proceso, con el codigo de retorno al final.
     * @throws std::runtime_error si no se puede abrir el proceso.
     */
    static std::string execute_shell_command(const std::string &cmd) {
        std::string full_cmd;
#if defined(_WIN32)
        full_cmd   = "cmd.exe /C \"" + cmd + "\" 2>&1";
        FILE *pipe = _popen(full_cmd.c_str(), "r");
#else
        full_cmd = cmd + " 2>&1";
        FILE *pipe = popen(full_cmd.c_str(), "r");
#endif
        if (!pipe) throw std::runtime_error("failed to open pipe for command");

        std::string result;
        char        buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) result.append(buf);

#if defined(_WIN32)
        int rc = _pclose(pipe);
#else
        int rc = pclose(pipe);
#endif
        result += "\n[exit code: " + std::to_string(rc) + "]";
        return result;
    }

    // ---- implementaciones de comandos ----
    // Cada funcion recibe la cadena de argumentos que sigue al nombre del comando.

    // hook noop: activa la medicion de time_exec en el scheduler sin hacer nada
    static void noop_debug_hook(runtime::ProcessVM *, runtime::DebugStage) {}

    /**
     * @brief Lista todos los managers activos con su ID, nombre y numero de VMs.
     * @param args Sin uso (puede estar vacio).
     */
    static void command_vms(const std::string &) {
        auto snaps = mgr.snapshot();
        if (snaps.empty()) { std::cout << "(no hay managers activos)\n"; return; }
        for (auto &[id, snap] : snaps) {
            std::cout << "[" << id << "] "
                      << snap.name_manager
                      << "  VMs=" << snap.vm_count
                      << (snap.has_listener ? "  TCP=si" : "  TCP=no") << "\n";
        }
    }

    /**
     * @brief Ejecuta un .velb en background creando un manager temporal.
     *
     * Formato: <nombre> <ruta.velb> [--schedulers N]
     *
     * @param args Argumentos del comando: nombre, ruta y opciones opcionales.
     */
    static void command_run(const std::string &args) {
        CmdParams p = parse_cmd_params(args);
        if (!p.valid) {
            vesta::scout() << "Uso: run <nombre> <ruta.velb> [--schedulers N] [--stats]\n";
            return;
        }

        auto path = fs::normalize_path_safe(p.path);
        if (!fs::file_exists(path)) {
            vesta::scout() << "No existe el archivo: " << path.string() << "\n"; return;
        }
        if (!fs::is_regular_file(path)) {
            vesta::scout() << "No es archivo regular: " << path.string() << "\n"; return;
        }
        if (!fs::can_read(path)) {
            vesta::scout() << "No se puede leer: " << path.string() << "\n"; return;
        }
        auto maybe_abs = fs::get_existing_absolute_path(path);
        if (!maybe_abs) {
            vesta::scout() << "No se pudo resolver la ruta: " << path.string() << "\n"; return;
        }

        // leer opciones --schedulers y --stats del resto de la linea
        size_t num_schedulers = 1;
        bool   show_stats     = false;
        {
            auto ws = split_words(p.rest);
            for (auto it = ws.begin(); it != ws.end(); ++it) {
                if (*it == "--schedulers" && std::next(it) != ws.end()) {
                    try { num_schedulers = std::stoul(*std::next(it)); } catch (...) {}
                }
                if (*it == "--stats") show_stats = true;
            }
        }

        std::string abs_path = maybe_abs->string();
        std::string name     = p.name;

        // registrar en mgr global para que sea visible por mgrinfo/vmstat/schedtop
        runtime::ManageVM *vm_mgr_raw = mgr.add_manager(name, nullptr);
        size_t registered_id = vm_mgr_raw->id;

        // lanzar en hilo separado para no bloquear el REPL
        std::thread([abs_path, name, num_schedulers, show_stats, vm_mgr_raw, registered_id]() {
            try {
                auto t0 = std::chrono::steady_clock::now();
                runtime::VM *vm = vm_mgr_raw->loader.create_vm_instance(num_schedulers);
                if (!vm) { vesta::scout() << "[run] '" << name << "': error al crear VM\n";
                           mgr.remove_manager(registered_id); return; }
                runtime::ProcessVM *proc = vm_mgr_raw->loader.load_executable(*vm, abs_path);
                if (!proc) { vesta::scout() << "[run] '" << name << "': error al cargar\n";
                             mgr.remove_manager(registered_id); return; }
                // activar hooks de medicion solo si el usuario pidio --stats
                if (show_stats) {
                    for (auto &sched : vm->schedulers)
                        sched->add_debug_hook(noop_debug_hook);
                }
                vm->make_ready(proc->pid);
                vm->start();
                while (vm->has_alive_processes())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                auto t1 = std::chrono::steady_clock::now();
                vm->stop();
                if (show_stats) {
                    double   elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
                    uint64_t total_instr = 0;
                    for (auto &sched : vm->schedulers)
                        total_instr += sched->profiler_instr_counter * 256ULL;
                    double mips = (elapsed_ms > 0)
                        ? (total_instr / 1e6) / (elapsed_ms / 1000.0) : 0.0;
                    std::ostringstream ss;
                    ss << ansi::c(ansi::BOLD) << ansi::c(ansi::BR_CYAN)
                       << "[stats] '" << name << "'" << ansi::c(ansi::RESET) << "\n";
                    ss << "  Tiempo:              " << std::fixed << std::setprecision(2)
                       << elapsed_ms << " ms\n";
                    ss << "  Instrucciones aprox: " << total_instr << "\n";
                    ss << "  MIPS aprox:          " << std::fixed << std::setprecision(2)
                       << mips << "\n";
                    ss << "  Schedulers:          " << vm->schedulers.size() << "\n";
                    for (size_t i = 0; i < vm->schedulers.size(); ++i) {
                        auto &sched = *vm->schedulers[i];
                        ss << "    [sched " << i << "]"
                           << "  instr="     << (sched.profiler_instr_counter * 256ULL)
                           << "  time_exec=" << sched.time_exec << " ns\n";
                    }
                    vesta::scout() << ss.str();
                }
                vesta::scout() << "[run] '" << name << "' finalizado\n";
            } catch (const std::exception &e) {
                vesta::scout() << "[run] '" << name << "' error: " << e.what() << "\n";
            }
            mgr.remove_manager(registered_id); // limpiar del registro al terminar
        }).detach();

        vesta::scout() << "[run] '" << name << "' lanzado en background (id=" << registered_id << ")\n";
    }

    /**
     * @brief Lista el contenido de un directorio con colores e informacion de cada entrada.
     *
     * Directorios en azul brillante, archivos en verde brillante.
     * Columnas: tipo (D/F), tamano, fecha de modificacion, nombre.
     * Orden: directorios primero, luego archivos, ambos alfabeticos.
     *
     * @param args Ruta del directorio; si esta vacio usa el directorio actual.
     */
    static void command_ls(const std::string &args) {
        namespace fsp = std::filesystem;
        fsp::path target = args.empty() ? fsp::current_path() : expand_path(args);

        if (!fsp::exists(target) || !fsp::is_directory(target)) {
            std::cout << "ls: no es un directorio: " << target.string() << "\n";
            return;
        }

        struct Entry {
            bool        is_dir;
            std::string name;
            uintmax_t   size_bytes;
            std::string mtime_str;
        };

        std::vector<Entry> entries;
        for (const auto &e : fsp::directory_iterator(target)) {
            Entry ent;
            ent.is_dir     = fsp::is_directory(e);
            ent.name       = e.path().filename().string();
            ent.size_bytes = 0;
            if (!ent.is_dir) {
                try { ent.size_bytes = fsp::file_size(e); } catch (...) {}
            }
            // convertir file_time_type a system_clock para formatear con strftime
            try {
                auto ft   = fsp::last_write_time(e);
                auto diff = ft - fsp::file_time_type::clock::now();
                auto stp  = std::chrono::system_clock::now()
                          + std::chrono::duration_cast<std::chrono::system_clock::duration>(diff);
                auto tt   = std::chrono::system_clock::to_time_t(stp);
                char buf[32] = "?";
                struct tm *tmi = std::localtime(&tt);
                if (tmi) std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tmi);
                ent.mtime_str = buf;
            } catch (...) { ent.mtime_str = "?"; }
            entries.push_back(std::move(ent));
        }

        // ordenar: directorios primero, luego archivos; alfabetico dentro de cada grupo
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.name < b.name;
        });

        // formatea tamano en unidades legibles
        auto fmt_size = [](uintmax_t sz) -> std::string {
            if (sz < 1024)            return std::to_string(sz) + " B";
            if (sz < 1024 * 1024)     return std::to_string(sz / 1024) + " KB";
            return std::to_string(sz / (1024 * 1024)) + " MB";
        };

        // cabecera
        std::cout << ansi::c(ansi::BOLD)
                  << std::left << std::setw(4) << "T"
                  << std::setw(12) << "Tamano"
                  << std::setw(20) << "Modificado"
                  << "Nombre" << ansi::c(ansi::RESET) << "\n"
                  << std::string(60, '-') << "\n";

        for (auto &e : entries) {
            const char *col    = e.is_dir ? ansi::c(ansi::BR_BLUE) : ansi::c(ansi::BR_GREEN);
            std::string type_s = e.is_dir ? "D" : "F";
            std::string size_s = e.is_dir ? "<DIR>" : fmt_size(e.size_bytes);
            std::string name_s = e.is_dir ? e.name + "/" : e.name;
            std::cout << col
                      << std::left << std::setw(4)  << type_s
                      << std::setw(12) << size_s
                      << std::setw(20) << e.mtime_str
                      << name_s << ansi::c(ansi::RESET) << "\n";
        }
    }

    /**
     * @brief Compila un archivo .vel a .velb usando el ensamblador en modo worker.
     * @param args "<archivo.vel> [-o <salida>]"
     */
    static void command_build(const std::string &args) {
        auto words = split_words(args);
        if (words.empty()) { std::cout << "Uso: build <archivo.vel> [-o <salida>]\n"; return; }
        std::string src = words[0];
        std::string out = "out"; // prefijo por defecto
        for (size_t i = 1; i + 1 < words.size(); ++i) {
            if (words[i] == "-o") { out = words[i + 1]; break; }
        }
        int rc = asm_multi_process::run_worker(src, out);
        if (rc == EXIT_SUCCESS)
            std::cout << "[build] OK -> " << out << ".velb\n";
        else
            std::cout << "[build] error (codigo " << rc << ")\n";
    }

    /**
     * @brief Preprocesa un archivo .vel con vpp y muestra o guarda el resultado.
     *
     * Expande macros, directivas #define/#if/#foreach/#import, etc. sin compilar.
     * Util para depurar macros y verificar la salida del preprocesador.
     *
     * Formato: <archivo.vel> [-o <salida>] [-D NAME[=val]] [-I ruta] [-M ruta]
     *
     * @param args Argumentos del comando.
     */
    static void command_vpp(const std::string &args) {
#ifndef VESTA_HAS_PREPROCESSOR
        vesta::scout() << "[vpp] El preprocesador no esta disponible en esta build "
                          "(recompilar con VESTA_BUILD_PREPROCESSOR=ON)\n";
        (void)args;
#else
        // --- parsear argumentos ---
        auto words = split_words(args);
        if (words.empty()) {
            vesta::scout() << "Uso: vpp <archivo.vel> [-o <salida>] "
                              "[-D NAME[=val]] [-I ruta] [-M ruta]\n";
            return;
        }

        std::string              src_file;
        std::string              out_file;   // vacio = stdout del REPL
        std::vector<std::string> defines;
        std::vector<std::string> inc_paths;
        std::vector<std::string> imp_paths;

        for (size_t i = 0; i < words.size(); ++i) {
            const auto &w = words[i];
            if (w == "-o" && i + 1 < words.size()) {
                out_file = words[++i];
            } else if (w.size() > 2 && w.substr(0, 2) == "-D") {
                defines.push_back(w.substr(2));
            } else if (w == "-D" && i + 1 < words.size()) {
                defines.push_back(words[++i]);
            } else if (w.size() > 2 && w.substr(0, 2) == "-I") {
                inc_paths.push_back(w.substr(2));
            } else if (w == "-I" && i + 1 < words.size()) {
                inc_paths.push_back(words[++i]);
            } else if (w.size() > 2 && w.substr(0, 2) == "-M") {
                imp_paths.push_back(w.substr(2));
            } else if (w == "-M" && i + 1 < words.size()) {
                imp_paths.push_back(words[++i]);
            } else if (src_file.empty()) {
                src_file = w;
            }
        }

        // --- leer el archivo fuente ---
        std::ifstream ifs(src_file, std::ios::binary);
        if (!ifs) {
            vesta::scout() << "[vpp] No se puede abrir: " << src_file << "\n";
            return;
        }
        std::string source((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());

        // --- configurar el preprocesador ---
        bool had_error = false;
        vpp::Preprocessor pp([&had_error](const vpp::Diagnostic &d) {
            vesta::scout() << d.format() << "\n";
            if (d.level >= vpp::DiagLevel::ERR) had_error = true;
        });

        // directorio del archivo fuente para #include relativos
        std::string src_dir =
            std::filesystem::path(src_file).parent_path().string();
        if (src_dir.empty()) src_dir = ".";
        pp.options().include_paths.push_back(src_dir);

        // rutas de stdlib (igual que en run_worker)
        std::string exe_dir =
            std::filesystem::path(fs::get_executable_path()).parent_path().string();
        pp.options().import_paths.push_back(exe_dir + "/preprocessor/include_lib");
        pp.options().import_paths.push_back(exe_dir + "/include_lib");
        pp.options().import_paths.push_back(src_dir);

        // rutas adicionales del usuario
        for (auto &p : inc_paths) pp.options().include_paths.push_back(p);
        for (auto &p : imp_paths) pp.options().import_paths.push_back(p);
        for (auto &d : defines)   pp.options().predefines.push_back(d);

        // macros de plataforma
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

        // --- ejecutar el preprocesador ---
        std::string result = pp.process(source, src_file);

        if (had_error) {
            vesta::scout() << "[vpp] preprocesado fallido con "
                           << pp.diagnostics().error_count() << " error(es)\n";
            return;
        }

        if (pp.diagnostics().warning_count() > 0)
            vesta::scout() << "[vpp] " << pp.diagnostics().warning_count()
                           << " advertencia(s)\n";

        // --- escribir resultado ---
        if (out_file.empty()) {
            // sin -o: imprimir directamente en el REPL con numeracion de lineas
            std::istringstream ss(result);
            std::string        line;
            size_t             n = 1;
            vesta::scout() << ansi::c(ansi::DIM)
                           << "--- " << src_file << " (preprocesado) ---\n"
                           << ansi::c(ansi::RESET);
            while (std::getline(ss, line)) {
                vesta::scout() << ansi::c(ansi::DIM)
                               << std::setw(4) << n++ << "  "
                               << ansi::c(ansi::RESET)
                               << line << "\n";
            }
            vesta::scout() << ansi::c(ansi::DIM) << "---\n" << ansi::c(ansi::RESET);
        } else {
            std::ofstream ofs(out_file, std::ios::binary);
            if (!ofs) {
                vesta::scout() << "[vpp] No se puede crear: " << out_file << "\n";
                return;
            }
            ofs << result;
            vesta::scout() << "[vpp] OK -> " << out_file
                           << "  (" << result.size() << " bytes)\n";
        }
#endif
    }

    /**
     * @brief Desensambla un archivo .velb usando las tablas internas de VestaVM.
     *
     * Delega en disasm::disasm_velb() del modulo src/disasm/disasm.cpp.
     *
     * @param args "<archivo.velb>"
     */
    static void command_disasm(const std::string &args) {
        auto words = split_words(args);
        if (words.empty()) { std::cout << "Uso: disasm <archivo.velb>\n"; return; }
        try {
            disasm::disasm_velb(words[0], std::cout);
        } catch (const std::exception &e) {
            std::cout << "[disasm] error: " << e.what() << "\n";
        }
    }

    /**
     * @brief Ejecuta un .velb en una VM temporal en segundo plano.
     * @param args "<archivo.velb> [--schedulers N]"
     */
    static void command_exec(const std::string &args) {
        auto words = split_words(args);
        if (words.empty()) { std::cout << "Uso: exec <archivo.velb> [--schedulers N]\n"; return; }
        std::string file        = words[0];
        size_t      schedulers_ = 1;
        for (size_t i = 1; i + 1 < words.size(); ++i) {
            if (words[i] == "--schedulers") {
                try { schedulers_ = std::stoul(words[i + 1]); } catch (...) {}
            }
        }
        // registrar en mgr global para monitorear con mgrinfo/vmstat/schedtop
        runtime::ManageVM *exec_mgr = mgr.add_manager(file, nullptr);
        size_t exec_id = exec_mgr->id;
        vesta::scout() << "[exec] lanzando '" << file << "' (id=" << exec_id << ")...\n";

        std::thread([file, schedulers_, exec_mgr, exec_id]() {
            try {
                runtime::VM *vm = exec_mgr->loader.create_vm_instance(schedulers_);
                if (!vm) { vesta::scout() << "[exec] error: no se pudo crear VM\n";
                           mgr.remove_manager(exec_id); return; }
                runtime::ProcessVM *proc = exec_mgr->loader.load_executable(*vm, file);
                if (!proc) { vesta::scout() << "[exec] error: no se pudo cargar el archivo\n";
                             mgr.remove_manager(exec_id); return; }
                vm->make_ready(proc->pid);
                vm->start();
                while (vm->has_alive_processes())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                vm->stop();
                vesta::scout() << "[exec] '" << file << "' finalizado\n";
            } catch (const std::exception &e) {
                vesta::scout() << "[exec] error: " << e.what() << "\n";
            }
            mgr.remove_manager(exec_id); // limpiar del registro al terminar
        }).detach();
    }

    /**
     * @brief Detiene y elimina el manager con el ID indicado.
     * @param args "<id>"
     */
    static void command_kill(const std::string &args) {
        auto words = split_words(args);
        if (words.empty()) { std::cout << "Uso: kill <id>\n"; return; }
        try {
            size_t id = std::stoul(words[0]);
            if (mgr.remove_manager(id))
                std::cout << "[kill] manager " << id << " eliminado\n";
            else
                std::cout << "[kill] ID " << id << " no encontrado\n";
        } catch (...) {
            std::cout << "Uso: kill <id>  (ID debe ser un numero entero)\n";
        }
    }

    /**
     * @brief Muestra el directorio de trabajo actual.
     * @param args Sin uso.
     */
    static void command_pwd(const std::string &) {
        std::lock_guard<std::mutex> lk(vesta::cout_mutex);
        std::cout << std::filesystem::current_path().string() << "\n";
    }

    /**
     * @brief Limpia la pantalla de la consola.
     * @param args Sin uso.
     */
    static void command_clear(const std::string &) {
        std::lock_guard<std::mutex> lk(vesta::cout_mutex);
        clear_screen();
    }

    /**
     * @brief Cambia el directorio de trabajo actual.
     *
     * Soporta: sin argumentos (ir a HOME), "-" (volver al anterior) y rutas normales.
     *
     * @param args Ruta destino, "-" o cadena vacia para HOME.
     */
    static void command_cd(const std::string &args) {
        // directorio anterior memorizado entre llamadas
        static std::filesystem::path previous_dir = std::filesystem::current_path();

        try {
            std::filesystem::path target;
            if (args.empty()) {
                // sin argumentos: ir al HOME
                const char *home = std::getenv(
#if defined(_WIN32)
                    "USERPROFILE"
#else
                    "HOME"
#endif
                );
                if (!home) {
                    std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                    std::cerr << "cd: HOME no definido\n"; return;
                }
                target = std::filesystem::path(home);
            } else if (args == "-") {
                target = previous_dir; // volver al directorio anterior
            } else {
                target = expand_path(args);
                if (target.is_relative()) target = std::filesystem::current_path() / target;
            }

            target = std::filesystem::weakly_canonical(target);

            if (!std::filesystem::exists(target) || !std::filesystem::is_directory(target)) {
                std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                std::cerr << "cd: no existe o no es directorio: " << target.string() << "\n";
                return;
            }

            // cambiar directorio y actualizar el anterior
            std::filesystem::path old = std::filesystem::current_path();
            std::filesystem::current_path(target);
            previous_dir = old;

            std::lock_guard<std::mutex> lk(vesta::cout_mutex);
            std::cout << "Directorio actual: " << std::filesystem::current_path().string() << "\n";
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lk(vesta::cout_mutex);
            std::cerr << "cd: error: " << e.what() << "\n";
        }
    }

    /**
     * @brief Ejecuta un comando de shell en un hilo separado (no bloquea el REPL).
     * @param args Comando de shell a ejecutar.
     */
    static void command_cmd(const std::string &args) {
        if (args.empty()) { std::cout << "Uso: cmd <comando de shell>\n"; return; }
        std::thread([args]() {
            try {
                std::string out = execute_shell_command(args);
                std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                std::cout << out << std::endl;
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                std::cerr << "Error ejecutando shell: " << e.what() << std::endl;
            }
        }).detach();
    }

    /**
     * @brief Muestra informacion detallada de un manager de VM.
     * @param args "<id>"
     */
    static void command_mgrinfo(const std::string &args) {
        auto words = split_words(args);
        if (words.empty()) { std::cout << "Uso: mgrinfo <id>\n"; return; }
        size_t id;
        try { id = std::stoul(words[0]); } catch (...) {
            std::cout << "Uso: mgrinfo <id>  (ID debe ser un numero)\n"; return;
        }
        std::string info;
        bool found = mgr.modify_manager(id, [&](std::unique_ptr<runtime::ManageVM> &m) {
            info = m->to_string_vm_manager_info();
        });
        if (!found) { std::cout << "[mgrinfo] manager " << id << " no encontrado\n"; return; }
        std::cout << info << "\n";
    }

    /**
     * @brief Muestra estadisticas de una instancia VM y sus schedulers.
     * @param args "<mgr_id> <vm_id>"
     */
    static void command_vmstat(const std::string &args) {
        auto words = split_words(args);
        if (words.size() < 2) { std::cout << "Uso: vmstat <mgr_id> <vm_id>\n"; return; }
        size_t mgr_id, vm_id;
        try { mgr_id = std::stoul(words[0]); vm_id = std::stoul(words[1]); } catch (...) {
            std::cout << "Uso: vmstat <mgr_id> <vm_id>\n"; return;
        }
        std::string result;
        bool found = mgr.modify_manager(mgr_id, [&](std::unique_ptr<runtime::ManageVM> &m) {
            runtime::VM *vm = m->get_vm(vm_id);
            if (!vm) { result = "[vmstat] VM " + std::to_string(vm_id) + " no encontrada\n"; return; }
            std::ostringstream ss;
            ss << vm->to_string() << "\n";
            ss << "Schedulers: " << vm->schedulers.size() << "\n";
            for (auto &sched : vm->schedulers) {
                ss << "  [sched " << sched->id_scheduler << "]"
                   << "  alive=" << sched->alive_count.load()
                   << "  instr_ctr=" << sched->profiler_instr_counter
                   << "  time_exec=" << sched->time_exec << "ns"
                   << (sched->is_waiting.load() ? "  [IDLE]" : "  [RUN]") << "\n";
            }
            result = ss.str();
        });
        if (!found) { std::cout << "[vmstat] manager " << mgr_id << " no encontrado\n"; return; }
        std::cout << result;
    }

    /**
     * @brief Vista htop interactiva de schedulers y procesos de una VM.
     *
     * Navegacion de dos niveles:
     *   SCHEDULERS -> PROCESSES -> PROC_INFO
     *
     * Teclas en vista SCHEDULERS:
     *   j / flecha abajo : bajar cursor
     *   k / flecha arriba: subir cursor
     *   Enter            : entrar en lista de procesos del scheduler seleccionado
     *   q / ESC          : salir
     *
     * Teclas en vista PROCESSES:
     *   j/k / flechas    : navegar procesos
     *   Enter / i        : ver info completa del proceso
     *   x                : matar proceso seleccionado
     *   q / ESC          : volver a SCHEDULERS
     *
     * Teclas en vista PROC_INFO:
     *   cualquier tecla  : volver a PROCESSES
     *
     * @param args "<mgr_id> <vm_id>"
     */
    static void command_schedtop(const std::string &args) {
        auto words = split_words(args);
        if (words.size() < 2) { std::cout << "Uso: schedtop <mgr_id> <vm_id>\n"; return; }
        size_t mgr_id, vm_id;
        try { mgr_id = std::stoul(words[0]); vm_id = std::stoul(words[1]); } catch (...) {
            std::cout << "Uso: schedtop <mgr_id> <vm_id>\n"; return;
        }

        // obtener puntero a la VM sin mantener mutex durante el bucle de display
        runtime::VM *vm_ptr = nullptr;
        bool found = mgr.modify_manager(mgr_id, [&](std::unique_ptr<runtime::ManageVM> &m) {
            vm_ptr = m->get_vm(vm_id);
        });
        if (!found) { std::cout << "[schedtop] manager " << mgr_id << " no encontrado\n"; return; }
        if (!vm_ptr) { std::cout << "[schedtop] VM " << vm_id << " no encontrada\n"; return; }

        size_t n = vm_ptr->schedulers.size();
        if (n == 0) { std::cout << "[schedtop] VM sin schedulers\n"; return; }

        // activar hooks de medicion de tiempo (noop, pero necesario para acumular time_exec)
        for (auto &s : vm_ptr->schedulers)
            s->add_debug_hook(noop_debug_hook);

        // --- setup terminal raw (lectura de teclas sin bloqueo) ---
#if !defined(_WIN32)
        struct termios term_old;
        bool term_ok = (tcgetattr(STDIN_FILENO, &term_old) == 0);
        if (term_ok) {
            struct termios raw = term_old;
            raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
            raw.c_cc[VMIN]  = 0; // lectura no bloqueante
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        // leer una tecla sin bloquear; devuelve -1 si no hay entrada
        // flechas se codifican como -72 (arriba) y -80 (abajo) para unificar con Windows
        auto read_key = [&]() -> int {
            unsigned char c;
            if (::read(STDIN_FILENO, &c, 1) != 1) return -1;
            if (c == 27) { // posible secuencia ESC+[+letra para flechas
                unsigned char b1, b2;
                if (::read(STDIN_FILENO, &b1, 1) == 1 && b1 == '[') {
                    if (::read(STDIN_FILENO, &b2, 1) == 1) {
                        if (b2 == 'A') return -72; // flecha arriba
                        if (b2 == 'B') return -80; // flecha abajo
                    }
                }
                return 27; // ESC solitario
            }
            return (int)c;
        };
#else
        // en Windows _kbhit/_getch son no bloqueantes y no requieren setup
        auto read_key = [&]() -> int {
            if (!_kbhit()) return -1;
            int c = _getch();
            if (c == 0 || c == 224) { // prefijo de tecla especial (flechas, F-keys)
                if (!_kbhit()) return -1;
                int c2 = _getch();
                if (c2 == 72) return -72; // flecha arriba
                if (c2 == 80) return -80; // flecha abajo
                return -c2; // otras teclas especiales descartadas con valor negativo
            }
            return c;
        };
#endif

        // --- estado de navegacion ---
        enum class View { SCHEDULERS, PROCESSES, PROC_INFO };
        View   view         = View::SCHEDULERS;
        size_t sched_cursor = 0;
        size_t proc_cursor  = 0;

        struct ProcSnap { uint32_t sched_id; uint64_t local_pid; runtime::vm_state state; };
        std::vector<ProcSnap> proc_list; // procesos visibles en vista PROCESSES
        std::string           proc_info; // texto completo para vista PROC_INFO

        // estadisticas para calculo de deltas
        std::vector<uint64_t> prev_instr(n, 0), prev_time(n, 0);
        bool first_frame = true;
        int  frame_tick  = 0; // incrementa cada 50ms; redibuja cada 10 ticks (500ms)

        bool top_running = true;
        while (top_running) {
            // --- procesar entrada de teclado (no bloqueante) ---
            bool key_pressed = false;
            int  key = read_key();
            if (key != -1) {
                key_pressed = true;
                switch (view) {
                    case View::SCHEDULERS:
                        if ((key == 'j' || key == -80) && sched_cursor + 1 < n)
                            ++sched_cursor;
                        else if ((key == 'k' || key == -72) && sched_cursor > 0)
                            --sched_cursor;
                        else if (key == '\r' || key == '\n') {
                            // entrar en la lista de procesos del scheduler seleccionado
                            view = View::PROCESSES;
                            proc_cursor = 0;
                            proc_list.clear();
                            if (sched_cursor < n) {
                                auto &s = *vm_ptr->schedulers[sched_cursor];
                                for (auto &p : s.processes)
                                    if (p) proc_list.push_back(
                                        {p->pid.scheduler_id, p->pid.local_pid, p->state});
                            }
                        }
                        else if (key == 'q' || key == 27) top_running = false;
                        break;

                    case View::PROCESSES:
                        if ((key == 'j' || key == -80) && proc_cursor + 1 < proc_list.size())
                            ++proc_cursor;
                        else if ((key == 'k' || key == -72) && proc_cursor > 0)
                            --proc_cursor;
                        else if ((key == '\r' || key == '\n' || key == 'i') && !proc_list.empty()) {
                            // mostrar informacion detallada del proceso seleccionado
                            auto &ps = proc_list[proc_cursor];
                            GlobalPID gpid{ps.sched_id, ps.local_pid};
                            proc_info = "[proceso no encontrado]\n";
                            if (sched_cursor < n) {
                                auto &sched = *vm_ptr->schedulers[sched_cursor];
                                auto  it    = sched.pid_index.find(gpid);
                                if (it != sched.pid_index.end())
                                    proc_info = it->second->to_string();
                            }
                            view = View::PROC_INFO;
                        }
                        else if ((key == 'x' || key == 'X') && !proc_list.empty()) {
                            // matar el proceso seleccionado
                            auto &ps = proc_list[proc_cursor];
                            GlobalPID gpid{ps.sched_id, ps.local_pid};
                            if (sched_cursor < n)
                                vm_ptr->schedulers[sched_cursor]->kill(gpid);
                            // reconstruir lista de procesos tras el kill
                            proc_list.clear();
                            if (sched_cursor < n) {
                                auto &s = *vm_ptr->schedulers[sched_cursor];
                                for (auto &p : s.processes)
                                    if (p) proc_list.push_back(
                                        {p->pid.scheduler_id, p->pid.local_pid, p->state});
                            }
                            if (!proc_list.empty() && proc_cursor >= proc_list.size())
                                proc_cursor = proc_list.size() - 1;
                        }
                        else if (key == 'q' || key == 27) {
                            view = View::SCHEDULERS;
                            proc_cursor = 0;
                        }
                        break;

                    case View::PROC_INFO:
                        view = View::PROCESSES; // cualquier tecla vuelve atras
                        break;
                }
            }

            // --- redibujado cada 500ms o inmediatamente tras pulsacion de tecla ---
            ++frame_tick;
            if (frame_tick >= 10 || key_pressed) {
                if (frame_tick >= 10) frame_tick = 0;

                // snapshot de estadisticas de los schedulers
                std::vector<uint64_t>              curr_instr(n), curr_time(n);
                std::vector<int>                   alive(n);
                std::vector<bool>                  waiting(n);
                std::vector<std::vector<ProcSnap>> procs_snap(n);

                for (size_t i = 0; i < n; ++i) {
                    auto &s   = *vm_ptr->schedulers[i];
                    curr_instr[i] = s.profiler_instr_counter;
                    curr_time[i]  = s.time_exec;
                    alive[i]      = s.alive_count.load();
                    waiting[i]    = s.is_waiting.load();
                    for (auto &p : s.processes)
                        if (p) procs_snap[i].push_back(
                            {p->pid.scheduler_id, p->pid.local_pid, p->state});
                }

                // calcular IPS y CPU% a partir de deltas (no disponible en primer frame)
                std::vector<double> ips(n, 0.0), cpu(n, 0.0);
                if (!first_frame) {
                    for (size_t i = 0; i < n; ++i) {
                        uint64_t di = curr_instr[i] - prev_instr[i]; // ticks de 256 instruc
                        uint64_t dt = curr_time[i]  - prev_time[i];  // ns en exec en ~500ms
                        ips[i] = (double)di * 256.0;
                        cpu[i] = std::min(100.0, (double)dt / 5e6);  // dt_ns / 500ms_ns * 100
                    }
                }
                prev_instr  = curr_instr;
                prev_time   = curr_time;
                first_frame = false;

                // refrescar lista de procesos en vista PROCESSES (para que refleje cambios)
                if (view == View::PROCESSES && !key_pressed && sched_cursor < n) {
                    proc_list = procs_snap[sched_cursor];
                    if (!proc_list.empty() && proc_cursor >= proc_list.size())
                        proc_cursor = proc_list.size() - 1;
                }

                // construir pantalla completa en buffer antes de escribir (evita parpadeo)
                std::ostringstream screen;
                screen << "\x1B[2J\x1B[H";

                if (view == View::SCHEDULERS) {
                    screen << ansi::c(ansi::BOLD) << ansi::c(ansi::BR_CYAN)
                           << "=== schedtop | mgr=" << mgr_id << "  vm=" << vm_id
                           << " ===" << ansi::c(ansi::RESET)
                           << " " << ansi::c(ansi::DIM)
                           << "[j/k=nav  Enter=procesos  q=salir]"
                           << ansi::c(ansi::RESET) << "\n\n";

                    screen << ansi::c(ansi::BOLD) << std::left
                           << std::setw(6)  << " "
                           << std::setw(8)  << "SCHED"
                           << std::setw(8)  << "ESTADO"
                           << std::setw(8)  << "VIVOS"
                           << std::setw(16) << "IPS/500ms"
                           << std::setw(10) << "CPU%"
                           << "PROCESOS" << ansi::c(ansi::RESET) << "\n";
                    screen << std::string(75, '-') << "\n";

                    for (size_t i = 0; i < n; ++i) {
                        bool sel = (i == sched_cursor);
                        if (sel) screen << ansi::c(ansi::BR_YELLOW) << "> ";
                        else     screen << "  ";

                        screen << std::left << std::setw(8) << i;
                        if (waiting[i])
                            screen << ansi::c(ansi::DIM)      << std::setw(8) << "IDLE" << ansi::c(ansi::RESET);
                        else
                            screen << ansi::c(ansi::BR_GREEN)  << std::setw(8) << "RUN"  << ansi::c(ansi::RESET);

                        screen << std::setw(8) << alive[i];

                        char ips_buf[32], cpu_buf[16];
                        snprintf(ips_buf, sizeof(ips_buf), "%.0f", ips[i]);
                        snprintf(cpu_buf, sizeof(cpu_buf), "%.1f%%", cpu[i]);
                        screen << std::setw(16) << ips_buf << std::setw(10) << cpu_buf;

                        for (auto &ps : procs_snap[i])
                            screen << " " << ansi::c(ansi::CYAN)
                                   << "(" << ps.sched_id << "," << ps.local_pid << ")"
                                   << ansi::c(ansi::RESET)
                                   << "[" << runtime::vm_state_to_str(ps.state) << "]";
                        if (procs_snap[i].empty())
                            screen << ansi::c(ansi::DIM) << " (ninguno)" << ansi::c(ansi::RESET);
                        if (sel) screen << ansi::c(ansi::RESET);
                        screen << "\n";
                    }

                } else if (view == View::PROCESSES) {
                    screen << ansi::c(ansi::BOLD) << ansi::c(ansi::BR_CYAN)
                           << "=== schedtop | sched=" << sched_cursor
                           << " ===" << ansi::c(ansi::RESET)
                           << " " << ansi::c(ansi::DIM)
                           << "[j/k=nav  Enter/i=info  x=matar  q=volver]"
                           << ansi::c(ansi::RESET) << "\n\n";

                    if (proc_list.empty()) {
                        screen << ansi::c(ansi::DIM) << "(sin procesos)" << ansi::c(ansi::RESET) << "\n";
                    } else {
                        screen << ansi::c(ansi::BOLD) << std::left
                               << std::setw(6) << " "
                               << std::setw(14) << "PID (sched,loc)"
                               << "ESTADO" << ansi::c(ansi::RESET) << "\n";
                        screen << std::string(40, '-') << "\n";
                        for (size_t i = 0; i < proc_list.size(); ++i) {
                            auto &ps = proc_list[i];
                            bool  sel = (i == proc_cursor);
                            if (sel) screen << ansi::c(ansi::BR_YELLOW) << "> ";
                            else     screen << "  ";

                            char pid_buf[32];
                            snprintf(pid_buf, sizeof(pid_buf), "(%u,%llu)",
                                     ps.sched_id, (unsigned long long)ps.local_pid);
                            screen << std::left << std::setw(14) << pid_buf
                                   << runtime::vm_state_to_str(ps.state);
                            if (sel) screen << ansi::c(ansi::RESET);
                            screen << "\n";
                        }
                    }

                } else { // PROC_INFO
                    screen << ansi::c(ansi::BOLD) << ansi::c(ansi::BR_CYAN)
                           << "=== schedtop | info de proceso ===" << ansi::c(ansi::RESET)
                           << " " << ansi::c(ansi::DIM) << "[cualquier tecla = volver]"
                           << ansi::c(ansi::RESET) << "\n\n";
                    screen << proc_info << "\n";
                }

                screen << "\n" << ansi::c(ansi::DIM)
                       << "Actualizacion: 500ms" << ansi::c(ansi::RESET) << "\n";

                { std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                  std::cout << screen.str() << std::flush; }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // restaurar terminal en POSIX
#if !defined(_WIN32)
        if (term_ok) tcsetattr(STDIN_FILENO, TCSANOW, &term_old);
#endif

        // desactivar hooks al salir para no penalizar el rendimiento
        for (auto &s : vm_ptr->schedulers) {
            s->has_hooks = false;   // desactiva la invocacion de los hooks
            s->debug_hooks.clear(); // libera el vector (miembro publico)
        }

        std::cout << "\x1B[2J\x1B[H" << std::flush;
        std::cout << ansi::c(ansi::BR_CYAN) << "[schedtop]" << ansi::c(ansi::RESET) << " saliendo\n";
    }

    /**
     * @brief Muestra el estado completo de un proceso virtual especifico.
     * @param args "<mgr_id> <vm_id> <sched_id> <pid>"
     */
    static void command_procinfo(const std::string &args) {
        auto words = split_words(args);
        if (words.size() < 4) {
            std::cout << "Uso: procinfo <mgr_id> <vm_id> <sched_id> <pid>\n"; return;
        }
        size_t   mgr_id, vm_id, sched_id;
        uint64_t local_pid;
        try {
            mgr_id    = std::stoul(words[0]);
            vm_id     = std::stoul(words[1]);
            sched_id  = std::stoul(words[2]);
            local_pid = std::stoull(words[3]);
        } catch (...) {
            std::cout << "Uso: procinfo <mgr_id> <vm_id> <sched_id> <pid>  (todos numericos)\n"; return;
        }
        std::string result;
        bool found = mgr.modify_manager(mgr_id, [&](std::unique_ptr<runtime::ManageVM> &m) {
            runtime::VM *vm = m->get_vm(vm_id);
            if (!vm) { result = "[procinfo] VM " + std::to_string(vm_id) + " no encontrada\n"; return; }
            if (sched_id >= vm->schedulers.size()) {
                result = "[procinfo] scheduler " + std::to_string(sched_id) + " no existe\n"; return;
            }
            auto &sched = *vm->schedulers[sched_id];
            GlobalPID gpid{(uint32_t)sched_id, local_pid};
            auto it = sched.pid_index.find(gpid);
            if (it == sched.pid_index.end()) {
                result = "[procinfo] proceso (" + std::to_string(sched_id) + ","
                       + std::to_string(local_pid) + ") no encontrado\n";
                return;
            }
            result = it->second->to_string();
        });
        if (!found) { std::cout << "[procinfo] manager " << mgr_id << " no encontrado\n"; return; }
        std::cout << result;
    }

    // ---- tabla de comandos ----

    /**
     * @brief Descriptor de un comando del REPL.
     *
     * Cada entrada asocia un nombre de comando a su handler y su documentacion.
     * Anadir un comando nuevo = anadir una entrada a @c cmd_table.
     */
    struct CmdEntry {
        const char *name;                            ///< Nombre exacto del comando (primer token).
        const char *usage;                           ///< Linea de uso para help.
        const char *brief;                           ///< Descripcion breve para help.
        void (*fn)(const std::string &args);         ///< Handler; args = todo lo que sigue al nombre.
    };

    /**
     * @brief Tabla de todos los comandos despachables por nombre.
     *
     * El bucle principal de @c VestaViewManager::run() busca en esta tabla el
     * primer token del comando introducido y llama a @c fn con el resto de la linea.
     */
    static const CmdEntry cmd_table[] = {
        // nombre      uso                                         descripcion breve                                handler
        { "ls",     "ls [ruta]",                                 "Listar contenido de directorio",                command_ls     },
        { "vms",    "vms",                                       "Listar managers activos con ID y estado",       command_vms    },
        { "kill",   "kill <id>",                                 "Detener y eliminar manager por ID",             command_kill   },
        { "build",  "build <archivo.vel> [-o <salida>]",                       "Compilar .vel a .velb",                                     command_build  },
        { "vpp",    "vpp <archivo.vel> [-o <salida>] [-D N] [-I r] [-M r]", "Preprocesar .vel y mostrar/guardar resultado expandido",     command_vpp    },
        { "disasm", "disasm <archivo.velb>",                                 "Desensamblar bytecode VestaVM",                             command_disasm },
        { "exec",   "exec <archivo.velb> [--schedulers N]",      "Ejecutar .velb en background",                  command_exec   },
        { "run",    "run <nombre> <ruta.velb> [--schedulers N] [--stats]", "Ejecutar .velb con manager nombrado (--stats para estadisticas)", command_run },
        { "pwd",    "pwd",                                       "Mostrar directorio de trabajo actual",          command_pwd    },
        { "cls",    "cls",                                       "Limpiar pantalla",                              command_clear  },
        { "clear",  "clear",                                     "Limpiar pantalla",                              command_clear  },
        { "cd",     "cd [ruta|-]",                               "Cambiar directorio (- = anterior, vacio = HOME)", command_cd  },
        { "cmd",      "cmd <comando>",                                 "Ejecutar comando de shell (asincrono)",          command_cmd      },
        { "mgrinfo",  "mgrinfo <id>",                                "Info detallada de un manager de VM",             command_mgrinfo  },
        { "vmstat",   "vmstat <mgr_id> <vm_id>",                     "Estadisticas de una instancia VM",               command_vmstat   },
        { "schedtop", "schedtop <mgr_id> <vm_id>",                   "Vista htop de schedulers (ENTER para salir)",    command_schedtop },
        { "procinfo", "procinfo <mgr_id> <vm_id> <sched_id> <pid>",  "Info de un proceso virtual",                     command_procinfo },
    };

    // numero de entradas en la tabla
    static constexpr size_t CMD_TABLE_SIZE = sizeof(cmd_table) / sizeof(cmd_table[0]);

    // ---- senal global Ctrl+C ----

    static std::atomic<bool> *global_interrupt_ptr = nullptr;

    static void global_sigint_handler(int) {
        if (global_interrupt_ptr) *global_interrupt_ptr = true;
    }

    // ---- helpers de historial ----

    static void load_history_impl(Impl &I) {
        std::lock_guard lk(I.hist_m);
        std::ifstream   f(I.cfg.history_file);
        if (!f) return;
        std::string line;
        while (std::getline(f, line))
            if (!line.empty()) I.history.push_back(line);
    }

    static void save_history_impl(Impl &I) {
        std::lock_guard lk(I.hist_m);
        std::ofstream   f(I.cfg.history_file, std::ios::trunc);
        if (!f) return;
        size_t start = I.history.size() > I.cfg.history_max
            ? I.history.size() - I.cfg.history_max : 0;
        for (size_t i = start; i < I.history.size(); ++i) f << I.history[i] << "\n";
    }

    static void add_history_entry_impl(Impl &I, const std::string &line) {
        if (line.empty()) return;
        std::lock_guard lk(I.hist_m);
        if (!I.history.empty() && I.history.back() == line) return; // deduplicar consecutivos
        I.history.push_back(line);
        if (I.history.size() > I.cfg.history_max) I.history.erase(I.history.begin());
    }

    // ---- helpers de cola de comandos ----

    static void cmd_queue_push_impl(Impl &I, std::string cmd) {
        { std::lock_guard lk(I.q_m); if (I.q_closed) return; I.cmd_queue.push_back(std::move(cmd)); }
        I.q_cv.notify_one();
    }

    static bool cmd_queue_pop_impl(Impl &I, std::string &out) {
        std::unique_lock lk(I.q_m);
        I.q_cv.wait(lk, [&] { return !I.cmd_queue.empty() || I.q_closed; });
        if (I.cmd_queue.empty()) return false;
        out = std::move(I.cmd_queue.front());
        I.cmd_queue.pop_front();
        return true;
    }

    static void cmd_queue_close_impl(Impl &I) {
        { std::lock_guard lk(I.q_m); I.q_closed = true; }
        I.q_cv.notify_all();
    }

    // ---- worker loop ----

    static void worker_loop_impl(Impl &I) {
        while (I.running) {
            std::string cmd;
            if (!cmd_queue_pop_impl(I, cmd)) break;
            std::function<void(const std::string &)> cb;
            { std::lock_guard lk(I.cb_m); cb = I.vm_execute_cb; }
            if (cb) {
                cb(cmd);
            } else {
                // fallback mientras no hay callback registrado
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                std::cout << "[VM] ejecutado: " << cmd << std::endl;
            }
        }
    }

    // ---- comandos especiales del REPL ----

    /**
     * @brief Imprime la ayuda del REPL generada a partir de cmd_table.
     * @param I Estado del REPL (para leer cfg.multiline_end).
     */
    static void print_help_impl(const Impl &I) {
        // cabecera con color
        auto H = [](const char *s) {
            std::cout << ansi::c(ansi::BOLD) << ansi::c(ansi::BR_CYAN) << s << ansi::c(ansi::RESET) << "\n";
        };
        auto CMD = [](const char *usage, const char *brief) {
            std::cout << "  "
                      << ansi::c(ansi::BR_YELLOW) << std::left << std::setw(46) << usage << ansi::c(ansi::RESET)
                      << " " << brief << "\n";
        };

        H("Comandos especiales:");
        CMD("exit",          "salir");
        CMD("help",          "mostrar esta ayuda");
        CMD("history",       "listar historial con indices");
        CMD("!N",            "ejecutar entrada N del historial  (ej: !3)");
        CMD(":prev / :next", "historial anterior / siguiente");
        CMD("complete PREF", "listar completions para PREF");
        CMD("interprete",    "abrir el interprete interactivo de Vesta");
        std::cout << "\n";
        H("Comandos:");
        for (size_t i = 0; i < CMD_TABLE_SIZE; ++i) {
            if (std::string(cmd_table[i].name) == "clear") continue; // alias de cls
            CMD(cmd_table[i].usage, cmd_table[i].brief);
        }
        std::cout << "\n"
                  << ansi::c(ansi::DIM) << "Multilinea: termina con '" << I.cfg.multiline_end << "' para enviar bloque.\n"
                  << "Nota: 'run' y 'exec' lanzan la VM en segundo plano; 'cmd' ejecuta shell.\n"
                  << ansi::c(ansi::RESET);
    }

    static void print_history_impl(Impl &I) {
        std::lock_guard lk(I.hist_m);
        for (size_t i = 0; i < I.history.size(); ++i)
            std::cout << i << ": " << I.history[i] << "\n";
    }

    static void handle_history_exec_impl(Impl &I, const std::string &cmd) {
        std::string num = cmd.substr(1);
        try {
            size_t idx = std::stoul(num);
            std::lock_guard lk(I.hist_m);
            if (idx < I.history.size()) {
                std::string hcmd = I.history[idx];
                std::cout << "[exec] " << hcmd << "\n";
                add_history_entry_impl(I, hcmd);
                cmd_queue_push_impl(I, hcmd);
            } else {
                std::cout << "Indice fuera de rango\n";
            }
        } catch (...) {
            std::cout << "Uso: !N  (N = indice de history)\n";
        }
    }

    static void show_prev_history_impl(Impl &I) {
        std::lock_guard lk(I.hist_m);
        if (I.history.empty()) { std::cout << "(historial vacio)\n"; return; }
        if (I.history_cursor < 0) I.history_cursor = (int)I.history.size() - 1;
        std::cout << I.history[I.history_cursor] << "\n";
        I.history_cursor = (I.history_cursor - 1 + (int)I.history.size()) % (int)I.history.size();
    }

    static void show_next_history_impl(Impl &I) {
        std::lock_guard<std::mutex> lk(I.hist_m);
        if (I.history.empty()) { std::cout << "(historial vacio)\n"; return; }
        I.history_cursor = (I.history_cursor + 1) % (int)I.history.size();
        std::cout << I.history[I.history_cursor] << "\n";
    }

    /**
     * @brief Sugiere completions para un prefijo dado.
     * @param I   Estado del REPL (no usado actualmente; reservado para futura consulta de simbolos).
     * @param pref Prefijo para el que se buscan candidatos.
     */
    static void handle_complete_impl(const Impl &I, const std::string &pref) {
        // construir lista de candidatos desde cmd_table + comandos especiales
        std::vector<std::string> candidates = {"exit","help","history","interprete","complete"};
        for (size_t i = 0; i < CMD_TABLE_SIZE; ++i)
            candidates.push_back(cmd_table[i].name);

        bool any = false;
        for (auto &c : candidates)
            if (c.rfind(pref, 0) == 0) { std::cout << c << " "; any = true; }
        if (!any) std::cout << "(no hay candidatos)";
        std::cout << "\n";
        (void)I;
    }

    // ---- helpers del bucle de lectura ----

    // devuelve true si la linea parece iniciar un bloque multilinea
    static bool likely_start_block(const std::string &line) {
        std::string s = line;
        auto p = s.find_first_not_of(" \t");
        if (p != std::string::npos) s = s.substr(p);
        if (s.rfind("func ", 0) == 0) return true;
        if (s.find('{') != std::string::npos) return true;
        return false;
    }

    // balance de llaves en una linea: +1 por '{', -1 por '}'
    static int brace_delta(const std::string &line) {
        int d = 0;
        for (char c : line) { if (c == '{') ++d; else if (c == '}') --d; }
        return d;
    }

    // ---- VestaViewManager ----

    VestaViewManager::VestaViewManager(Config cfg) {
        impl_ = new Impl(std::move(cfg));
    }

    VestaViewManager::~VestaViewManager() {
        stop();
        delete impl_;
        impl_ = nullptr;
    }

    void VestaViewManager::set_execute_callback(std::function<void(const std::string &)> cb) {
        std::lock_guard lk(impl_->cb_m);
        impl_->vm_execute_cb = std::move(cb);
    }

    /**
     * @brief Bucle principal del REPL: lee lineas, despacha comandos via cmd_table.
     *
     * Flujo por iteracion:
     *   1. Leer linea (con soporte de bloque multilinea).
     *   2. Trim y verificar comandos especiales (exit, help, history, !N, :prev, :next, interprete, complete).
     *   3. Separar primer token (nombre del comando) del resto (args).
     *   4. Buscar en cmd_table y llamar al handler si hay coincidencia.
     *   5. Si no hay coincidencia, encolar para el callback de VM.
     */
    void VestaViewManager::run() {
        if (impl_->running) return;
        impl_->running = true;

        global_interrupt_ptr = &impl_->interrupted_flag;
        std::signal(SIGINT, global_sigint_handler);

        ansi::init(); // habilitar colores si el terminal lo soporta

        load_history_impl(*impl_);
        impl_->worker_thread = std::thread(worker_loop_impl, std::ref(*impl_));

        while (impl_->running) {
            if (impl_->interrupted_flag.exchange(false)) {
                std::cout << ansi::c(ansi::YELLOW) << "^C" << ansi::c(ansi::RESET) << "\n";
            }

            // prompt con color: "vesta> " en cian negrita
            std::cout << ansi::c(ansi::BOLD) << ansi::c(ansi::CYAN)
                      << impl_->cfg.prompt
                      << ansi::c(ansi::RESET) << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) { std::cout << "\n"; break; } // EOF

            if (impl_->interrupted_flag.exchange(false)) { std::cout << "^C\n"; continue; }

            // --- acumulacion multilinea ---
            std::string full = line;
            const std::string &term = impl_->cfg.multiline_end;

            if (full.size() >= term.size() &&
                full.substr(full.size() - term.size()) == term) {
                // la linea ya tiene el terminador: quitar y usar tal cual
                full = full.substr(0, full.size() - term.size());
            } else if (likely_start_block(full)) {
                int balance = brace_delta(full);
                while (balance > 0) {
                    std::cout << "... " << std::flush;
                    std::string more;
                    if (!std::getline(std::cin, more)) { full.clear(); break; }
                    if (impl_->interrupted_flag.exchange(false)) {
                        std::cout << "^C\n"; full.clear(); break;
                    }
                    if (more.size() >= term.size() &&
                        more.substr(more.size() - term.size()) == term) {
                        std::string part = more.substr(0, more.size() - term.size());
                        full += "\n" + part;
                        balance += brace_delta(part);
                        break;
                    } else {
                        full += "\n" + more;
                        balance += brace_delta(more);
                    }
                }
            }

            // --- trim ---
            auto s0 = full.find_first_not_of(" \t\r\n");
            if (s0 == std::string::npos) continue;
            auto        s1  = full.find_last_not_of(" \t\r\n");
            std::string cmd = full.substr(s0, s1 - s0 + 1);

            // --- comandos especiales (no despachables por tabla) ---
            if (cmd == "exit") { add_history_entry_impl(*impl_, cmd); break; }

            if (cmd == "help") {
                print_help_impl(*impl_); continue;
            }
            if (cmd == "history") {
                print_history_impl(*impl_); continue;
            }
            if (!cmd.empty() && cmd[0] == '!') {
                handle_history_exec_impl(*impl_, cmd); continue;
            }
            if (cmd == ":prev") { show_prev_history_impl(*impl_); continue; }
            if (cmd == ":next") { show_next_history_impl(*impl_); continue; }
            if (cmd == "interprete") {
                VestaInterprete interp{};
                interp.run_interprete();
                continue;
            }

            // --- despacho via cmd_table ---
            // separar primer token (nombre del comando) del resto (args)
            auto sp = cmd.find(' ');
            std::string cmd_name = (sp == std::string::npos) ? cmd : cmd.substr(0, sp);
            std::string cmd_args = (sp == std::string::npos) ? std::string{} : cmd.substr(sp + 1);

            // complete es especial porque necesita acceso a Impl
            if (cmd_name == "complete") {
                if (cmd_args.empty()) { std::cout << "Uso: complete <prefijo>\n"; }
                else                  { handle_complete_impl(*impl_, cmd_args); }
                continue;
            }

            // buscar en la tabla de comandos
            bool handled = false;
            for (size_t i = 0; i < CMD_TABLE_SIZE; ++i) {
                if (cmd_name == cmd_table[i].name) {
                    cmd_table[i].fn(cmd_args);
                    handled = true;
                    break;
                }
            }

            // anadir al historial siempre; encolar solo si no fue un comando de tabla
            add_history_entry_impl(*impl_, cmd);
            if (!handled) cmd_queue_push_impl(*impl_, cmd);
        }

        stop();
    }

    void VestaViewManager::stop() {
        if (!impl_->running) return;
        impl_->running = false;
        cmd_queue_close_impl(*impl_);
        if (impl_->worker_thread.joinable()) impl_->worker_thread.join();
        save_history_impl(*impl_);
        global_interrupt_ptr = nullptr;
        std::signal(SIGINT, SIG_DFL);
    }
}
