/**
 * @file loader.cpp
 * @brief Implementacion del loader de ejecutables VELB para VestaVM.
 *
 * Implementa @c loader::Loader: @c parse_velb_header(), @c parse_table_spaces(),
 * @c parser_table_sections(), @c parser_import_table(), @c parse_velb(),
 * @c load_executable() y @c create_vm_instance().
 * Valida el header, reserva memoria en el @c ArenaManager y crea el @c ProcessVM
 * con el PC inicializado segun @c init_pc del ejecutable.
 */
#include "loader/loader.h"

#include <cstring>
#include "emmit/bytereader.h"
#include "emmit/struct_context.h"
#include "runtime/manager_runtime.h"
#include "ffi/vesta_plugin.h"
#include "cli/sync_io.h"

/*
 *  Loader
 *  ??? load_executable(path)
 *  ?     ??? parse_velb_header()
 *  ?     ??? load_spaces()
 *  ?     ??? load_sections()
 *  ?     ??? resolve_labels()
 *  ?     ??? build_runtime_context()
 *  ??? create_vm_instance()
 */
namespace loader {
    Loader::Loader(
        runtime::ManageVM &instance_manager
    ): instance_manager(instance_manager) {}

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
        uint64_t                 i = 0;

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
                           "El ejecutable es demasiado pequeno para contener un header VELB", reader);
        }

        exe.header.magic.firma = reader.read32();
        if (exe.header.magic.firma != MAGIC_NUMBER_VELB) {
            throw_error_at(ErrorKind::InvalidMagic,
                           "Magic number VELB invalido", reader);
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

        // La version de la VM debe estar dentro del rango [min_v, max_v] exigido por el ejecutable.
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

        exe.header.checksum  = reader.read64();
        exe.header.flags     = reader.read64();
        exe.header.timestamp = reader.read64();
        exe.header.arch      = reader.read32();

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

        // offset a la tabla de importacion
        exe.header.offset_import_table = reader.read64();

        // offset a la tabla de etiquetas o labels
        exe.header.offset_label_table = reader.read64();

        // obtener cuantas entradas tiene la tabla de importacion
        exe.header.size_import_table = reader.read32();

        // obtener cuantas labels tiene la tabla de labels
        exe.header.size_label_table = reader.read32();

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
            space.range.address_init  = exe.header.address_spaces[i].address.address_init;
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
            sec.memory.address_init  = reader_child.read64();
            sec.memory.address_final = reader_child.read64();
            uint64_t offset_string   = reader_child.read64();
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
                    "La seccion '" + sec.name + "' no pertenece a ningun espacio de direcciones",
                    reader_child
                );
            }

            // calcular file offset de la seccion
            sec.file_offset = space->file_offset + (sec.memory.address_init - space->range.address_init);

            // Insertar la seccion en el espacio correspondiente
            space->add_section(sec);
            exe.sections.push_back(&space->table_section[sec.name]);
        }

        // es muy importante saber donde empieza el bytecode real generado por el ensamblador
        // para que el loader pueda parchearlo y cargarlo de forma correcta.
        exe.offset_real_bytecode = (
            exe.header.table_offset +
            exe.sections.size() * sizeof(section_range_memory)
            // debemos calcular el final de la tabla de secciones que es
            // donde empieza el bytecode real dentro del archivo
        );
    }

    void Loader::parser_import_table(Executable &exe, ByteReader &reader) {
        /**
         * Si el offset es 0 entonces no hay tabla de importacion.
         */
        if (exe.header.offset_import_table == 0) {
            return;
        }

        // nos desplazamos hasta la tabla de importacion moviendo el cursor
        // al offset indicado
        reader.seek(exe.header.offset_import_table);

        std::vector<entry_import_table> import_table_from_file;

        // obtener la tabla de importacion del archivo:
        for (size_t i = 0; i < exe.header.size_import_table; i++) {
            entry_import_table entry;
            entry.offset_module_string    = reader.read32();
            entry.offset_function_string  = reader.read32();
            entry.offset_signature_string = reader.read32();
            entry.offset_bytecode         = reader.read32();
            import_table_from_file.push_back(entry);
        }

        for (auto &imp: import_table_from_file) {
            uint32_t module_idx   = ffi_loader.intern_module(read_string_at(reader, imp.offset_module_string));
            uint32_t function_idx = ffi_loader.intern_function(read_string_at(reader, imp.offset_function_string));

            // si el metodo no tiene firma no se usa firma
            std::string sig = "";
            //if (imp.offset_signature_string != 0) { // aun no se usa por que no se a implementado
            //    sig = read_string_at(reader, imp.offset_signature_string);
            //}
            uint32_t sig_idx = ffi_loader.intern_signature(sig);

            ffi::NativeImportKey key{module_idx, function_idx, sig_idx};

            auto &entry = ffi_loader.imports[key]; // crea si no existe

            if (entry.patch_sites.empty()) {
                entry.key.module_idx    = module_idx;
                entry.key.function_idx  = function_idx;
                entry.key.signature_idx = sig_idx;
            }

            entry.patch_sites.push_back(imp.offset_bytecode);
        }

        // resolver todas las instrucciones que usan la tabla de importacion
        // nativa.
        this->ffi_loader.resolve_all(exe.bytecode.data(), exe.offset_real_bytecode);

        // construir la tabla de callbacks de la API del plugin y notificar
        // a los modulos que exporten "vesta_init".
        this->plugin_api = {};
        this->plugin_api.api_version = VESTA_PLUGIN_API_VERSION;
        this->plugin_api.manager     = static_cast<VestaManager *>(&instance_manager);

        this->plugin_api.create_vm  = [](VestaManager *mgr, uint32_t n) -> uint64_t {
            return static_cast<runtime::ManageVM *>(mgr)->create_vm(static_cast<size_t>(n));
        };
        this->plugin_api.destroy_vm = [](VestaManager *mgr, uint64_t vm_id) -> int {
            return static_cast<runtime::ManageVM *>(mgr)->destroy_vm(vm_id) ? 1 : 0;
        };
        this->plugin_api.get_vm     = [](VestaManager *mgr, uint64_t vm_id) -> VestaVM_t * {
            return static_cast<VestaVM_t *>(
                static_cast<runtime::ManageVM *>(mgr)->get_vm(vm_id));
        };
        this->plugin_api.has_vm     = [](VestaManager *mgr, uint64_t vm_id) -> int {
            return static_cast<runtime::ManageVM *>(mgr)->has_vm(vm_id) ? 1 : 0;
        };
        this->plugin_api.vm_count   = [](VestaManager *mgr) -> uint64_t {
            return static_cast<uint64_t>(
                static_cast<runtime::ManageVM *>(mgr)->vm_count());
        };
        this->plugin_api.start_vm   = [](VestaVM_t *vm) {
            static_cast<runtime::VM *>(vm)->start();
        };
        this->plugin_api.stop_vm    = [](VestaVM_t *vm) {
            static_cast<runtime::VM *>(vm)->stop();
        };
        this->plugin_api.wait_vm    = [](VestaVM_t *vm) {
            static_cast<runtime::VM *>(vm)->wait();
        };
        this->plugin_api.spawn_process = [](VestaVM_t *vm, uint32_t *out_sched, uint64_t *out_pid) {
            GlobalPID gid = static_cast<runtime::VM *>(vm)->spawn_process();
            if (out_sched) *out_sched = gid.scheduler_id;
            if (out_pid)   *out_pid   = gid.local_pid;
        };
        this->plugin_api.make_ready = [](VestaVM_t *vm, uint32_t sched_id, uint64_t local_pid) {
            GlobalPID pid{sched_id, local_pid};
            static_cast<runtime::VM *>(vm)->make_ready(pid);
        };
        this->plugin_api.vm_read_bytes = [](uint64_t proc_ptr, uint64_t vm_addr,
                                      void *dst, uint64_t len) -> uint64_t {
            auto *proc = reinterpret_cast<runtime::ProcessVM *>(proc_ptr);
            proc->vm_mem.read_bytes(vm_addr, dst, static_cast<size_t>(len));
            return len;
        };
        this->plugin_api.vm_write_bytes = [](uint64_t proc_ptr, uint64_t vm_addr,
                                       const void *src, uint64_t len) -> uint64_t {
            auto *proc = reinterpret_cast<runtime::ProcessVM *>(proc_ptr);
            proc->vm_mem.write_bytes(vm_addr, src, static_cast<size_t>(len));
            return len;
        };
        this->plugin_api.log = [](const char *msg) {
            vesta::print_threadsafe(std::string(msg));
        };

        this->ffi_loader.call_plugin_inits(&this->plugin_api);
    }

    std::unique_ptr<Executable> Loader::parse_velb(std::vector<uint8_t> bytecode) {
        auto exe      = std::make_unique<Executable>();
        exe->bytecode = bytecode;

        ByteReader reader(exe->bytecode);

        // parseamos el header
        parse_velb_header(*exe, reader);

        // parseamos la tabla de espacio de direcciones que siempre va despues del header
        parse_table_spaces(*exe, reader);

        // parseamos la tabla de secciones, requiere haber parseado previamente la tabla
        // de espacios de direcciones, ya que se va a anadir a estos.
        parser_table_sections(*exe, reader);

        // parseamos la tabla de importacion.
        parser_import_table(*exe, reader);

        return exe;
    }

    runtime::ProcessVM *Loader::load_executable(runtime::VM &vm, std::string path) {
        // Leer archivo completo
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("No se pudo abrir el ejecutable: " + path);
        }

        std::vector<uint8_t> bytecode(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>()
        );

        // Delegar en la version bytecode
        return load_executable(vm, bytecode);
    }



    runtime::ProcessVM *Loader::load_executable(runtime::VM &vm, std::vector<uint8_t> raw_bytecode_file) {
        if (raw_bytecode_file.empty()) {
            throw std::runtime_error(
                "Loader::load_executable: Se intento cargar un ejecutable con raw_bytecode_file vacio");
        }
        auto exe = parse_velb(raw_bytecode_file);

        GlobalPID           pid      = vm.spawn_process();
        runtime::ProcessVM *proccess = vm.get_process(pid);

        // configuramos RIP (PC tambien llamado)
        proccess->registers.rip.qword(exe->init_pc);

        // copiamos cada seccion del ejecutable a la memoria virtual
        // de la VM
        for (auto *sec: exe->sections) {
            uint64_t vm_addr = sec->memory.address_init;
            uint64_t size    = sec->size_real;
            uint64_t offset  = sec->file_offset;

            // no se debe usar raw_bytecode_file ya que no contiene simbolos resueltos, se debe
            // usar exe->bytecode que contiene los mismos datos de raw_bytecode_file pero
            // con las instrucciones parcheadas
            const uint8_t *src = exe->bytecode.data() + exe->offset_real_bytecode + offset;

            // copiar a la memoria virtual de la VM
            proccess->vm_mem.vm_to_host_memcpy(vm_addr, src, exe->bytecode.size());

            // mostrar datos de la region de memoria reservada para la seccion.
            //runtime::dump_vm_region(&proccess->tlb, vm_addr, exe->bytecode.size());
        }
        // poner ejecutable a la pila de ejecutuables
        executables.push_back(std::move(exe));

        //vm->vm_mem[0x10] = 1;
        return proccess;
    }

    void Loader::resolve_labels(Assembly::Bytecode::Section &section) {}

    void Loader::load_sections(Assembly::Bytecode::Label &label) {}

    void Loader::load_spaces(Assembly::Bytecode::Space &space) {}

    void Loader::build_runtime_context() {}

    Executable &Loader::get_last_instance_unlocked() {
        return *executables.back();
    }

    Executable &Loader::get_last_instance() {
        std::lock_guard lock(loader_mutex);
        return get_last_instance_unlocked();
    }


    runtime::VM *Loader::create_vm_instance(size_t num_schedulers) {
        // bloqueamos el acceso si otro hilo intenta entrar
        std::lock_guard lock(loader_mutex);

        // cramos una instancia VM y configuramos el PC
        uint64_t     id = instance_manager.create_vm(num_schedulers);
        runtime::VM *vm = instance_manager.get_vm(id);

        return vm;
    }

    /**
     * @brief Instancia una clase generica con tipos concretos (monomorphization en runtime).
     *
     * Construye la clave de cache como "ClassName<T1,T2,...>" concatenando los nombres
     * de los tipos concretos.  Si ya existe en generic_cache_ devuelve el puntero
     * cacheado.  Si no, clona el ClassInfo de @p generic, asigna el nombre especializado,
     * vincula los type_params a los tipos concretos y lo almacena en generic_store_.
     *
     * Esta funcion es thread-safe: usa loader_mutex internamente.
     *
     * @param generic    ClassInfo de la clase generica origen (debe tener CLASS_FLAG_GENERIC).
     * @param type_args  Array de ClassInfo* con los tipos concretos.
     * @param count      Numero de elementos en type_args.
     * @return Puntero al ClassInfo especializado (valido de por vida del Loader).
     */
    ClassInfo *Loader::specialize_class(ClassInfo *generic,
                                        ClassInfo **type_args,
                                        size_t count) {
        if (generic == nullptr || type_args == nullptr || count == 0) return generic;

        // construir la clave de cache: "ClassName<T1,T2,...>"
        std::string key(reinterpret_cast<const char *>(generic->name.data),
                        generic->name.size);
        key += '<';
        for (size_t i = 0; i < count; i++) {
            if (i > 0) key += ',';
            if (type_args[i] != nullptr) {
                key.append(reinterpret_cast<const char *>(type_args[i]->name.data),
                           type_args[i]->name.size);
            } else {
                key += "?"; // tipo desconocido: clave degenerada
            }
        }
        key += '>';

        std::lock_guard lock(loader_mutex);

        // buscar en cache antes de clonar
        auto it = generic_cache_.find(key);
        if (it != generic_cache_.end()) return it->second;

        // clonar el ClassInfo base y adaptar los campos necesarios
        auto clone = std::make_unique<ClassInfo>(*generic);

        // almacenar el nombre especializado como cadena estatica en el mismo bloque
        // NOTA: el nombre se guarda en generic_store_names_ para que viva junto al ClassInfo
        auto *name_buf = new char[key.size() + 1];
        std::memcpy(name_buf, key.c_str(), key.size() + 1);
        clone->name.data = reinterpret_cast<uint8_t *>(name_buf);
        clone->name.size = static_cast<uint32_t>(key.size());

        // vincular el origen (para distinguir especializaciones del original)
        clone->generic_parent = generic;

        // eliminar la marca GENERIC en la especializacion concreta
        clone->flags &= ~CLASS_FLAG_GENERIC;

        // sustituir los type_params por los tipos concretos proporcionados
        // NOTA: la resolucion de campos/metodos internos que usan T queda a cargo
        // del compilador de alto nivel o de una pasada de reflexion posterior.
        // Aqui solo vinculamos el descriptor para permitir instanceof/specialize.
        if (count <= generic->type_param_count && generic->type_params != nullptr) {
            auto *new_params = new GenericParam[generic->type_param_count];
            std::memcpy(new_params, generic->type_params,
                        generic->type_param_count * sizeof(GenericParam));
            for (size_t i = 0; i < count; i++) {
                new_params[i].constraint = type_args[i]; // vincular tipo concreto
            }
            clone->type_params = new_params;
        }

        ClassInfo *raw = clone.get();
        generic_store_.push_back(std::move(clone)); // tomar propiedad
        generic_cache_.emplace(std::move(key), raw);
        return raw;
    }

}
