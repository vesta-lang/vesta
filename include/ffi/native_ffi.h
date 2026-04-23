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


#ifndef NATIVE_FFI_H
#define NATIVE_FFI_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

using CallFn = uint64_t(*)(
    void *,
    uint64_t, uint64_t,
    uint64_t, uint64_t,
    uint64_t, uint64_t,
    uint64_t, uint64_t,
    uint64_t, uint64_t,
    uint64_t, uint64_t);

namespace ffi {
    using Fn00 = uint64_t(*)(void *);
    using Fn01 = uint64_t(*)(void *, uint64_t);
    using Fn02 = uint64_t(*)(void *, uint64_t, uint64_t);
    using Fn03 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t);
    using Fn04 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t);
    using Fn05 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    using Fn06 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    using Fn07 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    using Fn08 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    using Fn09 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t);
    using Fn10 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t);
    using Fn11 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
    using Fn12 = uint64_t(*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t);


    class FFIError : public std::runtime_error {
    public:
        explicit FFIError(const std::string &msg)
            : std::runtime_error(msg) {}
    };

    /**
     * Permite parchear una instruccion call o cualquier instruccion que reciba
     * un puntero de tamaño de CPU
     * @param bytecode_base base address del bytecode
     * @param off offset que modificar
     * @param fn direccion nativa que ingresar/escribir
     */
    static inline void patch_call(uint8_t *bytecode_base, uint32_t off, void *fn) {
        uint64_t  addr          = reinterpret_cast<uint64_t>(fn); // 64 bits en x86_64
        uint64_t *address_patch = (uint64_t *) (bytecode_base + off);
        *address_patch          = addr;
    }

    /**
     * @brief Clave compacta que identifica un símbolo nativo único.
     *
     * Esta estructura representa la combinación:
     *     módulo + función + firma
     *
     * Cada campo es un índice a las tablas internadas de strings
     * (modules, functions, signatures). Esto permite:
     *   - evitar duplicación de cadenas
     *   - comparaciones rápidas (enteros)
     *   - hashing eficiente
     *   - agrupar múltiples patch-sites del mismo símbolo
     */
    struct NativeImportKey {
        uint32_t module_idx    = 0; ///< Índice al nombre del módulo (kernel32.dll, libc.so…)
        uint32_t function_idx  = 0; ///< Índice al nombre de la función (GetTickCount, read…)
        uint32_t signature_idx = 0; ///< Índice a la firma del metodo (p.ej. "(void)->u32")

        /**
         * Permite sobrecargar el operando == para realizar una comparacion de los
         * indices dados
         * @param other clave a comparar
         * @return devuelve true si son iguales.
         */
        bool operator==(const NativeImportKey &other) const noexcept {
            return module_idx == other.module_idx &&
                    function_idx == other.function_idx &&
                    signature_idx == other.signature_idx;
        }
    };

    /**
     * @brief Información en tiempo de ejecución para un símbolo nativo.
     *
     * Esta estructura contiene:
     *   - la clave del símbolo (módulo/función/firma)
     *   - el puntero resuelto mediante LoadLibrary/dlopen + GetProcAddress/dlsym
     *   - todos los offsets de bytecode donde debe parchearse un CALL
     *
     * El loader agrupa todas las importaciones repetidas en una sola entrada,
     * de modo que cada símbolo nativo se resuelve una única vez.
     */
    struct NativeImportRuntime {
        NativeImportKey       key;                    ///< Clave que identifica el símbolo nativo.
        void *                resolved_ptr = nullptr; ///< Dirección de la función nativa ya resuelta.
        std::vector<uint32_t> patch_sites;            ///< Offsets de bytecode donde se debe parchear el CALL.
    };

    /**
     * @brief Hash para NativeImportKey.
     *
     * Combina los tres índices (módulo, función, firma) en un entero de 64 bits
     * para obtener un hash rápido y con baja colisión.
     */
    struct NativeImportKeyHash {
        size_t operator()(NativeImportKey const &k) const noexcept {
            uint64_t h = k.module_idx;
            h          = (h << 20) ^ k.function_idx;
            h          = (h << 20) ^ k.signature_idx;
            return std::hash<uint64_t>{}(h);
        }
    };

    /**
     * @brief Sistema de FFI (Foreign Function Interface) de VestaVM.
     *
     * Esta clase gestiona:
     *   - Internado de cadenas (módulos, funciones, firmas)
     *   - Tabla de símbolos nativos agrupados por clave
     *   - Patch-sites para cada símbolo
     *   - Resolución de funciones nativas en tiempo de carga
     *
     * El objetivo es evitar redundancia, acelerar comparaciones y permitir
     * un linking nativo eficiente y escalable.
     */
    class FFI {
    public:
        /**
         * @name Tablas de strings internadas
         * @{
         */

        /**
         * @brief Tabla de nombres de módulos nativos.
         *
         * Cada entrada se almacena una sola vez. Ejemplos:
         *   - "kernel32.dll"
         *   - "user32.dll"
         *   - "libc.so"
         */
        std::vector<std::string> modules;

        /**
         * @brief Mapa para internar módulos.
         *
         * Permite convertir un string -> índice en O(1).
         */
        std::unordered_map<std::string, uint32_t> module_map;

        /**
         * @brief Tabla de nombres de funciones nativas.
         *
         * Ejemplos:
         *   - "GetTickCount"
         *   - "read"
         *   - "send"
         */
        std::vector<std::string> functions;

        /**
         * @brief Mapa para internar funciones.
         */
        std::unordered_map<std::string, uint32_t> function_map;

        /**
         * @brief Tabla de firmas de funciones nativas.
         *
         * Ejemplos:
         *   - "(void)->u32"
         *   - "(i32,i32)->i32"
         *   - "(str)->void"
         */
        std::vector<std::string> signatures;

        /**
         * @brief Mapa para internar firmas.
         */
        std::unordered_map<std::string, uint32_t> signature_map;

        /** @} */

        /**
         * @brief Tabla de símbolos nativos agrupados por clave.
         *
         * Cada entrada representa un símbolo único:
         *     módulo + función + firma
         *
         * Y contiene todos los offsets de bytecode donde debe parchearse.
         */
        std::unordered_map<NativeImportKey, NativeImportRuntime, NativeImportKeyHash> imports;

        /**
         * @brief Interna un nombre de módulo y devuelve su índice.
         *
         * Si el módulo ya existe, devuelve el índice existente.
         * Si no existe, lo añade a la tabla `modules` y devuelve el nuevo índice.
         *
         * @param name Nombre del módulo (p. ej. "kernel32.dll").
         * @return Índice único en la tabla `modules`.
         */
        uint32_t intern_module(const std::string &name);

        /**
         * @brief Interna un nombre de función nativa.
         *
         * Igual que intern_module, pero para funciones.
         *
         * @param name Nombre de la función (p. ej. "GetTickCount").
         * @return Índice único en la tabla `functions`.
         */
        uint32_t intern_function(const std::string &name);

        /**
         * @brief Interna una firma de función nativa.
         *
         * Las firmas describen los tipos de argumentos y retorno.
         *
         * @param sig Firma textual (p.ej. "(i32,i32)->i32").
         * @return Índice único en la tabla `signatures`.
         */
        uint32_t intern_signature(const std::string &sig);


        /**
         * cache de módulos nativos
         */
        std::unordered_map<std::string, void *> native_modules;

        /**
         * Permite obtener el base address de un modulo(. os/.dll) del sistema
         * @param name nombre del modulo a cargar
         * @return direccion base del modulos
         */
        void *load_native_module(const std::string &name);

        /**
         * Permite resolver la direccion del metodo
         * @param module base address del modulo del sistema del que obtener una
         * direccion
         * @param func nombre del metodo que cargar
         * @return direccion nativa del metodo.
         */
        void *resolve_native_symbol(void *module, const std::string &func);

        /**
         * Permite resolver todas las instrucciones que fueran indicadas
         * en la tabla de importacion
         * @param file_base puntero a el archivo base
         * @param offset_real_bytecode offset al bytecode real a parchear
         */
        void resolve_all(uint8_t *file_base, uint64_t offset_real_bytecode);
    };
} // namespace ffi
#endif //NATIVE_FFI_H
