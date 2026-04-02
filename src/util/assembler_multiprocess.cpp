/**
 * @file assembler_multiprocess.cpp
 * @brief Infraestructura para ensamblado paralelo usando multiproceso + multihilo.
 *
 * Este módulo implementa el modelo "driver + workers" de VestaVM:
 *
 *  - El **driver** ejecuta múltiples hilos (ThreadPool).
 *  - Cada hilo lanza un **worker** como proceso independiente (`vm.exe --worker`).
 *  - El driver captura la salida de cada worker, registra tiempos y errores,
 *    y finalmente ejecuta el linker global.
 *
 * Ventajas del modelo:
 *  - Aislamiento total entre workers (cada uno es un proceso separado).
 *  - Uso eficiente de todos los núcleos (multihilo en el driver).
 *  - Seguridad: un worker que falla no afecta al driver.
 *  - Escalabilidad: miles de archivos pueden ensamblarse en paralelo.
 *
 * @note Este header solo declara la interfaz; la implementación está en el .cpp.
 */
#include "util/assembler_multiprocess.h"
#include "profiler/timer.h"
#include "util/fs_utils.h"


namespace asm_multi_process {
    int run_worker(const std::string &file_name,
                   const std::string &output_prefix) {
        //if (arch.empty()) {
        //    std::cerr << "--arch es requerido en modo --worker\n";
        //    return EXIT_FAILURE;
        //}

        Timer global;

        // Leer archivo fuente
        const std::string name_file(file_name);
        std::ifstream file(name_file);
        if (!file.is_open()) {
            std::cerr << "ERROR: No se pudo abrir: " << name_file << "\n";
            return 1;
        }

        std::string code((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

        if (code.empty()) {
            std::cerr << "ERROR: Archivo vacio\n";
            return 1;
        }

        // Lexer + Parser

        vm::Lexer lexer(code);
        vm::Parser parser(lexer);


        std::vector<std::unique_ptr<vm::ASTNode> > program;
        Timer t_parser;
        try {
            program = parser.parse();
        } catch (const vm::ParseError &e) {
            std::cerr << "Parse error: " << e.what() << "\n";
            return 1;
        }
        vesta::scout() << "[Tiempo parser] " << t_parser.us() << " us " << t_parser.ms() << " ms\n";

        // Resolver imports
        std::unordered_set<std::string> imported_files;
        Assembly::Bytecode::resolve_imports(program, imported_files);

        // Ensamblar
        Assembler asmblr;
        Timer t_asm;
        std::vector<uint8_t> bytecode;
        try {
            bytecode = asmblr.assemble(program);
        } catch (const std::exception &e) {
            std::cerr << "Assembler error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
        vesta::scout() << "[Tiempo Assembler] " << t_asm.us() << " us " << t_asm.ms() << " ms\n";


        // LINKER
        Assembly::Bytecode::Linker::LinkerOptions opts;
        opts.optimize_bytecode = true;
        opts.generate_map_file = true;
        opts.output_path = output_prefix + ".velb";
        opts.map_file_path = output_prefix + ".velb-map";
        opts.verbose = true;

        Timer t_linker;
        Linker::Linker linker(opts);

        // Añadir el ensamblado crudo
        linker.add_assembly_unit(bytecode, &asmblr.ctx);

        // añadir objetos externos
        // linker.add_object_file("libmath.velo");
        // linker.add_static_library("stdlib.vela");

        // Construir ejecutable
        linker.write_to_file(opts.output_path);

        // Generar map file
        if (opts.generate_map_file)
            linker.write_map_file(opts.map_file_path);

        // Reporte
        const auto &report = linker.get_report();
        vesta::scout() << "[Tiempo Linker] " << t_linker.us() << " us " << t_linker.ms() << " ms\n";

        vesta::scout() << "\n=== LINKER REPORT ===\n";
        vesta::scout() << "Modulos enlazados: " << report.modules_linked << "\n";
        vesta::scout() << "Simbolos resueltos: " << report.symbols_resolved << "\n";
        vesta::scout() << "Relocaciones aplicadas: " << report.relocations_applied << "\n";
        vesta::scout() << "Optimizaciones aplicadas: " << report.optimizations_applied << "\n";

        if (!report.errors.empty()) {
            vesta::scout() << "\n=== ERRORES ===\n";
            for (auto &e: report.errors) vesta::scout() << " - " << e << "\n";
        }

        if (!report.warnings.empty()) {
            vesta::scout() << "\n=== WARNINGS ===\n";
            for (auto &w: report.warnings) vesta::scout() << " - " << w << "\n";
        }

        vesta::scout() << std::dec;
        vesta::scout() << "\n[Tiempo total] " << global.us() << " us "
                << global.ms() << " ms\n";

        struct WorkerTimes {
            long parser_us;
            long assembler_us;
            long linker_us;
            long total_us;
        };
        json j;
        j["parser_us"] = t_parser.us();
        j["assembler_us"] = t_asm.us();
        j["linker_us"] = t_linker.us();
        j["total_us"] = global.us();
        std::cout << "\n__VESTA_TIMES__ " << j.dump() << "\n";

        return EXIT_SUCCESS;
    }

    std::string run_and_capture(const std::string &cmd) {
        std::string result;
        char buffer[256];

        FILE *pipe = _popen(cmd.c_str(), "r");
        if (!pipe) return "ERROR: no se pudo ejecutar el comando\n";

        while (fgets(buffer, sizeof(buffer), pipe)) {
            result += buffer;
        }

        _pclose(pipe);
        return result;
    }

    int run_driver(const std::string &folder, int threads, const std::string &output) {
        Timer global;
        // Buscar archivos .vel
        std::vector<std::string> files;
        for (auto &entry: std::filesystem::directory_iterator(folder)) {
            if (entry.path().extension() == ".vel") {
                files.push_back(entry.path().string());
            }
        }

        if (files.empty()) {
            std::cerr << "No hay archivos .vel en " << folder << "\n";
            return EXIT_FAILURE;
        }

        // Crear ThreadPool
        ThreadPool pool(threads);

        std::mutex results_m;
        std::vector<std::string> obj_files;
        bool error = false;

        total_workers = files.size();
        completed_workers = 0;
        worker_current_file.resize(total_workers);

        progress_running = true;
        std::thread progress_thread(progress_thread_func);

        struct WorkerLog {
            std::string file;
            std::string output;
            bool failed;
            long parser_us;
            long assembler_us;
            long linker_us;
            long total_us;
        };
        std::vector<WorkerLog> logs;

        // Encolar tareas
        for (int i = 0; i < files.size(); i++) {
            std::string f = files[i];
            int index = i; // i es el índice del archivo en el vector files
            pool.enqueue([&, index, f] {
                worker_current_file[index] = f; {
                    std::lock_guard lk(progress_mutex);
                }

                std::string out = f + ".obj";
                std::string cmd = fs::get_executable_path();
                cmd += (" --worker \"" + f + "\" -o \"" + out + "\"");

                std::string output_comand = run_and_capture(cmd); {
                    std::lock_guard lk(results_m);

                    bool failed =
                            output_comand.find("ERROR") != std::string::npos ||
                            output_comand.find("Parse error") != std::string::npos;

                    size_t pos = output_comand.find("__VESTA_TIMES__");
                    long parser_us = 0, assembler_us = 0, linker_us = 0, total_us = 0;

                    if (pos != std::string::npos) {
                        std::string json_str = output_comand.substr(pos + 16);
                        try {
                            auto j = json::parse(json_str);
                            parser_us = j["parser_us"];
                            assembler_us = j["assembler_us"];
                            linker_us = j["linker_us"];
                            total_us = j["total_us"];
                        } catch (...) {
                            // si falla el parseo, no pasa nada
                        }
                    }

                    logs.push_back({
                        f,
                        output_comand,
                        failed,
                        parser_us,
                        assembler_us,
                        linker_us,
                        total_us,
                    });

                    if (!failed) {
                        obj_files.push_back(out);
                    } else {
                        error = true;
                    }
                }
                // Actualizar progreso
                last_finished = index;
                completed_workers++;
            });
        }

        pool.shutdown();
        progress_running = false;
        progress_thread.join();

        vesta::scout() << "\n\n===== LOGS DE COMPILACIÓN =====\n";

        for (auto &log: logs) {
            vesta::scout() << "\n=== OUTPUT de " << log.file << " ===\n";
            vesta::scout() << log.output << "\n";

            if (log.failed) {
                vesta::scout() << "[FALLO]\n";
            } else {
                vesta::scout() << "[OK]\n";
            }
        }
        vesta::scout() << "\n===== TIEMPOS POR ARCHIVO =====\n";

        for (auto &s: logs) {
            vesta::scout() << "\n" << s.file << "\n";
            vesta::scout() << "  parser:    " << s.parser_us << " us\n";
            vesta::scout() << "  assembler: " << s.assembler_us << " us\n";
            vesta::scout() << "  linker:    " << s.linker_us << " us\n";
            vesta::scout() << "  total:     " << s.total_us << " us\n";
        }


        if (error) {
            std::cerr << "Falló la compilación paralela.\n";
            std::cout << "\n[Tiempo total driver] " << global.us() << " us "
                    << global.ms() << " ms\n";
            return EXIT_FAILURE;
        }

        // Llamar al linker
        std::string cmd = fs::get_executable_path(); // obtenemos el nombre del ejecutable
        for (auto &o: obj_files) cmd += "\"" + o + "\" ";
        cmd += "-o \"" + output + "\"";

        std::string output_comand = run_and_capture(cmd);
        vesta::scout() << output << "\n";

        std::cout << "\n[Tiempo total driver] " << global.us() << " us "
                << global.ms() << " ms\n";
        return EXIT_SUCCESS;
    }

    void print_progress() {
        std::lock_guard lock(progress_mutex);

        int done = completed_workers.load();
        int total = total_workers;

        if (total == 0) return;

        float pct = (float) done / (float) total;
        int width = 40;
        int filled = (int) (pct * width);

        std::cout << "\r\033[1;32m["; // verde brillante

        for (int i = 0; i < filled; i++) std::cout << "#";
        std::cout << "\033[1;31m"; // rojo brillante
        for (int i = filled; i < width; i++) std::cout << "-";

        std::cout << "\033[0m] "; // reset

        std::cout << (int) (pct * 100) << "%  ("
                << done << "/" << total << ")  ";

        int idx = last_finished.load();
        std::string file = (idx >= 0 ? worker_current_file[idx] : "");
        // archivo actual
        std::cout << "\033[36m" << file << "\033[0m";

        std::cout << std::flush;
    }
}
