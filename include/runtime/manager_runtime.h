/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file manager_runtime.h
 * @brief Gestor de instancias VM y clases auxiliares de red TLS.
 *
 * Define:
 *   - ManageVM: gestor que posee multiples instancias VM, un ArenaManager publico,
 *     un Loader publico y un listener TCP opcional.
 *   - ManagerTLSConnection: conexion TLS gestionada por el manager (envia saludo y cierra).
 *   - ManagerTCPListener: servidor TCP que crea ManagerTLSConnection al aceptar clientes.
 *   - Helpers estaticos: vm_summary() y print_vm_list_table().
 */

#ifndef MANAGER_RUNTIME_H
#define MANAGER_RUNTIME_H

#include <memory>
#include <vector>

#include "net/tcp_server.h"
#include "runtime.h"
#include "cli/sync_io.h"
#include "loader/loader.h"
#include "openssl/ssl.h"
#include "util/sqlite_singleton.h"

namespace runtime {

    class VM; ///< Declaracion adelantada de la instancia VM

    /**
     * @brief Genera un resumen textual de una instancia VM para impresion en tablas.
     *
     * Devuelve "<null>" si el puntero es nulo, o una cadena con la direccion
     * del puntero.  Adaptar para incluir ID o nombre si VM lo expone.
     *
     * @param vm Puntero a la instancia VM (puede ser nullptr).
     * @return   Cadena con el resumen de la VM.
     */
    static std::string vm_summary(VM *vm) {
        if (!vm) return "<null>";
        std::ostringstream ss;
        ss << "ptr=" << vm; // mostrar direccion hasta que VM exponga un ID publico
        return ss.str();
    }

    /**
     * @brief Imprime una tabla con el resumen de todas las instancias VM activas.
     *
     * Itera el vector de unique_ptr<VM> y llama a to_string() de cada instancia,
     * escribiendo el resultado en el stream de salida indicado.
     *
     * @param out Stream de salida donde se imprime la tabla.
     * @param vms Vector de instancias VM a listar.
     */
    static void print_vm_list_table(std::ostream &out,
                                    const std::vector<std::unique_ptr<runtime::VM> > &vms) {
        for (size_t i = 0; i < vms.size(); ++i) {
            const runtime::VM *vm = vms[i].get();
            out << vm->to_string() << '\n'; // usar to_string() de cada instancia
        }
    }


    /**
     * @brief Conexion TLS gestionada por el ManagerTCPListener.
     *
     * Implementacion minima: al iniciarse envia un saludo de prueba y cierra
     * la conexion TLS de forma ordenada.  Derivar para implementar protocolos reales.
     */
    class ManagerTLSConnection : public TLSConnection {
    public:
        /**
         * @brief Construye una conexion TLS con el descriptor de socket y el contexto SSL.
         *
         * @param fd  Descriptor de socket del cliente aceptado.
         * @param ssl Puntero al objeto SSL asociado al socket.
         */
        ManagerTLSConnection(socket_t fd, SSL *ssl) : TLSConnection(fd, ssl) {}

        /**
         * @brief Inicia la sesion TLS: envia un mensaje de saludo y cierra la conexion.
         *
         * Escribe el texto "Hola Mundo desde TLS!\n" como bytes y llama a SSL_shutdown()
         * hasta que se complete el handshake de cierre (ret != 0).
         */
        void start() override {
            std::string msg = "Hola Mundo desde TLS!\n";
            std::vector<uint8_t> buf(msg.begin(), msg.end());
            write_data(buf); // enviar el mensaje al cliente

            // cierre TLS ordenado: SSL_shutdown puede necesitar varios intentos
            int ret = 0;
            do { ret = SSL_shutdown(get_ssl()); } while (ret == 0);

            stop(); // cerrar el socket
        }
    };

    /**
     * @brief Servidor TCP que acepta conexiones y crea instancias ManagerTLSConnection.
     *
     * Hereda de TCPServer y sobreescribe create_connection() para instanciar
     * el tipo concreto de conexion gestionada por el manager.
     */
    class ManagerTCPListener : public TCPServer {
    public:
        /**
         * @brief Construye el listener en el puerto indicado con el contexto TLS dado.
         *
         * @param port Puerto TCP en el que el servidor escucha conexiones entrantes.
         * @param ctx  Contexto TLS (puede ser nullptr para conexiones sin cifrado).
         */
        ManagerTCPListener(uint16_t port, TLSContext *ctx) : TCPServer(port, ctx) {}

    protected:
        /**
         * @brief Crea una nueva conexion TLS al aceptar un cliente.
         *
         * Si TLS esta habilitado realiza el handshake SSL_accept(); si falla
         * imprime los errores en stderr y devuelve nullptr.
         *
         * @param client_fd Descriptor de socket del cliente aceptado.
         * @param ctx       Contexto TLS del servidor.
         * @return          Nueva ManagerTLSConnection, o nullptr si falla el handshake.
         */
        Connection *create_connection(socket_t client_fd, TLSContext *ctx) override {
            SSL *ssl = nullptr;
            if (tls_enabled && ctx) {
                ssl = SSL_new(ctx->get());           // crear objeto SSL para esta conexion
                SSL_set_fd(ssl, (int) client_fd);   // asociar el descriptor al SSL
                if (SSL_accept(ssl) <= 0) {
                    ERR_print_errors_fp(stderr);    // imprimir errores de OpenSSL
                    SSL_free(ssl);                  // liberar SSL en caso de fallo
                    return nullptr;                 // rechazar la conexion
                }
            }
            return new ManagerTLSConnection(client_fd, ssl); // conexion aceptada
        }
    };

    /**
     * @brief Gestor de multiples instancias VM con ArenaManager y Loader publicos.
     *
     * ManageVM posee el ciclo de vida de todas las instancias VM que crea.
     * Proporciona un ArenaManager compartido para memoria publica entre instancias
     * y un Loader publico para resolver simbolos entre ellas.
     *
     * Las VMs se almacenan como unique_ptr<VM> para garantizar:
     *   - Propiedad exclusiva sin riesgo de doble liberacion.
     *   - Estabilidad de los punteros tras realocaciones del vector interno.
     *   - Seguridad multihilo mediante vm_mutex al acceder al vector.
     *
     * Ciclo de vida tipico:
     *   1. ManageVM mgr(listener, id)      - construir el gestor.
     *   2. uint64_t vm_id = mgr.create_vm(N) - crear instancias con N schedulers.
     *   3. VM *vm = mgr.get_vm(vm_id)       - obtener y operar con la instancia.
     *   4. mgr.destroy_vm(vm_id)            - destruir instancia individual.
     *   5. mgr.~ManageVM()                  - destruye todas las VMs restantes.
     */
    class ManageVM {
    public:
        uint64_t id; ///< Identificador unico del gestor dentro del sistema

        /**
         * @brief Nombre identificativo del gestor para diagnostico y logs.
         */
        std::string name_manager;

        /**
         * @brief ArenaManager publico compartido entre todas las instancias VM del gestor.
         *
         * Permite que distintas VMs compartan bloques de memoria pre-alocalizada
         * sin necesidad de acceder al heap del SO en cada operacion.
         */
        vm::ArenaManager manager_mem{};

        /**
         * @brief Listener TCP opcional para aceptar conexiones de VMs remotas.
         *
         * Puede ser nullptr si el gestor opera unicamente en modo local sin red.
         */
        ManagerTCPListener *listener = nullptr;

        /**
         * @brief Loader publico del gestor para resolver simbolos entre instancias.
         *
         * Permite que multiples VMs carguen y compartan modulos de bytecode.
         */
        loader::Loader loader{*this};

        /**
         * @brief Construye el gestor de instancias VM.
         *
         * @param listener Listener TCP (puede ser nullptr para modo local).
         * @param id       Identificador unico del gestor.
         */
        ManageVM(ManagerTCPListener *listener, uint64_t id);

        /**
         * @brief Destructor: destruye todas las instancias VM gestionadas.
         *
         * Llama a destroy_all_vms() para liberar recursos en orden.
         */
        ~ManageVM();

        /**
         * @brief Crea una nueva instancia VM con el numero de schedulers indicado.
         *
         * @param num_schedulers Numero de hilos nativos que puede usar la instancia.
         * @return ID unico asignado a la nueva VM.
         */
        uint64_t create_vm(size_t num_schedulers);

        /**
         * @brief Destruye la instancia VM con el ID indicado.
         *
         * @param vm_id ID de la VM a destruir.
         * @return true si la VM fue encontrada y destruida; false si no existe.
         */
        bool destroy_vm(uint64_t vm_id);

        /**
         * @brief Devuelve un puntero a la instancia VM con el ID indicado.
         *
         * La direccion devuelta es estable mientras la VM no sea destruida.
         *
         * @param vm_id ID de la VM a buscar.
         * @return Puntero a la VM, o nullptr si no existe.
         */
        VM *get_vm(uint64_t vm_id);

        /**
         * @brief Comprueba si existe una instancia VM con el ID indicado.
         *
         * @param vm_id ID a verificar.
         * @return true si existe; false en caso contrario.
         */
        bool has_vm(uint64_t vm_id) const;

        /**
         * @brief Devuelve el numero de instancias VM actualmente gestionadas.
         *
         * @return Numero de VMs activas en el vector de instancias.
         */
        size_t vm_count() const;

        /**
         * @brief Destruye todas las instancias VM y reinicia el contador de IDs.
         *
         * Equivale a un emergency cleanup: libera todos los recursos de las VMs.
         */
        void destroy_all_vms();

        /**
         * @brief Vector de instancias VM gestionadas por este manager.
         *
         * Se usa unique_ptr<VM> para propiedad exclusiva, estabilidad de punteros
         * y seguridad multihilo junto con vm_mutex.
         */
        std::vector<std::unique_ptr<VM> > vms;

        /**
         * @brief Imprime el estado del gestor en la salida sincronizada (stdout).
         */
        void print_vm_manager_info();

        /**
         * @brief Genera un resumen textual del estado del gestor.
         *
         * @return Cadena con nombre, contador, numero de VMs, listener, loader y tabla de VMs.
         */
        std::string to_string_vm_manager_info() const;

    private:
        mutable std::mutex vm_mutex; ///< Protege el acceso concurrente al vector vms

        /**
         * @brief Contador monotonamente creciente para asignar IDs unicos a las VMs.
         *
         * Nunca se reutiliza un ID, ni siquiera tras destruir una VM.
         */
        uint64_t counter_vm = 0;
    };

} // namespace runtime

#endif // MANAGER_RUNTIME_H
