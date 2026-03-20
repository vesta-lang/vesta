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

#ifndef ASSEMBLY_H
#define ASSEMBLY_H

#include <capstone/capstone.h>
#include <keystone/keystone.h>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <fstream>

struct ArchSupport {
    std::vector<std::string> capstone;
    std::vector<std::string> keystone;
};

#ifndef CS_ARCH_ARM64
#define _CS_ARCH_ARM64 1
#else
#define _CS_ARCH_ARM64 CS_ARCH_ARM64
#endif

const ArchSupport& get_available_architectures();
bool assemble_file(const std::string& asmPath,
                   const std::string& arch,
                   bool save_output,
                   const std::string& prefix);

bool disassemble_file(const std::string& binPath,
                      const std::string& arch,
                      bool save_output,
                      const std::string& prefix);
#endif //ASSEMBLY_H
