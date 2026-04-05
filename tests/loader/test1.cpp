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
size_t peak_memory = 0;

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

#include "emmit/parser_to_bytecode.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "profiler/timer.h"
#include "linker/velb_linker_bytecode.h"

#include "runtime/manager_runtime.h"


using namespace Assembly::Bytecode;

int main() {
    Timer global;
    runtime::ManageVM manager = runtime::ManageVM(nullptr, 0);

    // Leer archivo fuente
    const std::string name_file("test.vel");
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
    std::cout << "[Tiempo parser] " << t_parser.us() << " us " << t_parser.ms() << " ms\n";

    // Resolver imports
    std::unordered_set<std::string> imported_files;
    resolve_imports(program, imported_files);

    // Ensamblar
    Assembler asmblr;
    Timer t_asm;
    auto bytecode = asmblr.assemble(program);
    std::cout << "[Tiempo Assembler] " << t_asm.us() << " us " << t_asm.ms() << " ms\n";

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
    opts.output_path = "program.velb";
    opts.map_file_path = "program.velb-map";
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
    std::cout << "[Tiempo Linker] " << t_linker.us() << " us " << t_linker.ms() << " ms\n";

    std::cout << "\n=== LINKER REPORT ===\n";
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

    Timer t_loader;
    runtime::VM *vm = manager.loader.load_executable(opts.output_path);
    std::cout << "[Tiempo Loader] " << t_loader.us() << " us " << t_loader.ms() << " ms\n";


    vm->join();

    std::cout << std::dec;
    std::cout << "\n[Tiempo total] " << global.us() << " us "
            << global.ms() << " ms\n";

    return 0;
}
