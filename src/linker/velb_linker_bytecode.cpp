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

#include "emmit/parser_to_bytecode.h"

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

    /**
     * - calcular el offset base donde se añadirá la nueva sección
     * - desplazar labels
     * - desplazar relocaciones
     * - concatenar bytecode
     * @param dest
     * @param src
     */
    /*void Linker::merge_section(Section &dest, const Section &src) {
        uint64_t base = dest.size_real;

        // Copiar labels con offset ajustado
        for (const auto &[labelName, lbl]: src.table_label) {
            Label newLbl = lbl;
            newLbl.address += base;
            dest.table_label[labelName] = newLbl;
        }

        // Copiar bytecode
        dest.bytecode.insert(dest.bytecode.end(),
                             src.bytecode.begin(),
                             src.bytecode.end());

        // Actualizar tamaño
        dest.size_real += src.size_real;
    }*/

    /**
     * - concatenar secciones
     * - recalcular offsets
     * - actualizar labels
     * - desplazar bytecode
     * - ajustar relocaciones
     * @param dest
     * @param src
     */
    void Linker::merge_space_address(Space &dest, const Space &src) {
        for (const auto &[secName, sec]: src.table_section) {
            auto it = dest.table_section.find(secName);

            if (it == dest.table_section.end()) {
                // No existe, copiar sección completa
                dest.table_section[secName] = sec;
            } else {
                // Ya existe, fusionar secciones
                //merge_section(it->second, sec);
            }
        }
    }

    void Linker::add_assembly_unit(const std::vector<uint8_t> &bytecode,
                                   const Context *ctx) {
        /**
         * Problemas resueltos:
         *      - 1 espacio de direcciones solapados, si se solapan se emite un error.
         *      - 2 La posibilidad de que dos archivos (modulos) contengan el mismo espacio de direcciones
         *          con un mismo nombre, si ese caso se da, fusionamos los espacios de direcciones.
         *      - 3 Este problema es causado por la solucion del segundo problema, al fusionar espacios de
         *          direcciones, estamos espandiendo el tamaño del espacio en uno de los modulos, donde
         *          otras secciones de este podria esperar existir en ese rango de direcciones, lo que causara
         *          un solapamiento de los espacios de direcciones.
         *          La solucion a este problema puede ser reubicar el siguiente espacio de direcciones, pero
         *          debo emitir un warning de esto. Mientras todos los modulos usen los mismos espacios de
         *          direcciones o no use espacios de direcciones muy solapados entre ellos, no deberia haber
         *          problema.
         */
        Module m;
        m.name = "<assembly-unit>";
        m.bytecode = bytecode;
        m.ctx = *ctx; // copia del contexto
        m.is_object = false;

        // Fusionar espacios del módulo en el linker
        for (const auto &[spaceName, space]: ctx->space_address) {
            // Comprobar si se solapa con otros espacios ya existentes
            for (const auto &[existingName, existingSpace]: spaces_address) {
                /**
                 * Si el espacio que estamos revisando (existingName)
                 * tiene el mismo nombre que el espacio que estamos añadiendo (spaceName),
                 * entonces NO debemos comprobar solapamiento entre ellos.
                 * Porque:
                 *   - Si tienen el mismo nombre, no son espacios distintos.
                 *   - Son dos partes del mismo espacio lógico (por ejemplo, ambos son "code").
                 *   - Esos espacios no deben compararse entre sí, sino fusionarse.
                 */
                if (existingName == spaceName)
                    continue; // este se fusiona, no se compara

                if (ranges_overlap(&existingSpace.range, &space.range)) {
                    throw std::runtime_error(
                        "Error: el espacio '" + spaceName +
                        "' del modulo se solapa con el espacio '" +
                        existingName + "' ya existente."
                    );
                }
            }

            // Existe ya este espacio en el linker?
            auto it = spaces_address.find(spaceName);

            if (it == spaces_address.end()) {
                // No existe -> copiarlo tal cual
                spaces_address[spaceName] = space;
            } else {
                // Ya existe -> fusionar
                merge_space_address(it->second, space);
            }
        }

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


        // Concatenar bytecode de todos los módulos
        for (auto &mod: modules) {
            final_executable.insert(final_executable.end(),
                                    mod.bytecode.begin(),
                                    mod.bytecode.end());
        }

        // Construir final_sections a partir de los Context de cada módulo
        for (auto &mod: modules) {
            // para cada espacio de direcciones del modulo
            for (const auto &[spaceName, space]: mod.ctx.space_address) {
                // cada cada seccion dentro del espacio de direcciones del modulo
                for (const auto &[sectionName, section]: space.table_section) {
                    section_info_linker sec{};
                    sec.memory.address = section.memory; // aqui se esta almacenando la direccion virtual de inicio y final
                    //sec.memory.address.address_final = section.size_real; // debemos cambiar la direccion final para usar
                    // la direccion real dentro del archivo, y no usar la direccion final virtual.
                    // la seccion puede ocupar 49 bytes realmente en el archivo, pero al cargarse demandar 1kB de
                    // memoria.
                    sec.name = section.name;
                    // tal vez deba calcular el offset al nombre de la seccion aqui tambien?

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

        // cantidad de espacios de direcciones
        final_header.n_spaces = spaces_address.size();

        // Extraer espacios a un vector temporal
        std::vector<std::pair<std::string, Space> > ordered_spaces;
        ordered_spaces.reserve(spaces_address.size());

        for (auto &kv: spaces_address) {
            ordered_spaces.push_back(kv);
        }

        // Ordenar por address_init
        std::sort(ordered_spaces.begin(), ordered_spaces.end(),
                  [](auto &a, auto &b) {
                      return a.second.range.address_init < b.second.range.address_init;
                  });

        // Copiar al header en orden de direcciones
        final_header.address_spaces = new table_spaces_address[final_header.n_spaces];

        // La tabla de secciones irá justo después del header, es necesario
        // haber asignado primero final_header.n_spaces, o esta funcion fallara,
        // ya que para calcular el offset de la tabla, es necesario conocercuantas entradas
        // de espacio de direcciones hay
        final_header.table_offset = compute_sections_base_offset();

        // donde esta la seccion de strings
        final_header.offset_section_strings = final_header.table_offset;

        build_section_strings(final_header.table_offset);

        // los strings va antes que la tabla de secciones
        final_header.table_offset += spaces_address["MetaSpace"].table_section["strings"].size_real;

        for (int i = 0; i < final_header.n_spaces; ++i) {
            table_spaces_address *const space = &final_header.address_spaces[i];

            // al macenar las direcciones
            space->address = ordered_spaces[i].second.range;

            // por ahora los offset a los nombres de la seccion de strings no se calculara aqui.
            space->offset_section_strings = string_offsets[
                ordered_spaces[i].second.name_section
            ];
        }

        // añadir a cada seccion el offset al nombre de su stirng
        for (auto & final_section : final_sections) {
            final_section.memory.offset_string = string_offsets[final_section.name];
        }

    }

    void Linker::build_section_strings(uint64_t offset_init) {
        string_pool.clear();
        string_offsets.clear();
        string_blob.clear();

        // Recoger strings de espacios de direcciones
        for (const auto &[name, space]: spaces_address) {
            string_pool.push_back(name);
        }

        // Recoger strings de secciones
        for (const auto &[spaceName, space]: spaces_address) {
            for (const auto &[secName, sec]: space.table_section) {
                string_pool.push_back(secName);
            }
        }

        // Recoger strings de labels
        for (const auto &[spaceName, space]: spaces_address) {
            for (const auto &[secName, sec]: space.table_section) {
                for (const auto &[labName, lab]: sec.table_label) {
                    string_pool.push_back(labName);
                }
            }
        }

        // Eliminar duplicados
        std::sort(string_pool.begin(), string_pool.end());
        string_pool.erase(std::unique(string_pool.begin(), string_pool.end()),
                          string_pool.end());

        // Construir blob y mapa de offsets
        uint64_t offset = offset_init; // depende del offset de inicio, todo_ se calcula en base a este

        for (const auto &s: string_pool) {
            string_offsets[s] = offset;

            // copiar caracteres
            for (char c: s)
                string_blob.push_back(static_cast<uint8_t>(c));

            // terminador nulo
            string_blob.push_back(0);

            offset += s.size() + 1;
        }

        // Crear sección especial "strings"
        //sec.bytecode = string_blob;
        Section *sec = &spaces_address["MetaSpace"].table_section["strings"];
        // Insertar en el espacio "meta"
        sec->size_align_section = 1;
        sec->size_real = align_up(
            string_blob.size(), 16);
    }

    uint64_t Linker::compute_sections_base_offset() const {
        // el espacio real de todo_ el header es la cantidad de espacios de direcciones
        // po el tamaño de una entrada de rango de memoria (16 bytes para inicio y fin)
        const uint64_t size_table_space_address = (final_header.n_spaces * sizeof(table_spaces_address));

        // se resta el tamaño del puntero de table_spaces_address, ya que en el archivo,
        // la tabla va espacio de direcciones va direcamente incrustado, en lugar de
        // ser un puntero.
        const uint32_t relative_size_header = sizeof(HeaderVELB) - sizeof(table_spaces_address *);

        const uint64_t size_table_sections = final_sections.size() * sizeof(section_range_memory);

        // el hader siempre esta alineado a 16 bytes
        return align_up((relative_size_header +
                         size_table_space_address) /*+ size_table_sections*/, 16);
    }

    void Linker::compute_symbol_file_offsets() {
        for (auto &[name, info]: symbol_info) {
            for (const auto &sec: final_sections) {
                if (info.absolute >= sec.memory.address.address_init &&
                    info.absolute < sec.memory.address.address_final) {
                    uint64_t delta = info.absolute - sec.memory.address.address_init;
                    info.file_offset = sec.memory.offset_string + delta;
                    break;
                }
            }
        }
    }

    /**
     * Obtener el string de un offset en especifico
     * @param offset offset del string a buscar
     * @return string si existe en ese offset
     */
    std::string Linker::get_string_from_offset(uint64_t offset) const {
        if (offset >= string_blob.size())
            return "<invalid-offset>";

        const char *start = reinterpret_cast<const char *>(&string_blob[offset]);

        // Buscar el terminador nulo sin salirnos del buffer
        size_t max_len = string_blob.size() - offset;
        size_t len = strnlen(start, max_len);

        return std::string(start, len);
    }

    void Linker::build_section_table() {
        // Validar que hay secciones
        if (final_sections.empty()) {
            throw std::runtime_error("No hay secciones para construir la tabla.");
        }

        // Ordenar por dirección virtual
        std::sort(final_sections.begin(), final_sections.end(),
                  [](const section_info_linker &a,
                     const section_info_linker &b) {
                      return a.memory.address.address_init < b.memory.address.address_init;
                  });

        // debemos restar el tamaño de la seccion de cadenas.
        compute_symbol_file_offsets();

        // Validar que no se solapan, primero debemos a ver calculado los offset de cada string anteriormente
        for (size_t i = 1; i < final_sections.size(); ++i) {
            std::string n1 = final_sections[i - 1].name;
            std::string n2 = final_sections[i].name;
            if (final_sections[i].memory.address.address_init <
                final_sections[i - 1].memory.address.address_final) {
                // si es el espacio de direcciones especiales, entonces, lo ignoramos aunque
                // se solape siempre y cuando sus direcciones tengas tamaño 0 (no se usa dentro
                // de la VM) por el codigo
                if ((
                        (n2 == "strings") &&
                        (
                            final_sections[i].memory.address.address_final -
                            final_sections[i].memory.address.address_init) == 0
                    ) || (n1 == "strings")
                        // si lo agrego el ensamblador, entonces el espacio deberia ser 0,
                        // si lo añadio el usuario, y no asigno
                        // estos espacios, se debera analizar
                ) continue;

                throw std::runtime_error(
                    "Secciones solapadas en memoria virtual. seccion: " + n1 + " y seccion: " + n2);
            }
        }


        //    NO asignamos offsets aquí (eso lo hace build_executable)
        //    porque depende del tamaño del header y de la tabla misma.
    }


    std::vector<uint8_t> Linker::build_executable() {
        result->output.reserve(4096); // evita realocaciones
        resolve_symbols();
        apply_relocations();
        optimize_modules();
        merge_address_spaces();
        merge_sections();

        build_header();
        build_section_table();

        // escribir tabla de espacio de direcciones
        auto write_space_address = [&](HeaderVELB &v) {
            for (int i = 0; i < final_header.n_spaces; ++i) {
                result->emit64(final_header.address_spaces[i].address.address_init);
                result->emit64(final_header.address_spaces[i].address.address_final);

                // offset a la tabla de strings
                result->emit64(final_header.address_spaces[i].offset_section_strings);
                result->emit64(0);
            }
        };

        result->emit32(final_header.magic.firma);
        result->emit32(final_header.format_v);

        result->emit32(final_header.max_v);
        result->emit32(final_header.min_v);

        result->emit64(final_header.checksum);

        result->emit64(final_header.flags);
        result->emit64(final_header.timestamp);
        result->emit32(final_header.arch);

        result->emit32(final_header.count);

        result->emit64(final_header.table_offset);

        result->emit64(final_header.n_spaces);

        // indicar offset la seccion de cadenas espaciales.
        result->emit64(final_header.offset_section_strings);

        // Indicar el valor de PC al cargar el programa
        result->emit64(final_header.start_pc);

        // el header siempre debe estar alineado a 16 bytes
        while (result->offset % 16 != 0) {
            result->emit8(0x00);
        }

        // escribir tabla de espacios
        write_space_address(final_header);

        for (uint64_t i = 0; i < string_blob.size(); ++i) {
            result->emit8(string_blob[i]);
        }

        // la tabla de strings alineado a 16 bytes
        while (result->offset % 16 != 0) {
            result->emit8(0x00);
        }

        // escribir tabla se secciones, supondre que el offset de la tabla este
        // bien calculada y se este apuntando a este sitio.
        for (auto &sec: final_sections) {
            result->emit64(sec.memory.address.address_init);
            result->emit64(sec.memory.address.address_final);
            result->emit64(sec.memory.offset_string);
        }

        // añadir el bytecode al final
        result->output.insert(result->output.end(), final_executable.begin(), final_executable.end());

        return result->output;
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

        if (result->output.size() >= sizeof(HeaderVELB)) {
            HeaderVELB hdr{};
            std::memcpy(&hdr, result->output.data(), sizeof(HeaderVELB));

            f << "=== VELB HEADER ===\n";
            f << "Magic: "
                    << (char) hdr.magic.firma_byte[0]
                    << (char) hdr.magic.firma_byte[1]
                    << (char) hdr.magic.firma_byte[2]
                    << (char) hdr.magic.firma_byte[3] << "\n";

            f << "Format version: " << hdr.format_v << "\n";
            f << "VM version: min=" << hdr.min_v << " max=" << hdr.max_v << "\n";
            f << "Checksum: 0x" << std::hex << hdr.checksum << std::dec << "\n";
            f << "Flags: 0x" << std::hex << hdr.flags << std::dec << "\n";
            f << "Timestamp: " << hdr.timestamp << "\n";
            f << "Target arch: " << hdr.arch << "\n";
            f << "Section count: " << hdr.count << "\n";
            f << "Section table offset: 0x" << std::hex << hdr.table_offset << std::dec << "\n";
            f << "Spaces count: " << hdr.n_spaces << "\n";
            f << "Strings table offset: 0x" << std::hex << hdr.offset_section_strings << std::dec << "\n";
            f << "Start PC: 0x" << std::hex << hdr.start_pc << std::dec << "\n\n";
        }


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

        if (final_executable.size() >= sizeof(HeaderVELB) +
            sizeof(table_spaces_address) * final_header.n_spaces) {
            auto *spaces = reinterpret_cast<const table_spaces_address *>(
                final_executable.data() + sizeof(HeaderVELB)
            );

            f << "=== ADDRESS SPACES ===\n";
            for (uint64_t i = 0; i < final_header.n_spaces; ++i) {
                const auto &sp = spaces[i];
                f << "[" << i << "]\n";
                f << "  VA Range: 0x" << std::hex << sp.address.address_init
                        << " - 0x" << sp.address.address_final << std::dec << "\n";
                f << "  String offset: 0x" << std::hex << sp.offset_section_strings
                        << " (" << std::dec << sp.offset_section_strings << ")\n\n";
            }
        }

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
            const size_t size = compute_sections_base_offset();

            f << "[" << get_string_from_offset(sec.memory.offset_string - size) << "]\n";

            f << "  VA Range: 0x" << std::hex << sec.memory.address.address_init
                    << " - 0x" << sec.memory.address.address_final << "\n";

            f << "  FILE Off String: 0x" << sec.memory.offset_string << "\n";

            f << "  Size: " << std::dec
                    << (sec.memory.address.address_final - sec.memory.address.address_init)
                    << " bytes\n";

            f << "  OffsetString table: " << std::hex << sec.memory.offset_string << " "
                    << std::dec << sec.memory.offset_string
                    << "\n\n";
        }

        f << "=== FINAL SIZE ===\n";
        f << "Executable size: " << final_executable.size() << " bytes\n";
    }
}
