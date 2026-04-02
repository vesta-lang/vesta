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

#include "cli/cli.h"

#include <cstdio>

#include "cli/cli_init_manager_and_server.h"
#include "cli/sync_io.h"

namespace cli {
    static ManagerOfManagersAndServer mgr = ManagerOfManagersAndServer();

    struct Impl {
        Config cfg;

        // estado
        std::atomic<bool> running{false};
        std::atomic<bool> interrupted_flag{false};

        // historial
        std::vector<std::string> history;
        std::mutex hist_m;
        int history_cursor = -1;

        // cola de comandos
        std::deque<std::string> cmd_queue;
        std::mutex q_m;
        std::condition_variable q_cv;
        bool q_closed = false;

        // worker
        std::thread worker_thread;

        // callback
        std::function<void(const std::string &)> vm_execute_cb;
        std::mutex cb_m;

        Impl(Config c) : cfg(std::move(c)) {
        }
    };

    /**
     * Permite listar todos los manager disponibles
     */
    static void command_list_vmgr() {
    }

    /**
     * Comando que permite ejecutar un bytecode creand un nuevo manager
     * @param cmd ruta del archivo a ejecutar
     */
    static void command_run(const std::string &cmd) {
        // parsear parámetros: nombre y ruta
        CmdParams p = parse_cmd_params(cmd);
        if (!p.valid) {
            std::lock_guard lk(vesta::cout_mutex);
            vesta::scout() << "Uso: <name> <ruta_al_bytecode> [opciones]\n";
            return;
        }

        // normalizar la ruta (usa tu utilitario vfs::normalize_path_safe)
        auto path = fs::normalize_path_safe(p.path);
        if (!fs::file_exists(path)) {
            std::lock_guard lk(vesta::cout_mutex);
            vesta::scout() << "No existe el archivo: " << path.string() << "\n";
            return;
        }

        if (!fs::is_regular_file(path)) {
            std::lock_guard lk(vesta::cout_mutex);
            vesta::scout() << "La ruta no es un archivo regular: " << path.string() << "\n";
            return;
        }

        if (!fs::can_read(path)) {
            std::lock_guard lk(vesta::cout_mutex);
            vesta::scout() << "No se puede leer el archivo: " << path.string() << "\n";
            return;
        }

        auto maybe_abs2 = fs::get_existing_absolute_path(path);
        if (!maybe_abs2) {
            std::lock_guard lk(vesta::cout_mutex);
            vesta::scout() << "No se pudo resolver la ruta: " << path.string() << "\n";
            return;
        }

        std::cout << "Intentando ejecutar: " << maybe_abs2->string() << "\n";

        // p.name identificador y maybe_abs2->string() ruta absoluta

        // Construcción del manager
        runtime::ManageVM vm{nullptr};
        vm.name_manager = p.name;

        // Añadir moviendo la instancia
        size_t id = mgr.add_manager(std::move(vm));

        // Obtener copia segura del manager
        auto maybe = mgr.get_manager_copy(id);
        if (!maybe) {
            vesta::scout() << "No se pudo obtener copia del manager\n";
            return;
        }

        // Construir la representación textual fuera del mutex,
        // espero que esto no de problemas en el futuro
        std::string info = maybe->to_string_vm_manager_info();

        // Imprimir bajo mutex (bloque corto)
        //{
        //std::lock_guard lk(vesta::cout_mutex);
        vesta::scout() << "Manager creado con ID: " << id << "\n";
        vesta::scout() << info;
        //}
    }


    // Señal global para Ctrl+C. Se usa para notificar la instancia activa.
    static std::atomic<bool> *global_interrupt_ptr = nullptr;

    static void global_sigint_handler(int) {
        if (global_interrupt_ptr) *global_interrupt_ptr = true;
    }

    // Utilidades
    static std::vector<std::string> split_words(const std::string &s) {
        std::istringstream iss(s);
        std::vector<std::string> out;
        std::string w;
        while (iss >> w) out.push_back(w);
        return out;
    }

    // Impl helpers
    static void load_history_impl(Impl &I) {
        std::lock_guard lk(I.hist_m);
        std::ifstream f(I.cfg.history_file);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) I.history.push_back(line);
        }
    }

    static void save_history_impl(Impl &I) {
        std::lock_guard lk(I.hist_m);
        std::ofstream f(I.cfg.history_file, std::ios::trunc);
        if (!f) return;
        size_t start = I.history.size() > I.cfg.history_max ? I.history.size() - I.cfg.history_max : 0;
        for (size_t i = start; i < I.history.size(); ++i) f << I.history[i] << "\n";
    }

    static void add_history_entry_impl(Impl &I, const std::string &line) {
        if (line.empty()) return;
        std::lock_guard lk(I.hist_m);
        if (!I.history.empty() && I.history.back() == line) return;
        I.history.push_back(line);
        if (I.history.size() > I.cfg.history_max) I.history.erase(I.history.begin());
    }

    static void cmd_queue_push_impl(Impl &I, std::string cmd) { {
            std::lock_guard lk(I.q_m);
            if (I.q_closed) return;
            I.cmd_queue.push_back(std::move(cmd));
        }
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

    static void cmd_queue_close_impl(Impl &I) { {
            std::lock_guard lk(I.q_m);
            I.q_closed = true;
        }
        I.q_cv.notify_all();
    }

    // Worker loop
    static void worker_loop_impl(Impl &I) {
        while (I.running) {
            std::string cmd;
            if (!cmd_queue_pop_impl(I, cmd)) break;
            std::function<void(const std::string &)> cb; {
                std::lock_guard lk(I.cb_m);
                cb = I.vm_execute_cb;
            }
            if (cb) {
                cb(cmd);
            } else {
                // fallback: simulación por ahora
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                std::cout << "[VM] ejecutado: " << cmd << std::endl;
            }
        }
    }

    // REPL helpers
    // Reemplaza la función print_help_impl existente por esta versión ampliada.
    static void print_help_impl(const Impl &I) {
        std::cout << "Comandos especiales:\n"
                << "  exit                 - salir\n"
                << "  help                 - mostrar esta ayuda\n"
                << "  history              - listar historial con indices\n"
                << "  !N                   - ejecutar entrada N del historial (ej: !3)\n"
                << "  :prev / :next        - mostrar entrada anterior / siguiente del historial\n"
                << "  complete PREF        - listar completions para PREF\n"
                << "\n"
                << "Comandos del sistema:\n"
                << "  cmd <comando>        - ejecutar <comando> en el shell (asíncrono)\n"
                << "    Ejemplo: cmd ls -la\n"
                << "\n"
                << "Ejecutar bytecode / crear manager:\n"
                << "  run <name> <ruta> [opciones]\n"
                << "    Crea un nuevo manager y carga el bytecode indicado por <ruta>.\n"
                << "    <name>  : identificador libre para el manager (primer token).\n"
                << "              Si contiene espacios, debe ir entre comillas.\n"
                << "    <ruta>  : ruta al fichero del bytecode (absoluta o relativa).\n"
                << "              Si contiene espacios, debe ir entre comillas.\n"
                << "    [opciones] : cualquier texto restante se pasa como 'rest' y\n"
                << "                 puede usarse para flags de la VM.\n"
                << "\n"
                << "    Reglas de parseo:\n"
                << "      - El primer token es el nombre; puede ir entre comillas \"...\" o '...'.\n"
                << "      - El segundo token es la ruta; también admite comillas y escapes.\n"
                << "      - Ejemplos:\n"
                << "         run myvm ./build/code.velb\n"
                << "         run \"My VM\" \"/home/user/code with spaces.velb\" --debug\n"
                << "         run name \"C:\\\\Program Files\\\\VM\\\\code.velb\"\n"
                << "\n"
                << "Pantalla y visual:\n"
                << "  cls/clear            - limpiar pantalla (Windows)\n"
                << "\n"
                << "Multilinea:\n"
                << "  Termina con '" << I.cfg.multiline_end << "' para enviar bloque.\n"
                << "\n"
                << "Notas de seguridad:\n"
                << "  Ejecutar comandos de shell o cargar bytecode puede ser peligroso.\n"
                << "  - Usa cmd/run solo en entornos de confianza.\n"
                << "  - Las rutas se validan (existencia/lectura) antes de crear el manager.\n"
                << "  - Para evitar bloqueos, las ejecuciones se lanzan en segundo plano y\n"
                << "    su salida se muestra cuando terminan.\n"
                << "\n"
                << "Consejos:\n"
                << "  - Si la ruta no se encuentra, verifica comillas y escapes.\n"
                << "  - Para rutas con ~ o variables de entorno, expándelas antes de usar.\n"
                << "  - Usa nombres únicos para managers para facilitar su identificación.\n";
    }


    static void print_history_impl(Impl &I) {
        std::lock_guard lk(I.hist_m);
        for (size_t i = 0; i < I.history.size(); ++i) {
            std::cout << i << ": " << I.history[i] << "\n";
        }
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
        if (I.history.empty()) {
            std::cout << "(historial vacio)\n";
            return;
        }
        if (I.history_cursor < 0) I.history_cursor = static_cast<int>(I.history.size()) - 1;
        std::cout << I.history[I.history_cursor] << "\n";
        I.history_cursor = (I.history_cursor - 1 + static_cast<int>(I.history.size())) % static_cast<int>(I.history.
                               size());
    }

    static void show_next_history_impl(Impl &I) {
        std::lock_guard<std::mutex> lk(I.hist_m);
        if (I.history.empty()) {
            std::cout << "(historial vacio)\n";
            return;
        }
        I.history_cursor = (I.history_cursor + 1) % static_cast<int>(I.history.size());
        std::cout << I.history[I.history_cursor] << "\n";
    }

    /**
     * @brief Permite sugerir completions para un prefijo dado. Esto 
     * consultaría la tabla de símbolos
     * 
     * @param I instancia del REPL para acceder a configuración o estado si es 
     * necesario
     * @param pref prefijo para el que se quieren sugerencias 
     */
    static void handle_complete_impl(const Impl &I, const std::string &pref) {
        // Reemplazar por consulta a tabla de símbolos de la VM.
        std::vector<std::string> candidates = {
            "run", "step", "break", "continue",
            "stack", "mem", "help", "exit"
        };
        bool any = false;
        for (auto &c: candidates)
            if (c.rfind(pref, 0) == 0) {
                std::cout << c << " ";
                any = true;
            }
        if (!any) std::cout << "(no hay candidatos)";
        std::cout << "\n";
    }

    // VestaViewManager methods
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

    // Comprueba si la línea inicia un bloque por palabra clave simple o contiene '{'
    static bool likely_start_block(const std::string &line) {
        // ejemplo: si empieza por "func " o contiene '{' al final o en cualquier parte
        std::string s = line;
        // trim left
        auto p = s.find_first_not_of(" \t");
        if (p != std::string::npos) s = s.substr(p);
        if (s.rfind("func ", 0) == 0) return true;
        if (s.find('{') != std::string::npos) return true;
        return false;
    }

    /**
     * @brief Ejecuta un comando de shell y devuelve su salida completa.
     * @param cmd Comando a ejecutar (sin el prefijo "cmd ").
     * @return Salida combinada stdout+stderr.
     * @throws std::runtime_error si no se puede abrir el proceso.
     *
     * Nota: usa popen/_popen; en POSIX redirige stderr a stdout con "2>&1".
     */
    static std::string execute_shell_command(const std::string &cmd) {
        std::string full_cmd;
#if defined(_WIN32)
        // En Windows, ejecutar con cmd.exe /C para que la sintaxis sea la esperada
        full_cmd = "cmd.exe /C \"" + cmd + "\" 2>&1";
        FILE *pipe = _popen(full_cmd.c_str(), "r");
#else
        // POSIX: usar /bin/sh -c o directamente popen con redirección
        full_cmd = cmd + " 2>&1";
        FILE *pipe = popen(full_cmd.c_str(), "r");
#endif
        if (!pipe) throw std::runtime_error("failed to open pipe for command");

        std::string result;
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result.append(buffer);
        }

#if defined(_WIN32)
        int rc = _pclose(pipe);
#else
        int rc = pclose(pipe);
#endif
        // añadir código de retorno al final
        result += "\n[exit code: " + std::to_string(rc) + "]";
        return result;
    }


    // Cuenta balance de llaves en una línea (incrementa por '{', decrementa por '}')
    static int brace_delta(const std::string &line) {
        int d = 0;
        for (char c: line) {
            if (c == '{') ++d;
            else if (c == '}') --d;
        }
        return d;
    }

    /// Helper: expande "~" al HOME y devuelve path absoluto
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
            if (p.size() == 1) return std::filesystem::path(home_s);
            // "~/<rest>"
            if (!home_s.empty()) return std::filesystem::path(home_s) / p.substr(2);
            // si no hay HOME, devolver p tal cual
            return std::filesystem::path(p.substr(1));
        }
        return std::filesystem::path(p);
    }

    /**
     * @brief Limpia la pantalla de la consola de forma portátil.
     *
     * En POSIX se envía la secuencia ANSI ESC[2J ESC[H para limpiar y mover el cursor.
     * En Windows intenta habilitar el modo de secuencias ANSI (VT) y, si no es posible,
     * cae en system("cls") como último recurso.
     *
     * Esta función no lanza excepciones; en caso de error simplemente no limpia.
     */
    static void clear_screen() {
#if defined(_WIN32)
        // Intentar habilitar Virtual Terminal Processing para secuencias ANSI
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD dwMode = 0;
            if (GetConsoleMode(hOut, &dwMode)) {
                // intentar habilitar VT processing
                if ((dwMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
                    DWORD newMode = dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                    if (SetConsoleMode(hOut, newMode)) {
                        // enviar secuencia ANSI
                        std::cout << "\x1B[2J\x1B[H" << std::flush;
                        return;
                    }
                } else {
                    std::cout << "\x1B[2J\x1B[H" << std::flush;
                    return;
                }
            }
        }
        // Fallback: usar comando del sistema, a veces no funciona
        std::system("cls");
#else
        // POSIX: secuencia ANSI para limpiar pantalla y colocar cursor en (1,1)
        std::cout << "\x1B[2J\x1B[H" << std::flush;
#endif
    }


    void VestaViewManager::run() {
        if (impl_->running) return;
        impl_->running = true;

        // registrar señal global
        global_interrupt_ptr = &impl_->interrupted_flag;
        std::signal(SIGINT, global_sigint_handler);

        load_history_impl(*impl_);

        // iniciar worker
        impl_->worker_thread = std::thread(worker_loop_impl, std::ref(*impl_));

        // REPL loop (bloqueante)
        while (impl_->running) {
            if (impl_->interrupted_flag.exchange(false)) {
                std::cout << "^C\n";
            }

            std::cout << impl_->cfg.prompt << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) {
                std::cout << "\n";
                break; // EOF
            }

            if (impl_->interrupted_flag.exchange(false)) {
                std::cout << "^C\n";
                continue;
            }

            std::string full = line;
            const std::string &term = impl_->cfg.multiline_end;

            // Si la línea termina con el terminador, quitarlo y usarla directamente
            if (full.size() >= term.size() &&
                full.substr(full.size() - term.size()) == term) {
                full = full.substr(0, full.size() - term.size());
            } else {
                // Si la línea parece iniciar un bloque (ej. "func ..." o contiene '{'),
                // entrar en modo bloque y acumular hasta que el balance de llaves sea 0
                bool start_block = likely_start_block(full);
                if (start_block) {
                    int balance = brace_delta(full);
                    // Si el inicio ya cerró el bloque en la misma línea y balance<=0, no leer más
                    if (balance > 0) {
                        // leer líneas hasta que balance vuelva a 0 o aparezca el terminador
                        while (true) {
                            std::cout << "... " << std::flush;
                            std::string more;
                            if (!std::getline(std::cin, more)) {
                                full.clear();
                                break;
                            }
                            if (impl_->interrupted_flag.exchange(false)) {
                                std::cout << "^C\n";
                                full.clear();
                                break;
                            }

                            // si la línea termina con terminador, quitarlo y añadir la parte previa
                            if (more.size() >= term.size() &&
                                more.substr(more.size() - term.size()) == term) {
                                std::string part = more.substr(0, more.size() - term.size());
                                full += "\n" + part;
                                balance += brace_delta(part);
                                break;
                            } else {
                                full += "\n" + more;
                                balance += brace_delta(more);
                                if (balance <= 0) break; // bloque cerrado
                            }
                        }
                    }
                    // si balance <= 0 ya está completo en la misma línea
                } else {
                    // No es inicio de bloque ni terminador: tratar como línea simple (no acumular)
                    // full ya es la línea completa
                }
            }

            // trim
            auto start = full.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            auto end = full.find_last_not_of(" \t\r\n");
            std::string cmd = full.substr(start, end - start + 1);

            // comandos especiales
            if (cmd == "exit") {
                add_history_entry_impl(*impl_, cmd);
                break;
            }
            if (cmd == "help") {
                print_help_impl(*impl_);
                continue;
            }
            if (cmd == "history") {
                print_history_impl(*impl_);
                continue;
            }
            if (!cmd.empty() && cmd[0] == '!') {
                handle_history_exec_impl(*impl_, cmd);
                continue;
            }
            if (cmd == ":prev") {
                show_prev_history_impl(*impl_);
                continue;
            }
            if (cmd == ":next") {
                show_next_history_impl(*impl_);
                continue;
            }

            // detectar comandos de limpieza de pantalla
            if (cmd == "cls" || cmd == "clear") {
                // proteger la salida si otros hilos también escriben
                std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                clear_screen();
                // añadir al historial
                add_history_entry_impl(*impl_, cmd);
                continue;
            }

            // dentro de run(), tras obtener 'cmd' y antes de encolar:
            if (cmd.rfind("cmd ", 0) == 0) {
                std::string shell = cmd.substr(4); // quitar "cmd "
                std::thread([shell]() {
                    // Integración asíncrona (no bloquear REPL)
                    try {
                        std::string out = execute_shell_command(shell);
                        std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                        std::cout << out << std::endl;
                    } catch (const std::exception &e) {
                        std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                        std::cerr << "Error ejecutando shell: " << e.what() << std::endl;
                    }
                }).detach();
                add_history_entry_impl(*impl_, cmd); // no encolar ni añadir al history?
                continue;
            }

            // Soporte para cambiar directorio: cd <path>, cd -, cd (sin args)
            if (cmd.rfind("cd", 0) == 0) {
                // separar tokens: "cd", "arg"
                std::string arg;
                if (cmd.size() > 2) {
                    // quitar "cd" y espacios iniciales
                    auto pos = cmd.find_first_not_of(" \t", 2);
                    if (pos != std::string::npos) arg = cmd.substr(pos);
                }

                // static para recordar el directorio anterior entre invocaciones
                static std::filesystem::path previous_dir = std::filesystem::current_path();

                try {
                    std::filesystem::path target;
                    if (arg.empty()) {
                        // cd sin argumentos -> HOME
                        const char *home = std::getenv(
#if defined(_WIN32)
                            "USERPROFILE"
#else
                            "HOME"
#endif
                        );
                        if (!home) {
                            std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                            std::cerr << "cd: HOME no definido\n";
                            add_history_entry_impl(*impl_, cmd);
                            continue;
                        }
                        target = std::filesystem::path(home);
                    } else if (arg == "-") {
                        // cd - -> volver al anterior
                        target = previous_dir;
                    } else {
                        // expandir ~ y resolver path relativo/absoluto
                        target = expand_path(arg);
                        if (target.is_relative()) target = std::filesystem::current_path() / target;
                    }

                    // normalizar y comprobar existencia
                    target = std::filesystem::weakly_canonical(target);

                    if (!std::filesystem::exists(target) || !std::filesystem::is_directory(target)) {
                        std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                        std::cerr << "cd: no existe o no es directorio: " << target.string() << "\n";
                        add_history_entry_impl(*impl_, cmd);
                        continue;
                    }

                    // cambiar directorio y actualizar previous_dir
                    std::filesystem::path old = std::filesystem::current_path();
                    std::filesystem::current_path(target);
                    previous_dir = old;

                    // informar al usuario
                    {
                        std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                        std::cout << "Directorio actual: " << std::filesystem::current_path().string() << "\n";
                    }

                    add_history_entry_impl(*impl_, cmd);
                } catch (const std::exception &e) {
                    std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                    std::cerr << "cd: error: " << e.what() << "\n";
                    add_history_entry_impl(*impl_, cmd);
                }
                continue;
            }

            if (cmd.rfind("run", 0) == 0) {
                std::string shell = cmd.substr(4);
                std::cout << "Ejecutando: " << shell << std::endl;
                command_run(shell);
            }

            // complete <pref>
            {
                auto words = split_words(cmd);
                if (!words.empty() && words[0] == "complete") {
                    if (words.size() < 2) {
                        std::cout << "Uso: complete <prefijo>\n";
                        continue;
                    }
                    handle_complete_impl(*impl_, words[1]);
                    continue;
                }
            }

            // normal: encolar para ejecución
            add_history_entry_impl(*impl_, cmd);
            cmd_queue_push_impl(*impl_, cmd);
        }

        stop();
    }

    void VestaViewManager::stop() {
        if (!impl_->running) return;
        impl_->running = false;
        cmd_queue_close_impl(*impl_);
        if (impl_->worker_thread.joinable()) impl_->worker_thread.join();
        save_history_impl(*impl_);
        // desregistrar señal
        global_interrupt_ptr = nullptr;
        std::signal(SIGINT, SIG_DFL);
    }
}
