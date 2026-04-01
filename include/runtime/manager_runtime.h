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

#ifndef MANAGER_RUNTIME_H
#define MANAGER_RUNTIME_H

#include <memory>
#include <vector>

#include "runtime.h"
#include "cli/sync_io.h"
#include "loader/loader.h"
#include "net/tcp_server.h"
#include "openssl/ssl.h"
#include "util/sqlite_singleton.h"

namespace runtime {
    class VM;

    // Helper: convierte un puntero a VM en string (por defecto muestra la dirección).
    // Si tiene métodos públicos (p.ej. id(), name(), to_string()), adaptar aquí.
    static std::string vm_summary(VM *vm) {
        if (!vm) return "<null>";
        std::ostringstream ss;
        ss << "ptr=" << vm;
        // Ejemplo: si VM tuviera un metodo id(): ss << " id=" << vm->id();
        // Ejemplo: si VM tuviera to_string(): ss << " " << vm->to_string();
        return ss.str();
    }

    // Helper: imprime una tabla simple con índice y resumen de VM
    static void print_vm_list_table(std::ostream &out, const std::vector<runtime::VM *> &vms) {
        for (size_t i = 0; i < vms.size(); ++i) {
            const VM *vm = vms[i];
            out << vm->to_string() << std::endl;
        }
    }

    class ManagerTLSConnection : public TLSConnection {
    public:
        ManagerTLSConnection(socket_t fd, SSL *ssl) : TLSConnection(fd, ssl) {
        }

        void start() override {
            // Enviar mensaje
            std::string msg = "Hola Mundo desde TLS!\n";
            std::vector<uint8_t> buf(msg.begin(), msg.end());
            write_data(buf);

            // Cierre TLS seguro
            int ret = 0;
            do { ret = SSL_shutdown(get_ssl()); } while (ret == 0);

            stop();
        }
    };

    class ManagerTCPListener : public TCPServer {
    public:
        ManagerTCPListener(uint16_t port, TLSContext *ctx) : TCPServer(port, ctx) {
        }

    protected:
        Connection *create_connection(socket_t client_fd, TLSContext *ctx) override {
            SSL *ssl = nullptr;
            if (tls_enabled && ctx) {
                ssl = SSL_new(ctx->get());
                SSL_set_fd(ssl, (int) client_fd);
                if (SSL_accept(ssl) <= 0) {
                    ERR_print_errors_fp(stderr);
                    SSL_free(ssl);
                    return nullptr;
                }
            }

            // devolvemos una conexion de tipo manager.
            return new ManagerTLSConnection(client_fd, ssl);
        }
    };

    class ManageVM {
    public:
        /**
         * Nombre del manager
         */
        std::string name_manager;

        /**
         * El manager puede gestionar memoria compartida entre instancias.
         */
        vm::ArenaManager manager_mem;

        /**
         * Cada instancia de VM puede escuchar en un puerto distinto.
         * No todas las instancias tienen por que ser un servicio de red,
         * pueden ejecutarse unicamente a nivel local.
         */
        ManagerTCPListener *listener;

        /**
         * El manager puede tener un loader publico
         */
        loader::Loader loader;

        /**
         * Base de datos para consultar usuario y credenciales.
         * Tambien se puede hacer registro de logs.
         */
        // Sqlite::SqliteSingleton &db; // no hace falta añadirlo por que es un singleton con metodos estaticos

        // Constructor por defecto
        ManageVM(ManagerTCPListener *listener);

        // Destructor - libera recursos
        ~ManageVM();

        /**
         * @brief Crea una nueva VM con ID unico
         * @return ID de la VM creada (0 si falla)
         */
        uint64_t create_vm();

        /**
         * @brief Destruye una VM por su ID
         * @param vm_id ID de la VM a eliminar
         * @return true si se elimino correctamente
         */
        bool destroy_vm(uint64_t vm_id);

        /**
         * @brief Obtiene VM const por ID
         * @param vm_id ID de la VM
         * @return Referencia const a VM (undefined si no existe)
         */
        VM *get_vm(uint64_t vm_id);

        /**
         * @brief Verifica si existe una VM con el ID dado
         * @param vm_id ID a verificar
         * @return true si existe
         */
        bool has_vm(uint64_t vm_id) const;

        /**
         * @brief Obtiene numero total de VMs activas
         * @return Cantidad de VMs
         */
        size_t vm_count() const;

        /**
         * @brief Libera todas las VMs (emergency cleanup)
         */
        void destroy_all_vms();

        // contiene todas las instancias de maquinas
        std::vector<VM *> vms;

        void print_vm_manager_info();

        std::string to_string_vm_manager_info() const;

    private:
        // se usa para generar los ID's de la VM, las VM nuevas no tendran un mismo ID
        // que una instancia ya creada o muerta en el mismo manager.
        uint64_t counter_vm;
    };
}

#endif //MANAGER_RUNTIME_H
