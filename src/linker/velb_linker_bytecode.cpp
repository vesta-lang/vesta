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
    static uint64_t simple_checksum(const std::vector<uint8_t> &data) {
        uint64_t sum = 0;
        for (auto b: data) sum += b;
        return sum;
    }

    void Linker::log_warning(const std::string &msg) {
        report.warnings.push_back(msg);
        if (options.verbose)
            std::cerr << "[Linker][WARN] " << msg << "\n";
    }

    void Linker::log_error(const std::string &msg) {
        report.errors.push_back(msg);
        std::cerr << "[Linker][ERROR] " << msg << "\n";
    }

    void Linker::add_object_file(const std::string &path) {
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

    void Linker::add_object_memory(const std::vector<uint8_t> &data) {
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

    void Linker::add_static_library(const std::string &path) {
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

    void Linker::add_assembly_unit(const std::vector<uint8_t> &bytecode,
                                   const Context *ctx) {
        Module m;
        m.name = "<assembly-unit>";
        m.bytecode = bytecode;

        m.ctx = *ctx; // copia del contexto

        m.is_object = false;

        // NO extraemos símbolos aquí
        // NO aplanamos nada
        // NO destruimos la jerarquía

        modules.push_back(std::move(m));
        report.modules_linked++;
    }


    void Linker::resolve_symbols() {
        global_symbols.clear();
        symbol_info.clear();

        // iteramos sobre cada modulo
        for (auto &mod: modules) {
            // Recorremos espacios -> secciones
            for (const auto &[spaceName, space]: mod.ctx.space_address) {
                for (const auto &[sectionName, section]: space.table_section) {
                    uint64_t base = section.memory.address_init;

                    // Extraer labels de la sección en un vector ordenado
                    std::vector<std::pair<std::string, const Label *> > ordered;

                    for (const auto &[labelName, label]: section.table_label) {
                        ordered.push_back({labelName, &label});
                    }

                    // ordenar las label por las direcciones.
                    std::sort(ordered.begin(), ordered.end(),
                              [](auto &a, auto &b) {
                                  return a.second->address < b.second->address;
                              });

                    // Calcular tamaño de cada label
                    for (size_t i = 0; i < ordered.size(); ++i) {
                        const auto &[labelName, label] = ordered[i];

                        uint64_t start = label->address;
                        uint64_t end;

                        uint64_t section_real_size = section.size_real;
                        if (i + 1 < ordered.size()) {
                            // siguiente label
                            end = ordered[i + 1].second->address;
                        } else {
                            end = section_real_size;
                        }

                        uint64_t size = end - start;

                        // Construir nombre completo
                        std::string fullName = sectionName + "." + labelName;

                        // Dirección absoluta
                        uint64_t absolute = base + start;

                        //
                        // Duplicados
                        if (global_symbols.count(fullName)) {
                            log_error("Símbolo duplicado: " + fullName +
                                      " en módulo " + mod.name);
                            continue;
                        }

                        // Registrar símbolo global
                        global_symbols[fullName] = absolute;

                        // Registrar información extendida
                        SymbolInfo info;
                        info.space = spaceName;
                        info.section = sectionName;
                        info.relative = start;
                        info.absolute = absolute;
                        info.file_offset = 0; // se rellenará después
                        info.size = size;
                        info.module = mod.name;

                        symbol_info[fullName] = info;

                        report.symbols_resolved++;
                    }
                }
            }
        }

        // TODO: resolver símbolos indefinidos
    }


    void Linker::apply_relocations() {
        // Recorrer todos los módulos cargados
        // Cada módulo tiene:
        // su propio bytecode
        // su propia tabla de relocaciones
        // su propio nombre (para logs)
        for (auto &mod: modules) {
            // Recorre todas las relocaciones del módulo.
            // Cada rel contiene:
            // rel.symbol -> nombre del símbolo a resolver
            // rel.offset -> posición en el bytecode donde escribir la dirección
            // rel.type -> tipo de relocación (ABS64, REL32, REL64)
            for (const auto &rel: mod.relocations) {
                // Busca el símbolo en la tabla global
                auto it = global_symbols.find(rel.symbol);

                // Si no existe, error de símbolo no resuelto.
                // equivalente a: ld: undefined reference to `foo'
                if (it == global_symbols.end()) {
                    log_error("Relocacion: simbolo no resuelto: " + rel.symbol);
                    continue;
                }

                // Obtiene la dirección final del símbolo
                // Esta dirección ya fue calculada previamente por el linker al:
                //     concatenar módulos
                //     aplicar alineaciones
                //     asignar direcciones virtuales
                //     resolver secciones
                uint64_t target_addr = it->second;

                if (rel.offset + sizeof(uint64_t) > mod.bytecode.size()) {
                    log_error("Relocacion fuera de rango en modulo " + mod.name);
                    continue;
                }

                switch (rel.type) {
                    // Escribe la dirección absoluta de 64 bits.
                    // mov rax, [foo]   ->   se escribe la dirección absoluta de foo
                    case Relocation::Type::Absolute64: {
                        std::memcpy(&mod.bytecode[rel.offset], &target_addr, sizeof(uint64_t));
                        break;
                    }

                    // Esto es un PC-relative displacement, típico de:
                    // call foo; jmp bar
                    // El +4 es porque la instrucción ya habrá avanzado 4 bytes cuando se evalúa el PC.
                    // int32_t rel32 = target_addr - (rel.offset + 4);
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

                    // Lo mismo que REL32, pero con 64 bits.
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

        for (auto &mod: modules) {
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
        final_executable.clear();
        final_sections.clear();


        // Concatenar bytecode de todos los módulos (igual que antes)
        for (auto &mod: modules) {
            final_executable.insert(final_executable.end(),
                                    mod.bytecode.begin(),
                                    mod.bytecode.end());
        }

        // Construir final_sections a partir de los Context de cada módulo
        for (auto &mod: modules) {
            for (const auto &[spaceName, space]: mod.ctx.space_address) {
                for (const auto &[sectionName, section]: space.table_section) {
                    section_range_memory sec{};
                    sec.address = section.memory;

                    std::memset(sec.name.name, 0, sizeof(sec.name.name));
                    std::memcpy(sec.name.name,
                                section.name.c_str(),
                                std::min(section.name.size(), sizeof(sec.name.name)));

                    // De momento NO sabemos aún el offset real en el archivo.
                    // Lo rellenamos luego en build_executable().
                    sec.offset = 0;

                    final_sections.push_back(sec);
                }
            }
        }
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

    uint64_t Linker::compute_sections_base_offset() const {
        return sizeof(HeaderVELB) +
               final_sections.size() * sizeof(section_range_memory);
    }

    uint64_t Linker::assign_section_offsets(uint64_t start_offset) {
        uint64_t current_offset = start_offset;

        for (auto &sec: final_sections) {
            uint64_t size = sec.address.address_final - sec.address.address_init;
            sec.offset = current_offset;
            current_offset += size;
        }

        return current_offset;
    }

    void Linker::compute_symbol_file_offsets() {
        for (auto &[name, info]: symbol_info) {
            for (const auto &sec: final_sections) {
                if (info.absolute >= sec.address.address_init &&
                    info.absolute < sec.address.address_final) {
                    uint64_t delta = info.absolute - sec.address.address_init;
                    info.file_offset = sec.offset + delta;
                    break;
                }
            }
        }
    }

    void Linker::build_section_table() {
        // Validar que hay secciones
        if (final_sections.empty()) {
            throw std::runtime_error("No hay secciones para construir la tabla.");
        }

        // Ordenar por dirección virtual
        std::sort(final_sections.begin(), final_sections.end(),
                  [](const section_range_memory &a,
                     const section_range_memory &b) {
                      return a.address.address_init < b.address.address_init;
                  });

        // Validar que no se solapan
        for (size_t i = 1; i < final_sections.size(); ++i) {
            if (final_sections[i].address.address_init <
                final_sections[i - 1].address.address_final) {
                throw std::runtime_error("Secciones solapadas en memoria virtual.");
            }
        }

        uint64_t current_offset = compute_sections_base_offset();
        assign_section_offsets(current_offset);
        compute_symbol_file_offsets();
        //    NO asignamos offsets aquí (eso lo hace build_executable)
        //    porque depende del tamaño del header y de la tabla misma.
    }


    std::vector<uint8_t> Linker::build_executable() {
        resolve_symbols();
        apply_relocations();
        optimize_modules();
        merge_address_spaces();
        merge_sections();
        build_header();
        build_section_table();

        // construir el binario final
        std::vector<uint8_t> result;

        // Header
        result.insert(result.end(),
                      reinterpret_cast<const uint8_t *>(&final_header),
                      reinterpret_cast<const uint8_t *>(&final_header) + sizeof(final_header));

        // Tabla de secciones
        for (const auto &sec: final_sections) {
            result.insert(result.end(),
                          reinterpret_cast<const uint8_t *>(&sec),
                          reinterpret_cast<const uint8_t *>(&sec) + sizeof(sec));
        }

        // Bytecode final
        result.insert(result.end(), final_executable.begin(), final_executable.end());

        return result;
    }

    void Linker::write_to_file(const std::string &path) {
        auto exe = build_executable();

        std::ofstream f(path, std::ios::binary);
        if (!f) {
            log_error("No se pudo abrir para escribir: " + path);
            return;
        }

        f.write(reinterpret_cast<const char *>(exe.data()), static_cast<std::streamsize>(exe.size()));
        if (!f) {
            log_error("Error escribiendo ejecutable: " + path);
        }

        if (options.generate_map_file && !options.map_file_path.empty()) {
            write_map_file(options.map_file_path);
        }
    }

    void Linker::write_map_file(const std::string &path) {
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
        for (size_t i = 0; i < modules.size(); ++i)
            f << "[" << i << "] " << modules[i].name << "\n";
        f << "\n";

        // Ordenar símbolos por VA
        std::vector<std::pair<std::string, SymbolInfo> > sorted;
        for (auto &p: symbol_info)
            sorted.push_back(p);

        std::sort(sorted.begin(), sorted.end(),
                  [](auto &a, auto &b) {
                      return a.second.absolute < b.second.absolute;
                  });

        f << "=== SYMBOLS BY SECTION ===\n";

        std::string current_section;

        for (auto &[name, info]: sorted) {
            // Cambio de sección -> imprimir encabezado
            if (info.section != current_section) {
                current_section = info.section;
                f << "\n[SECTION " << current_section << "]\n";
            }

            f << "  " << name << "\n";
            f << "    VA:   0x" << std::hex << info.absolute << "\n";
            f << "    FILE: 0x" << info.file_offset << "\n";
            f << "    REL:  0x" << info.relative << "\n";
            f << "    SIZE: " << std::dec << info.size << " bytes\n";

            // Mostrar código hexadecimal
            f << "    HEX:  ";

            if (info.file_offset + info.size <= final_executable.size()) {
                for (uint64_t i = 0; i < info.size; ++i) {
                    uint8_t b = final_executable[info.file_offset + i];
                    f << std::hex << std::setw(2) << std::setfill('0')
                            << (int) b << " ";
                }
            } else {
                f << "(fuera de rango)";
            }

            f << "\n";
        }

        f << "\n=== SECTIONS ===\n";
        for (const auto &sec: final_sections) {
            f << "[" << std::string(sec.name.name,
                                    strnlen(sec.name.name, 16)) << "]\n";

            f << "  VA Range: 0x" << std::hex << sec.address.address_init
                    << " - 0x" << sec.address.address_final << "\n";

            f << "  FILE Off: 0x" << sec.offset << "\n";

            f << "  Size: " << std::dec
                    << (sec.address.address_final - sec.address.address_init)
                    << " bytes\n\n";
        }

        f << "=== FINAL SIZE ===\n";
        f << "Executable size: " << final_executable.size() << " bytes\n";
    }
}
