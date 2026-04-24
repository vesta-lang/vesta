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
 * @file assembly.cpp
 * @brief Implementacion del soporte de ensamblado/desensamblado nativo via Keystone/Capstone.
 *
 * Implementa:
 *  - @c get_available_architectures() : detecta las arquitecturas soportadas por Keystone
 *    y Capstone probando la apertura de cada engine.  El resultado se cachea mediante
 *    una variable static local (inicializacion unica).
 *  - @c assemble_file()    : lee un archivo .asm, lo ensambla con Keystone y muestra o
 *    guarda el binario resultante.
 *  - @c disassemble_file() : lee un binario, lo desensambla con Capstone y muestra o
 *    guarda el texto desmontado.
 */
#include "assembly/assembly.h"
#include <capstone/capstone.h>
#include <keystone/keystone.h>

const ArchSupport& get_available_architectures() {
    static ArchSupport archs;
    static bool initialized = false;

    if (!initialized) {
        /* probar cada arquitectura Capstone intentando abrir su engine */
        struct { cs_arch arch; cs_mode mode; const char* name; } capstone_list[] = {
            {CS_ARCH_X86,   CS_MODE_32,           "X86-32"},
            {CS_ARCH_X86,   CS_MODE_64,           "X86-64"},
            {CS_ARCH_ARM,   CS_MODE_ARM,           "ARM"},
            {CS_ARCH_MIPS,  CS_MODE_MIPS32,        "MIPS"},
            {CS_ARCH_PPC,   CS_MODE_32,            "PPC"},
            {CS_ARCH_SPARC, CS_MODE_BIG_ENDIAN,    "SPARC"},
            {CS_ARCH_SYSZ,  CS_MODE_BIG_ENDIAN,    "SYSTEMZ"},
            {CS_ARCH_RISCV, CS_MODE_RISCV32,       "RISCV"}
        };

        for (auto &c : capstone_list) {
            csh handle;
            if (cs_open(c.arch, c.mode, &handle) == CS_ERR_OK) {
                archs.capstone.push_back(c.name); /* arquitectura disponible en Capstone */
                cs_close(&handle);
            }
        }

        /* probar cada arquitectura Keystone de la misma forma */
        struct { ks_arch arch; ks_mode mode; const char* name; } keystone_list[] = {
            {KS_ARCH_X86,     KS_MODE_32,            "X86-32"},
            {KS_ARCH_X86,     KS_MODE_64,            "X86-64"},
            {KS_ARCH_ARM,     KS_MODE_ARM,            "ARM"},
            {KS_ARCH_ARM64,   KS_MODE_LITTLE_ENDIAN,  "AArch64"},
            {KS_ARCH_MIPS,    KS_MODE_MIPS32,         "MIPS"},
            {KS_ARCH_PPC,     KS_MODE_32,             "PPC"},
            {KS_ARCH_SPARC,   KS_MODE_BIG_ENDIAN,     "SPARC"},
            {KS_ARCH_SYSTEMZ, KS_MODE_BIG_ENDIAN,     "SYSTEMZ"},
            {KS_ARCH_RISCV,   KS_MODE_RISCV32,        "RISCV"}
        };

        for (auto &k : keystone_list) {
            ks_engine *ks;
            if (ks_open(k.arch, k.mode, &ks) == KS_ERR_OK) {
                archs.keystone.push_back(k.name); /* arquitectura disponible en Keystone */
                ks_close(ks);
            }
        }

        initialized = true;
    }

    return archs;
}

bool assemble_file(const std::string &file, const std::string &arch_name,
                   bool save_output, const std::string &prefix) {
    /* verificar que Keystone admite la arquitectura solicitada */
    const ArchSupport &archs = get_available_architectures();
    if (std::find(archs.keystone.begin(), archs.keystone.end(), arch_name) == archs.keystone.end()) {
        std::cerr << "Keystone no soporta la arquitectura: " << arch_name << "\n";
        return false;
    }

    /* mapear el nombre de arquitectura a los enums internos de Keystone */
    ks_arch arch;
    ks_mode mode;
    if (arch_name == "X86-32") {
        arch = KS_ARCH_X86;
        mode = KS_MODE_32;
    } else if (arch_name == "X86-64") {
        arch = KS_ARCH_X86;
        mode = KS_MODE_64;
    } else if (arch_name == "ARM") {
        arch = KS_ARCH_ARM;
        mode = KS_MODE_ARM;
    } else if (arch_name == "AArch64") {
        arch = KS_ARCH_ARM64;
        mode = KS_MODE_LITTLE_ENDIAN;
    } else {
        std::cerr << "Arquitectura desconocida: " << arch_name << "\n";
        return false;
    }

    /* inicializar el motor Keystone con la arquitectura/modo elegidos */
    ks_engine *ks;
    if (ks_open(arch, mode, &ks) != KS_ERR_OK) {
        std::cerr << "Error inicializando Keystone\n";
        return false;
    }

    /* leer todo el contenido del archivo .asm en memoria */
    std::ifstream f(file);
    if (!f) {
        std::cerr << "No se pudo abrir el archivo: " << file << "\n";
        ks_close(ks);
        return false;
    }
    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string asm_code = buffer.str();

    /* ensamblar el texto; encode recibe el buffer de bytes resultante */
    unsigned char *encode;
    size_t size, count;
    if (ks_asm(ks, asm_code.c_str(), 0, &encode, &size, &count) != KS_ERR_OK) {
        std::cerr << "Error ensamblando: " << ks_strerror(ks_errno(ks)) << "\n";
        ks_close(ks);
        return false;
    }

    /* mostrar el resumen y los bytes en hexadecimal */
    std::cout << "Assembled " << count << " instructions, " << size << " bytes:\n";
    for (size_t i = 0; i < size; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)encode[i] << " ";
    }
    std::cout << "\n";

    /* si se solicito guardar, volcar el binario a disco */
    if (save_output) {
        std::ofstream out(prefix + "_assembled.bin", std::ios::binary);
        if (out) {
            out.write((char*)encode, size);
            std::cout << "Guardado en: " << prefix << "_assembled.bin\n";
        }
    }

    /* liberar el buffer asignado por Keystone y cerrar el motor */
    ks_free(encode);
    ks_close(ks);
    return true;
}

bool disassemble_file(const std::string& binPath, const std::string& arch_name,
                      bool save_output, const std::string& prefix) {
    /* verificar que Capstone admite la arquitectura solicitada */
    const ArchSupport &archs = get_available_architectures();
    if (std::find(archs.capstone.begin(), archs.capstone.end(), arch_name) == archs.capstone.end()) {
        std::cerr << "Capstone no soporta la arquitectura: " << arch_name << "\n";
        return false;
    }

    /* mapear el nombre de arquitectura a los enums internos de Capstone */
    cs_arch arch;
    cs_mode mode;
    if (arch_name == "X86-32") {
        arch = CS_ARCH_X86;
        mode = CS_MODE_32;
    } else if (arch_name == "X86-64") {
        arch = CS_ARCH_X86;
        mode = CS_MODE_64;
    } else if (arch_name == "ARM") {
        arch = CS_ARCH_ARM;
        mode = CS_MODE_ARM;
    } else if (arch_name == "AArch64") {
        /* _CS_ARCH_ARM64 es el valor interno de Capstone para AArch64 */
        arch = static_cast<cs_arch>(_CS_ARCH_ARM64);
        mode = CS_MODE_LITTLE_ENDIAN;
    } else {
        std::cerr << "Arquitectura desconocida: " << arch_name << "\n";
        return false;
    }

    /* leer el binario completo en un vector de bytes */
    std::ifstream f(binPath, std::ios::binary);
    if (!f) {
        std::cerr << "No se pudo abrir archivo: " << binPath << "\n";
        return false;
    }
    std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    /* abrir el motor Capstone con la arquitectura/modo elegidos */
    csh handle;
    if (cs_open(arch, mode, &handle) != CS_ERR_OK) {
        std::cerr << "Error inicializando Capstone\n";
        return false;
    }

    /* desensamblar; 0x1000 es la direccion virtual base asumida */
    cs_insn *insn;
    size_t count = cs_disasm(handle, code.data(), code.size(), 0x1000, 0, &insn);
    if (count > 0) {
        /* imprimir cada instruccion: direccion, mnemonic y operandos */
        for (size_t i = 0; i < count; i++) {
            std::cout << "0x" << std::hex << insn[i].address << ":\t"
                      << insn[i].mnemonic << "\t" << insn[i].op_str << "\n";
        }
        cs_free(insn, count);

        /* si se solicito guardar, volcar el texto de desensamblado a disco */
        if (save_output) {
            std::ofstream out(prefix + "_disassembled.txt");
            if (out) {
                for (size_t i = 0; i < count; i++) {
                    out << "0x" << std::hex << insn[i].address << ":\t"
                        << insn[i].mnemonic << "\t" << insn[i].op_str << "\n";
                }
                std::cout << "Guardado en: " << prefix << "_disassembled.txt\n";
            }
        }
    } else {
        std::cerr << "No se pudo desensamblar\n";
        cs_close(&handle);
        return false;
    }

    /* cerrar el motor Capstone antes de salir */
    cs_close(&handle);
    return true;
}