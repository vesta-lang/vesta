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
 * @file aot/object_writer.cpp
 * @brief Phase AOT.4 -- fachada C++ del emisor de ejecutables (sobre el shim C).
 *
 * Convierte las estructuras C++ (@c WriterSection, @c LayoutConfig,
 * @c ImportCall) en los structs C planos de @c aot_emit_shim.h y despacha al
 * emisor PE o ELF.  No incluye LibPEparse: el shim es la unica frontera.
 */

#include "aot/object_writer.h"

#include <utility>

namespace aot {

    int ObjectWriter::add_section(WriterSection s) {
        sections_.push_back(std::move(s));
        return static_cast<int>(sections_.size()) - 1;
    }

    int ObjectWriter::add_text(std::vector<uint8_t> code) {
        WriterSection s;
        s.name  = ".text";
        s.flags = SecFlag::CODE | SecFlag::EXEC | SecFlag::READ;
        s.data  = std::move(code);
        const int idx = add_section(std::move(s));
        set_entry(idx, 0);  // por defecto el _start esta al inicio de .text
        return idx;
    }

    int ObjectWriter::add_rodata(std::vector<uint8_t> data) {
        WriterSection s;
        s.name  = ".rodata";
        s.flags = SecFlag::DATA | SecFlag::READ;
        s.data  = std::move(data);
        return add_section(std::move(s));
    }

    bool ObjectWriter::write(const std::string &path, std::string &err) {
        if (sections_.empty()) { err = "ObjectWriter: sin secciones"; return false; }

        // Construir los AotSection (los punteros apuntan a los buffers de
        // sections_, que viven mientras dure esta llamada).
        std::vector<AotSection> csecs(sections_.size());
        for (size_t i = 0; i < sections_.size(); ++i) {
            const WriterSection &w = sections_[i];
            AotSection &c = csecs[i];
            c.name     = w.name.c_str();
            c.flags    = w.flags;
            c.data     = w.data.empty() ? nullptr : w.data.data();
            c.size     = static_cast<uint32_t>(w.data.size());
            c.bss_size = w.bss_size;
            c.vaddr    = w.vaddr;
            c.align    = w.align;
        }

        AotLayoutCfg ccfg;
        ccfg.image_base      = cfg_.image_base;
        ccfg.section_align   = cfg_.section_align;
        ccfg.file_align      = cfg_.file_align;
        ccfg.stack_reserve   = cfg_.stack_reserve;
        ccfg.stack_commit    = cfg_.stack_commit;
        ccfg.heap_reserve    = cfg_.heap_reserve;
        ccfg.heap_commit     = cfg_.heap_commit;
        ccfg.pe_subsystem    = cfg_.pe_subsystem;
        ccfg.elf_stack_vaddr = cfg_.elf_stack_vaddr;
        ccfg.elf_stack_size  = cfg_.elf_stack_size;

        // Relocations cross-seccion (refs a datos / simbolos de seccion).  El
        // shim las resuelve tras el layout.
        std::vector<AotReloc> crelocs(relocs_.size());
        for (size_t i = 0; i < relocs_.size(); ++i) {
            const AbsReloc &r = relocs_[i];
            AotReloc &c = crelocs[i];
            c.site_section   = r.site_section;
            c.site_off       = r.site_off;
            c.target_section = r.target.section;
            c.target_off     = r.target.offset;
            c.target_is_size = r.target.is_size ? 1 : 0;
            c.kind           = static_cast<int>(r.kind);  // espejo de AOT_RELOC_*
            c.addend         = r.addend;
        }
        const AotReloc *crel_ptr = crelocs.empty() ? nullptr : crelocs.data();
        const int       crel_n   = static_cast<int>(crelocs.size());

        char errbuf[256] = {0};
        int ok = 0;

        if (fmt_ == ObjFormat::PE) {
            std::vector<AotImport> cimps(imports_.size());
            for (size_t i = 0; i < imports_.size(); ++i) {
                const ImportCall &ic = imports_[i];
                cimps[i].dll          = ic.dll.c_str();
                cimps[i].func         = ic.func.c_str();
                cimps[i].call_section = ic.call_section;
                cimps[i].call_off     = ic.call_off;
            }
            ok = aot_emit_pe(path.c_str(), &ccfg,
                             csecs.data(), static_cast<int>(csecs.size()),
                             entry_sec_, entry_off_,
                             cimps.empty() ? nullptr : cimps.data(),
                             static_cast<int>(cimps.size()),
                             crel_ptr, crel_n,
                             errbuf, sizeof(errbuf));
        } else {
            ok = aot_emit_elf(path.c_str(), &ccfg,
                              csecs.data(), static_cast<int>(csecs.size()),
                              entry_sec_, entry_off_,
                              crel_ptr, crel_n,
                              errbuf, sizeof(errbuf));
        }

        if (!ok) { err = errbuf[0] ? errbuf : "ObjectWriter: error desconocido"; return false; }
        return true;
    }

} // namespace aot
