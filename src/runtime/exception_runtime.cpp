/**
 * @file exception_runtime.cpp
 * @brief Implementacion del sistema FatalError
 */

#include "runtime/exception_runtime.h"
#include "runtime/proceso_runtime.h"
#include "runtime/scheduler.h"
#include "runtime/runtime.h"
#include "loader/loader.h"
#include "loader/class_registry.h"
#include "debug/debugger.h"

#include "vx/diag/diag_catalog.h"
#include "vxdbg/codec.h"
#include "vxdbg/roots.h"

#include <fstream>
#include <memory>


#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <csignal> // std::signal / std::raise
#endif

namespace runtime {

/**
 * @brief Carpeta del grafo de conocimiento del programa.
 *
 * La misma que usa el compilador al emitirlo.  Se repite aqui en vez de
 * incluir el frontend: el runtime no depende de el, y el dia que dejaran de
 * coincidir lo unico que pasaria es que la traza sale mas escueta.
 */
static std::string vxdbg_cache_dir() {
    if (const char *v = std::getenv("VX_CACHE_DIR"); v && v[0])
        return std::string(v) + "/vxdbg";
    return ".cache/vxdbg";
}


// ---------------------------------------------------------------------
// Estado global de la clase FatalError
// ---------------------------------------------------------------------

loader::ClassInfo *g_fatal_error_class = nullptr;
static std::once_flag g_fatal_init_flag;

// ---------------------------------------------------------------------
// Tabla de debug por metodo.  Mapea MethodInfo* -> MethodDebug.
// Mutex porque puede llenarse concurrentemente desde varios procesos
// (cada VM ejecuta su __module_init).  Lecturas concurrentes con
// shared_lock no necesarias en MVP -- el lookup es raro (solo en
// build_stack_trace que solo corre tras un crash).
// ---------------------------------------------------------------------

static std::unordered_map<loader::MethodInfo *, MethodDebug> g_method_debug;
static std::mutex g_method_debug_mtx;

void register_method_debug(loader::MethodInfo *method, const char *file,
                           size_t file_len, uint32_t line) {
    if (!method) return;
    std::lock_guard<std::mutex> lk(g_method_debug_mtx);
    MethodDebug md;
    md.source_file.assign(file ? file : "", file ? file_len : 0);
    md.start_line = line;
    g_method_debug[method] = std::move(md);
}

const MethodDebug *lookup_method_debug(loader::MethodInfo *method) {
    if (!method) return nullptr;
    std::lock_guard<std::mutex> lk(g_method_debug_mtx);
    auto it = g_method_debug.find(method);
    if (it == g_method_debug.end()) return nullptr;
    return &it->second;
}

// Offsets de los fields de FatalError dentro del slot de 56 bytes.
// Documentados en exception_runtime.h.  Constantes para que los
// accesos sean directos sin tener que mirar el FieldInfo en cada
// throw (perf-critical path).
constexpr uint32_t FATAL_OFF_KIND = sizeof(loader::ObjectHeader); // 24
constexpr uint32_t FATAL_OFF_PC = FATAL_OFF_KIND + 8;             // 32
constexpr uint32_t FATAL_OFF_MSG_PTR = FATAL_OFF_PC + 8;          // 40
constexpr uint32_t FATAL_OFF_TRACE_PTR = FATAL_OFF_MSG_PTR + 8;   // 48
constexpr uint32_t FATAL_SLOT_SIZE = FATAL_OFF_TRACE_PTR + 8;     // 56

// ---------------------------------------------------------------------
// Init: registra FatalError en el ClassRegistry del Loader publico.
// ---------------------------------------------------------------------

void init_exception_classes(loader::Loader &loader_ref) {
    // Lock once: la primera VM hace el define_class; las siguientes
    // ven g_fatal_error_class ya inicializado y salen sin tocar el
    // registry.  init_once garantiza thread-safety.
    std::call_once(g_fatal_init_flag, [&loader_ref]() {
        auto &reg = loader_ref.class_registry();

        // Si ya existe (e.g. test que reinicia la VM), reutilizamos.
        if (auto *existing = reg.find_class("FatalError")) {
            g_fatal_error_class = existing;
            return;
        }

        // Definicion de los 4 fields fijos.  Todos kind=PRIMITIVE
        // size 8 para que los offsets sean predecibles y sin padding
        // adicional (el ABI documentado en exception_runtime.h asume
        // qword-aligned).
        std::vector<loader::FieldDecl> fields;
        fields.reserve(4);
        {
            loader::FieldDecl f{};
            f.name = "kind";
            f.kind = loader::FIELD_PRIMITIVE;
            f.size_bytes = 8; // se pad a 8 aunque sea i32 logico
            f.access = loader::FIELD_PUBLIC;
            fields.push_back(f);
        }
        {
            loader::FieldDecl f{};
            f.name = "pc";
            f.kind = loader::FIELD_PRIMITIVE;
            f.size_bytes = 8;
            f.access = loader::FIELD_PUBLIC;
            fields.push_back(f);
        }
        {
            loader::FieldDecl f{};
            f.name = "message";
            f.kind = loader::FIELD_PRIMITIVE; // tratado como puntero opaco
            f.size_bytes = 8;
            f.access = loader::FIELD_PUBLIC;
            fields.push_back(f);
        }
        {
            loader::FieldDecl f{};
            f.name = "stack_trace";
            f.kind = loader::FIELD_PRIMITIVE;
            f.size_bytes = 8;
            f.access = loader::FIELD_PUBLIC;
            fields.push_back(f);
        }

        // Sin metodos por ahora (en Vesta se accederan via getfield
        // directo).  Sin super (hereda de Object implicito).
        g_fatal_error_class = reg.define_class("FatalError",
                                               /*super=*/nullptr,
                                               /*interfaces=*/{}, fields,
                                               /*methods=*/{},
                                               /*flags=*/0);

        // BugFix R4: registrar las clases excepcion estandar comunes
        // con solo un field `message` (string ptr).  El lowering del
        // frontend Vesta detecta `new <ExceptionClass>(msg)` y emite la
        // secuencia inline (newobj + store message) sin requerir
        // constructor explicito en bytecode.  El `catch (X e)` accede
        // a `e.message` via getfield offset 24 (justo despues del
        // ObjectHeader).
        const char *std_exc_names[] = {
            "RuntimeException",         "ArithmeticException",
            "IllegalArgumentException", "IndexOutOfBoundsException",
            "NullPointerException",     "IOException",
            "ClassCastException",       "UnsupportedOperationException",
        };
        std::vector<loader::FieldDecl> exc_fields;
        {
            loader::FieldDecl f{};
            f.name = "message";
            f.kind = loader::FIELD_PRIMITIVE;
            f.size_bytes = 8;
            f.access = loader::FIELD_PUBLIC;
            exc_fields.push_back(f);
        }
        for (const char *n : std_exc_names) {
            if (!reg.find_class(n)) {
                (void)reg.define_class(n,
                                       /*super=*/nullptr,
                                       /*interfaces=*/{}, exc_fields,
                                       /*methods=*/{},
                                       /*flags=*/0);
            }
        }
    });
}

// ---------------------------------------------------------------------
// Stack trace builder estilo Java
// ---------------------------------------------------------------------

/**
 * @brief Nombre de la funcion que contiene una direccion de codigo.
 *
 * La cadena de marcos solo conoce METODOS registrados, asi que para todo lo
 * demas -- una funcion libre, o un metodo comptime, que al bajarse se
 * convierte en un simbolo generado sin `MethodInfo` detras -- la traza se
 * quedaba en un `<top>` con una direccion en crudo, que no dice nada.
 *
 * La tabla de simbolos del ejecutable si sabe donde empieza cada funcion, asi
 * que basta con quedarse con la de direccion mas alta que no pase del PC.
 *
 * @param vm     Proceso, para llegar a los ejecutables cargados.
 * @param pc     Direccion a resolver.
 * @param out_off Desplazamiento dentro de la funcion, si se encuentra.
 * @return El nombre, o nullptr si ninguna tabla lo cubre.
 */
/**
 * @brief Pasa un simbolo interno a algo que se pueda leer.
 *
 * Un nombre como `code.__macro_std__comptime__literal__parse_int_lit` no le
 * dice nada a nadie.  Se le quita el prefijo de seccion, la marca de cuerpo
 * comptime y se devuelven los puntos al camino del modulo, que es como lo
 * escribio quien programo: `std.comptime.literal.parse_int_lit`.
 *
 * @param raw Nombre tal como esta en la tabla de simbolos.
 * @return El nombre legible.
 */
static std::string demangle_symbol(const std::string &raw) {
    std::string s = raw;
    if (s.rfind("code.", 0) == 0) s.erase(0, 5);
    if (s.rfind("__macro_", 0) == 0) s.erase(0, 8);
    /* Los separadores del mangling vuelven a ser puntos de modulo. */
    std::string out;
    out.reserve(s.size());
    for (size_t k = 0; k < s.size();) {
        if (k + 1 < s.size() && s[k] == '_' && s[k + 1] == '_') {
            out.push_back('.');
            k += 2;
        } else {
            out.push_back(s[k]);
            ++k;
        }
    }
    return out;
}

/**
 * @brief Que declaro un simbolo, segun el grafo de conocimiento del programa.
 *
 * El grafo se localiza por el CONTENIDO del artefacto que se esta ejecutando,
 * no por su nombre: asi un binario que se renombro o se movio sigue
 * explicandose con el suyo y nunca con el de otra compilacion.
 *
 * Se carga una sola vez.  Si no hay nada -- porque el programa se compilo en
 * otra maquina, o porque se borro la cache -- se sigue sin decir nada extra: la
 * traza vale igual, solo que mas escueta.
 *
 * @param vm Proceso que fallo.
 * @param symbol Simbolo ya resuelto.
 * @return Descripcion breve ("metodo de Lector"), o vacio.
 */
static std::string entity_note_for_symbol(ProcessVM *vm,
                                          const std::string &symbol) {
    struct Grafo {
        bool intentado = false;
        bool hay = false;
        vxdbg::ArtifactMap map;
        std::unique_ptr<vxdbg::FileNodeStore> store;
    };
    static Grafo g;
    // El grafo se carga una vez y se comparte.  Hace falta candado porque el
    // compilador puede compilar varios modulos a la vez, y un fallo en dos de
    // ellos entraria aqui desde dos hilos: sin el, uno leeria el mapa mientras
    // el otro lo esta construyendo.
    static std::mutex g_mtx;
    std::lock_guard<std::mutex> lk(g_mtx);

    if (!g.intentado) {
        g.intentado = true;
        std::string path;
        for (const auto &exe :
             vm->scheduler.vm_reference.loader_public.executables)
            if (exe && !exe->source_path.empty()) {
                path = exe->source_path;
                break;
            }
        if (!path.empty()) {
            std::ifstream f(path, std::ios::binary);
            if (f) {
                const std::string bytes(
                    (std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
                if (!bytes.empty()) {
                    const std::string dir = vxdbg_cache_dir();
                    g.store = std::make_unique<vxdbg::FileNodeStore>(dir);
                    const vxdbg::CacheRootRepository repo(dir, *g.store);
                    g.hay = repo.lookup(
                        vxdbg::BuildId{
                            vxdbg::hash_bytes(bytes.data(), bytes.size())},
                        g.map);
                }
            }
        }
    }
    if (!g.hay) return {};

    const auto id = g.map.find(symbol);
    if (id.hash.empty()) return {};
    vxdbg::LanguageEntity e;
    if (!vxdbg::load_node(*g.store, id.hash, e)) return {};
    // Se dice como lo llama SU lenguaje, no la especie generica: quien lee
    // reconoce "metodo" o "constructor", no "Function".
    std::string nota = e.lang_kind.empty() ? std::string("declarado") : e.lang_kind;
    nota += " ";
    for (const auto &rel : e.relations) {
        if (rel.kind != vxdbg::RelationKind::DeclaredIn) continue;
        vxdbg::LanguageEntity duenyo;
        if (vxdbg::load_node(*g.store, rel.target.hash, duenyo) &&
            !duenyo.name.empty())
            nota += duenyo.name + ".";
        break;
    }
    nota += e.name;
    // Con la firma, porque es lo que distingue un metodo de sus sobrecargas y
    // lo que se parece a lo que hay escrito en el fuente.  El nombre con el que
    // se emitio -- `ctor_1` -- no se parece a nada.
    for (const auto &a : e.attributes)
        if (a.name == "signature") {
            nota += a.text;
            break;
        }
    return nota;
}

/**
 * @brief Codigo de catalogo de un tipo de fallo.
 *
 * El texto NO se escribe aqui: vive en el catalogo, con su codigo estable y en
 * todos los idiomas.  Un fallo en ejecucion es un diagnostico como cualquier
 * otro, y quien lo lee tiene el mismo derecho a leerlo en su idioma que quien
 * lee un error de compilacion.
 *
 * @param kind Tipo de fallo.
 * @return Su codigo.
 */
static const char *fatal_kind_code(uint32_t kind) {
    switch (kind) {
    case FATAL_NULL_POINTER: return "VX7001";
    case FATAL_DIVISION_BY_ZERO: return "VX7002";
    case FATAL_STACK_OVERFLOW: return "VX7003";
    case FATAL_STACK_UNDERFLOW: return "VX7004";
    case FATAL_ILLEGAL_INSTRUCTION: return "VX7005";
    case FATAL_INVALID_SYSCALL: return "VX7006";
    case FATAL_SEGMENTATION_FAULT: return "VX7007";
    case FATAL_NATIVE_CRASH: return "VX7008";
    case FATAL_NATIVE_EXCEPTION: return "VX7009";
    case FATAL_OUT_OF_MEMORY: return "VX7010";
    case FATAL_USER_ABORT: return "VX7011";
    default: return "VX7012";
    }
}

/**
 * @brief Ensena un fallo que nadie capturo.
 *
 * Va a la salida de error y no a la estandar: es un fallo, y quien encadene la
 * salida del programa con otra cosa no debe tragarselo mezclado con los datos.
 *
 * @param vm Proceso que fallo.
 * @param kind Tipo de fallo.
 */
/// Que fallo acabo con el proceso.  0 = ninguno.  Lo lee quien decide con
/// que codigo sale el programa; ponerlo aqui evita que cada sitio que arranca
/// una VM tenga que ir a buscarlo por su cuenta.
static std::atomic<uint32_t> g_last_fatal_kind{0};

int last_fatal_exit_code() {
    switch (g_last_fatal_kind.load(std::memory_order_relaxed)) {
    case 0: return 0;
    /* SIGFPE: operacion aritmetica invalida. */
    case FATAL_DIVISION_BY_ZERO: return 128 + 8;
    /* SIGSEGV: la memoria no era suya.  El desbordamiento de pila entra aqui
     * porque es exactamente eso: pasarse del final. */
    case FATAL_NULL_POINTER:
    case FATAL_SEGMENTATION_FAULT:
    case FATAL_STACK_OVERFLOW:
    case FATAL_STACK_UNDERFLOW:
    case FATAL_NATIVE_CRASH: return 128 + 11;
    /* SIGILL: el codigo no era ejecutable. */
    case FATAL_ILLEGAL_INSTRUCTION: return 128 + 4;
    /* SIGABRT: el programa se rindio -- panic, memoria agotada, una excepcion
     * nativa que nadie recogio. */
    case FATAL_USER_ABORT:
    case FATAL_OUT_OF_MEMORY:
    case FATAL_NATIVE_EXCEPTION: return 128 + 6;
    /* Lo que no encaja en ninguna senal sale con el fallo generico de siempre. */
    default: return 1;
    }
}

static void report_uncaught_fatal(ProcessVM *vm, uint32_t kind) {
    g_last_fatal_kind.store(kind, std::memory_order_relaxed);
    const char *code = fatal_kind_code(kind);
    const std::string texto = vx::diag::format(code, {});
    std::fprintf(stderr, "\n%s [%s]", texto.c_str(), code);
    if (vm->fatal_msg_buf && vm->fatal_msg_buf[0] != '\0')
        std::fprintf(stderr, ": %s", vm->fatal_msg_buf);
    std::fprintf(stderr, "\n");
    if (vm->fatal_trace_buf && vm->fatal_trace_buf[0] != '\0')
        std::fprintf(stderr, "%s", vm->fatal_trace_buf);
    std::fflush(stderr);
}

static const std::string *symbol_for_pc(ProcessVM *vm, uint64_t pc,
                                        uint64_t &out_off) {
    const std::string *best = nullptr;
    uint64_t best_addr = 0;
    for (const auto &exe_ptr :
         vm->scheduler.vm_reference.loader_public.executables) {
        if (!exe_ptr) continue;
        for (const auto &kv : exe_ptr->symbol_table) {
            if (kv.second <= pc && kv.second >= best_addr) {
                best_addr = kv.second;
                best = &kv.first;
            }
        }
    }
    /* La tabla trae tanto la funcion como sus bloques internos, y el bloque
     * suele ganar por estar mas cerca del PC.  Pero `..._entry_0` o
     * `..._assert_fail_58` son detalles de como se genero el codigo, no algo
     * que quien programa reconozca; si existe un simbolo que es PREFIJO del
     * encontrado, ese es la funcion y es el que hay que ensenar. */
    if (best) {
        const std::string *fn = best;
        for (const auto &exe_ptr :
             vm->scheduler.vm_reference.loader_public.executables) {
            if (!exe_ptr) continue;
            for (const auto &kv : exe_ptr->symbol_table) {
                if (kv.first.size() < fn->size() &&
                    best->rfind(kv.first, 0) == 0) {
                    fn = &kv.first;
                }
            }
        }
        out_off = pc - best_addr;
        return fn;
    }
    return best;
}

size_t build_stack_trace(ProcessVM *vm, char *out, size_t out_size) {
    if (!out || out_size < 2) return 0;
    size_t pos = 0;
    auto append = [&](const char *s, size_t n) {
        const size_t avail = (out_size - 1) - pos;
        const size_t cp = (n < avail) ? n : avail;
        std::memcpy(out + pos, s, cp);
        pos += cp;
    };
    auto append_str = [&](const char *s) {
        if (!s) return;
        append(s, std::strlen(s));
    };
    auto append_hex = [&](uint64_t v) {
        char buf[20];
        int n =
            std::snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
        if (n > 0) append(buf, (size_t)n);
    };
    auto append_dec = [&](uint64_t v) {
        char buf[24];
        int n = std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
        if (n > 0) append(buf, (size_t)n);
    };

    // Cabecera con info del proceso y PC actual.
    append_str("Stack trace (Vesta):\n");

    // Frame 0: el PC actual (donde ocurrio el error).  Si hay
    // method en frame_stack lo usamos; si no, solo PC.
    loader::FrameHeader *fr = vm->frame_stack;
    const uint64_t cur_pc = vm->registers.rip.raw();

    // Helper: append "(file.vx:line)" usando primero DebugInfo del
    // .velb cargado (precision pc_offset -> line); fallback a
    // MethodDebug (start_line del metodo).  Item 4: ahora el stack
    // trace muestra la linea EXACTA de la instruccion fallida en
    // lugar de la linea del inicio del metodo.
    /* Posicion en el fuente para un simbolo resuelto por direccion.
     *
     * La seccion de depuracion guarda UN solo fichero por unidad ensamblada,
     * asi que para el codigo que viene de otro modulo el nombre de fichero que
     * hay registrado es el del modulo raiz -- la LINEA es correcta, el fichero
     * no.  Ensenar un fichero equivocado es peor que no ensenarlo: manda a
     * mirar donde no es.  Cuando el simbolo lleva modulo en el nombre (que ya
     * dice donde vive) se muestra solo la linea; si es del propio fichero, se
     * muestran los dos. */
    auto append_pos = [&](uint64_t pc, bool tiene_modulo) {
        for (const auto &exe_ptr :
             vm->scheduler.vm_reference.loader_public.executables) {
            if (!exe_ptr || !exe_ptr->debug_info) continue;
            auto info =
                exe_ptr->debug_info->lookup_line(static_cast<uint32_t>(pc));
            if (info.found && info.line > 0) {
                append_str(" (");
                if (!tiene_modulo && info.file && info.file[0] != ' ') {
                    append_str(info.file);
                    append_str(":");
                } else {
                    append_str("linea ");
                }
                append_dec((uint64_t)info.line);
                append_str(")");
                return;
            }
        }
    };

    auto append_dbg = [&](loader::MethodInfo *m, uint64_t pc,
                          const char *pc_label) {
        // Buscar DebugInfo precise via los Executables cargados.
        // Accedemos via scheduler.vm_reference.loader_public (la VM
        // propietaria es el owner del Loader publico).
        for (const auto &exe_ptr :
             vm->scheduler.vm_reference.loader_public.executables) {
            if (!exe_ptr || !exe_ptr->debug_info) continue;
            // pc absoluto -> bytecode_offset relativo al inicio del
            // code section del executable.  Asumimos un solo executable
            // (caso comun); para multi-velb el primero que matchee.
            auto info =
                exe_ptr->debug_info->lookup_line(static_cast<uint32_t>(pc));
            if (info.found && info.line > 0) {
                append_str(" (");
                if (info.file && info.file[0] != '\0') {
                    append_str(info.file);
                } else if (m) {
                    const MethodDebug *md = lookup_method_debug(m);
                    if (md)
                        append(md->source_file.data(), md->source_file.size());
                    else
                        append_str("?");
                } else {
                    append_str("?");
                }
                append_str(":");
                append_dec((uint64_t)info.line);
                append_str(")");
                return;
            }
        }
        // Fallback: MethodDebug con start_line.
        const MethodDebug *md = m ? lookup_method_debug(m) : nullptr;
        if (md && !md->source_file.empty()) {
            append_str(" (");
            append(md->source_file.data(), md->source_file.size());
            append_str(":");
            append_dec((uint64_t)md->start_line);
            append_str(")");
        } else {
            append_str(" (");
            append_str(pc_label);
            append_str("=");
            append_hex(pc);
            append_str(")");
        }
    };

    // Primer frame: usar metodo del top de la pila si existe.
    append_str("  at ");
    /* El nombre del marco de arriba sale del PC, no del metodo del marco.  El
     * marco solo conoce METODOS registrados, y el codigo que falla a menudo no
     * lo es -- una funcion libre, o un metodo de struct, que al bajarse se
     * vuelven un simbolo suelto.  Cuando eso pasa, `fr->method` sigue apuntando
     * al metodo de QUIEN LLAMO, con lo que se ensenaba un nombre que no
     * corresponde a la linea de al lado. */
    uint64_t off_top = 0;
    const std::string *sym_top = symbol_for_pc(vm, cur_pc, off_top);
    if (sym_top == nullptr && fr && fr->method) {
        const auto &mname = fr->method->name;
        if (mname.data && mname.size > 0) {
            append((const char *)mname.data, mname.size);
        } else {
            append_str("<unknown>");
        }
        // Clase del metodo (owner_class o owner_class si esta).
        if (fr->method->owner_class) {
            const auto &cname = fr->method->owner_class->name;
            if (cname.data && cname.size > 0) {
                append_str(" [");
                append((const char *)cname.data, cname.size);
                append_str("]");
            }
        }
        append_dbg(fr->method, cur_pc, "pc");
        append_str("\n");
    } else {
        if (const std::string *sym = sym_top) {
            const std::string legible = demangle_symbol(*sym);
            append(legible.c_str(), legible.size());
            // Que ES lo que fallo, no solo como se llama: si el grafo lo sabe,
            // se dice ("constructor de Lector") en vez de dejar un nombre suelto
            // que quien lee tiene que ir a buscar al fuente.
            const std::string nota = entity_note_for_symbol(vm, *sym);
            if (!nota.empty()) {
                append_str(" [");
                append(nota.c_str(), nota.size());
                append_str("]");
            }
            append_pos(cur_pc,
                       legible.find('.') != std::string::npos);
        } else {
            append_str("<top> (pc=");
            append_hex(cur_pc);
            append_str(")");
        }
        append_str("\n");
    }

    // Frames intermedios: recorrer frame_stack hasta el origen.
    // Limitamos a 64 frames para evitar runaway en casos degenerados.
    int depth = 0;
    /* Se empieza en el marco SIGUIENTE: el de arriba ya se conto.  Empezando en
     * `fr` salia repetido, y una pila cuyo primer marco aparece dos veces hace
     * dudar de todo lo demas que diga. */
    for (loader::FrameHeader *p = (fr ? fr->prev : nullptr);
         p != nullptr && depth < 64; p = p->prev, ++depth) {
        append_str("  at ");
        if (p->method) {
            const auto &mname = p->method->name;
            if (mname.data && mname.size > 0) {
                append((const char *)mname.data, mname.size);
            } else {
                append_str("<unknown>");
            }
            if (p->method->owner_class) {
                const auto &cname = p->method->owner_class->name;
                if (cname.data && cname.size > 0) {
                    append_str(" [");
                    append((const char *)cname.data, cname.size);
                    append_str("]");
                }
            }
            append_dbg(p->method, p->return_pc, "return_pc");
        } else {
            append_str("<frameless> (return_pc=");
            append_hex(p->return_pc);
            append_str(")");
        }
        append_str("\n");
    }
    if (depth >= 64) {
        append_str("  ... (truncated, depth >= 64)\n");
    }

    /* Cadena de llamadas por la PILA.  Los marcos de arriba solo cubren
     * METODOS registrados; el resto del camino -- una funcion libre, o un
     * metodo comptime, que al bajarse se vuelve un simbolo generado sin
     * `MethodInfo` detras -- se reconstruye mirando que direcciones guardadas
     * en la pila caen dentro de una funcion conocida.
     *
     * Es una lectura conservadora: puede colarse algun dato que por casualidad
     * parezca una direccion de retorno.  Preferible a no decir nada, que es lo
     * que habia.  Se van saltando las repeticiones seguidas del mismo simbolo
     * y se corta a 32 entradas. */
    {
        const uint64_t rsp = vm->registers.stack_pointer.qword();
        const uint64_t top = vm->stack_high;
        const std::string *prev = nullptr;
        int shown = 0;
        for (uint64_t a2 = rsp; a2 + 8 <= top && shown < 32; a2 += 8) {
            uint64_t v = 0;
            vm->vm_mem.read_bytes(a2, &v, sizeof(v));
            if (v == 0) continue;
            uint64_t off = 0;
            const std::string *sym = symbol_for_pc(vm, v, off);
            if (!sym || off > 0x10000) continue; /* fuera de toda funcion */
            /* `code.s_N` son literales de datos, no codigo: un valor que cae
             * ahi es un dato que se parece a una direccion, no una llamada. */
            if (sym->rfind("code.s_", 0) == 0) continue;
            /* `gdata.*` es el area de variables globales, tampoco codigo. */
            if (sym->rfind("gdata.", 0) == 0) continue;
            if (prev && *prev == *sym) continue;
            prev = sym;
            append_str("  llamada desde ");
            const std::string legible = demangle_symbol(*sym);
            append(legible.c_str(), legible.size());
            append_pos(v, legible.find('.') != std::string::npos);
            append_str("\n");
            ++shown;
        }
    }

    // Pid del proceso para diagnostico cross-process.
    append_str("  in process pid=");
    append_dec(vm->pid.local_pid);
    append_str(" sched=");
    append_dec(vm->pid.scheduler_id);
    append_str("\n");

    out[pos] = '\0';
    return pos;
}

// ---------------------------------------------------------------------
// Helper interno: asegura que vm tiene los buffers reservados.
// Lazy alloc en el primer throw_fatal del proceso.
// ---------------------------------------------------------------------

static void ensure_fatal_buffers(ProcessVM *vm) {
    if (!vm->fatal_slot) {
        vm->fatal_slot = std::calloc(1, FATAL_SLOT_SIZE);
    }
    if (!vm->fatal_msg_buf) {
        vm->fatal_msg_buf = (char *)std::malloc(256);
        if (vm->fatal_msg_buf) vm->fatal_msg_buf[0] = '\0';
    }
    if (!vm->fatal_trace_buf) {
        vm->fatal_trace_buf = (char *)std::malloc(4096);
        if (vm->fatal_trace_buf) vm->fatal_trace_buf[0] = '\0';
    }
}

// ---------------------------------------------------------------------
// Forward decl: do_throw vive en exec_instruction_oop.cpp.
// ---------------------------------------------------------------------
void do_throw(ProcessVM *vm, uint64_t exception_ptr);

/**
 * @brief Mapea FATAL_* a state_err_thread para la ruta antigua.
 */
static state_err_thread fatal_to_thread_err(uint32_t kind) noexcept {
    switch (kind) {
    case FATAL_NULL_POINTER: return THREAD_NULL_POINTER;
    case FATAL_DIVISION_BY_ZERO: return THREAD_DIVISION_BY_ZERO;
    case FATAL_STACK_OVERFLOW: return THREAD_STACK_OVERFLOW;
    case FATAL_STACK_UNDERFLOW: return THREAD_STACK_UNDERFLOW;
    case FATAL_ILLEGAL_INSTRUCTION: return THREAD_ILLEGAL_INSTRUCTION;
    case FATAL_INVALID_SYSCALL: return THREAD_INVALID_SYSCALL;
    case FATAL_SEGMENTATION_FAULT: return THREAD_SEGMENTATION_FAULT;
    default: return THREAD_SEGMENTATION_FAULT;
    }
}

// ---------------------------------------------------------------------
// throw_fatal: ruta dual (capturable / fatal-original).
// ---------------------------------------------------------------------

void throw_fatal(ProcessVM *vm, uint32_t kind, const char *message) {
    if (!vm) return;

    // Notificar al debugger de la excepcion ANTES de elegir ruta
    // (capturable o fatal).  El cliente recibe evento "exception"
    // con clase + pid; util para parar y ver el contexto incluso
    // si el codigo del usuario captura la excepcion.  Si no hay
    // debugger activo (caso comun), la rama es 1 carga + 1 branch
    // bien predicho (~0 ns en hot path).
    if (vm->scheduler.vm_reference.debugger != nullptr) {
        vm->scheduler.vm_reference.debugger->on_exception(
            vm->pid.local_pid,
            message ? std::string(message) : std::string("FatalError"));
    }

    // FAST PATH: si no hay handler activo, ruta antigua.  Esto es
    // el caso comun (programas sin try/catch envolvente).  Cero
    // overhead anadido: 1 lectura + 1 branch.
    if (vm->exc_frame_stack == nullptr) {
        /* Guardar el mensaje aunque no haya quien lo capture.  Antes solo se
         * conservaba en la ruta CON handler, asi que un fallo sin `try` se
         * llevaba consigo la unica pista de que habia pasado.  Quien recoja
         * el cadaver del proceso -- el compilador, cuando esto ocurre durante
         * una ejecucion comptime -- necesita poder decirlo. */
        ensure_fatal_buffers(vm);
        if (vm->fatal_msg_buf) {
            if (message) {
                std::strncpy(vm->fatal_msg_buf, message, 255);
                vm->fatal_msg_buf[255] = '\0';
            } else {
                vm->fatal_msg_buf[0] = '\0';
            }
        }
        /* La traza se construye AQUI, con el proceso todavia coherente.
         * Hacerlo despues, cuando alguien recoge el cadaver, significa
         * recorrer una cadena de marcos que ya no tiene por que ser valida --
         * y eso se lleva por delante a quien lo intente. */
        if (vm->fatal_trace_buf) {
            build_stack_trace(vm, vm->fatal_trace_buf, 4096);
        }
        /* Y se ENSEÑA.  Guardarlo sin decir nada dejaba al programa parandose
         * en seco: sin mensaje, sin traza y con codigo de salida cero, o sea
         * afirmando que todo fue bien.  Cualquier guion o integracion continua
         * que lo llamara se lo creia.  Un fallo que nadie captura es lo mas
         * parecido a un error de verdad que hay, y tiene que verse. */
        report_uncaught_fatal(vm, kind);
        vm->err_thread = fatal_to_thread_err(kind);
        vm->scheduler.on_event(EVT_ERROR);
        return;
    }

    // Si por algun motivo la clase FatalError no esta registrada
    // (init_exception_classes no se llamo), no podemos lanzar como
    // excepcion -- caemos al camino antiguo.
    if (g_fatal_error_class == nullptr) {
        vm->err_thread = fatal_to_thread_err(kind);
        vm->scheduler.on_event(EVT_ERROR);
        return;
    }

    // SLOW PATH (handler activo): construir la instancia FatalError.
    ensure_fatal_buffers(vm);
    if (!vm->fatal_slot) {
        // OOM en el slot mismo: ruta antigua.
        vm->err_thread = fatal_to_thread_err(kind);
        vm->scheduler.on_event(EVT_ERROR);
        return;
    }

    // Inicializar ObjectHeader.
    auto *hdr = reinterpret_cast<loader::ObjectHeader *>(vm->fatal_slot);
    std::memset(hdr, 0, sizeof(loader::ObjectHeader));
    hdr->class_ptr = g_fatal_error_class;
    hdr->flags = 0;
    hdr->hash_code = 0;

    // Llenar fields por offset directo.
    char *base = (char *)vm->fatal_slot;
    const uint64_t pc_now = vm->registers.rip.raw();
    *(uint64_t *)(base + FATAL_OFF_KIND) = (uint64_t)kind;
    *(uint64_t *)(base + FATAL_OFF_PC) = pc_now;

    // Copiar mensaje al buffer reusable (truncar a 255).
    if (vm->fatal_msg_buf && message) {
        std::strncpy(vm->fatal_msg_buf, message, 255);
        vm->fatal_msg_buf[255] = '\0';
    }
    *(uint64_t *)(base + FATAL_OFF_MSG_PTR) =
        (uint64_t)(uintptr_t)vm->fatal_msg_buf;

    // Construir stack trace (ya incluye el PC y los frames).
    if (vm->fatal_trace_buf) {
        build_stack_trace(vm, vm->fatal_trace_buf, 4096);
    }
    *(uint64_t *)(base + FATAL_OFF_TRACE_PTR) =
        (uint64_t)(uintptr_t)vm->fatal_trace_buf;

    // Lanzar via do_throw (mismo patron que `throw new X` en bytecode).
    do_throw(vm, (uint64_t)(uintptr_t)vm->fatal_slot);
}

// ---------------------------------------------------------------------
// exec_instr_panic: lee mensaje de la VM y lanza FATAL_USER_ABORT.
// ---------------------------------------------------------------------

// =====================================================================
// OS-level access violation -> FatalError capturable.
// =====================================================================

// Thread-local: ProcessVM cuyo bytecode esta corriendo.  El handler
// de AV/SIGSEGV lo consulta para identificar el receptor del longjmp.
// Limitacion: el VM corre cada proceso en su scheduler, asi que un
// mismo OS thread tiene UN ProcessVM activo a la vez.  Si en el
// futuro se anade preemption con context-switch dentro del run_loop,
// habria que actualizar el TLS antes de cambiar de proceso.
#if defined(__GNUC__) || defined(__clang__)
static __thread ProcessVM *t_executing_proc = nullptr;
#elif defined(_MSC_VER)
static __declspec(thread) ProcessVM *t_executing_proc = nullptr;
#else
static thread_local ProcessVM *t_executing_proc = nullptr;
#endif

#if defined(_WIN32)
/* Sprint JIT thunk TLS-direct (Win64 only):
 *
 * Reservamos UN slot TLS dedicado via TlsAlloc().  El thunk
 * x86-64 generado lee proc directamente via @c gs:[0x1480 + idx*8]
 * (slots 0..63 viven embebidos en el TEB).  Coste: 1 instr ~3 ns
 * vs 25-30 ns del call a @c get_current_executing_process.
 *
 * El offset 0x1480 corresponde a TlsSlots[0] dentro del TEB en
 * Win64 (verificable en wikipedia.org/wiki/Win32_Thread_Information_Block).
 * Slots >= 64 estan en TlsExpansionSlots accesibles via
 * @c gs:[0x1780] + indirect; el thunk los rechaza y cae al call. */
static DWORD g_proc_tls_index = TLS_OUT_OF_INDEXES;
static std::once_flag g_proc_tls_once;

static void init_proc_tls_index_once() {
    std::call_once(g_proc_tls_once, []() { g_proc_tls_index = TlsAlloc(); });
}

unsigned long jit_proc_tls_index() noexcept {
    init_proc_tls_index_once();
    return static_cast<unsigned long>(g_proc_tls_index);
}
#endif

void set_current_executing_process(ProcessVM *proc) noexcept {
    t_executing_proc = proc;
#if defined(_WIN32)
    /* Mantener el slot TLS dedicado sincronizado con el thread_local
     * C++.  El thunk lee el slot directo via gs:[]; sin este sync,
     * el thunk leeria stale data. */
    init_proc_tls_index_once();
    if (g_proc_tls_index != TLS_OUT_OF_INDEXES) {
        TlsSetValue(g_proc_tls_index, proc);
    }
#endif
}

ProcessVM *get_current_executing_process() noexcept {
    return t_executing_proc;
}

} // namespace runtime

/* Sprint JIT-cross-fn 2026-06-01: extern "C" shim para que el header
 * publico `jit/interp_jit_bridge.h` pueda invocar
 * `set_current_executing_process` sin necesitar incluir
 * `runtime/exception_runtime.h` (que arrastraria todo el runtime).  El shim es
 * solo un wrapper de 1 line. */
extern "C" void runtime_set_current_executing_process_c(void *proc) {
    runtime::set_current_executing_process(
        static_cast<runtime::ProcessVM *>(proc));
}

namespace runtime {

static std::once_flag g_av_handler_init_flag;

#if defined(_WIN32)
/// Handle del VEH instalado por ::install_host_av_handler.  Se guarda para poder
/// retirarlo (::uninstall_host_av_handler) ANTES de que el codigo de la libreria
/// quede sin mapear: si libvesta se descarga (FreeLibrary) sin retirar el VEH,
/// la cadena de manejadores conserva un puntero a codigo muerto y el cierre del
/// proceso (o cualquier excepcion posterior) salta a esa direccion -> segfault.
static void *g_av_veh_handle = nullptr;
#endif

#if defined(_WIN32)
/**
 * @brief Stub al que el VEH redirige RIP cuando ocurre un AV
 *        capturable.  Corre en contexto de usuario normal (no async)
 *        asi que el longjmp es seguro aqui.
 *
 * Marcado @c noinline para que el compilador no inline el contenido
 * en otro punto del binary -- queremos una direccion estable que el
 * VEH pueda apuntar.
 */
static void __attribute__((noinline)) av_recovery_stub() {
    ProcessVM *proc = t_executing_proc;
    if (proc != nullptr && proc->av_recovery_active) {
        std::longjmp(proc->av_recovery_jmpbuf, 1);
    }
    // Sin recovery activo: no podemos hacer nada utilidad; salir.
    // En la practica esto no deberia ocurrir porque el VEH ya
    // verifica av_recovery_active antes de redirigir.
    std::abort();
}

/**
 * @brief Vectored Exception Handler global.  Solo trata
 *        EXCEPTION_ACCESS_VIOLATION cuando el thread actual esta
 *        ejecutando bytecode con un try/catch envolvente.  En todos
 *        los demas casos retorna EXCEPTION_CONTINUE_SEARCH para no
 *        interferir con el manejo normal de excepciones (otros
 *        plugins, debugger, etc.).
 */
static LONG WINAPI vx_av_veh(EXCEPTION_POINTERS *info) {
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // Manejamos AV y divisiones por cero (Bug fix 2026-05-23).
    // Otras excepciones (illegal instr, debug breakpoints, etc.) siguen
    // su curso normal.
    uint32_t kind_local = 0; // 0=AV
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        kind_local = 0;
    } else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        kind_local = 1;
    } else if (code == EXCEPTION_INT_OVERFLOW) {
        kind_local = 2;
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ProcessVM *proc = t_executing_proc;
    if (proc == nullptr || !proc->av_recovery_active) {
        // No hay bytecode corriendo o el scheduler no ha armado el
        // setjmp: la VM crashea como antes (comportamiento legado).
        return EXCEPTION_CONTINUE_SEARCH;
    }
    /* Gana el PRIMER fallo, no el ultimo.  Al desviar la ejecucion al stub de
     * recuperacion desde codigo compilado, el desvio puede provocar un segundo
     * fallo antes de que nadie haya leido el primero; si se sobrescribiera, una
     * division entre cero acabaria contandose como acceso invalido -- que es
     * exactamente lo que pasaba en JIT.  El que hay que explicar es el que
     * rompio el programa, no el que provoco el intento de recuperarlo. */
    const bool primero = (proc->pending_av_kind == 0xFFFFFFFFu);
    if (primero) proc->pending_av_kind = kind_local;
    // Capturar la direccion del fault (segundo elemento de
    // ExceptionInformation: 0=read/write flag, 1=virtual addr) -- solo
    // para AV; para div0 / overflow no aplica.
    if (primero && kind_local == 0 &&
        info->ExceptionRecord->NumberParameters >= 2) {
        proc->pending_av_addr =
            (uint64_t)info->ExceptionRecord->ExceptionInformation[1];
    } else if (primero) {
        proc->pending_av_addr = 0;
    }
    // Redirigir RIP al stub que hara longjmp en contexto normal.
    // No tocamos RSP: el stub es una funcion C++ valida; reusara
    // el frame del callee, que ya tiene espacio suficiente.
#if defined(_M_X64) || defined(__x86_64__)
    info->ContextRecord->Rip = (DWORD64)(uintptr_t)&av_recovery_stub;
#elif defined(_M_IX86) || defined(__i386__)
    info->ContextRecord->Eip = (DWORD)(uintptr_t)&av_recovery_stub;
#elif defined(_M_ARM64) || defined(__aarch64__)
    info->ContextRecord->Pc = (DWORD64)(uintptr_t)&av_recovery_stub;
#else
    // Plataforma desconocida: dejar que la VM crashee como antes.
    return EXCEPTION_CONTINUE_SEARCH;
#endif
    return EXCEPTION_CONTINUE_EXECUTION;
}

void install_host_av_handler() noexcept {
    std::call_once(g_av_handler_init_flag, []() {
        // Primer parametro = 1 -> registrar al inicio de la cadena
        // (antes del default Windows handler).  Asi capturamos AVs
        // antes de que llegue el "Application has stopped" del OS.
        // Guardamos el handle para poder retirarlo al descargar la libreria.
        g_av_veh_handle = AddVectoredExceptionHandler(1u, &vx_av_veh);
    });
}

void uninstall_host_av_handler() noexcept {
    // Retirar el VEH.  Imprescindible si libvesta se descarga con FreeLibrary:
    // tras desmapear la DLL, un VEH colgante apuntaria a codigo muerto.
    if (g_av_veh_handle) {
        RemoveVectoredExceptionHandler(g_av_veh_handle);
        g_av_veh_handle = nullptr;
    }
}
#else
// POSIX: handler de SIGSEGV / SIGFPE.  El handler usa siglongjmp directamente
// ya que en POSIX longjmp desde un signal handler es bien definido
// si se usa sigsetjmp con savesigs=1.
static void posix_signal_handler(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    ProcessVM *proc = t_executing_proc;
    if (proc == nullptr || !proc->av_recovery_active) {
        std::signal(sig, SIG_DFL);
        std::raise(sig);
        return;
    }
    // Bug fix 2026-05-23: capturar tambien SIGFPE (div/0).
    if (sig == SIGFPE) {
        proc->pending_av_kind = 1; // DIVIDE_BY_ZERO (o overflow)
        proc->pending_av_addr = 0;
    } else {
        proc->pending_av_kind = 0; // AV
        proc->pending_av_addr = info ? (uint64_t)(uintptr_t)info->si_addr : 0;
    }
    std::longjmp(proc->av_recovery_jmpbuf, 1);
}

void install_host_av_handler() noexcept {
    std::call_once(g_av_handler_init_flag, []() {
        struct sigaction sa{};
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
        sa.sa_sigaction = &posix_signal_handler;
        sigemptyset(&sa.sa_mask);
        (void)sigaction(SIGSEGV, &sa, nullptr);
        (void)sigaction(SIGBUS, &sa, nullptr);
        (void)sigaction(SIGFPE, &sa, nullptr);
    });
}

void uninstall_host_av_handler() noexcept {
    // Restaurar el comportamiento por defecto para que, si la libreria se
    // descarga (dlclose), no quede un handler apuntando a codigo desmapeado.
    std::signal(SIGSEGV, SIG_DFL);
    std::signal(SIGBUS, SIG_DFL);
    std::signal(SIGFPE, SIG_DFL);
}
#endif

} // namespace runtime

// Definicion del exec_instr fuera del namespace runtime para matchear
// la declaracion en runtime/exec_instruction.h (que usa namespace runtime
// implicitamente al ser includado en archivos que ya estan en namespace
// runtime).  Pero para mantener consistencia con los otros exec_instr_*,
// va dentro del namespace runtime tambien.

namespace runtime {

/**
 * @brief Layout de SetMethDebugParams (24 bytes en VM mem).
 */
struct SetMethDebugParams {
    uint64_t method_ptr;
    uint64_t file_addr;
    uint32_t file_len;
    uint32_t start_line;
};

void exec_instr_setmethdbg(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_method = instr.data_instruction.reg_data.reg1;
    const uint8_t r_params = instr.data_instruction.reg_data.reg2;
    (void)r_method; // por ahora usamos solo el params (que tambien
                    // lleva method_ptr); r_method redundante para
                    // futuros usos del decoder.

    const uint64_t params_addr = vm->registers.regs[r_params].qword();
    if (params_addr == 0) return;

    SetMethDebugParams sp{};
    vm->vm_mem.read_bytes(params_addr, &sp, sizeof(sp));
    if (sp.method_ptr == 0) return;

    // Leer nombre de archivo del VM mem (truncar a 256 bytes).
    char fbuf[256];
    size_t flen = (size_t)sp.file_len;
    if (flen > 255) flen = 255;
    if (sp.file_addr != 0 && flen > 0) {
        vm->vm_mem.read_bytes(sp.file_addr, fbuf, flen);
    }
    fbuf[flen] = '\0';

    register_method_debug(reinterpret_cast<loader::MethodInfo *>(sp.method_ptr),
                          fbuf, flen, sp.start_line);
}

void exec_instr_panic(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_addr = instr.data_instruction.reg_data.reg1;
    const uint8_t r_len = instr.data_instruction.reg_data.reg2;

    const uint64_t vm_addr = vm->registers.regs[r_addr].qword();
    const uint64_t len = vm->registers.regs[r_len].qword();

    // Leer el mensaje desde la VM mem a un buffer de pila.  Truncar a
    // 255 bytes (la copia interna en throw_fatal lo trunca a 256
    // incluyendo nul).  Si len = 0 usamos un mensaje por defecto.
    char buf[256];
    if (len == 0 || vm_addr == 0) {
        std::strncpy(buf, "panic", sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    } else {
        const size_t n_copy = (len < 255) ? (size_t)len : 255;
        vm->vm_mem.read_bytes(vm_addr, buf, n_copy);
        buf[n_copy] = '\0';
    }

    throw_fatal(vm, FATAL_USER_ABORT, buf);
}

void throw_fatalf(ProcessVM *vm, uint32_t kind, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        throw_fatal(vm, kind, "<fmt error>");
        return;
    }
    // vsnprintf trunca implicitamente y ya pone nul terminador.
    throw_fatal(vm, kind, buf);
}

} // namespace runtime
