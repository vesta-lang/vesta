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

#include "linker/velb_linker_bytecode.h"

namespace Assembly::Bytecode::Linker {

    // cambiar en un futuro, por ahora para pruebas me vale
    static uint64_t simple_checksum(const std::vector<uint8_t>& data) {
        uint64_t sum = 0;
        for (auto b : data) sum += b;
        return sum;
    }

    void Linker::log_warning(const std::string& msg) {
        report.warnings.push_back(msg);
        if (options.verbose)
            std::cerr << "[Linker][WARN] " << msg << "\n";
    }

    void Linker::log_error(const std::string& msg) {
        report.errors.push_back(msg);
        std::cerr << "[Linker][ERROR] " << msg << "\n";
    }

    void Linker::add_object_file(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            log_error("No se pudo abrir objeto: " + path);
            return;
        }

        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());

        if (data.empty()) {
            log_warning("Objeto vacio: " + path);
            return;
        }

        // TODO: aquí parsear formato .velo/.velb parcial.
        // Por ahora, lo tratamos como un módulo con bytecode plano y sin contexto real.

        Module m;
        m.name = path;
        m.bytecode = std::move(data);
        m.is_object = true;

        // TODO: rellenar m.ctx, m.local_symbols, m.relocations según el formato real.

        modules.push_back(std::move(m));
        report.modules_linked++;
    }

    void Linker::add_object_memory(const std::vector<uint8_t>& data) {
        if (data.empty()) {
            log_warning("add_object_memory: buffer vacio");
            return;
        }

        Module m;
        m.name = "<memory-object>";
        m.bytecode = data;
        m.is_object = true;

        // TODO: parsear cabecera interna si la hay.

        modules.push_back(std::move(m));
        report.modules_linked++;
    }

    void Linker::add_static_library(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            log_error("No se pudo abrir libreria estatica: " + path);
            return;
        }

        // TODO: parsear header VELA, tabla de módulos, etc.
        // Por ahora solo registramos la ruta.

        StaticLibrary lib;
        lib.path = path;
        libraries.push_back(std::move(lib));
    }

    void Linker::add_assembly_unit(const std::vector<uint8_t>& bytecode,
                                   const Context& ctx) {
        Module m;
        m.name = "<assembly-unit>";
        m.bytecode = bytecode;
        m.ctx = ctx;
        m.is_object = false;

        // TODO: extraer símbolos locales de ctx y rellenar m.local_symbols
        // TODO: extraer relocaciones generadas por el ensamblador

        modules.push_back(std::move(m));
        report.modules_linked++;
    }

    void Linker::resolve_symbols() {
        // Paso 1: recolectar símbolos locales de todos los módulos
        for (auto& mod : modules) {
            for (const auto& [name, addr] : mod.local_symbols) {
                if (global_symbols.count(name)) {
                    log_error("Simbolo duplicado: " + name + " en modulo " + mod.name);
                    continue;
                }
                global_symbols[name] = addr;
                report.symbols_resolved++;
            }
        }

        // Paso 2 (simplificado): no usamos aún .vela para resolver símbolos faltantes.
        // Debo iterar sobre libraries y cargaría módulos
        // adicionales según símbolos indefinidos.

        // Si no se permiten símbolos indefinidos, podría comprobarlo aquí.
        if (!options.allow_undefined_symbols) {
            // TODO: detectar símbolos referenciados pero no definidos.
            // De momento lo omitimos.
        }
    }

    void Linker::apply_relocations() {
        for (auto& mod : modules) {
            for (const auto& rel : mod.relocations) {
                auto it = global_symbols.find(rel.symbol);
                if (it == global_symbols.end()) {
                    log_error("Relocacion: simbolo no resuelto: " + rel.symbol);
                    continue;
                }

                uint64_t target_addr = it->second;

                if (rel.offset + sizeof(uint64_t) > mod.bytecode.size()) {
                    log_error("Relocacion fuera de rango en modulo " + mod.name);
                    continue;
                }

                switch (rel.type) {
                    case Relocation::Type::Absolute64: {
                        std::memcpy(&mod.bytecode[rel.offset], &target_addr, sizeof(uint64_t));
                        break;
                    }
                    case Relocation::Type::Relative32: {
                        // offset relativo simplificado (suponemos PC en rel.offset + 4)
                        int32_t rel32 = static_cast<int32_t>(target_addr - (rel.offset + 4));
                        if (rel.offset + sizeof(int32_t) > mod.bytecode.size()) {
                            log_error("Reloc REL32 fuera de rango en modulo " + mod.name);
                            continue;
                        }
                        std::memcpy(&mod.bytecode[rel.offset], &rel32, sizeof(int32_t));
                        break;
                    }
                    case Relocation::Type::Relative64: {
                        int64_t rel64 = static_cast<int64_t>(target_addr - (rel.offset + 8));
                        std::memcpy(&mod.bytecode[rel.offset], &rel64, sizeof(int64_t));
                        break;
                    }
                }

                report.relocations_applied++;
            }
        }
    }

    void Linker::optimize_modules() {
        if (!options.optimize_bytecode)
            return;

        for (auto& mod : modules) {
            // TODO: implementar optimizaciones reales de bytecode. Usaria Optimizer
            // Por ahora, no hacemos nada, solo contamos el módulo.
            // Ejemplo: eliminar NOPs, fusionar instrucciones triviales, etc.

            report.optimizations_applied++;
        }
    }

    void Linker::merge_address_spaces() {
        // Aquí debería usar Context para fusionar los espacios de direcciones
        // de todos los modulos. Por ahora, lo simplificamos:
        //
        // - asumimos un unico espacio de direcciones
        // - las secciones se concatenan linealmente

        // TODO: usar Context para algo real.
    }

    void Linker::merge_sections() {
        // Para simplificar:
        // - concatenamos todos los bytecodes de los módulos en final_executable
        // - creamos una única sección [.code] que cubre todo

        final_executable.clear();

        uint64_t base_addr = 0x100000000; // ejemplo
        uint64_t current_addr = base_addr;

        section_range_memory code_sec{};
        code_sec.address.address_init = base_addr;

        for (auto& mod : modules) {
            // En un diseño real, aquí respetaría secciones de ctx (code, data, etc.)
            // lo hare en un futuro
            final_executable.insert(final_executable.end(),
                                    mod.bytecode.begin(),
                                    mod.bytecode.end());
            current_addr += mod.bytecode.size();
        }

        code_sec.address.address_final = current_addr;
        std::memset(code_sec.name.name, 0, sizeof(code_sec.name.name));
        std::memcpy(code_sec.name.name, ".code", 5);

        final_sections.clear();
        final_sections.push_back(code_sec);
    }

    void Linker::build_header() {
        std::memset(&final_header, 0, sizeof(final_header));

        final_header.magic.firma = MAGIC_NUMBER_VELB;
        final_header.format_v = 1;

        final_header.max_v = 0; // 0.0.0 => compatible con futuras
        final_header.min_v = 0; // 0.0.0 => retrocompatible

        final_header.checksum = simple_checksum(final_executable);

        final_header.flags = 0;

        final_header.timestamp = static_cast<uint64_t>(std::time(nullptr));
        final_header.arch = 1; // debo definir las arch posibles

        final_header.count = static_cast<section_count>(final_sections.size());

        // La tabla de secciones irá justo después del header
        final_header.table_offset = sizeof(HeaderVELB);

        // Espacios de direcciones: por ahora, uno solo que cubre todo
        for (int i = 0; i < 8; ++i) {
            final_header.address_spaces[i].address_init = 0;
            final_header.address_spaces[i].address_final = 0;
        }

        if (!final_sections.empty()) {
            final_header.address_spaces[0].address_init = final_sections[0].address.address_init;
            final_header.address_spaces[0].address_final = final_sections[0].address.address_final;
        }
    }

    void Linker::build_section_table() {
        // En este diseño, la tabla de secciones se escribirá en build_executable()
        // justo después del header, usando final_sections.
    }

    std::vector<uint8_t> Linker::build_executable() {
        resolve_symbols();
        apply_relocations();
        optimize_modules();
        merge_address_spaces();
        merge_sections();
        build_header();
        build_section_table();

        std::vector<uint8_t> result;
        result.reserve(sizeof(HeaderVELB) +
                       final_sections.size() * sizeof(section_range_memory) +
                       final_executable.size());

        // Header
        result.insert(result.end(),
                      reinterpret_cast<const uint8_t*>(&final_header),
                      reinterpret_cast<const uint8_t*>(&final_header) + sizeof(final_header));

        // Tabla de secciones
        for (const auto& sec : final_sections) {
            result.insert(result.end(),
                          reinterpret_cast<const uint8_t*>(&sec),
                          reinterpret_cast<const uint8_t*>(&sec) + sizeof(sec));
        }

        // Bytecode final
        result.insert(result.end(), final_executable.begin(), final_executable.end());

        return result;
    }

    void Linker::write_to_file(const std::string& path) {
        auto exe = build_executable();

        std::ofstream f(path, std::ios::binary);
        if (!f) {
            log_error("No se pudo abrir para escribir: " + path);
            return;
        }

        f.write(reinterpret_cast<const char*>(exe.data()), static_cast<std::streamsize>(exe.size()));
        if (!f) {
            log_error("Error escribiendo ejecutable: " + path);
        }

        if (options.generate_map_file && !options.map_file_path.empty()) {
            write_map_file(options.map_file_path);
        }
    }

    void Linker::write_map_file(const std::string& path) {
        std::ofstream f(path);
        if (!f) {
            log_error("No se pudo escribir map file: " + path);
            return;
        }

        f << "VELB MAP FILE\n";
        f << "Executable: " << options.output_path << "\n";

        std::time_t t = std::time(nullptr);
        f << "Generated: " << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S") << "\n";
        f << "Linker Version: 1.0\n\n";

        f << "=== MODULES INCLUDED ===\n";
        for (size_t i = 0; i < modules.size(); ++i) {
            f << "[" << i << "] " << modules[i].name << "\n";
        }
        f << "\n";

        f << "=== GLOBAL SYMBOLS ===\n";
        for (const auto& [name, addr] : global_symbols) {
            f << "0x" << std::hex << addr << "  " << name << "\n";
        }
        f << std::dec << "\n";

        f << "=== SECTIONS ===\n";
        for (const auto& sec : final_sections) {
            f << "[";
            f.write(sec.name.name, 16);
            f << "]\n";
            f << "  Range: 0x" << std::hex << sec.address.address_init
              << " - 0x" << sec.address.address_final << "\n";
            f << "  Size: " << std::dec
              << (sec.address.address_final - sec.address.address_init) << " bytes\n\n";
        }

        f << "=== RELOCATIONS APPLIED ===\n";
        // De momento no guardamos un log detallado de cada relocación aplicada.
        f << "Total: " << report.relocations_applied << "\n\n";

        f << "=== OPTIMIZATIONS ===\n";
        f << "Total: " << report.optimizations_applied << "\n\n";

        f << "=== FINAL SIZE ===\n";
        f << "Executable size: " << final_executable.size() << " bytes\n";
    }

}