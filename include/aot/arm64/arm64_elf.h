/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file arm64_elf.h
 * @brief Emisor MINIMO de un ejecutable ELF64 AArch64 (Phase H.4a).
 *
 * Envuelve unos bytes de codigo maquina arm64 (ya ensamblados) en un ELF64
 * ET_EXEC con e_machine=EM_AARCH64 y un unico segmento PT_LOAD RWX mapeado en
 * @p base_vaddr.  El punto de entrada apunta justo despues de las cabeceras.
 * Es el primer paso de la emision de objetos arm64: un contenedor estandar,
 * cargable por @c qemu-system-aarch64 @c -kernel (que parsea el ELF y salta al
 * entry), sin el `-device loader` del binario plano.  Los formatos completos
 * (ELF relocatable, PE-COFF ARM64, Mach-O) + relocaciones vienen en H.4b+.
 */

#ifndef VX_AOT_ARM64_ELF_H
#define VX_AOT_ARM64_ELF_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace aot {

/**
 * @brief Escribe un ELF64 AArch64 ejecutable con @p code como unico segmento.
 * @param path       Ruta del .elf de salida.
 * @param code       Bytes de codigo maquina arm64.
 * @param size       Numero de bytes de @p code.
 * @param base_vaddr Direccion virtual de carga (alineada a pagina).  El entry es
 *                   @c base_vaddr + tamano de cabeceras.
 * @return true si se escribio correctamente.
 */
bool write_elf64_aarch64_exec(const std::string &path, const uint8_t *code,
                              size_t size, uint64_t base_vaddr);

} // namespace aot

#endif // VX_AOT_ARM64_ELF_H
