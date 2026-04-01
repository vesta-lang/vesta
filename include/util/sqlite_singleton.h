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

#include <mutex>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>

#include "sqlite3.h"
#include "json.hpp"

namespace Sqlite {
    using json = nlohmann::json;

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
        static std::mutex db_mutex;
        static bool initialized;

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
         * @param err podemos almacenar el error que ocurrio al realizar alguna sentencia.
         * @return true si exito, false si error
         *
         * **No retorna datos**. Para SELECT usa execute_json().
         * Maneja errores internamente y los imprime en stderr.
         *
         * @code
         * SqliteSingleton::execute("CREATE TABLE IF NOT EXISTS lexer_tokens (token TEXT, line INTEGER)");
         * @endcode
         */
        static bool execute(const std::string &sql, std::string *err = nullptr);

        /**
         * @brief Ejecuta una sentencia SQL y devuelve el resultado como un array JSON.
         *
         * Cada fila se representa como un objeto JSON donde las claves son los nombres
         * de columna y los valores son cadenas (o la representación JSON del valor).
         *
         * Ejemplo de retorno:
         * @code
         * [
         *   {"id": "0", "val": "v0"},
         *   {"id": "1", "val": "v1"}
         * ]
         * @endcode
         *
         * @param sql Sentencia SQL a ejecutar (normalmente un SELECT).
         * @return Un objeto nlohmann::json:
         * - Si la ejecución es correcta, devuelve un array JSON con las filas.
         * - Si ocurre un error, devuelve un objeto JSON con la clave "error" y el mensaje:
         *   {"error": "mensaje de error"}
         *
         * @note Esta función usa internamente sqlite3_exec y el callback construye
         *       el JSON fila a fila. Los valores se insertan como cadenas; si necesitas
         *       tipos nativos, conviértelos después de recibir el JSON.
         *
         * @warning La seguridad en concurrencia depende del modo de compilación de
         *          SQLite (serialized/multi-thread). Si SQLite no está en modo
         *          serialized, no es seguro compartir la misma sqlite3* entre hilos.
         *
         * @see query_json_rows_map para convertir el JSON en una estructura C++ más
         *      cómoda para tests (vector de mapas).
         */
        static nlohmann::json execute_json(const std::string &sql);

        /**
         * @brief Ejecuta una consulta SQL (usando execute_json) y devuelve las filas
         *        como un vector de mapas (columna -> valor).
         *
         * Este helper convierte la salida JSON de execute_json en una estructura
         * más cómoda para tests y código que prefiera acceso por nombre de columna.
         *
         * Cada fila se representa como std::map<std::string,std::string>, donde:
         * - la clave es el nombre de la columna,
         * - el valor es una cadena: si el valor JSON es null se devuelve cadena vacía,
         *   si es string se devuelve el string sin comillas, para otros tipos se usa
         *   json::dump() para obtener su representación textual.
         *
         * @param sql Sentencia SQL a ejecutar (normalmente un SELECT).
         * @param err Puntero opcional para recibir el mensaje de error. Si no es
         *            nullptr y ocurre un error, se escribe el mensaje en *err.
         *            Si no hay error, *err queda sin modificar o vacío.
         * @return Vector de filas; cada fila es un std::map con pares (columna, valor).
         *         Si ocurre un error, se devuelve un vector vacío y (si err != nullptr)
         *         se rellena con el mensaje de error.
         *
         * @example
         * @code
         * std::string err;
         * auto rows = SqliteSingleton::query_json_rows_map("SELECT id,val FROM kv2;", &err);
         * if (!err.empty()) {
         *     // manejar error
         * } else {
         *     for (const auto &row : rows) {
         *         std::string id = row.at("id");
         *         std::string val = row.at("val");
         *     }
         * }
         * @endcode
         *
         * @note No lanza excepciones por ausencia de columnas; comprueba la existencia
         *       de claves antes de acceder con at() si no estás seguro.
         */
        static std::vector<std::map<std::string, std::string> > query_json_rows_map(
            const std::string &sql, std::string *err = nullptr) {
            json j = SqliteSingleton::execute_json(sql);
            if (j.is_object() && j.contains("error")) {
                if (err) *err = j["error"].get<std::string>();
                return {};
            }
            std::vector<std::map<std::string, std::string> > rows;
            for (const auto &r: j) {
                if (!r.is_object()) continue;
                std::map<std::string, std::string> rowmap;
                for (auto it = r.begin(); it != r.end(); ++it) {
                    if (it.value().is_null()) rowmap[it.key()] = "";
                    else if (it.value().is_string()) rowmap[it.key()] = it.value().get<std::string>();
                    else rowmap[it.key()] = it.value().dump();
                }
                rows.push_back(std::move(rowmap));
            }
            return rows;
        }
    };

    /**
     * Mutex para impresion
     */
    static std::mutex print_mutex;
    // Imprime la tabla en el stream dado
    static void print_table(std::ostream &out,
                     const std::vector<std::map<std::string, std::string> > &rows,
                     const std::vector<std::string> &columns_order = {},
                     bool right_align_numbers = true,
                     size_t max_width = 60) {

        std::lock_guard<std::mutex> lk(print_mutex);
        // Determinar columnas (orden explícito si se pasa, si no, unión ordenada)
        std::vector<std::string> cols;
        if (!columns_order.empty()) {
            cols = columns_order;
        } else {
            std::set<std::string> colset;
            for (const auto &r: rows) for (const auto &p: r) colset.insert(p.first);
            cols.assign(colset.begin(), colset.end());
        }

        // Calcular anchos (mínimo ancho = longitud del nombre de columna)
        std::vector<size_t> width(cols.size());
        for (size_t i = 0; i < cols.size(); ++i) width[i] = cols[i].size();
        for (const auto &r: rows) {
            for (size_t i = 0; i < cols.size(); ++i) {
                auto it = r.find(cols[i]);
                if (it != r.end()) width[i] = std::max(width[i], std::min(max_width, it->second.size()));
            }
        }

        // Helper para imprimir separador
        auto print_sep = [&](char left, char mid, char right, char fill = '-') {
            out << left;
            for (size_t i = 0; i < cols.size(); ++i) {
                out << std::string(width[i] + 2, fill);
                if (i + 1 < cols.size()) out << mid;
            }
            out << right << "\n";
        };

        // Cabecera
        print_sep('+', '+', '+');
        out << "|";
        for (size_t i = 0; i < cols.size(); ++i) {
            out << " " << std::left << std::setw((int) width[i]) << cols[i] << " " << "|";
        }
        out << "\n";
        print_sep('+', '+', '+');

        // Filas
        for (const auto &r: rows) {
            out << "|";
            for (size_t i = 0; i < cols.size(); ++i) {
                auto it = r.find(cols[i]);
                std::string val = (it != r.end()) ? it->second : "";
                if (val.size() > max_width) val = val.substr(0, max_width - 3) + "...";

                // decidir alineación: números a la derecha si se pide
                bool is_number = !val.empty() && std::all_of(val.begin(), val.end(), [](char c) {
                    return (c >= '0' && c <= '9') || c == '.' || c == '-';
                });
                if (right_align_numbers && is_number) {
                    out << " " << std::right << std::setw((int) width[i]) << val << " " << "|";
                } else {
                    out << " " << std::left << std::setw((int) width[i]) << val << " " << "|";
                }
            }
            out << "\n";
        }

        // Pie
        print_sep('+', '+', '+');
    }

    // Variante que devuelve la tabla como string
    inline std::string table_to_string(const std::vector<std::map<std::string, std::string> > &rows,
                                       const std::vector<std::string> &columns_order = {},
                                       bool right_align_numbers = true,
                                       size_t max_width = 60) {
        std::ostringstream ss;
        print_table(ss, rows, columns_order, right_align_numbers, max_width);
        return ss.str();
    }
};
#endif //SQLITESINGLETON_H
