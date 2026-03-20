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

#ifndef SQLITESINGLETON_H
#define SQLITESINGLETON_H

#include <sqlite3.h>
#include <string>
#include <memory>
#include <mutex>

#include "json.hpp"


/**
 * @class SqliteSingleton
 * @brief Singleton thread-safe para gestion de base de datos SQLite en VMProject.
 *
 * Proporciona acceso global unico a una instancia de SQLite con:
 * - Inicializacion lazy (primera llamada)
 * - Thread-safety con mutex
 * - Gestion automatica de memoria (RAII)
 * - Queries simples y JSON nativo para bytecode/lexer/parser
 *
 * **Uso tipico en VMProject:**
 * @code
 * SqliteSingleton::execute("CREATE TABLE bytecode(id INTEGER PRIMARY KEY, code BLOB)");
 * auto result = SqliteSingleton::execute_json("SELECT code FROM bytecode WHERE id=1");
 * @endcode
 */
class SqliteSingleton {
private:
    // Constructor privado
    SqliteSingleton() = default;

    /**
     * @struct SqliteDeleter
     * @brief Custom deleter para std::unique_ptr<sqlite3>
     *
     * Llama sqlite3_close() automaticamente en el destructor del unique_ptr,
     * evitando memory leaks incluso con excepciones.
     */
    struct SqliteDeleter {
        /**
         * @brief Cierra la conexion SQLite si existe
         * @param db Puntero a sqlite3 (puede ser nullptr)
         */
        void operator()(sqlite3 *db) const {
            if (db) sqlite3_close(db);
        }
    };

    static std::unique_ptr<sqlite3, SqliteDeleter> db_instance;
    static std::mutex                              db_mutex;
    static bool                                    initialized;

    static constexpr const char *DEFAULT_DB_PATH = "vm_data.db";

public:
    SqliteSingleton(const SqliteSingleton &) = delete;

    SqliteSingleton &operator=(const SqliteSingleton &) = delete;

    SqliteSingleton(SqliteSingleton &&) = delete;

    SqliteSingleton &operator=(SqliteSingleton &&) = delete;

    /**
     * @brief Obtiene la instancia unica de SQLite (lazy initialization)
     * @param db_path Ruta a la base de datos (default: "vm_data.db")
     * @return Puntero a sqlite3 o nullptr si falla
     *
     * **Thread-safe**: Solo un thread inicializa la DB.
     * Otros threads esperan y reutilizan la misma instancia.
     *
     * @note Primera llamada crea la DB, siguientes retornan cache.
     */
    static sqlite3 *get_instance(const std::string &db_path = DEFAULT_DB_PATH);

    /**
     * @brief Cierra la conexion SQLite y resetea el singleton
     *
     * Llama sqlite3_close() via custom deleter.
     * util para graceful shutdown de la VM.
     *
     * **Thread-safe**: Protegido por mutex interno.
     */
    static void cleanup();

    /**
     * @brief Ejecuta SQL simple (CREATE, INSERT, UPDATE, DELETE)
     * @param sql Query SQL a ejecutar
     * @return true si exito, false si error
     *
     * **No retorna datos**. Para SELECT usa execute_json().
     * Maneja errores internamente y los imprime en stderr.
     *
     * @code
     * SqliteSingleton::execute("CREATE TABLE IF NOT EXISTS lexer_tokens (token TEXT, line INTEGER)");
     * @endcode
     */
    static bool execute(const std::string &sql);

    /**
     * @brief Ejecuta SELECT y retorna resultados como JSON array
     * @param sql Query SELECT
     * @return Array de objetos JSON: [{"col1": "val1", "col2": 123}, ...]
     *
     * **Perfecto para VMProject**:
     * - Bytecode storage/retrieval
     * - Lexer token debugging
     * - Parser AST serialization
     *
     * @code
     * auto tokens = SqliteSingleton::execute_json("SELECT * FROM lexer_tokens");
     * // [{"token": "CLASS", "line": 5}, {"token": "IDENTIFIER", "line": 6}]
     * @endcode
     *
     * @note Error retorna: {"error": "mensaje"}
     */
    static nlohmann::json execute_json(const std::string &sql);
};

#endif //SQLITESINGLETON_H
