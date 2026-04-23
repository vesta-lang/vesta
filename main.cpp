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
#include <openssl/sha.h>

#include "cxxopts.hpp"

#include "cli/cli.h"
#include "cli/runtime_api_commands.h"
#include "util/assembler_multiprocess.h"
#include "util/sqlite_singleton.h"
#include "runtime/manager_runtime.h"
#include "loader/loader.h"
#include "profiler/timer.h"


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
            ("stats", "Mostrar estadísticas de ejecución al finalizar (tiempo, MIPS)");

    auto result = options.parse(argc, argv);

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
