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

#ifndef ARENA_H
#define ARENA_H

#include <cstddef>
#include <iostream>
#include <unordered_map>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

#include "net/net.h"

#define ALIGN_4K(x) (((x) + 4095) & ~4095ULL)

namespace vm {
    /**
     * @enum MemPerm
     * @brief Permisos de memoria para buffers ejecutables o mapeos.
     *
     * Representa permisos de memoria que se pueden combinar mediante operaciones
     * bit a bit. Usado en funciones de asignacion y proteccion de memoria.
     *
     * @note Cada valor ocupa un bit en un uint8_t:
     *       - NONE  = 0
     *       - READ  = 0x1
     *       - WRITE = 0x2
     *       - EXEC  = 0x4
     *
     * @example
     * MemPerm p = MemPerm::READ | MemPerm::WRITE; // Lectura y escritura
     */
    enum class MemPerm : uint8_t {
        NONE  = 0,
        READ  = 1 << 0,
        WRITE = 1 << 1,
        EXEC  = 1 << 2,
    };

    /**
     * @brief Operador bit a bit OR para combinar permisos.
     *
     * Permite combinar multiples permisos de MemPerm usando `|`.
     *
     * @param a Primer permiso
     * @param b Segundo permiso
     * @return MemPerm Resultado de la combinacion bit a bit
     *
     * @example
     * MemPerm p = MemPerm::READ | MemPerm::WRITE; // Combina READ y WRITE
     */
    inline MemPerm operator|(MemPerm a, MemPerm b) {
        return static_cast<MemPerm>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }

    /**
     * @brief Comprueba si un conjunto de permisos contiene un permiso especifico.
     *
     * Esta funcion hace una comparacion bit a bit entre los permisos actuales
     * y el permiso que se desea verificar.
     *
     * @param perms Conjunto de permisos actuales (MemPerm), por ejemplo READ | WRITE.
     * @param test Permiso especifico que queremos verificar si esta activo.
     * @return true Si el permiso @p test esta activo dentro de @p perms.
     * @return false Si el permiso @p test no esta presente.
     *
     * @note MemPerm se espera que sea un enum con valores que representen bits individuales:
     *       por ejemplo: READ = 0x1, WRITE = 0x2, EXEC = 0x4.
     *
     * @example
     * MemPerm p = MemPerm::READ | MemPerm::WRITE;
     * bool can_exec = has_perm(p, MemPerm::EXEC); // false
     * bool can_write = has_perm(p, MemPerm::WRITE); // true
     */
    inline bool has_perm(MemPerm perms, MemPerm test) {
        return (static_cast<uint8_t>(perms) & static_cast<uint8_t>(test)) != 0;
    }

    /**
     * @brief Asigna memoria de manera portatil con permisos especificados.
     *
     * Esta funcion reserva un bloque de memoria de tamaño `size` redondeado
     * a multiplo de la pagina (4 KB) y establece los permisos de lectura,
     * escritura y ejecucion segun `perms`. Funciona tanto en Windows
     * como en sistemas POSIX.
     *
     * @param size Tamaño de memoria a reservar (en bytes)
     * @param perms Permisos de memoria (MemPerm::READ, MemPerm::WRITE, MemPerm::EXEC)
     * @return void* Puntero a la memoria reservada, o nullptr si falla.
     *
     * @note En Windows usa VirtualAlloc, en POSIX mmap.
     *       La memoria debe liberarse con free_memory().
     *
     * @example
     * void* buf = allocate_memory(4096, MemPerm::READ | MemPerm::WRITE);
     */
    inline void *allocate_memory(size_t size, MemPerm perms) {
        // Redondear al multiplo de pagina
#ifdef _WIN32
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        size = (size + si.dwPageSize - 1) & ~(si.dwPageSize - 1);
#else
    long pagesize = sysconf(_SC_PAGESIZE);
    size = (size + pagesize - 1) & ~(pagesize - 1);
#endif

#ifdef _WIN32
        DWORD flProtect = 0;
        if (has_perm(perms, MemPerm::EXEC)) {
            if (has_perm(perms, MemPerm::READ) && has_perm(perms, MemPerm::WRITE)) flProtect = PAGE_EXECUTE_READWRITE;
            else if (has_perm(perms, MemPerm::READ)) flProtect = PAGE_EXECUTE_READ;
            else if (has_perm(perms, MemPerm::WRITE)) flProtect = PAGE_EXECUTE_READWRITE; // no hay EXEC+WRITE solo
            else flProtect                                      = PAGE_EXECUTE;
        } else {
            if (has_perm(perms, MemPerm::READ) && has_perm(perms, MemPerm::WRITE)) flProtect = PAGE_READWRITE;
            else if (has_perm(perms, MemPerm::READ)) flProtect = PAGE_READONLY;
            else if (has_perm(perms, MemPerm::WRITE)) flProtect = PAGE_READWRITE; // no hay solo WRITE
            else flProtect                                      = PAGE_NOACCESS;
        }

        void *mem = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, flProtect);
        if (!mem) {
            std::cerr << "VirtualAlloc fallo\n";
            return nullptr;
        }
        return mem;
#else
    int prot = 0;
    if (has_perm(perms, MemPerm::READ)) prot |= PROT_READ;
    if (has_perm(perms, MemPerm::WRITE)) prot |= PROT_WRITE;
    if (has_perm(perms, MemPerm::EXEC)) prot |= PROT_EXEC;

    void* mem = mmap(nullptr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        std::cerr << "mmap fallo: " << std::strerror(errno) << "\n";
        return nullptr;
    }
    return mem;
#endif
    }

    /**
     * @brief Libera memoria previamente asignada con allocate_memory().
     *
     * @param mem Puntero a la memoria a liberar
     * @param size Tamaño de la memoria (debe coincidir con allocate_memory)
     *
     * @note Redondea el tamaño a multiplo de pagina automaticamente.
     */
    inline void free_memory(void *mem, size_t size) {
#ifdef _WIN32
        VirtualFree(mem, 0, MEM_RELEASE);
#else
    long pagesize = sysconf(_SC_PAGESIZE);
    size = (size + pagesize - 1) & ~(pagesize - 1);
    munmap(mem, size);
#endif
    }

    struct Arena {
        void *      ptr;   //< puntero al bloque
        size_t      size;  //< tamaño del bloque
        vm::MemPerm perms; //< permisos
    };

    class ArenaManager {
    public:
        ArenaManager() = default;

        ~ArenaManager();


        /**
         * @brief Crea un nuevo arenero de memoria
         * @param size Tamaño en bytes
         * @param perms Permisos de la memoria
         * @return ID unico del arenero
         */
        int create_arena(size_t size, vm::MemPerm perms);

        /**
         * @brief Libera un arenero previamente creado
         * @param id ID del arenero
         * @return true si se libero correctamente
         */
        bool free_arena(int id);

        /**
         * @brief Obtiene informacion del arenero
         * @param id ID del arenero
         * @return puntero a Arena, nullptr si no existe
         */
        const Arena *get_arena(int id) const;

        /**
         * @brief Libera todos los areneros
         */
        void free_all();

        size_t total_allocated_bytes_{0};

    protected: // Para que hereden, y TLB pueda usarlo
        std::unordered_map<int, Arena> arenas;
        int                            next_id{1}; ///< para generar IDs unicos

    };

    /**
     * tipo de dato para punteros reales a mapear a la VM
     */
    typedef void *host_ptr;

    /**
     * punteros virtuales de la VM.
     *
     * **Offset**    = 0xFFF     (`12bits`) [`12bits`] offset
     * Page Table    = 0xFFF     (`12bits`) [`24bits`] PT
     * Page Table1   = 0xFFFF    (`16bits`) [`40bits`] PT1
     * Page Table2   = 0xFFFFFF  (`24bits`) [`64bits`] PT2
     */
    enum class PageLevel : uint8_t { OFFSET=0, PT, PT1, PT2 };

    struct vm_map_ptr {
        uint64_t raw;

        uint64_t get(PageLevel level) const {
            switch(level) {
                case PageLevel::OFFSET: return raw & 0xFFF;
                case PageLevel::PT:     return (raw>>12) & 0xFFF;
                case PageLevel::PT1:    return (raw>>24) & 0xFFFF;
                case PageLevel::PT2:    return (raw>>40) & 0xFFFFFF;
            }
        }
        void set(PageLevel level, uint64_t value) {
            switch(level) {
                case PageLevel::OFFSET:
                    raw = (raw & ~0xFFFULL) | (value & 0xFFFULL);
                    break;
                case PageLevel::PT:
                    raw = (raw & ~(0xFFFULL << 12)) | ((value & 0xFFFULL) << 12);
                    break;
                case PageLevel::PT1:
                    raw = (raw & ~(0xFFFFULL << 24)) | ((value & 0xFFFFULL) << 24);
                    break;
                case PageLevel::PT2:
                    raw = (raw & ~(0xFFFFFFULL << 40)) | ((value & 0xFFFFFFULL) << 40);
                    break;
            }
        }
    };

    /**
     * Los punteros remotos son direcciones virtuales en la practica.
     */
    typedef vm_map_ptr remote_ptr;


    /**
     * Puntero mapeado, puede ser de 3 tipos.
     * - Se puede mapear memoria local.
     * - Se puede mapear memoria remota.
     * - Se puede mapear memoria virtual (otra direccion virtual).
     */
    typedef union ptr_mapped {
        vm_map_ptr ptr_vm;     // para mapear una direccion virtual.
        host_ptr   ptr_host;   // direccion real de la memoria mapeada
        remote_ptr ptr_remote; // direccion remota de la memoria
    } ptr_mapped;

    typedef enum type_ptr_mapped {
        MAPPED_PTR_VM,
        MAPPED_PTR_HOST,
        MAPPED_PTR_REMOTE
    } type_ptr_mapped;

    /**
     * Los punteros de la maquina virtual se mapean a memoria real, pero no tiene por que estar seguida
     * las arenas unas de otras. Hay una variante de este tipo de punteros donde se indica la direccion
     * IP de la maquina objetivo a la que dicha direccion pertenece. (MappedPtrRemote)
     */
    typedef struct MappedPtrHost {
        vm_map_ptr ptr_vm; // direccion virtual de la memoria
        ptr_mapped mapped; // direccion mapeada
    } MappedPtr;

    typedef struct __attribute__((packed)) MappedPtrRemoteIpv4 {
        vm_map_ptr ptr_vm;    // direccion virtual en una maquina remota
        HostIpv4   host_ipv4; // direccion de la maquina a la que pertenece la direccion
    } MappedPtrRemoteIpv4;

    typedef struct __attribute__((packed)) MappedPtrRemoteIpv6 {
        vm_map_ptr ptr_vm;    // direccion virtual en una maquina remota
        HostIpv6   host_ipv6; // direccion de la maquina a la que pertenece la direccion
    } MappedPtrRemote6;
} // namespace vm
#endif // ARENA_H
