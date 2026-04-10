#include "loader/loader.h"

#include "emmit/bytereader.h"
#include "emmit/struct_context.h"
#include "runtime/manager_runtime.h"

/*
 *  Loader
 *  ├── load_executable(path)
 *  │     ├── parse_velb_header()
 *  │     ├── load_spaces()
 *  │     ├── load_sections()
 *  │     ├── resolve_labels()
 *  │     └── build_runtime_context()
 *  └── create_vm_instance()
 */
namespace loader {
    Loader::Loader(
        runtime::ManageVM &instance_manager
    ): instance_manager(instance_manager) {
    }

    std::string Loader::read_string_at(const std::vector<uint8_t> &blob, uint64_t offset) {
        std::string result;

        while (offset < blob.size() && blob[offset] != 0) {
            result.push_back(static_cast<char>(blob[offset]));
            offset++;
        }

        return result;
    }


    std::vector<std::string> Loader::read_all_strings(const std::vector<uint8_t> &blob) {
        std::vector<std::string> result;
        uint64_t i = 0;

        while (i < blob.size()) {
            std::string s;

            while (i < blob.size() && blob[i] != 0) {
                s.push_back(static_cast<char>(blob[i]));
                i++;
            }

            result.push_back(s);

            // saltar el '\0'
            i++;
        }

        return result;
    }

    std::string Loader::read_string_at(ByteReader &reader, uint64_t offset) {
        // Guardamos el offset original
        uint64_t original = reader.offset;

        // Nos movemos al offset solicitado
        reader.seek(offset);

        // Leemos la cadena
        std::string result;
        while (!reader.eof() && reader.input[reader.offset] != 0) {
            result.push_back(static_cast<char>(reader.input[reader.offset]));
            reader.offset++;
        }

        // Restauramos el offset original
        reader.offset = original;

        return result;
    }


    void Loader::parse_velb_header(Executable &exe, ByteReader &reader) {
        if (reader.input.size() < sizeof(HeaderVELB)) {
            throw_error_at(ErrorKind::TruncatedHeader,
                           "El ejecutable es demasiado pequeño para contener un header VELB", reader);
        }

        exe.header.magic.firma = reader.read32();
        if (exe.header.magic.firma != MAGIC_NUMBER_VELB) {
            throw_error_at(ErrorKind::InvalidMagic,
                           "Magic number VELB inválido", reader);
        }

        exe.header.format_v = reader.read32();
        if (VERSION_VELB != exe.header.format_v) {
            throw_error_at(ErrorKind::InvalidVersion,
                           "Vesrion invalida de bytecode, la version actual es " +
                           std::to_string(VERSION_VELB) +
                           " pero la encontrada fue: " + std::to_string(exe.header.format_v),
                           reader
            );
        }

        // La versión de la VM debe estar dentro del rango [min_v, max_v] exigido por el ejecutable.
        exe.header.max_v = reader.read32();
        exe.header.min_v = reader.read32();
        if (exe.header.max_v < VERSION_VM || exe.header.min_v > VERSION_VM) {
            throw_error_at(ErrorKind::InvalidVersion,
                           "Vesrion invalida de maquina virtual, la version actual es " +
                           std::to_string(VERSION_VM) +
                           " pero la encontrada exigida por el codigo es: " + std::to_string(exe.header.min_v) + " - "
                           + std::to_string(exe.header.max_v),
                           reader
            );
        }

        exe.header.checksum = reader.read64();
        exe.header.flags = reader.read64();
        exe.header.timestamp = reader.read64();
        exe.header.arch = reader.read32();

        // cantidad de espacios de direcciones
        exe.header.count = reader.read32();

        // offset a la tabla de secciones
        exe.header.table_offset = reader.read64();

        // cantidad de espacios de direcciones que viene despues del header.
        exe.header.n_spaces = reader.read64();
        if (exe.header.n_spaces == 0) {
            throw_error_at(ErrorKind::NotFoundSpacesAddress,
                           "no se a definido la cantidad de espacios de direcciones.",
                           reader
            );
        }
        exe.header.address_spaces = new table_spaces_address[exe.header.count];

        // offset a la tabla de strings
        exe.header.offset_section_strings = reader.read64();

        // PC por el que empezar la ejecuccion
        exe.init_pc = exe.header.start_pc = reader.read64();

        // el header siempre debe estar alineado a 16 bytes
        while (reader.offset % 16 != 0) {
            (void) reader.read8();
        }
    }

    void Loader::parse_table_spaces(Executable &exe, ByteReader &reader) {
        for (size_t i = 0; i < exe.header.n_spaces; i++) {
            if (exe.header.address_spaces == nullptr) {
                throw_error_at(ErrorKind::InvalidFormat,
                               "No se a podido encontrar los espacios de direcciones por algun motivo desconocido.",
                               reader
                );
            }

            // direccion de inicio del espacio de direcciones
            exe.header.address_spaces[i].address.address_init = reader.read64();

            // direccion final del espacio de direcciones
            exe.header.address_spaces[i].address.address_final = reader.read64();

            // offser a la seccion metada de strings
            exe.header.address_spaces[i].offset_section_strings = reader.read64();

            // offset al bytecode para el espacio de direcciones
            exe.header.address_spaces[i].offset_bytecode = reader.read64();


            Space space{};

            // indicamos el espacio de direcciones
            space.range.address_init = exe.header.address_spaces[i].address.address_init;
            space.range.address_final = exe.header.address_spaces[i].address.address_final;

            // offset en el bytecode
            space.file_offset = exe.header.address_spaces[i].offset_bytecode;

            // obtenemos el nombre del espacio de direcciones:
            space.name_section = read_string_at(reader, exe.header.address_spaces[i].offset_section_strings);

            exe.spaces.push_back(space);
        }

        // la tabla de espacios siempre debe estar alineado a 16 bytes para que se pueda encontrar
        // la tabla de secciones.
        while (reader.offset % 16 != 0) {
            (void) reader.read8();
        }
    }

    Space *Loader::find_space_for_section(Executable &exe, const Section &sec) {
        for (auto &space: exe.spaces) {
            if (sec.memory.address_init >= space.range.address_init &&
                sec.memory.address_final <= space.range.address_final) {
                return &space;
            }
        }
        return nullptr;
    }


    void Loader::parser_table_sections(Executable &exe, ByteReader &reader) {
        // realizamos un punto de control completo para parsear la tabla de secciones.
        ByteReader reader_child = reader.subreader(
            exe.header.count * sizeof(section_range_memory)
        );

        // mover el cursor a la tabla de secciones
        reader_child.seek(exe.header.table_offset);

        for (size_t i = 0; i < exe.header.count; i++) {
            Section sec;
            sec.memory.address_init = reader_child.read64();
            sec.memory.address_final = reader_child.read64();
            uint64_t offset_string = reader_child.read64();
            try {
                // aqui usamos al padre, por que el offset de la tabla de strings no puede accederse
                // con el cursor hijo, ya que el cursor hijo solo puede acceder a la tabla de seeciones
                // mientras que el cursor padre puede acceder a cualquier parte del bytecode
                sec.name = read_string_at(reader, offset_string);
            } catch (const ByteReaderError &e) {
                throw_error_at(
                    ErrorKind::InvalidFormat,
                    std::string("Ha ocurrido un error de lectura al leer la tabla de strings: '") + e.what() + "'",
                    reader_child
                );
            }
            sec.size_real = sec.memory.address_final - sec.memory.address_init;

            // Buscar el espacio al que pertenece
            Space *space = find_space_for_section(exe, sec);

            if (!space) {
                throw_error_at(
                    ErrorKind::InvalidFormat,
                    "La sección '" + sec.name + "' no pertenece a ningún espacio de direcciones",
                    reader_child
                );
            }

            // calcular file offset de la sección
            sec.file_offset = space->file_offset + (sec.memory.address_init - space->range.address_init);

            // Insertar la sección en el espacio correspondiente
            space->add_section(sec);
            exe.sections.push_back(&space->table_section[sec.name]);
        }
    }

    std::unique_ptr<Executable> Loader::parse_velb(std::vector<uint8_t> bytecode) {
        auto exe = std::make_unique<Executable>();
        exe->bytecode = bytecode;

        ByteReader reader(exe->bytecode);

        // parseamos el header
        parse_velb_header(*exe, reader);

        // parseamos la tabla de espacio de direcciones que siempre va despues del header
        parse_table_spaces(*exe, reader);

        // parseamos la tabla de secciones, requiere haber parseado previamente la tabla
        // de espacios de direcciones, ya que se va a añadir a estos.
        parser_table_sections(*exe, reader);

        return exe;
    }

    runtime::VM *Loader::load_executable(std::string path) {
        // Leer archivo completo
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("No se pudo abrir el ejecutable: " + path);
        }

        std::vector<uint8_t> bytecode(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>()
        );

        // Delegar en la versión bytecode
        return load_executable(bytecode);
    }

    runtime::VM *Loader::load_executable(std::vector<uint8_t> bytecode) {
        // Parsear el formato VELB
        auto exe = parse_velb(std::move(bytecode));
        return create_vm_instance(std::move(exe));
    }


    void Loader::resolve_labels(Assembly::Bytecode::Section &section) {
    }

    void Loader::load_sections(Assembly::Bytecode::Label &label) {
    }

    void Loader::load_spaces(Assembly::Bytecode::Space &space) {
    }

    void Loader::build_runtime_context() {
    }

    Executable &Loader::get_last_instance_unlocked() {
        return *executables.back();
    }

    Executable &Loader::get_last_instance() {
        std::lock_guard lock(loader_mutex);
        return get_last_instance_unlocked();
    }

    runtime::VM *Loader::create_vm_instance(std::unique_ptr<Executable> exe_) {
        // bloqueamos el acceso si otro hilo intenta entrar
        std::lock_guard lock(loader_mutex);

        executables.push_back(std::move(exe_));

        // obtener el ultimo ejecutable agregado sin volver a alterar el mutex
        // en el proceso
        Executable &exe = get_last_instance_unlocked();

        // cramos una instancia VM y configuramos el PC
        uint64_t id = instance_manager.create_vm();
        runtime::VM *vm = instance_manager.get_vm(id);

        // configuramos RIP (PC tambien llamado)
        vm->rip.qword(exe.init_pc);

        // copiamos cada seccion del ejecutable a la memoria virtual
        // de la VM
        for (auto *sec: exe.sections) {
            uint64_t vm_addr = sec->memory.address_init;
            uint64_t size = sec->size_real;
            uint64_t offset = sec->file_offset;

            // puntero al bytecode real
            const uint8_t *src = exe.bytecode.data() + offset;

            // copiar a la memoria virtual de la VM
            vm->vm_mem.vm_to_host_memcpy(vm_addr, src, size);

            // mostrar datos de la region de memoria reservacada para la seccion.
            runtime::dump_vm_region(vm, vm_addr, size);
        }
        //vm->vm_mem[0x10] = 1;

        return vm;
    }
}
