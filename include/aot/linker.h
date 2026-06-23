/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file aot/linker.h
 * @brief Phase AOT.5 -- linker propio (enlazado de objetos .o sin ld/gcc).
 *
 * Enlaza uno o mas objetos relocatables (ELF64 @c ET_REL, los que emite
 * @c --emit obj o un .o de gcc) en un ejecutable nativo, resolviendo los
 * simbolos cross-file y las relocaciones (@c R_X86_64_PC32 / @c PLT32 /
 * @c 64 / @c 32 / @c 32S).  REUSA @c aot::ObjectWriter para el contenedor de
 * salida y su motor de relocs: el linker solo PARSEA los .o, fusiona las
 * secciones por nombre y traduce cada reloc del .o a una @c AbsReloc.
 *
 * Pensado para dev-OS: el usuario controla la base de carga (@c image_base) y
 * el punto de entrada (@c entry).  Con @c entry vacio se sintetiza un @c _start
 * que llama a @c main y termina el proceso (ejecutable hosted); con @c entry
 * fijado se usa ese simbolo directamente SIN stub (kernel / bootloader cuyo
 * punto de entrada es propio, p.ej. @c _kstart).
 *
 * Slice 1: ELF64 in -> ELF64 out.  PE/COFF y x86-32 son follow-ups.
 */

#ifndef AOT_LINKER_H
#define AOT_LINKER_H

#include "aot/object_writer.h" // ObjFormat, LayoutConfig

#include <string>
#include <vector>

namespace aot {

/**
 * @brief Opciones del enlazado.
 */
struct LinkOptions {
    ObjFormat fmt = ObjFormat::ELF; ///< formato de salida (ELF / PE).
    uint64_t image_base = 0;        ///< base de carga (0 => default del writer).
    std::string entry;              ///< simbolo de entrada; vacio => _start->main.
    LayoutConfig layout;            ///< config de layout opcional (align/pila/...).
    std::string link_script;        ///< .vex de configuracion (fn link()); vacio
                                    ///< => sin script (solo CLI flags).
    bool debug = false;             ///< valor de debug_build() en el link-script.
};

/**
 * @brief Enlaza @p inputs en el ejecutable @p out_path.
 * @param inputs   Rutas de los objetos .o (ELF64 ET_REL).
 * @param out_path Ruta del ejecutable de salida.
 * @param opts     Opciones (formato, base, entry).
 * @param err      Recibe el mensaje de error si falla.
 * @return true en exito.
 */
bool aot_link(const std::vector<std::string> &inputs,
              const std::string &out_path, const LinkOptions &opts,
              std::string &err);

} // namespace aot

#endif // AOT_LINKER_H
