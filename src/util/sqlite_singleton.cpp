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

#include "util/sqlite_singleton.h"
#include <iostream>
#include <sstream>
#include "json.hpp"

using json = nlohmann::json;

/**
 * "Puntero inteligente con destructor personalizado"
 */
std::unique_ptr<sqlite3, SqliteSingleton::SqliteDeleter> SqliteSingleton::db_instance = nullptr;

/**
 * Mutex para evitar el Race Condition
 */
std::mutex SqliteSingleton::db_mutex{};
bool       SqliteSingleton::initialized = false;

sqlite3 *SqliteSingleton::get_instance(const std::string &db_path) {
    // Solo 1 thread pasa a la vez
    std::lock_guard<std::mutex> lock(db_mutex);

    if (!initialized) {
        sqlite3 *raw_db = nullptr;
        int      rc     = sqlite3_open(db_path.c_str(), &raw_db);
        if (rc != SQLITE_OK) {
            std::cerr << "Error SQLite: " << sqlite3_errmsg(raw_db) << std::endl;
            sqlite3_close(raw_db);
            return nullptr;
        }
        db_instance.reset(raw_db); // Transfer ownership
        // SqliteDeleter() se ejecuta
        initialized = true;
        std::cout << "SQLite conectado: " << db_path << std::endl;
    }

    return db_instance.get();
}

void SqliteSingleton::cleanup() {
    // Solo 1 thread pasa a la vez
    std::lock_guard<std::mutex> lock(db_mutex);
    if (db_instance) {
        sqlite3_close(db_instance.get());
        db_instance.reset();
        initialized = false;
        std::cout << "SQLite desconectado." << std::endl;
    }
}

bool SqliteSingleton::execute(const std::string &sql) {
    sqlite3 *db = get_instance();
    if (!db) return false;

    char *err_msg = nullptr;
    int   rc      = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::cerr << "Error SQL: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

json SqliteSingleton::execute_json(const std::string &sql) {
    sqlite3 *db = get_instance();
    if (!db) {
        return json{{"error", "No database connection"}};
    }

    json result = json::array();

    auto callback = [](void *data, int argc, char **argv, char **col_name) -> int {
        // <- AQUi
        json &row = *static_cast<json *>(data);
        json  obj;
        for (int i = 0; i < argc; i++) {
            std::string col = col_name[i] ? col_name[i] : std::to_string(i);
            obj[col]        = argv[i] ? argv[i] : "";
        }
        row.push_back(obj);
        return 0;
    };

    char *err_msg = nullptr;
    json  rows; // Local para callback
    int   rc = sqlite3_exec(db, sql.c_str(), callback, &rows, &err_msg);

    if (rc != SQLITE_OK) {
        json error = {{"error", std::string(err_msg)}};
        sqlite3_free(err_msg);
        return error;
    }

    return rows;
}
