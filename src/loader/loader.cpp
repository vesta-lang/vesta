#include "loader/loader.h"

#include "emmit/struct_context.h"

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
    ):
       instance_manager(instance_manager) {
    }

    void parse_velb_header() {

    }

    void load_executable(std::string path) {

    }

    void load_executable(const Assembly::Bytecode::Executable& exe) {

    }

    void Loader::resolve_labels(Assembly::Bytecode::Section &section) {

    }

    void Loader::load_sections(Assembly::Bytecode::Label &label) {

    }

    void Loader::load_spaces(Assembly::Bytecode::Space &space) {

    }

    void Loader::build_runtime_context() {

    }

    void Loader::create_vm_instance() {

    }

}
