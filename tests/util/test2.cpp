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


// test_sqlite_singleton.cpp
// Test minimalista para SqliteSingleton usando solo C/C++ estándar y la API de sqlite3.
// Compilar con: g++ -std=c++17 test_sqlite_singleton.cpp -lsqlite3 -pthread -o test_sqlite_singleton

#include <atomic>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <filesystem>
#include <chrono>

#include "cli/sync_io.h"
#include "util/sqlite_singleton.h"


// Helper: elimina fichero si existe
static void remove_file_if_exists(const std::string &p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    (void) ec;
}

// Helper: ejecutar query y recoger filas (usando sqlite3 API)
static std::vector<std::vector<std::string> > query_rows(sqlite3 *db, const std::string &sql) {
    std::vector<std::vector<std::string> > rows;
    char *errmsg = nullptr;

    auto cb = [](void *userdata, int argc, char **argv, char **colname) -> int {
        auto *out = static_cast<std::vector<std::vector<std::string> > *>(userdata);
        std::vector<std::string> row;
        for (int i = 0; i < argc; ++i) {
            row.emplace_back(argv[i] ? argv[i] : "");
        }
        out->push_back(std::move(row));
        return 0;
    };

    int rc = sqlite3_exec(db, sql.c_str(), cb, &rows, &errmsg);
    if (rc != SQLITE_OK) {
        std::cerr << "query_rows: sqlite error: " << (errmsg ? errmsg : "(null)") << "\n";
        if (errmsg) sqlite3_free(errmsg);
        return {};
    }
    return rows;
}

// Test 1: crear instancia y cleanup
static bool test_create_and_cleanup(const std::string &dbpath) {
    remove_file_if_exists(dbpath);
    sqlite3 *db = Sqlite::SqliteSingleton::get_instance(dbpath);
    if (!db) {
        std::cerr << "[FAIL] get_instance returned nullptr\n";
        return false;
    }
    // comprobar que sqlite3_open devolvió handle válido consultando pragma user_version
    auto rows = query_rows(db, "PRAGMA user_version;");
    if (rows.empty()) {
        std::cerr << "[FAIL] PRAGMA user_version returned no rows\n";
        Sqlite::SqliteSingleton::cleanup();
        return false;
    }
    Sqlite::SqliteSingleton::cleanup();
    remove_file_if_exists(dbpath);
    std::cout << "[PASS] test_create_and_cleanup\n";
    return true;
}

// Test 2: execute create/insert and verify with query_rows
static bool test_execute_create_insert(const std::string &dbpath) {
    remove_file_if_exists(dbpath);
    sqlite3 *db = Sqlite::SqliteSingleton::get_instance(dbpath);
    if (!db) {
        std::cerr << "[FAIL] get_instance returned nullptr\n";
        return false;
    }

    bool ok = Sqlite::SqliteSingleton::execute("CREATE TABLE kv(key TEXT PRIMARY KEY, val TEXT);");
    if (!ok) {
        std::cerr << "[FAIL] CREATE TABLE failed\n";
        Sqlite::SqliteSingleton::cleanup();
        return false;
    }

    ok = Sqlite::SqliteSingleton::execute("INSERT INTO kv(key,val) VALUES('a','1'),('b','2');");
    if (!ok) {
        std::cerr << "[FAIL] INSERT failed\n";
        Sqlite::SqliteSingleton::cleanup();
        return false;
    }

    auto rows = query_rows(db, "SELECT key, val FROM kv ORDER BY key;");
    if (rows.size() != 2) {
        std::cerr << "[FAIL] Expected 2 rows, got " << rows.size() << "\n";
        Sqlite::SqliteSingleton::cleanup();
        return false;
    }
    if (rows[0].size() < 2 || rows[0][0] != "a" || rows[0][1] != "1") {
        std::cerr << "[FAIL] Row 0 mismatch\n";
        Sqlite::SqliteSingleton::cleanup();
        return false;
    }
    if (rows[1].size() < 2 || rows[1][0] != "b" || rows[1][1] != "2") {
        std::cerr << "[FAIL] Row 1 mismatch\n";
        Sqlite::SqliteSingleton::cleanup();
        return false;
    }

    Sqlite::SqliteSingleton::cleanup();
    remove_file_if_exists(dbpath);
    std::cout << "[PASS] test_execute_create_insert\n";
    return true;
}

// Test 3: execute returns false on bad SQL
static bool test_execute_bad_sql(const std::string &dbpath) {
    std::string err;
    remove_file_if_exists(dbpath);
    sqlite3 *db = Sqlite::SqliteSingleton::get_instance(dbpath);
    if (!db) {
        std::cerr << "[FAIL] get_instance returned nullptr\n";
        return false;
    }

    bool ok = Sqlite::SqliteSingleton::execute("THIS IS NOT SQL;", &err);
    if (ok) {
        std::cerr << "[FAIL] execute returned true for invalid SQL" << err << "\n";
        Sqlite::SqliteSingleton::cleanup();
        return false;
    } else {
        std::cerr << "[PASS] Fallo con exitosamente: " << err << "\n";
    }

    Sqlite::SqliteSingleton::cleanup();
    remove_file_if_exists(dbpath);
    std::cout << "[PASS] test_execute_bad_sql\n";
    return true;
}

// Test 4: concurrencia simple - varios hilos piden la instancia
static bool test_concurrency_get_instance(const std::string &dbpath) {
    remove_file_if_exists(dbpath);
    const int N = 8;
    std::vector<std::thread> threads;
    std::vector<sqlite3 *> results(N, nullptr);

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([i, &results, &dbpath]() {
            results[i] = Sqlite::SqliteSingleton::get_instance(dbpath);
        });
    }
    for (auto &t: threads) t.join();

    // comprobar no nulos y todos iguales
    for (int i = 0; i < N; ++i) {
        if (!results[i]) {
            std::cerr << "[FAIL] thread " << i << " got nullptr\n";
            Sqlite::SqliteSingleton::cleanup();
            remove_file_if_exists(dbpath);
            return false;
        }
        if (results[i] != results[0]) {
            std::cerr << "[FAIL] thread " << i << " got different pointer\n";
            Sqlite::SqliteSingleton::cleanup();
            remove_file_if_exists(dbpath);
            return false;
        }
    }

    Sqlite::SqliteSingleton::cleanup();
    remove_file_if_exists(dbpath);
    std::cout << "[PASS] test_concurrency_get_instance\n";
    return true;
}

// Test 5: múltiples hilos ejecutan SELECT concurrentemente
static bool test_concurrent_selects_verbose(const std::string &dbpath) {
    std::string err;
    remove_file_if_exists(dbpath);

    // Preparar DB con datos
    sqlite3 *db = Sqlite::SqliteSingleton::get_instance(dbpath);
    if (!db) {
        std::cerr << "[FAIL] get_instance returned nullptr\n";
        return false;
    }

    if (!Sqlite::SqliteSingleton::execute("CREATE TABLE kv2(id INTEGER PRIMARY KEY, val TEXT);", &err)) {
        std::cerr << "[FAIL] CREATE TABLE kv2 failed" << err << "\n";
        Sqlite::SqliteSingleton::cleanup();
        return false;
    }

    // Insertar 100 filas en transacción
    {
        std::string sql = "BEGIN TRANSACTION;";
        for (int i = 0; i < 100; ++i) {
            sql += "INSERT INTO kv2(id,val) VALUES(" + std::to_string(i) + ",'v" + std::to_string(i) + "');";
        }
        sql += "COMMIT;";
        if (!Sqlite::SqliteSingleton::execute(sql, &err)) {
            std::cerr << "[FAIL] bulk INSERT failed" << err << "\n";
            Sqlite::SqliteSingleton::cleanup();
            return false;
        }
    }

    // Worker: ejecuta SELECT y valida; imprime progreso cada X iteraciones
    auto worker = [&](int thread_id, int iterations, std::atomic<int> &errors, int progress_every) {
        for (int it = 0; it < iterations; ++it) {
            auto rows = query_rows(db, "SELECT id, val FROM kv2 ORDER BY id;");
            if (rows.size() != 100) {
                std::cerr << "[thread " << thread_id << "] unexpected row count: " << rows.size() << " (iter " << it <<
                        ")\n";
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (rows[0][0] != "0" || rows[0][1] != "v0") {
                errors.fetch_add(1);
                return;
            }
            if (rows[50][0] != "50" || rows[50][1] != "v50") {
                errors.fetch_add(1);
                return;
            }
            if (rows[99][0] != "99" || rows[99][1] != "v99") {
                errors.fetch_add(1);
                return;
            }

            if (progress_every > 0 && (it % progress_every) == 0) {
                // Mensaje de progreso (no demasiado frecuente)
                std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                std::cout << "[thread " << thread_id << "] iter " << it << "/" << iterations << "\n";
            }

            // pequeña pausa para aumentar concurrencia
            std::this_thread::yield();
        }
    };

    const int THREADS = 8;
    const int ITERATIONS = 200;
    const int PROGRESS_EVERY = 50;

    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker, i, ITERATIONS, std::ref(errors), PROGRESS_EVERY);
    }
    for (auto &t: threads) t.join();

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    Sqlite::SqliteSingleton::cleanup();
    remove_file_if_exists(dbpath);

    if (errors.load(std::memory_order_relaxed) == 0) {
        std::cout << "[PASS] test_concurrent_selects_verbose (time " << ms << " ms)\n";
        return true;
    } else {
        std::cerr << "[FAIL] test_concurrent_selects_verbose: errors = " << errors.load() << " (time " << ms <<
                " ms)\n";
        return false;
    }
}

static std::vector<std::map<std::string, std::string> > query_json_rows_map(
    const std::string &sql, std::string *err = nullptr) {
    Sqlite::json j = Sqlite::SqliteSingleton::execute_json(sql);
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

// Test: múltiples hilos ejecutan SELECT concurrentemente usando execute_json
static bool test_concurrent_selects_json(const std::string &dbpath) {
    std::string err;
    remove_file_if_exists(dbpath);

    // Preparar DB con datos
    sqlite3 *db = Sqlite::SqliteSingleton::get_instance(dbpath);
    if (!db) {
        std::cerr << "[FAIL] get_instance returned nullptr\n";
        return false;
    }

    if (!Sqlite::SqliteSingleton::execute("CREATE TABLE kv2(id INTEGER PRIMARY KEY, val TEXT);", &err)) {
        std::cerr << "[FAIL] CREATE TABLE kv2 failed: " << err << "\n";
        Sqlite::SqliteSingleton::cleanup();
        return false;
    }

    // Insertar 100 filas en transacción
    {
        std::string sql = "BEGIN TRANSACTION;";
        for (int i = 0; i < 100; ++i) {
            sql += "INSERT INTO kv2(id,val) VALUES(" + std::to_string(i) + ",'v" + std::to_string(i) + "');";
        }
        sql += "COMMIT;";
        if (!Sqlite::SqliteSingleton::execute(sql, &err)) {
            std::cerr << "[FAIL] bulk INSERT failed: " << err << "\n";
            Sqlite::SqliteSingleton::cleanup();
            return false;
        }
    }

    // Worker: ejecuta SELECT via query_json_rows_map y valida por nombre de columna
    auto worker = [&](int thread_id, int iterations, std::atomic<int> &errors, int progress_every) {
        for (int it = 0; it < iterations; ++it) {
            std::string local_err;
            auto rows = query_json_rows_map("SELECT id, val FROM kv2 ORDER BY id;", &local_err);
            Sqlite::print_table(std::cout, rows);
            
            if (!local_err.empty()) {
                std::cerr << "[thread " << thread_id << "] SELECT error: " << local_err << " (iter " << it << ")\n";
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            if (rows.size() != 100) {
                std::cerr << "[thread " << thread_id << "] unexpected row count: " << rows.size() << " (iter " << it <<
                        ")\n";
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            // Validar existencia de columnas y valores concretos de forma segura
            try {
                const auto &r0 = rows[0];
                const auto &r50 = rows[50];
                const auto &r99 = rows[99];

                auto it_id0 = r0.find("id");
                auto it_val0 = r0.find("val");
                if (it_id0 == r0.end() || it_val0 == r0.end()) {
                    std::cerr << "[thread " << thread_id << "] missing columns in row 0\n";
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (it_id0->second != "0" || it_val0->second != "v0") {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                auto it_id50 = r50.find("id");
                auto it_val50 = r50.find("val");
                if (it_id50 == r50.end() || it_val50 == r50.end()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (it_id50->second != "50" || it_val50->second != "v50") {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                auto it_id99 = r99.find("id");
                auto it_val99 = r99.find("val");
                if (it_id99 == r99.end() || it_val99 == r99.end()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (it_id99->second != "99" || it_val99->second != "v99") {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            } catch (const std::exception &e) {
                std::cerr << "[thread " << thread_id << "] validation exception: " << e.what() << "\n";
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            if (progress_every > 0 && (it % progress_every) == 0) {
                // Protege la salida por consola si usas un mutex global (vesta::cout_mutex en tu proyecto)
                std::lock_guard<std::mutex> lk(vesta::cout_mutex);
                std::cout << "[thread " << thread_id << "] iter " << it << "/" << iterations << "\n";
            }

            std::this_thread::yield();
        }
    };

    const int THREADS = 8;
    const int ITERATIONS = 200;
    const int PROGRESS_EVERY = 50;

    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker, i, ITERATIONS, std::ref(errors), PROGRESS_EVERY);
    }
    for (auto &t: threads) t.join();

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    Sqlite::SqliteSingleton::cleanup();
    remove_file_if_exists(dbpath);

    if (errors.load(std::memory_order_relaxed) == 0) {
        std::cout << "[PASS] test_concurrent_selects_json (time " << ms << " ms)\n";
        return true;
    } else {
        std::cerr << "[FAIL] test_concurrent_selects_json: errors = " << errors.load() << " (time " << ms << " ms)\n";
        return false;
    }
}

int main() {
    const std::string dbpath = "test_sqlite_singleton.db";
    int failures = 0;

    if (!test_create_and_cleanup(dbpath)) ++failures;
    if (!test_execute_create_insert(dbpath)) ++failures;
    if (!test_execute_bad_sql(dbpath)) ++failures;
    if (!test_concurrency_get_instance(dbpath)) ++failures;
    if (!test_concurrent_selects_verbose(dbpath)) ++failures;
    if (!test_concurrent_selects_json(dbpath)) ++failures;

    std::vector<std::map<std::string, std::string> > rows = {
        {{"id", "0"}, {"name", "Alice"}, {"age", "30"}},
        {{"id", "1"}, {"name", "Bob"}, {"age", "27"}},
        {{"id", "2"}, {"name", "Clara"}} // age ausente
    };

    // Imprimir con orden automático de columnas
    Sqlite::print_table(std::cout, rows);

    // Imprimir con orden explícito de columnas
    std::vector<std::string> order = {"id", "name", "age"};
    std::cout << "\nCon orden explícito:\n";
    Sqlite::print_table(std::cout, rows, order);

    // Obtener como string
    std::string s = Sqlite::table_to_string(rows, order);
    // usar s en logs o tests


    if (failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    } else {
        std::cerr << failures << " TEST(S) FAILED\n";
        return 1;
    }
}
