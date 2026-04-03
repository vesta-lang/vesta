#ifndef LOADER_H
#define LOADER_H

#include "runtime/vm_address_space.h"

#include <cstdint>   // uint8_t, uint32_t
#include <cstddef>   // size_t
#include <mutex>

#include "arena/arena_manager.h"
#include "emmit/bytereader.h"
#include "linker/velb_linker_bytecode.h"

namespace runtime {
    class ManageVM;
}

#define LOADER_THROW(kind, msg) throw loader::LoaderError(kind, msg)
#define LOADER_THROW_AT(kind, msg, reader) \
throw loader::LoaderError(kind, \
    std::string(msg) + " (offset=" + std::to_string(reader.offset) + ")")


namespace loader {
    using namespace Assembly::Bytecode;

    enum class ErrorKind {
        TruncatedHeader,
        InvalidFormat,
        InvalidMagic,
        InvalidVersion,
        CorruptedExecutable,
        NotFoundSpacesAddress
    };

    class LoaderError : public std::exception {
    public:
        LoaderError(ErrorKind kind, std::string msg)
            : kind(kind), message(std::move(msg)) {
        }

        const char *what() const noexcept override {
            return message.c_str();
        }

        ErrorKind kind;
        std::string message;
    };

    [[noreturn]]
    inline void throw_error(ErrorKind kind, const std::string &msg) {
        throw LoaderError(kind, msg);
    }

    [[noreturn]]
    inline void throw_error_at(ErrorKind kind, const std::string &msg, const ByteReader &reader) {
        throw LoaderError(
            kind,
            msg + " (offset=" + std::to_string(reader.offset) + ")"
        );
    }

    /**
     * Modificadores de acceso
     */
    typedef enum FieldAccess {
        FIELD_PUBLIC,
        FIELD_PRIVATE,
        FIELD_PROTECTED,
        FIELD_DEFAULT
    } FieldAccess;

    /**
     * Tipos de campo
     * (pueden ser primitivos o clases definidas por el usuario,
     * estructuras, tipos de datos (typedef), o enumeraciones)
     */
    typedef enum FieldKind {
        FIELD_PRIMITIVE,
        FIELD_CLASS,
        FIELD_STRUCT,
        FIELD_TYPEDEF,
        FIELD_ENUM,
        FIELD_ASPECT
    } FieldKind;

    /**
     * representacion de un string simple
     */
    typedef struct stringx {
        uint8_t *data;
        uint32_t size;
    } stringx;

    struct ClassInfo; // forward declaration

    /**
     * Información de un campo
     */
    typedef struct FieldInfo {
        stringx name; // nombre del campo
        FieldAccess access; // public/private/protected/default
        FieldKind kind; // tipo de dato
        ClassInfo *type_class; // si es FIELD_CLASS o FIELD_STRUCT
        uint32_t size; // tamaño en bytes
        uint32_t offset; // offset dentro del objeto
        bool is_static; // si es un campo estático
    } FieldInfo;

    /**
     * Manejador de excepciones
     */
    typedef struct HandlerException {
        ClassInfo *type; // null = catch-all
        uint32_t start_pc;
        uint32_t end_pc;
        uint32_t handler_pc;
    } HandlerException;

    /**
     * Informacion del metodo
     */
    typedef struct MethodInfo {
        stringx name;
        HandlerException *handlers;
        size_t handler_count;
        uint8_t *code; // puntero al bytecode
        // aquí irían más cosas: num locals, tamaño de operand stack, etc.
    } MethodInfo;

    // Header de frame en la pila (en memoria de la VM)?
    typedef struct FrameHeader {
        FrameHeader *prev; // frame anterior (caller)
        MethodInfo *method; // metodo actual
        uint32_t return_pc; // PC al que volver si se hace return
        // después de esto, en memoria, irían locals, operand stack, etc.
    } FrameHeader;

    /**
     * @struct Executable
     * @brief Representa un ejecutable VELB completamente enlazado y listo para cargar en la VM.
     *
     * Esta estructura es el resultado final del linker y la entrada principal del loader.
     * Contiene toda la información necesaria para:
     *  - reservar espacios de memoria
     *  - copiar secciones ensambladas
     *  - resolver símbolos y labels
     *  - inicializar el PC
     *  - cargar metadatos y relocaciones
     *
     * Es equivalente a un "ELF ejecutable" pero adaptado al formato VELB.
     */
    typedef struct Executable {
        /**
         * @brief Identificador del formato del ejecutable.
         *
         * Siempre debe ser `"velb"`. Permite validar que el archivo cargado
         * corresponde al formato correcto.
         */
        std::string format = "velb";

        /**
         * @brief Versión del formato VELB.
         *
         * Permite compatibilidad futura entre versiones del loader y del linker.
         */
        velb_version_format version = 1;

        /**
         * @brief Espacios de direcciones definidos en el ejecutable.
         *
         * Cada `Space` representa un rango de direcciones virtuales:
         *  - `anonymous`
         *  - `stack`
         *  - `heap`
         *  - `data`
         *  - etc.
         *
         * El loader debe reservar memoria para cada uno. Se puede indicar si reservar el rango completo o
         * realizar reserva lazy.
         */
        std::vector<Space> spaces{};

        /**
         * @brief Secciones ensambladas del ejecutable.
         *
         * Cada `Section` contiene bytecode ya resuelto y listo para copiarse
         * en el espacio correspondiente. Equivalente a `.text`, `.data`, `.rodata` en ELF.
         */
        std::vector<Section *> sections{};

        /**
         * @brief Símbolos globales con dirección absoluta.
         *
         * Cada `Label` contiene:
         *  - nombre del símbolo
         *  - dirección absoluta final tras el linking
         *
         * El loader los registra en la tabla de símbolos de la VM.
         */
        std::vector<Label *> labels{};

        /**
         * @brief Bytecode final concatenado.
         *
         * Opcional: algunos loaders prefieren trabajar con `sections`,
         * otros con un buffer plano. Se mantiene por compatibilidad.
         */
        std::vector<uint8_t> bytecode{};

        /**
         * @brief Dirección inicial del PC.
         *
         * Determinada por la directiva `@InitPc` o por el linker.
         * El loader debe asignarla al registro PC de la VM.
         */
        uint64_t init_pc = 0;

        /**
         * @brief Cabecera del ejecutable VELB.
         *
         * Contiene información adicional como:
         *  - tamaño del ejecutable
         *  - checksum
         *  - flags
         *  - versión del linker
         *  - punto de entrada
         *
         * Es redundante con algunos campos, pero útil para validación.
         */
        HeaderVELB header{};

        /**
         * @brief Relocaciones aplicadas durante el linking.
         *
         * Normalmente solo se usa para debugging o herramientas de análisis.
         * El ejecutable final ya tiene todas las direcciones resueltas. En teoria, aunque
         * se puede llamar al linker dinamico.
         */
        std::vector<Assembly::Bytecode::Relocation> relocations{};

        /**
         * @brief Metadatos arbitrarios en formato JSON.
         *
         * Puede incluir:
         *  - autor
         *  - timestamp
         *  - flags de compilación
         *  - información de build
         *  - dependencias
         */
        //Sqlite::json metadata;

        // Opciones del linker
        // Assembly::Bytecode::Linker::LinkerOptions options; ///< Opciones usadas para generar este ejecutable
    } Executable;

    class Loader {
    public:
        /**
         * Vector de ejecutables cargados alguna vez.
         *
         * Se almacenan como std::unique_ptr<Executable> por varias razones:
         *
         * 1. Evita copias grandes:
         *    Un Executable puede contener tablas, bytecode y estructuras pesadas.
         *    Guardarlo como unique_ptr evita copiar todo_ el objeto al hacer push_back.
         *
         * 2. Estabilidad de direcciones:
         *    Aunque el vector se realoque internamente, los punteros siguen siendo válidos.
         *    Esto permite devolver referencias a Executable sin riesgo de que queden inválidas.
         *
         * 3. Propiedad clara:
         *    El Loader es el dueño exclusivo de cada Executable.
         *    No hay aliasing, no hay referencias compartidas, no hay riesgo de doble free.
         *
         * 4. Seguridad en multihilo:
         *    Con un mutex protegiendo el vector, las referencias a los Executable
         *    permanecen estables incluso si el vector crece.
         *
         * En resumen: unique_ptr permite almacenar ejecutables grandes de forma eficiente,
         * segura y con direcciones estables, algo que un std::vector<Executable> no garantiza.
         */
        std::vector<std::unique_ptr<Executable> > executables;

        /**
         * referencia al manager de instancias de VM
         */
        runtime::ManageVM &instance_manager;

        explicit Loader(
            runtime::ManageVM &instance_manager);

        /**
         * Permite obtener una cadena de la seccion strings, en base a su offset
         * @param blob contenido de la seccion de cadena que leer.
         * @param offset offset de la cadena a leer.
         * @return cadena encontrada en el offset indicado.
         */
        std::string read_string_at(const std::vector<uint8_t> &blob, uint64_t offset);

        /**
         * Permite obtener un string en base a un offset string a traves de un reader.
         * El reader no se modificara pero indicara los limites de lectura y el contenido del que
         * se puede leer.
         * @param reader reader que usar para obtener un string
         * @param offset offset string que usar para obtener una cadena valida.
         * @return cadena obtenida a traves del offset
         */
        std::string read_string_at(ByteReader &reader, uint64_t offset);

        /**
         * Permite obtener una tabla de cadenas generada de la seccion de la tabla de strings.
         * @param blob contenido entero de la seccion de cadenas a cargar
         * @return tabla de cadenas.
         */
        std::vector<std::string> read_all_strings(const std::vector<uint8_t> &blob);

        void parse_velb_header(Executable &exe, ByteReader &reader);

        void parse_table_spaces(Executable &exe, ByteReader &reader);

        /**
         * Permite buscar a que espacio pertenece una seccion del ejecutable, debe
         * haberse analizado la tabla de espacios de direcciones previamente para poder
         * hacer esto.
         * @param exe datos del ejecutable
         * @param sec seccion que se quiere usar para buscar el espacio de direcciones
         * @return espacio de direcciones al que pertenece la seccion
         */
        Space *find_space_for_section(Executable &exe, const Section &sec);

        /**
         * parsea la tabla de secciones sin modificar el reader. Crea un punto de control
         * del reader.
         * @param exe Datos del ejecutable, debe haberse analizado antes el header, o haberse indicado
         * el offset a la tabla de secciones
         * @param reader reader padre del que realiazar un punto de control
         */
        void parser_table_sections(Executable &exe, ByteReader &reader);

        Executable parse_velb(std::vector<uint8_t> bytecode);

        void load_executable(std::string path);

        void load_executable(std::vector<uint8_t> bytecode);

        void resolve_labels(Assembly::Bytecode::Section &section);

        void load_sections(Assembly::Bytecode::Label &label);

        void load_spaces(Assembly::Bytecode::Space &space);

        void build_runtime_context();

        Executable &get_last_instance();

        void create_vm_instance(Executable &exe);

    private:
        /**
         * Un mutex en el loader para evitar problemas en el
         * caso de usar multihilo
         */
        std::mutex loader_mutex;
    };
}

#endif
