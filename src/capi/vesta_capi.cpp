/**
 * @file vesta_capi.cpp
 * @brief Implementacion de la API C estable (C-ABI) de VestaVM.
 *
 * Expone @c vesta_compile / @c vesta_run / @c vesta_eval / @c vesta_version /
 * @c vesta_free para embeber el compilador + la VM desde cualquier lenguaje
 * via la libreria compartida @c libvesta.
 *
 * Estrategia de reuso (sin reescribir el pipeline):
 *   - Compilar .vex -> .velb: se replica la secuencia que usa el path
 *     @c --vex de @c main.cpp: @c vex::compile_vex_source produce el texto
 *     @c .vel + el IR serializado; se escribe el @c .vel a un fichero
 *     temporal y se invoca @c asm_multi_process::run_worker (que ensambla
 *     + linka) para producir el @c .velb final; se leen sus bytes a memoria
 *     y se borran los temporales.
 *   - Ejecutar .velb: se replica el path @c --run: se construye una
 *     @c runtime::ManageVM, se crea una VM, se carga el bytecode desde
 *     memoria (@c Loader::load_executable con bytes), se hace @c make_ready,
 *     se arranca el scheduler y se espera con la condition variable
 *     @c done_cv.  El valor de retorno de @c main es el registro R0 del
 *     proceso principal.
 *
 * Toda excepcion C++ se captura en la frontera C; nunca se propaga al
 * llamante (que podria no tener manejo de excepciones C++).
 */

#include "capi/vesta.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "loader/loader.h"
#include "runtime/manager_runtime.h"
#include "runtime/proceso_runtime.h"
#include "runtime/runtime.h"
#include "util/assembler_multiprocess.h"
#include "vex/compiler.h"
#include "vex/diagnostic.h"

namespace {

namespace fs = std::filesystem;

/// Duplica una cadena C++ a un buffer en heap compatible con @c vesta_free
/// (asignado con @c std::malloc).  Devuelve NULL si la asignacion falla.
char *dup_cstr(const std::string &s) {
    // +1 para el terminador NUL.
    char *p = static_cast<char *>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return p;
}

/// Rellena @c out_err (si no es NULL) con una copia en heap del mensaje.
void set_err(char **out_err, const std::string &msg) {
    if (out_err) *out_err = dup_cstr(msg);
}

/// Construye un texto legible con todos los diagnosticos de error.
std::string format_diags(const vex::Diagnostics &diags) {
    std::ostringstream os;
    // Recorrer todos los diagnosticos; mostrar errores y warnings con
    // su localizacion en formato gcc-like (fichero:linea:columna).
    for (const auto &d : diags.all()) {
        const char *lvl = "info";
        if (d.level == vex::DiagLevel::ERR) lvl = "error";
        else if (d.level == vex::DiagLevel::WARN) lvl = "warning";
        else if (d.level == vex::DiagLevel::NOTE) lvl = "note";
        os << d.loc.file << ":" << d.loc.line << ":" << d.loc.column << ": "
           << lvl << ": " << d.message << "\n";
    }
    return os.str();
}

/// Genera un prefijo de fichero temporal unico dentro del directorio temporal
/// del sistema (p.ej. "C:/Temp/vesta_capi_ab12cd34").  Sin extension.
std::string make_temp_prefix() {
    // Directorio temporal del SO (TMP/TEMP en Windows, /tmp en POSIX).
    fs::path base;
    try {
        base = fs::temp_directory_path();
    } catch (...) {
        // Fallback al directorio actual si el SO no expone uno.
        base = fs::current_path();
    }
    // Sufijo aleatorio para evitar colisiones entre llamadas concurrentes
    // del mismo proceso o procesos distintos.
    std::random_device rd;
    std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    std::ostringstream name;
    name << "vesta_capi_" << std::hex << r;
    return (base / name.str()).string();
}

/// Lee un fichero binario completo a un vector de bytes.  Devuelve false si
/// no se puede abrir.
bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    out.assign((std::istreambuf_iterator<char>(f)),
               std::istreambuf_iterator<char>());
    return true;
}

/// Borra de forma silenciosa un fichero (no falla si no existe).
void try_remove(const std::string &path) {
    std::error_code ec;
    fs::remove(path, ec);
}

/**
 * @brief Compila fuente Vex a bytes .velb (logica interna compartida).
 *
 * @param src        Codigo fuente.
 * @param unit_name  Nombre logico del modulo.
 * @param out_bytes  [salida] Bytes del .velb.
 * @param err        [salida] Mensaje de error si retorna false.
 * @return true si exito.
 */
bool compile_to_velb_bytes(const std::string &src, const std::string &unit_name,
                           std::vector<uint8_t> &out_bytes, std::string &err) {
    // 1. Compilar .vex -> texto .vel + IR serializado (reusa el frontend).
    vex::CompileOptions copts;
    copts.module_name = unit_name.empty() ? std::string("main") : unit_name;

    vex::CompileResult cr =
        vex::compile_vex_source(src, copts.module_name + ".vex", copts);

    if (!cr.ok || cr.diagnostics.has_errors()) {
        err = "fallo de compilacion Vex:\n" + format_diags(cr.diagnostics);
        return false;
    }

    // 2. Escribir el .vel intermedio a un fichero temporal.  run_worker
    //    opera sobre ficheros y produce <prefijo>.velb.
    const std::string prefix = make_temp_prefix();
    const std::string vel_path = prefix + ".vel";
    const std::string velb_path = prefix + ".velb";

    {
        std::ofstream ofs(vel_path);
        if (!ofs.is_open()) {
            err = "no se pudo crear el temporal: " + vel_path;
            return false;
        }
        ofs << cr.vel_text;
    }

    // 3. Ensamblar + linkar .vel -> .velb (saltando el preprocesador, ya que
    //    el .vel no contiene directivas VPP).  Se pasa el IR pre-serializado
    //    para que el .velb v3 lleve la seccion @ir (habilita auto-JIT).
    int rc = asm_multi_process::run_worker(vel_path, prefix,
                                           /*skip_preprocessor=*/true,
                                           /*keep_labels=*/false,
                                           /*ir_section_bytes=*/&cr.ir_section_bytes,
                                           /*emit_map=*/false);

    if (rc != 0) {
        try_remove(vel_path);
        try_remove(velb_path);
        err = "fallo al ensamblar/linkar el .vel a .velb (run_worker rc=" +
              std::to_string(rc) + ")";
        return false;
    }

    // 4. Leer los bytes del .velb resultante.
    bool read_ok = read_file_bytes(velb_path, out_bytes);

    // 5. Limpiar temporales (no afecta al resultado si falla el borrado).
    try_remove(vel_path);
    try_remove(velb_path);

    if (!read_ok || out_bytes.empty()) {
        err = "el .velb generado esta vacio o no se pudo leer";
        return false;
    }
    return true;
}

/**
 * @brief Ejecuta bytes .velb en una VM nueva y recupera R0 (logica interna).
 *
 * @param velb_bytes  Bytes del .velb.
 * @param argv        Argumentos del programa Vex.
 * @param out_exit    [salida] Valor de R0 del proceso main.
 * @param err         [salida] Mensaje de error si retorna false.
 * @return true si exito.
 */
bool run_velb_bytes(std::vector<uint8_t> velb_bytes,
                    const std::vector<std::string> &argv, int &out_exit,
                    std::string &err) {
    // Construir el manager + una VM con un unico scheduler (suficiente para
    // ejecutar el proceso main; mismo modelo que el path --run por defecto).
    runtime::ManageVM mgr(nullptr, 0);
    runtime::VM *vm = mgr.loader.create_vm_instance(/*num_schedulers=*/1);
    if (!vm) {
        err = "no se pudo crear la instancia de VM";
        return false;
    }

    // Cargar el bytecode desde memoria (sin pasar por disco).
    runtime::ProcessVM *proc =
        mgr.loader.load_executable(*vm, std::move(velb_bytes));
    if (!proc) {
        err = "no se pudo cargar el ejecutable .velb";
        return false;
    }

    // Argumentos del programa (args_count/args_get del lado Vex).
    if (!argv.empty()) {
        vm->script_args = argv;
    }

    // Poner el proceso principal en READY y arrancar el scheduler.
    vm->make_ready(proc->pid);
    vm->start();

    // Esperar a que la VM termine via la condition variable (sin polling).
    {
        std::unique_lock<std::mutex> lk(vm->done_mtx);
        vm->done_cv.wait(lk, [&] {
            return !vm->vm_running.load(std::memory_order_acquire);
        });
    }

    // Leer el resultado (R0 del proceso principal) ANTES de parar la VM:
    // el puntero proc sigue siendo valido hasta vm->stop().
    out_exit = static_cast<int>(proc->registers.regs[0].qword());

    vm->stop();
    return true;
}

} // namespace

extern "C" {

VESTA_API const char *vesta_version(void) {
    // Cadena estatica: no requiere liberacion por el llamante.
    return "vesta-capi 1.0";
}

VESTA_API int vesta_compile(const char *src, const char *unit_name,
                            unsigned char **out_velb, size_t *out_len,
                            char **out_err) {
    if (out_err) *out_err = nullptr;
    if (out_velb) *out_velb = nullptr;
    if (out_len) *out_len = 0;

    if (!src || !out_velb || !out_len) {
        set_err(out_err, "argumentos invalidos (src/out_velb/out_len NULL)");
        return 1;
    }

    try {
        std::vector<uint8_t> bytes;
        std::string err;
        const std::string unit = unit_name ? unit_name : "main";
        if (!compile_to_velb_bytes(src, unit, bytes, err)) {
            set_err(out_err, err);
            return 1;
        }
        // Copiar los bytes a un buffer en heap (malloc) para el llamante.
        unsigned char *buf =
            static_cast<unsigned char *>(std::malloc(bytes.size()));
        if (!buf) {
            set_err(out_err, "sin memoria al copiar el .velb");
            return 1;
        }
        std::memcpy(buf, bytes.data(), bytes.size());
        *out_velb = buf;
        *out_len = bytes.size();
        return 0;
    } catch (const std::exception &e) {
        set_err(out_err, std::string("excepcion en vesta_compile: ") + e.what());
        return 2;
    } catch (...) {
        set_err(out_err, "excepcion desconocida en vesta_compile");
        return 2;
    }
}

VESTA_API int vesta_run(const unsigned char *velb, size_t len, int argc,
                        const char *const *argv, int *out_exit,
                        char **out_err) {
    if (out_err) *out_err = nullptr;
    if (out_exit) *out_exit = 0;

    if (!velb || len == 0) {
        set_err(out_err, "argumentos invalidos (velb NULL o len 0)");
        return 1;
    }

    try {
        // Convertir el bytecode a vector y los argumentos a strings.
        std::vector<uint8_t> bytes(velb, velb + len);
        std::vector<std::string> args;
        if (argv && argc > 0) {
            args.reserve(static_cast<size_t>(argc));
            for (int i = 0; i < argc; ++i) {
                args.emplace_back(argv[i] ? argv[i] : "");
            }
        }

        int exit_val = 0;
        std::string err;
        if (!run_velb_bytes(std::move(bytes), args, exit_val, err)) {
            set_err(out_err, err);
            return 1;
        }
        if (out_exit) *out_exit = exit_val;
        return 0;
    } catch (const std::exception &e) {
        set_err(out_err, std::string("excepcion en vesta_run: ") + e.what());
        return 2;
    } catch (...) {
        set_err(out_err, "excepcion desconocida en vesta_run");
        return 2;
    }
}

VESTA_API int vesta_eval(const char *src, const char *unit_name, int *out_exit,
                         char **out_err) {
    if (out_err) *out_err = nullptr;
    if (out_exit) *out_exit = 0;

    if (!src) {
        set_err(out_err, "argumentos invalidos (src NULL)");
        return 1;
    }

    try {
        const std::string unit = unit_name ? unit_name : "main";
        std::vector<uint8_t> bytes;
        std::string err;
        if (!compile_to_velb_bytes(src, unit, bytes, err)) {
            set_err(out_err, err);
            return 1;
        }
        int exit_val = 0;
        if (!run_velb_bytes(std::move(bytes), {}, exit_val, err)) {
            set_err(out_err, err);
            return 1;
        }
        if (out_exit) *out_exit = exit_val;
        return 0;
    } catch (const std::exception &e) {
        set_err(out_err, std::string("excepcion en vesta_eval: ") + e.what());
        return 2;
    } catch (...) {
        set_err(out_err, "excepcion desconocida en vesta_eval");
        return 2;
    }
}

VESTA_API void vesta_free(void *p) {
    // Aceptar NULL es seguro.  Los buffers se asignaron con malloc.
    if (p) std::free(p);
}

} // extern "C"
