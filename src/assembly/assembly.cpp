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

#include "assembly/assembly.h"

const ArchSupport& get_available_architectures() {
    // static hace que la detección se ejecute solo una vez
    static ArchSupport archs;
    static bool initialized = false;

    if (!initialized) {
        // --- Capstone ---
        struct { cs_arch arch; cs_mode mode; const char* name; } capstone_list[] = {
            {CS_ARCH_X86, CS_MODE_32, "X86-32"},
            {CS_ARCH_X86, CS_MODE_64, "X86-64"},
            {CS_ARCH_ARM, CS_MODE_ARM, "ARM"},
            {CS_ARCH_MIPS, CS_MODE_MIPS32, "MIPS"},
            {CS_ARCH_PPC, CS_MODE_32, "PPC"},
            {CS_ARCH_SPARC, CS_MODE_BIG_ENDIAN, "SPARC"},
            {CS_ARCH_SYSZ, CS_MODE_BIG_ENDIAN, "SYSTEMZ"},
            {CS_ARCH_RISCV, CS_MODE_RISCV32, "RISCV"}
        };

        for (auto &c : capstone_list) {
            csh handle;
            if (cs_open(c.arch, c.mode, &handle) == CS_ERR_OK) {
                archs.capstone.push_back(c.name);
                cs_close(&handle);
            }
        }

        // --- Keystone ---
        struct { ks_arch arch; ks_mode mode; const char* name; } keystone_list[] = {
            {KS_ARCH_X86, KS_MODE_32, "X86-32"},
            {KS_ARCH_X86, KS_MODE_64, "X86-64"},
            {KS_ARCH_ARM, KS_MODE_ARM, "ARM"},
            {KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN, "AArch64"},
            {KS_ARCH_MIPS, KS_MODE_MIPS32, "MIPS"},
            {KS_ARCH_PPC, KS_MODE_32, "PPC"},
            {KS_ARCH_SPARC, KS_MODE_BIG_ENDIAN, "SPARC"},
            {KS_ARCH_SYSTEMZ, KS_MODE_BIG_ENDIAN, "SYSTEMZ"},
            {KS_ARCH_RISCV, KS_MODE_RISCV32, "RISCV"}
        };

        for (auto &k : keystone_list) {
            ks_engine *ks;
            if (ks_open(k.arch, k.mode, &ks) == KS_ERR_OK) {
                archs.keystone.push_back(k.name);
                ks_close(ks);
            }
        }

        initialized = true; // marcado como inicializado
    }

    return archs;
}

bool assemble_file(const std::string &file, const std::string &arch_name,
                   bool save_output, const std::string &prefix) {
    const ArchSupport &archs = get_available_architectures();
    if (std::find(archs.keystone.begin(), archs.keystone.end(), arch_name) == archs.keystone.end()) {
        std::cerr << "Keystone no soporta la arquitectura: " << arch_name << "\n";
        return false;
    }

    // Mapear nombre a ks_arch/ks_mode
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

    ks_engine *ks;
    if (ks_open(arch, mode, &ks) != KS_ERR_OK) {
        std::cerr << "Error inicializando Keystone\n";
        return false;
    }

    std::ifstream f(file);
    if (!f) {
        std::cerr << "No se pudo abrir el archivo: " << file << "\n";
        ks_close(ks);
        return false;
    }
    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string asm_code = buffer.str();

    unsigned char *encode;
    size_t size, count;
    if (ks_asm(ks, asm_code.c_str(), 0, &encode, &size, &count) != KS_ERR_OK) {
        std::cerr << "Error ensamblando: " << ks_strerror(ks_errno(ks)) << "\n";
        ks_close(ks);
        return false;
    }

    std::cout << "Assembled " << count << " instructions, " << size << " bytes:\n";
    for (size_t i = 0; i < size; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)encode[i] << " ";
    }
    std::cout << "\n";

    if (save_output) {
        std::ofstream out(prefix + "_assembled.bin", std::ios::binary);
        if (out) {
            out.write((char*)encode, size);
            std::cout << "Guardado en: " << prefix << "_assembled.bin\n";
        }
    }

    ks_free(encode);
    ks_close(ks);
    return true;
}

bool disassemble_file(const std::string& binPath, const std::string& arch_name,
                      bool save_output, const std::string& prefix) {
    const ArchSupport &archs = get_available_architectures();
    if (std::find(archs.capstone.begin(), archs.capstone.end(), arch_name) == archs.capstone.end()) {
        std::cerr << "Capstone no soporta la arquitectura: " << arch_name << "\n";
        return false;
    }

    // Mapear nombre a cs_arch/cs_mode
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
        arch = static_cast<cs_arch>(_CS_ARCH_ARM64);
        mode = CS_MODE_LITTLE_ENDIAN;
    } else {
        std::cerr << "Arquitectura desconocida: " << arch_name << "\n";
        return false;
    }

    std::ifstream f(binPath, std::ios::binary);
    if (!f) {
        std::cerr << "No se pudo abrir archivo: " << binPath << "\n";
        return false;
    }
    std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    csh handle;
    if (cs_open(arch, mode, &handle) != CS_ERR_OK) {
        std::cerr << "Error inicializando Capstone\n";
        return false;
    }

    cs_insn *insn;
    size_t count = cs_disasm(handle, code.data(), code.size(), 0x1000, 0, &insn);
    if (count > 0) {
        for (size_t i = 0; i < count; i++) {
            std::cout << "0x" << std::hex << insn[i].address << ":\t"
                      << insn[i].mnemonic << "\t" << insn[i].op_str << "\n";
        }
        cs_free(insn, count);

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

    cs_close(&handle);
    return true;
}