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


#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <string>

size_t total_allocated = 0;
size_t peak_memory     = 0;

/**
 * Metodo de "limpiado de cache" llamado "cache thrashing",
 * expulsa casi todo_ de L1/L2/L3
 */
void flush_cache() {
    static const size_t SIZE   = 50 * 1024 * 1024; // 50 MB
    static char *       buffer = NULL;

    if (!buffer) buffer = (char *) malloc(SIZE);

    for (size_t i = 0; i < SIZE; i += 64) {
        buffer[i]++; // tocar cada línea de caché
    }
}

void *operator new(std::size_t size) {
    total_allocated += size;
    peak_memory = std::max(peak_memory, total_allocated);
    return malloc(size);
}

void operator delete(void *ptr) noexcept {
    // no saber el tamaño aquí
    free(ptr);
}

void print_memory_stats() {
    std::cout << "Memoria actual: " << total_allocated
            << " bytes, maximo: " << peak_memory << " bytes\n";
}

// modo perfilado activado
//#define VM_PROFILE

#include "emmit/parser_to_bytecode.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "profiler/timer.h"
#include "linker/velb_linker_bytecode.h"

#include "runtime/manager_runtime.h"

#ifndef NO_COLOR
#define C_RESET   "\033[0m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_MAGENTA "\033[35m"
#define C_CYAN    "\033[36m"
#define C_BOLD    "\033[1m"
#else
#   undef C_RESET
#   undef C_RED
#   undef C_GREEN
#   undef C_YELLOW
#   undef C_BLUE
#   undef C_MAGENTA
#   undef C_CYAN
#   undef C_BOLD
#   define C_RESET   ""
#   define C_RED     ""
#   define C_GREEN   ""
#   define C_YELLOW  ""
#   define C_BLUE    ""
#   define C_MAGENTA ""
#   define C_CYAN    ""
#   define C_BOLD    ""
#endif

#ifdef _WIN32
#define SLEEP _sleep
#else
#define SLEEP usleep
#endif
using namespace Assembly::Bytecode;

//#define BENCHMARK_VM

int main() {
    Timer             global;
    runtime::ManageVM manager = runtime::ManageVM(nullptr, 0);

    // Leer archivo fuente
    const std::string name_file("test.vel");
    std::ifstream     file(name_file);
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

    vm::Lexer  lexer(code);
    vm::Parser parser(lexer);


    std::vector<std::unique_ptr<vm::ASTNode> > program;
    Timer                                      t_parser;
    try {
        program = parser.parse();
    } catch (const vm::ParseError &e) {
        std::cerr << "Parse error: " << e.what() << "\n";
        return 1;
    }
    std::cout << C_CYAN << "[Tiempo parser] " << C_RESET << t_parser.us() << " us " << t_parser.ms() << " ms\n";

    // Resolver imports
    std::unordered_set<std::string> imported_files;
    resolve_imports(program, imported_files);

    // Ensamblar
    Assembler asmblr;
    Timer     t_asm;
    auto      bytecode = asmblr.assemble(program);
    std::cout << C_CYAN << "[Tiempo Assembler] " << C_RESET << t_asm.us() << " us " << t_asm.ms() << " ms\n";

#ifdef DEBUG_EMIT
    std::cout << "\n=== CONTEXTO GENERADO POR EL ENSAMBLADOR ===\n";
    print_context(asmblr.ctx);

    std::cout << "\n=== BYTES GENERADOS ===\n";
    for (size_t i = 0; i < bytecode.size(); ++i) {
        if (i % 16 == 0)
            std::cout << "\n" << std::setw(8) << std::setfill('0')
                    << std::hex << i << ": ";
        std::cout << std::setw(2) << std::setfill('0')
                << std::hex << (int) bytecode[i] << " ";
    }
    std::cout << std::dec << "\n\n";

    print_context_with_bytes(asmblr.ctx, bytecode);
#endif

    // LINKER
    Linker::LinkerOptions opts;
    opts.optimize_bytecode = true;
    opts.generate_map_file = true;
    opts.output_path       = "program.velb";
    opts.map_file_path     = "program.velb-map";
    opts.verbose           = true;

    Timer          t_linker;
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
    std::cout << C_CYAN << "[Tiempo Linker] " << C_RESET << t_linker.us() << " us " << t_linker.ms() << " ms\n";

    std::cout << C_BOLD << C_BLUE << "\n=== LINKER REPORT ===\n" << C_RESET;
    std::cout << "Modulos enlazados: " << report.modules_linked << "\n";
    std::cout << "Simbolos resueltos: " << report.symbols_resolved << "\n";
    std::cout << "Relocaciones aplicadas: " << report.relocations_applied << "\n";
    std::cout << "Optimizaciones aplicadas: " << report.optimizations_applied << "\n";

    if (!report.errors.empty()) {
        std::cout << "\n=== ERRORES ===\n";
        for (auto &e: report.errors) std::cout << " - " << e << "\n";
    }

    if (!report.warnings.empty()) {
        vesta::scout() << "\n=== WARNINGS ===\n";
        for (auto &w: report.warnings) std::cout << " - " << w << "\n";
    }


    print_memory_stats();

    Timer        t_loader;
    runtime::VM *vm = manager.loader.load_executable(opts.output_path);

#ifdef BENCHMARK_VM
    std::vector<uint8_t> bench_code;
    bench_code.reserve(4 * 2'000'000); // 2M iteraciones -> 4M instrucciones

    const uint8_t add_instr[4][4] = {
        {0x04, 0x7f, 0x04, 0x7f},
        {0x00, 0x05, 0x40, 0xDF},
        {0x04, 0x3e, 0x04, 0x7f},
        {0x00, 0x05, 0xC0, 0xDF}
    };

    for (size_t i = 0; i < 2'000'000; i++) {
        bench_code.insert(bench_code.end(), add_instr[0], add_instr[0] + 4);
        bench_code.insert(bench_code.end(), add_instr[1], add_instr[1] + 4);
        bench_code.insert(bench_code.end(), add_instr[2], add_instr[2] + 4);
        bench_code.insert(bench_code.end(), add_instr[3], add_instr[3] + 4);
    }
    // HLT = 00 03
    bench_code.push_back(0x00);
    bench_code.push_back(0x03);
    // cagro codigo de pruebas:
    vm->load_raw_code(0, bench_code);
#endif

    std::cout << C_CYAN << "[Tiempo Loader] " << C_RESET << t_loader.us() << " us " << t_loader.ms() << " ms\n";

    vm->add_debug_hook([](runtime::VM *vm, runtime::DebugStage stage) {
        if (!vm) return;

        switch (stage) {
            // --- INICIO DE FASES ---
            case runtime::DebugStage::DecodeBegin:
            case runtime::DebugStage::ExecuteBegin:
            case runtime::DebugStage::OnEventBegin:
                vm->debug_timer.reset();
                break;

            // --- FIN DE FASES ---

            case runtime::DebugStage::DecodeEnd: {
                vm->time_decode += vm->debug_timer.ns();
                vm->debug_timer.reset();
                break;
            }

            case runtime::DebugStage::ExecuteEnd: {
                vm->time_exec += vm->debug_timer.ns();
                break;
            }

            case runtime::DebugStage::OnEventEnd: {
                vm->time_event += vm->debug_timer.ns();
                break;
            }

            default:
                break;
        }
    });

    // configuracion de un HOOK para la VM
    vm->add_debug_hook([](runtime::VM *vm, runtime::DebugStage stage) {
        if (!vm) return;
        const char *stage_name = runtime::debug_stage_name(stage);

        const char *stage_color = C_RESET;
        switch (stage) {
            case runtime::DebugStage::DecodeBegin:
            case runtime::DebugStage::DecodeEnd:
                stage_color = C_CYAN;
                break;
            case runtime::DebugStage::ExecuteBegin:
            case runtime::DebugStage::ExecuteEnd:
                stage_color = C_GREEN;
                break;
            case runtime::DebugStage::OnEventBegin:
            case runtime::DebugStage::OnEventEnd:
                stage_color = C_YELLOW;
                break;
            default:
                stage_color = C_RESET;
                break;
        }

        if (vm->decoded_ptr != nullptr) {
            printf("%s[%s]%s PC=%llu  OP1=%u OP2=%u  MODE=%u  STATE=%s\n",
                   stage_color,
                   stage_name,
                   C_RESET,
                   vm->rip.raw(),
                   vm->decoded_ptr->flags_info.is_not_extended,
                   vm->decoded_ptr->flags_info.opcode_index,
                   vm->decoded_ptr->flags_info.mode,
                   vm_state_to_str(vm->state)
            );
        }

        if (stage == runtime::DebugStage::ExecuteBegin) {
            printf("%s[%s]%s Procediendo a ejecutar: %s%s%s\n", stage_color,
                   stage_name,
                   C_RESET,
                   C_YELLOW,
                   vm->decoded_ptr->metadata->name,
                   C_RESET
            );
        }

        if (stage == runtime::DebugStage::ExecuteEnd) {
            printf(C_CYAN "[STATE AFTER INSTRUCTION]\n" C_RESET);

            // --- REGISTERS ---
            for (int i = 0; i < 16; ++i) {
                printf(" R%02d=0x%016llx", i, vm->regs[i].qword());
                if ((i % 2) == 1) printf("\n");
            }
            printf("\n");

            for (int i = 0; i < 4; ++i) {
                printf(" CUR%02d=0x%016llx", i, vm->cur[i].qword());
                if ((i % 2) == 1) printf("\n");
            }
            printf("\n");

            // --- FLAGS ---
            printf("\nFLAGS=[");
            printf("\n");
            printf("\tCF=%u ", vm->flags.bits.CF);
            printf("\tOF=%u ", vm->flags.bits.OF);
            printf("\n");
            printf("\tSF=%u ", vm->flags.bits.SF);
            printf("\tZF=%u ", vm->flags.bits.ZF);
            printf("\n");
            printf("\tDM=%u", vm->flags.bits.DM);
            printf("\n");
            printf("]\n");

            // --- POINTERS ---
            printf(" IP=0x%016llx\n", vm->rip.raw());
            printf(" SP=0x%016llx\n", vm->stack_pointer.raw());
            printf(" BP=0x%016llx\n", vm->base_pointer.raw());

            printf(C_MAGENTA "[PROFILE]\n" C_RESET);

            printf("  decode = %lld ns (%lld ms)\n",
                   vm->time_decode,
                   vm->time_decode / 1'000'000);

            printf("  exec = %lld ns   (%lld ms)\n",
                   vm->time_exec,
                   vm->time_exec / 1'000'000);

            printf("  event = %lld ns  (%lld ms)\n",
                   vm->time_event,
                   vm->time_event / 1'000'000);

            SLEEP(500);

            vm->debug_timer.reset();
            vm->time_decode = 0;
            vm->time_exec   = 0;
            vm->time_event  = 0;
        }
        if (stage == runtime::DebugStage::OnEventBegin)
            printf(">>> EVENT: %s\n\n", vm_state_to_str(vm->state));
    });

#ifdef BENCHMARK_VM

    vm->has_hooks                = false;
    const uint64_t INSTR_PER_RUN = 8'000'000; // 4 ADD * 2M iteraciones

    uint64_t total_ns = 0;
    uint64_t min_ns   = UINT64_MAX;
    uint64_t max_ns   = 0;

    const int RUNS = 10000;

    for (int i = 0; i < RUNS; i++) {
        // Reset de la VM antes de cada ejecución
        vm->rip.ptr_vm.raw = 0;
        vm->has_hooks      = false;

        // limpio la cache de la VM y la CPU para poder intentar medir un rendimiento real
        // for (auto &entry: vm->icache) {
        //     entry = {};
        //     entry.pc = UINT64_MAX;
        // }
        // vm->decoded_ptr = nullptr; // retaurar el puntero decoder para que el cache se reinicie.
        //flush_cache(); // limpiar cache de CPU
        // -------------------------------------------------------------

        Timer t_bench;
        std::thread(&runtime::profiler_thread, vm).detach();
        vm->start();
        vm->join();
        uint64_t ns = t_bench.ns();

        total_ns += ns;
        if (ns < min_ns) min_ns = ns;
        if (ns > max_ns) max_ns = ns;

        double ns_per_instr = double(ns) / INSTR_PER_RUN;
        double mips         = (INSTR_PER_RUN * 1000.0) / ns;
        /* std::cout << C_CYAN << "[RUN " << i << "] " << C_RESET
                 << ns << " ns  (" << (ns / 1000.0) << " us, "
                 << (ns / 1'000'000.0) << " ms)"
                 << "  |  " << mips << " MIPS\n";*/
    }

    SLEEP(3000);

    double avg_ns   = double(total_ns) / RUNS;
    double avg_mips = (INSTR_PER_RUN * 1000.0) / avg_ns;
    std::cout << C_GREEN << "\n=== RESULTADOS BENCHMARK ===\n" << C_RESET;

    std::cout << "Promedio: " << avg_ns << " ns  ("
            << (avg_ns / 1000.0) << " us, "
            << (avg_ns / 1'000'000.0) << " ms)\n";

    std::cout << "Minimo:   " << min_ns << " ns  ("
            << (min_ns / 1000.0) << " us, "
            << (min_ns / 1'000'000.0) << " ms)\n";

    std::cout << "Maximo:   " << max_ns << " ns  ("
            << (max_ns / 1000.0) << " us, "
            << (max_ns / 1'000'000.0) << " ms)\n";

    double min_mips = (INSTR_PER_RUN * 1000.0) / max_ns; // max_ns = peor tiempo
    double max_mips = (INSTR_PER_RUN * 1000.0) / min_ns; // min_ns = mejor tiempo

    std::cout << C_MAGENTA << "\n--- MIPS ---\n" << C_RESET;
    std::cout << "MIPS promedio: " << avg_mips << "\n";
    std::cout << "MIPS minimo:   " << min_mips << "\n";
    std::cout << "MIPS maximo:   " << max_mips << "\n";

#else
    //vm->has_hooks = false;

    Timer t_bench;
    vm->start();
    std::thread(&runtime::profiler_thread, vm).detach();
    vm->join();
    auto runner_ns = t_bench.ns();
    std::cout << std::dec;
    std::cout << C_CYAN << "\n[VM_EXEC_CODE] " << C_RESET << "Tardo en ejecutar: " << runner_ns << " us\n";
#endif


    std::cout << std::dec;
    std::cout << C_CYAN << "\n[Tiempo total] " << C_RESET << global.us() << " us "
            << global.ms() << " ms\n";

    std::cout << vm->to_string() << std::endl;

    return 0;
}
