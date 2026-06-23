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
 * @file inspector.cpp
 * @brief Implementacion del inspector del ecosistema Vex (peticiones
 *        @c vesta/* del LSP).
 *
 * Cada metodo es BAJO DEMANDA: se sirve cuando el editor lo pide, no en
 * cada pulsacion.  Las vistas baratas reutilizan el @c CompileResult
 * cacheado por @c AnalysisEngine; las caras (ir-pre, diagramas) recompilan
 * con flags concretos y cachean el resultado en @c view_cache_.  El codigo
 * nativo del JIT y del AOT se desensambla con Capstone (x86-64).
 *
 * Robustez: ningun metodo propaga excepciones; ante un fallo controlado
 * devuelve un objeto con @c error / @c unsupported / @c incompatible.  El
 * caller (lsp_server) ademas envuelve todo en try/catch como red final.
 */

#include "lsp/inspector.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <capstone/capstone.h>

#include "aot/aot_analyze.h"
#include "analyze/bigo.h"
#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include "jit/code_cache.h"
#include "jit/jit_compiler.h"
#include "jit/runtime_entries.h"
#include "jit/selector.h"
#include "jit/vreg_pipeline.h"
#include "lsp/document_store.h"
#include "vex/compiler.h"

namespace lsp {

namespace {

/**
 * @brief Devuelve la primera linea fuente conocida de una funcion IR.
 *
 * El IR no guarda una SourceLoc por funcion; recorre sus instrucciones
 * buscando el primer @c source_line distinto de cero.  Best-effort para
 * ubicar funciones en la lista de @c vesta/functions.
 *
 * @param fn Funcion IR.
 * @return Linea 1-based, o 0 si ninguna instruccion lleva linea.
 */
uint32_t first_source_line(const ir::IrFunction &fn) {
    for (const auto &blk : fn.blocks) {
        for (const auto &ins : blk.instrs) {
            if (ins.source_line != 0)
                return ins.source_line;
        }
    }
    return 0;
}

/**
 * @brief Deserializa el modulo IR (post-opt) cacheado del CompileResult.
 *
 * @param result CompileResult con @c ir_module_cache_bytes.
 * @param out    Modulo IR destino.
 * @return true si la deserializacion fue exitosa y el modulo tiene
 *         funciones; false en otro caso.
 */
bool parse_post_opt_module(const vex::CompileResult &result, ir::IrModule &out) {
    if (result.ir_module_cache_bytes.empty())
        return false;
    return ir::parse_ir_module_cache(result.ir_module_cache_bytes, out);
}

/**
 * @brief Selecciona la funcion objetivo de las vistas JIT/AOT.
 *
 * Si @p wanted no esta vacio, busca esa funcion exacta.  Si esta vacio,
 * prefiere @c "main"; si no hay, devuelve la primera funcion no nativa y
 * no macro-compilada del modulo.
 *
 * @param mod    Modulo IR.
 * @param wanted Nombre pedido (vacio = auto).
 * @return Puntero a la funcion, o nullptr si no hay candidata.
 */
const ir::IrFunction *pick_function(const ir::IrModule &mod,
                                    const std::string &wanted) {
    if (!wanted.empty()) {
        for (const auto &fn : mod.functions) {
            if (fn.name == wanted)
                return &fn;
        }
        // Funciones comptime: el frontend las baja como @c __macro_<nombre>.
        // Si el hover pidio @c M_foo, probar @c __macro_M_foo.
        const std::string macro = "__macro_" + wanted;
        for (const auto &fn : mod.functions) {
            if (fn.name == macro)
                return &fn;
        }
        return nullptr;
    }
    // Auto: preferir main (nombre exacto o sufijo "main").
    const ir::IrFunction *first_user = nullptr;
    for (const auto &fn : mod.functions) {
        if (fn.is_native || fn.is_macro_compiled)
            continue;
        if (!first_user)
            first_user = &fn;
        if (fn.name == "main")
            return &fn;
    }
    return first_user;
}

/**
 * @brief Desensambla un buffer de bytes x86-64 a texto legible via Capstone.
 *
 * Una linea por instruccion con offset relativo, mnemonico y operandos.  Si
 * Capstone no puede abrir o decodificar, devuelve un mensaje informativo en
 * lugar de fallar.
 *
 * @param code      Bytes del codigo nativo.
 * @param code_size Numero de bytes.
 * @param base      Direccion base mostrada (offset relativo si 0).
 * @return Texto del desensamblado (multilinea).
 */
std::string disasm_x86_64(const uint8_t *code, size_t code_size,
                          uint64_t base) {
    if (!code || code_size == 0)
        return "(codigo vacio)";
    csh handle;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
        return "(Capstone: no se pudo abrir el desensamblador x86-64)";
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_OFF);
    cs_insn *insn = nullptr;
    const size_t count = cs_disasm(handle, code, code_size, base, 0, &insn);
    std::ostringstream oss;
    if (count > 0) {
        for (size_t i = 0; i < count; ++i) {
            // Offset relativo al inicio (mas estable que una direccion host).
            uint64_t off = insn[i].address - base;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04llx",
                          static_cast<unsigned long long>(off));
            oss << buf << "  " << insn[i].mnemonic;
            if (insn[i].op_str[0] != '\0')
                oss << ' ' << insn[i].op_str;
            oss << '\n';
        }
        cs_free(insn, count);
    } else {
        oss << "(Capstone no pudo desensamblar los bytes generados)\n";
    }
    cs_close(&handle);
    return oss.str();
}

/**
 * @brief Desensambla correlando cada instruccion maquina con su linea fuente.
 *
 * Solo-LSP (vista "Godbolt").  Combina el desensamblado de Capstone (offset
 * + mnemonico) con la @c line_map del codegen (byte_offset -> source_line)
 * para producir un array @c [{addr, text, line}].  La @c line_map viene
 * ordenada ascendentemente por @c byte_offset (orden de emision); para cada
 * instruccion Capstone en @p off se busca la entrada con el mayor
 * @c byte_offset <= off (una @c MInstr puede expandir a varias instrucciones
 * x86, todas atribuidas a su linea).  @c line==0 = sin atribucion (prologo,
 * epilogo, instrs sinteticas).
 *
 * @param code      Bytes del codigo nativo.
 * @param code_size Numero de bytes.
 * @param lm        Tabla byte_offset -> source_line (puede estar vacia).
 * @return Array JSON de @c {addr:"%04x", text, line}.
 */
/**
 * @brief Canonicaliza un registro x86 a su GPR de 64 bits.
 *
 * Para el rastreo de valores (debug-info) se trata @c eax/ax/al igual que
 * @c rax, @c r8d/r8w/r8b igual que @c r8, etc.  Asi @c mov eax, ecx propaga
 * el nombre del valor entre los registros aunque el codegen use el ancho de
 * 32 bits.
 *
 * @param handle Handle Capstone abierto (para @c cs_reg_name).
 * @param reg    Enum del registro (operando).
 * @return Cadena canonica ("rax".."r15") o "" si no es un GPR rastreable.
 */
std::string canon_reg(csh handle, unsigned reg) {
    if (reg == X86_REG_INVALID)
        return "";
    const char *nm = cs_reg_name(handle, reg);
    if (!nm)
        return "";
    std::string s = nm;
    static const std::unordered_map<std::string, std::string> kFam = {
        {"rax", "rax"}, {"eax", "rax"},  {"ax", "rax"},   {"al", "rax"},
        {"ah", "rax"},  {"rbx", "rbx"},  {"ebx", "rbx"},  {"bx", "rbx"},
        {"bl", "rbx"},  {"bh", "rbx"},   {"rcx", "rcx"},  {"ecx", "rcx"},
        {"cx", "rcx"},  {"cl", "rcx"},   {"ch", "rcx"},   {"rdx", "rdx"},
        {"edx", "rdx"}, {"dx", "rdx"},   {"dl", "rdx"},   {"dh", "rdx"},
        {"rsi", "rsi"}, {"esi", "rsi"},  {"si", "rsi"},   {"sil", "rsi"},
        {"rdi", "rdi"}, {"edi", "rdi"},  {"di", "rdi"},   {"dil", "rdi"},
        {"rbp", "rbp"}, {"ebp", "rbp"},  {"bp", "rbp"},   {"bpl", "rbp"},
        {"rsp", "rsp"}, {"esp", "rsp"},  {"sp", "rsp"},   {"spl", "rsp"},
        {"r8", "r8"},   {"r8d", "r8"},   {"r8w", "r8"},   {"r8b", "r8"},
        {"r9", "r9"},   {"r9d", "r9"},   {"r9w", "r9"},   {"r9b", "r9"},
        {"r10", "r10"}, {"r10d", "r10"}, {"r10w", "r10"}, {"r10b", "r10"},
        {"r11", "r11"}, {"r11d", "r11"}, {"r11w", "r11"}, {"r11b", "r11"},
        {"r12", "r12"}, {"r12d", "r12"}, {"r12w", "r12"}, {"r12b", "r12"},
        {"r13", "r13"}, {"r13d", "r13"}, {"r13w", "r13"}, {"r13b", "r13"},
        {"r14", "r14"}, {"r14d", "r14"}, {"r14w", "r14"}, {"r14b", "r14"},
        {"r15", "r15"}, {"r15d", "r15"}, {"r15w", "r15"}, {"r15b", "r15"},
    };
    auto it = kFam.find(s);
    return it == kFam.end() ? std::string() : it->second;
}

/// Etiqueta legible de un slot relativo a rbp: "[rbp-0x8]" / "[rbp+0x10]".
std::string frame_label(int64_t disp) {
    char b[32];
    if (disp < 0)
        std::snprintf(b, sizeof(b), "[rbp-0x%llx]",
                      static_cast<unsigned long long>(-disp));
    else
        std::snprintf(b, sizeof(b), "[rbp+0x%llx]",
                      static_cast<unsigned long long>(disp));
    return b;
}

nlohmann::json
disasm_x86_64_correlated(const uint8_t *code, size_t code_size,
                         const std::vector<jit::LineMapEntry> &lm,
                         const std::vector<jit::NativeReloc> &relocs,
                         const std::vector<std::pair<std::string, std::string>>
                             &arg_seed,
                         nlohmann::json *frame_out) {
    nlohmann::json arr = nlohmann::json::array();
    if (!code || code_size == 0)
        return arr;
    csh handle;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
        return arr;
    // DETAIL ON: necesitamos los operandos estructurados (reg/mem/imm + acceso)
    // para rastrear que valor con nombre vive en cada registro / slot del frame.
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    cs_insn *insn = nullptr;
    const size_t count = cs_disasm(handle, code, code_size, 0, 0, &insn);
    size_t lm_idx = 0; // cursor en lm (ambos en orden ascendente de offset).

    // Estado del rastreo (debug-info):
    //   regmap[canon]  -> nombre del valor fuente que el registro contiene.
    //   framemap[disp] -> {nombre, tamano} del slot [rbp+disp].
    std::unordered_map<std::string, std::string> regmap;
    struct FSlot {
        std::string name;
        uint16_t size;
        bool seen; // primer offset donde aparece (para orden de salida)
        uint64_t first_off;
    };
    std::map<int64_t, FSlot> framemap;
    // Seed: la convencion de llamada coloca cada argumento en su registro.
    for (const auto &pr : arg_seed)
        if (!pr.first.empty())
            regmap[pr.first] = pr.second;

    // Layout del prologo (registros guardados + area reservada).  Tras
    // `mov rbp, rsp`, [rbp+0]=rbp guardado, [rbp+8]=direccion de retorno, y
    // cada `push reg` posterior baja 8 bytes ([rbp-8], [rbp-16], ...).
    bool have_rbp = false;
    int64_t push_off = 0;     // offset (rel. rbp) del ultimo push tras rbp
    int64_t reserved_bytes = 0;
    bool reserved_done = false;
    std::vector<nlohmann::json> saved_regs; // {offset,size,kind,name}

    for (size_t i = 0; i < count; ++i) {
        const uint64_t off = insn[i].address; // base 0 -> offset relativo.
        const uint64_t end = off + insn[i].size;
        while (lm_idx + 1 < lm.size() && lm[lm_idx + 1].byte_offset <= off)
            ++lm_idx;
        uint32_t line = 0;
        if (lm_idx < lm.size() && lm[lm_idx].byte_offset <= off)
            line = lm[lm_idx].source_line;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04llx",
                      static_cast<unsigned long long>(off));
        std::string text = insn[i].mnemonic;
        if (insn[i].op_str[0] != '\0') {
            text += ' ';
            text += insn[i].op_str;
        }

        // --- Rastreo de valores con nombre + frame --------------------------
        std::vector<std::string> ann; // partes del comentario de valores
        auto add_ann = [&](const std::string &s) {
            for (const auto &e : ann)
                if (e == s)
                    return; // dedup
            ann.push_back(s);
        };
        cs_detail *det = insn[i].detail;
        if (det) {
            const cs_x86 &x = det->x86;
            const unsigned id = insn[i].id;
            // --- Layout del prologo -------------------------------------
            if (id == X86_INS_MOV && x.op_count == 2 &&
                x.operands[0].type == X86_OP_REG &&
                x.operands[1].type == X86_OP_REG &&
                canon_reg(handle, x.operands[0].reg) == "rbp" &&
                canon_reg(handle, x.operands[1].reg) == "rsp") {
                have_rbp = true; // mov rbp, rsp -> frame pointer establecido
            } else if (id == X86_INS_PUSH && have_rbp && x.op_count == 1 &&
                       x.operands[0].type == X86_OP_REG) {
                push_off -= 8;
                const char *rn = cs_reg_name(handle, x.operands[0].reg);
                nlohmann::json js;
                js["offset"] = push_off;
                js["label"] = frame_label(push_off);
                js["size"] = 8;
                js["kind"] = "saved";
                js["name"] = rn ? rn : "?";
                saved_regs.push_back(std::move(js));
            } else if (id == X86_INS_SUB && have_rbp && !reserved_done &&
                       x.op_count == 2 &&
                       x.operands[0].type == X86_OP_REG &&
                       canon_reg(handle, x.operands[0].reg) == "rsp" &&
                       x.operands[1].type == X86_OP_IMM) {
                reserved_bytes = x.operands[1].imm;
                reserved_done = true;
            }
            const bool is_mov = (id == X86_INS_MOV || id == X86_INS_MOVZX ||
                                 id == X86_INS_MOVSX || id == X86_INS_MOVSXD);
            bool handled = false;
            // POP es restauracion de epilogo (callee-saved).  El rastreo es
            // lineal (sin CFG), asi que el `pop rsi` del epilogo del PRIMER
            // bloque -en orden de direcciones- precede a otros bloques donde
            // ese mismo registro callee-saved sigue alojando el valor (p.ej.
            // el parametro durante toda la funcion).  Lo tratamos como
            // no-invalidante para no perder el nombre en bloques posteriores.
            if (id == X86_INS_POP)
                handled = true;
            if (is_mov && x.op_count == 2) {
                const cs_x86_op &d = x.operands[0];
                const cs_x86_op &s = x.operands[1];
                if (d.type == X86_OP_REG && s.type == X86_OP_REG) {
                    // mov reg, reg : propaga el nombre del origen al destino.
                    std::string dc = canon_reg(handle, d.reg);
                    std::string sc = canon_reg(handle, s.reg);
                    std::string nm = sc.empty() ? "" : regmap[sc];
                    if (!dc.empty()) {
                        if (nm.empty())
                            regmap.erase(dc);
                        else {
                            regmap[dc] = nm;
                            add_ann(dc + " = " + nm);
                        }
                    }
                    handled = true;
                } else if (d.type == X86_OP_REG && s.type == X86_OP_MEM &&
                           canon_reg(handle, s.mem.base) == "rbp") {
                    // mov reg, [rbp+disp] : carga desde un slot conocido.
                    std::string dc = canon_reg(handle, d.reg);
                    auto fit = framemap.find(s.mem.disp);
                    std::string nm =
                        fit == framemap.end() ? "" : fit->second.name;
                    if (!dc.empty()) {
                        if (nm.empty())
                            regmap.erase(dc);
                        else {
                            regmap[dc] = nm;
                            add_ann(dc + " = " + nm);
                        }
                    }
                    handled = true;
                } else if (d.type == X86_OP_MEM &&
                           canon_reg(handle, d.mem.base) == "rbp" &&
                           s.type == X86_OP_REG) {
                    // mov [rbp+disp], reg : guarda un valor con nombre -> slot.
                    std::string sc = canon_reg(handle, s.reg);
                    std::string nm = sc.empty() ? "" : regmap[sc];
                    if (!nm.empty()) {
                        FSlot &fs = framemap[d.mem.disp];
                        if (!fs.seen) {
                            fs.seen = true;
                            fs.first_off = off;
                        }
                        fs.name = nm;
                        fs.size = d.size ? d.size : 8;
                        add_ann(frame_label(d.mem.disp) + " = " + nm);
                    }
                    handled = true;
                } else if (d.type == X86_OP_REG && s.type == X86_OP_IMM) {
                    // mov reg, imm : el registro deja de tener un valor con
                    // nombre.
                    std::string dc = canon_reg(handle, d.reg);
                    if (!dc.empty())
                        regmap.erase(dc);
                    handled = true;
                }
            }
            if (!handled) {
                // Caso generico: anotar los registros/slots LEIDOS con nombre,
                // luego invalidar los registros ESCRITOS (resultado anonimo).
                for (uint8_t oi = 0; oi < x.op_count; ++oi) {
                    const cs_x86_op &op = x.operands[oi];
                    if (op.type == X86_OP_REG && (op.access & CS_AC_READ)) {
                        std::string c = canon_reg(handle, op.reg);
                        if (!c.empty() && !regmap[c].empty())
                            add_ann(c + " = " + regmap[c]);
                    } else if (op.type == X86_OP_MEM &&
                               canon_reg(handle, op.mem.base) == "rbp") {
                        auto fit = framemap.find(op.mem.disp);
                        if (fit != framemap.end() && !fit->second.name.empty())
                            add_ann(frame_label(op.mem.disp) + " = " +
                                    fit->second.name);
                    }
                }
                for (uint8_t oi = 0; oi < x.op_count; ++oi) {
                    const cs_x86_op &op = x.operands[oi];
                    if (op.type == X86_OP_REG && (op.access & CS_AC_WRITE)) {
                        std::string c = canon_reg(handle, op.reg);
                        if (!c.empty())
                            regmap.erase(c);
                    }
                }
            }
        }

        // Comentario combinado: simbolo de relocation (call/lea) + valores.
        std::vector<std::string> comments;
        for (const auto &rc : relocs) {
            if (rc.offset >= off && rc.offset < end && !rc.symbol.empty()) {
                std::string sym = rc.symbol;
                if (rc.addend)
                    sym += "+" + std::to_string(rc.addend);
                comments.push_back(std::move(sym));
                break;
            }
        }
        for (const auto &a : ann)
            comments.push_back(a);
        if (!comments.empty()) {
            text += "  ; ";
            for (size_t c = 0; c < comments.size(); ++c) {
                if (c)
                    text += ", ";
                text += comments[c];
            }
        }

        nlohmann::json ji;
        ji["addr"] = buf;
        ji["text"] = std::move(text);
        ji["line"] = line;
        arr.push_back(std::move(ji));
    }
    if (count > 0)
        cs_free(insn, count);
    cs_close(&handle);

    // Volcar el frame completo, ordenado de la cima (mas cercano a rbp+) hacia
    // abajo: retorno, rbp guardado, registros callee-saved, slots con nombre
    // (spills/locales) y el area reservada.
    if (frame_out) {
        std::vector<nlohmann::json> all;
        if (have_rbp) {
            // Marco estandar: [rbp+8]=retorno, [rbp+0]=rbp guardado.
            nlohmann::json jret;
            jret["offset"] = 8;
            jret["label"] = frame_label(8);
            jret["size"] = 8;
            jret["kind"] = "retaddr";
            jret["name"] = "direccion de retorno";
            all.push_back(std::move(jret));
            nlohmann::json jrbp;
            jrbp["offset"] = 0;
            jrbp["label"] = "[rbp]";
            jrbp["size"] = 8;
            jrbp["kind"] = "saved";
            jrbp["name"] = "rbp";
            all.push_back(std::move(jrbp));
        }
        for (auto &s : saved_regs)
            all.push_back(std::move(s));
        for (const auto &kv : framemap) {
            nlohmann::json js;
            js["offset"] = kv.first; // disp relativo a rbp (negativo = local)
            js["label"] = frame_label(kv.first);
            js["size"] = kv.second.size;
            js["kind"] = "local";
            js["name"] = kv.second.name;
            all.push_back(std::move(js));
        }
        if (reserved_bytes > 0) {
            nlohmann::json jr;
            jr["offset"] = push_off - reserved_bytes;
            jr["label"] = frame_label(push_off - reserved_bytes);
            jr["size"] = reserved_bytes;
            jr["kind"] = "reserved";
            jr["name"] = "area reservada (locales/shadow)";
            all.push_back(std::move(jr));
        }
        // Orden: offset descendente (cima del marco primero).
        std::sort(all.begin(), all.end(), [](const auto &a, const auto &b) {
            return a["offset"].template get<int64_t>() >
                   b["offset"].template get<int64_t>();
        });
        for (auto &e : all)
            frame_out->push_back(std::move(e));
    }
    return arr;
}

/**
 * @brief Recoge las lineas fuente .vex que abarca una funcion IR.
 *
 * Solo-LSP (vista "Godbolt"): el panel SOURCE muestra el codigo de la
 * funcion.  Calcula el rango [min,max] de @c source_line sobre las
 * instrucciones de @p fn (ignorando 0) y extrae esas lineas del documento.
 *
 * @param doc Texto completo del documento .vex.
 * @param fn  Funcion IR.
 * @return Array JSON de @c {line, text} (1-based), vacio si no hay lineas.
 */
nlohmann::json function_source_lines(const std::string &doc,
                                     const ir::IrFunction &fn) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const auto &blk : fn.blocks) {
        for (const auto &ins : blk.instrs) {
            if (ins.source_line == 0)
                continue;
            lo = std::min(lo, ins.source_line);
            hi = std::max(hi, ins.source_line);
        }
    }
    nlohmann::json arr = nlohmann::json::array();
    if (lo == UINT32_MAX || hi < lo)
        return arr;
    // Trocear el documento en lineas (1-based) y extraer [lo,hi].
    uint32_t cur = 1;
    size_t start = 0;
    for (size_t i = 0; i <= doc.size(); ++i) {
        if (i == doc.size() || doc[i] == '\n') {
            if (cur >= lo && cur <= hi) {
                std::string ln = doc.substr(start, i - start);
                if (!ln.empty() && ln.back() == '\r')
                    ln.pop_back();
                nlohmann::json jl;
                jl["line"] = cur;
                jl["text"] = std::move(ln);
                arr.push_back(std::move(jl));
            }
            start = i + 1;
            ++cur;
            if (cur > hi)
                break;
        }
    }
    return arr;
}

/**
 * @brief Construye la asociacion argumento -> registro de una funcion.
 *
 * Solo-LSP: el desensamblado no dice que registro lleva cada argumento.
 * Esta tabla la calcula desde @c fn.params (nombres) + la convencion de
 * llamada (orden de los registros de argumento).
 *
 * @param fn        Funcion IR.
 * @param arg_regs  Nombres de los registros de argumento en orden (ABI).
 * @return Array JSON de @c {name, reg}.
 */
nlohmann::json function_args(const ir::IrFunction &fn,
                             const std::vector<const char *> &arg_regs) {
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < fn.params.size() && i < arg_regs.size(); ++i) {
        const ir::IrValueId pid = fn.params[i];
        std::string nm;
        if (pid < fn.values.size())
            nm = fn.values[pid].name;
        // Quitar el '%' inicial de los nombres SSA ("%n" -> "n").
        if (!nm.empty() && nm[0] == '%')
            nm = nm.substr(1);
        if (nm.empty())
            nm = "arg" + std::to_string(i);
        nlohmann::json ji;
        ji["name"] = nm;
        ji["reg"] = arg_regs[i];
        arr.push_back(std::move(ji));
    }
    return arr;
}

/**
 * @brief Renderizado corto de una instruccion IR para la correlacion IR<->asm.
 *
 * Solo-LSP: formato compacto "<dst> = <op> <operandos>" (o sin dst), con el
 * nombre de funcion para CALL* y el inmediato para CONST.  Usa los nombres de
 * los valores SSA (ya etiquetados con el nombre de fuente cuando aplica).
 */
std::string ir_short(const ir::IrFunction &fn, const ir::IrInstr &in) {
    auto vn = [&](ir::IrValueId v) -> std::string {
        if (v == ir::IR_NO_VALUE)
            return "?";
        return v < fn.values.size() ? fn.values[v].name
                                    : ("%" + std::to_string(v));
    };
    std::string s;
    if (in.dst != ir::IR_NO_VALUE)
        s += vn(in.dst) + " = ";
    s += ir::ir_op_name(in.op);
    if (!in.func_name.empty()) {
        s += " ";
        s += in.func_name;
    }
    for (size_t k = 0; k < in.operands.size(); ++k) {
        s += (k == 0 ? " " : ", ");
        s += vn(in.operands[k]);
    }
    if (in.op == ir::IrOp::CONST)
        s += " " + std::to_string(static_cast<int64_t>(in.imm));
    return s;
}

/**
 * @brief Construye el mapa linea-de-fuente -> lista de ops IR de esa linea.
 *
 * Solo-LSP: une por @c source_line (clave fiable: tanto el desensamblado como
 * cada IrInstr la llevan).  Omite NOP/PHI (ruido).  Es la base de los modos de
 * correlacion IR<->asm (grupo/panel/3col).
 */
nlohmann::json ir_by_line(const ir::IrFunction &fn) {
    nlohmann::json o = nlohmann::json::object();
    for (const auto &blk : fn.blocks) {
        for (const auto &in : blk.instrs) {
            if (in.source_line == 0)
                continue;
            if (in.op == ir::IrOp::NOP || in.op == ir::IrOp::PHI)
                continue;
            o[std::to_string(in.source_line)].push_back(ir_short(fn, in));
        }
    }
    return o;
}

/**
 * @brief Mapea el nombre de tier textual al enum AOT.
 * @param tier "bare" | "embed" | "full" (insensible a otras formas).
 * @return Tier correspondiente (BARE por defecto).
 */
aot::Tier tier_from_str(const std::string &tier) {
    if (tier == "full")
        return aot::Tier::FULL;
    if (tier == "embed")
        return aot::Tier::EMBED;
    return aot::Tier::BARE;
}

/**
 * @brief Nombre legible de un kind de relocation nativa AOT.
 * @param k Kind de la relocation.
 * @return Cadena ASCII.
 */
const char *reloc_kind_str(jit::NativeReloc::Kind k) {
    switch (k) {
    case jit::NativeReloc::Kind::CALL_REL32: return "CALL_REL32";
    case jit::NativeReloc::Kind::ABS64: return "ABS64";
    case jit::NativeReloc::Kind::DATA_REL32: return "DATA_REL32";
    }
    return "?";
}

} // namespace

/**
 * @struct Inspector::JitState
 * @brief Subsistema JIT propio del inspector (aislado del runtime).
 *
 * Mantiene un @c CodeCache + @c RuntimeEntries + @c JitCompiler dedicados
 * para compilar funciones a x86-64 sin interferir con el JIT del proceso.
 * El mismo patron que usa @c auto_jit al inicializar su singleton.
 */
struct Inspector::JitState {
    jit::CodeCache cache;        ///< Cache de codigo nativo del inspector.
    jit::RuntimeEntries entries; ///< Punteros a runtime entries resueltos.
    jit::JitCompiler compiler;   ///< Compilador JIT.

    JitState() : cache(), entries(), compiler(cache, entries) {
        // Resolver los simbolos vrt_* una vez (estables durante el proceso).
        entries.resolve();
    }
};

Inspector::Inspector(AnalysisEngine &engine, DocumentStore &docs) noexcept
    : engine_(engine), docs_(docs) {}

Inspector::~Inspector() = default;

Inspector::JitState *Inspector::jit_state() {
    if (jit_)
        return jit_.get();
    if (jit_init_failed_)
        return nullptr;
    try {
        jit_ = std::make_unique<JitState>();
    } catch (...) {
        // Si la inicializacion del JIT falla, no reintentar en cada peticion.
        jit_init_failed_ = true;
        return nullptr;
    }
    return jit_.get();
}

namespace {
/// Forward-decls: las definiciones viven mas abajo (mismo TU, namespace
/// anonimo).  Reabrir @c namespace{} refiere a la MISMA entidad de enlace
/// interno, asi que @c Inspector::bytecode puede usarlas antes de definirse.
std::vector<std::string> ir_split_lines(const std::string &s);
std::string vel_extract_fn(const std::string &vel, const std::string &fn);
} // namespace

nlohmann::json Inspector::bytecode(const std::string &uri,
                                   const std::string &function) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const std::string &text = docs_.text(uri);
    // Sin funcion (panel del inspector): modulo entero, texto plano.
    if (function.empty()) {
        const DocAnalysis &an = engine_.analyze_document(uri, text);
        return {{"text", an.result.vel_text}};
    }

    // Por-funcion (hover): vista correlada linea .vex -> bytecode generado,
    // igual estilo que JIT/AOT.  Recompilamos con emit_debug para tener los
    // marcadores `// @line N` y atribuir cada instruccion .vel a su linea.
    // Cache por (uri, hash, fn) -- recompilar es barato pero no en cada frame.
    const uint64_t hsh = fnv1a_hash(text);
    const std::string key =
        uri + "|" + std::to_string(hsh) + "|bc-gb:" + function;
    auto it = view_cache_.find(key);
    if (it != view_cache_.end())
        return nlohmann::json::parse(it->second);

    vex::CompileOptions opts;
    opts.module_name = "main";
    opts.emit_debug = true;         // emite `// @line N` en el .vel
    opts.emit_comptime_fns = true;  // incluir comptime fns (inspeccion)
    vex::CompileResult res = vex::compile_vex_source(text, uri, opts);
    const std::string block = vel_extract_fn(res.vel_text, function);

    // Parsear el bloque: cada `// @line N` fija la linea actual; cada
    // instruccion la hereda; las etiquetas van con linea 0 (contexto).
    nlohmann::json asm_lines = nlohmann::json::array();
    int cur_line = 0;
    for (const auto &raw : ir_split_lines(block)) {
        size_t s = raw.find_first_not_of(" \t");
        if (s == std::string::npos)
            continue; // linea en blanco
        std::string t = raw.substr(s);
        if (t.rfind("// @line ", 0) == 0) {
            cur_line = std::atoi(t.c_str() + 9);
            continue;
        }
        if (t.rfind("//", 0) == 0)
            continue; // otros comentarios (parametros, etc.)
        const bool is_label =
            (raw[0] != ' ' && raw[0] != '\t' && !t.empty() && t.back() == ':');
        nlohmann::json ji;
        ji["addr"] = ""; // el bytecode no tiene offset de byte como el x86
        ji["text"] = t;
        ji["line"] = is_label ? 0 : cur_line;
        asm_lines.push_back(std::move(ji));
    }

    // Post-pase: las etiquetas (line 0) heredan la linea del bloque que
    // encabezan -- forward-fill desde la siguiente instruccion con linea>0,
    // o backward desde la previa.  Asi `factorial_ret:` cae en el bloque del
    // `return` en vez de la inexistente "linea 0".
    {
        int n = static_cast<int>(asm_lines.size());
        for (int i = 0; i < n; ++i) {
            if (asm_lines[i]["line"].get<int>() != 0)
                continue;
            int fill = 0;
            for (int j = i + 1; j < n; ++j) {
                int lj = asm_lines[j]["line"].get<int>();
                if (lj != 0) { fill = lj; break; }
            }
            if (fill == 0) // no hay siguiente; usar la previa
                for (int j = i - 1; j >= 0; --j) {
                    int lj = asm_lines[j]["line"].get<int>();
                    if (lj != 0) { fill = lj; break; }
                }
            if (fill != 0)
                asm_lines[i]["line"] = fill;
        }
    }

    // Fuente de la funcion (mismo rango que JIT/AOT) + args (VM ABI: R1..R12).
    nlohmann::json src = nlohmann::json::array();
    nlohmann::json args = nlohmann::json::array();
    nlohmann::json irbl = nlohmann::json::object();
    ir::IrModule mod;
    if (parse_post_opt_module(res, mod)) {
        const ir::IrFunction *fn = pick_function(mod, function);
        if (fn) {
            src = function_source_lines(text, *fn);
            args = function_args(
                *fn, {"R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9",
                      "R10", "R11", "R12"});
            irbl = ir_by_line(*fn);
        }
    }

    nlohmann::json out;
    out["text"] = block;
    out["function"] = function;
    out["asm_lines"] = std::move(asm_lines);
    out["source"] = std::move(src);
    out["ir_by_line"] = std::move(irbl);
    out["args"] = std::move(args);
    view_cache_[key] = out.dump();
    return out;
}

nlohmann::json Inspector::ir(const std::string &uri, const std::string &phase) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const std::string &text = docs_.text(uri);

    if (phase == "pre") {
        // El IR pre-opt NO esta en el CompileResult cacheado por defecto:
        // exige recompilar con emit_ir_preopt.  Cachear por (uri, hash).
        const uint64_t h = fnv1a_hash(text);
        const std::string key = uri + "|" + std::to_string(h) + "|ir-pre";
        auto it = view_cache_.find(key);
        if (it != view_cache_.end())
            return {{"text", it->second}};

        vex::CompileOptions opts;
        opts.module_name = "main";
        opts.emit_ir_preopt = true;
        vex::CompileResult res = vex::compile_vex_source(text, uri, opts);
        if (res.ir_module_cache_bytes_preopt.empty())
            return {{"error", "no se pudo generar el IR pre-optimizacion"}};
        ir::IrModule mod;
        if (!ir::parse_ir_module_cache(res.ir_module_cache_bytes_preopt, mod))
            return {{"error", "no se pudo deserializar el IR pre-optimizacion"}};
        std::ostringstream oss;
        ir::ir_print(mod, oss);
        std::string rendered = oss.str();
        view_cache_[key] = rendered;
        return {{"text", std::move(rendered)}};
    }

    // phase "post" (o cualquier otra): reutilizar el cache del motor.
    const DocAnalysis &an = engine_.analyze_document(uri, text);
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};
    std::ostringstream oss;
    ir::ir_print(mod, oss);
    return {{"text", oss.str()}};
}

namespace {

/// Parte @p s en lineas (sin el '\n').
std::vector<std::string> ir_split_lines(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t nl = s.find('\n', i);
        if (nl == std::string::npos) {
            if (i < s.size()) out.push_back(s.substr(i));
            break;
        }
        out.push_back(s.substr(i, nl - i));
        i = nl + 1;
    }
    return out;
}

/// Extrae el bloque @c @function <fn>( ... ) del dump @p dump (con sus
/// @c @template_of/@type_args precedentes), hasta el siguiente @c @function o
/// EOF.  Si no se encuentra (o fn vacio), devuelve el dump entero.
std::string ir_extract_fn(const std::string &dump, const std::string &fn) {
    if (fn.empty()) return dump;
    std::vector<std::string> lines = ir_split_lines(dump);
    const std::string want = "@function " + fn + "(";
    int found = -1;
    for (int i = 0; i < (int)lines.size(); ++i)
        if (lines[i].rfind(want, 0) == 0) { found = i; break; }
    if (found < 0) return dump;
    int start = found;
    while (start > 0 && (lines[start - 1].rfind("@template_of", 0) == 0 ||
                         lines[start - 1].rfind("@type_args", 0) == 0))
        --start;
    int end = (int)lines.size();
    for (int i = found + 1; i < (int)lines.size(); ++i)
        if (lines[i].rfind("@function ", 0) == 0) { end = i; break; }
    std::string out;
    for (int i = start; i < end; ++i) {
        out += lines[i];
        out += "\n";
    }
    return out;
}

/// Extrae el bloque .vel (bytecode textual) de UNA funcion del volcado del
/// modulo.  Las funciones se delimitan por una etiqueta a columna 0
/// @c "<fn>:"; las etiquetas internas del cuerpo llevan el prefijo
/// @c "<fn>_" (entry/while_header/ret/...).  El bloque va desde @c "<fn>:"
/// hasta la siguiente etiqueta de nivel superior (una que NO empieza por
/// @c "<fn>_").  Para funciones comptime el frontend emite @c "__macro_<fn>:",
/// asi que probamos ambos nombres.  Si no se encuentra, devuelve el volcado
/// entero (degrada con elegancia).
std::string vel_extract_fn(const std::string &vel, const std::string &fn) {
    if (fn.empty()) return vel;
    std::vector<std::string> lines = ir_split_lines(vel);
    // Helper: ¿la linea es una etiqueta a columna 0?  Devuelve el nombre
    // (sin los dos puntos) o "" si no lo es.
    auto label_of = [](const std::string &s) -> std::string {
        if (s.empty() || s[0] == ' ' || s[0] == '\t') return "";
        if (s.back() != ':') return "";
        std::string id = s.substr(0, s.size() - 1);
        for (char c : id)
            if (!(std::isalnum((unsigned char)c) || c == '_' || c == '.'))
                return "";
        return id;
    };
    // Buscar la etiqueta de la funcion (nombre directo o __macro_<fn>).
    std::string base;
    int found = -1;
    for (int i = 0; i < (int)lines.size(); ++i) {
        std::string lab = label_of(lines[i]);
        if (lab.empty()) continue;
        if (lab == fn || lab == "__macro_" + fn) {
            base = lab;
            found = i;
            break;
        }
    }
    if (found < 0) return vel;
    const std::string pfx = base + "_";
    int end = (int)lines.size();
    for (int i = found + 1; i < (int)lines.size(); ++i) {
        std::string lab = label_of(lines[i]);
        if (lab.empty()) continue;
        // Etiqueta de nivel superior distinta (no es interna de esta fn).
        if (lab != base && lab.rfind(pfx, 0) != 0) {
            end = i;
            break;
        }
    }
    std::string out;
    for (int i = found; i < end; ++i) {
        out += lines[i];
        out += "\n";
    }
    return out;
}

/// Una fila del diff alineado.  kind: "same" (l==r), "del" (solo l, eliminado),
/// "add" (solo r, generado), "chg" (l->r, cambiado).
struct DiffRow {
    std::string kind;
    std::string l, r;
};

/// Diff por lineas (LCS) que produce FILAS ALINEADAS.  Empareja una racha de
/// eliminaciones seguida de una de adiciones en filas "chg" (cambiado), de modo
/// que la version sin/optimizada queden lado a lado.  Capea el coste O(N*M).
std::vector<DiffRow> ir_diff_rows(const std::string &a, const std::string &b) {
    std::vector<std::string> A = ir_split_lines(a), B = ir_split_lines(b);
    const int n = (int)A.size(), m = (int)B.size();
    // Secuencia bruta de operaciones (same/del/add).
    std::vector<DiffRow> ops;
    if ((long long)n * m > 4000000LL) {
        for (int i = 0; i < n; ++i) ops.push_back({"del", A[i], ""});
        for (int j = 0; j < m; ++j) ops.push_back({"add", "", B[j]});
    } else {
        std::vector<std::vector<int>> L(n + 1, std::vector<int>(m + 1, 0));
        for (int i = n - 1; i >= 0; --i)
            for (int j = m - 1; j >= 0; --j)
                L[i][j] = (A[i] == B[j])
                              ? L[i + 1][j + 1] + 1
                              : std::max(L[i + 1][j], L[i][j + 1]);
        int i = 0, j = 0;
        while (i < n && j < m) {
            if (A[i] == B[j]) {
                ops.push_back({"same", A[i], B[i >= 0 ? j : j]});
                ops.back().r = B[j];
                ++i; ++j;
            } else if (L[i + 1][j] >= L[i][j + 1]) {
                ops.push_back({"del", A[i], ""}); ++i;
            } else {
                ops.push_back({"add", "", B[j]}); ++j;
            }
        }
        while (i < n) { ops.push_back({"del", A[i], ""}); ++i; }
        while (j < m) { ops.push_back({"add", "", B[j]}); ++j; }
    }
    // Emparejar rachas del+add consecutivas en filas "chg".
    std::vector<DiffRow> rows;
    for (size_t k = 0; k < ops.size();) {
        if (ops[k].kind == "del") {
            size_t d0 = k;
            while (k < ops.size() && ops[k].kind == "del") ++k;
            size_t a0 = k;
            while (k < ops.size() && ops[k].kind == "add") ++k;
            size_t nd = a0 - d0, na = k - a0;
            size_t paired = nd < na ? nd : na;
            for (size_t p = 0; p < paired; ++p)
                rows.push_back({"chg", ops[d0 + p].l, ops[a0 + p].r});
            for (size_t p = paired; p < nd; ++p)
                rows.push_back({"del", ops[d0 + p].l, ""});
            for (size_t p = paired; p < na; ++p)
                rows.push_back({"add", "", ops[a0 + p].r});
        } else {
            rows.push_back(ops[k]);
            ++k;
        }
    }
    return rows;
}

} // namespace

nlohmann::json Inspector::ir_diff(const std::string &uri,
                                  const std::string &function) {
    nlohmann::json pre = ir(uri, "pre");
    nlohmann::json post = ir(uri, "post");
    if (pre.contains("error")) return pre;
    if (post.contains("error")) return post;
    const std::string pre_block =
        ir_extract_fn(pre.value("text", std::string()), function);
    const std::string post_block =
        ir_extract_fn(post.value("text", std::string()), function);
    std::vector<DiffRow> rows = ir_diff_rows(pre_block, post_block);
    nlohmann::json jrows = nlohmann::json::array();
    for (const auto &row : rows)
        jrows.push_back({{"k", row.kind}, {"l", row.l}, {"r", row.r}});
    nlohmann::json out;
    out["rows"] = std::move(jrows);
    out["function"] = function;
    return out;
}

nlohmann::json Inspector::complexity(const std::string &uri) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    // Coste PARCIAL + composicion interprocedural (coste efectivo real).
    analyze::ModuleCost mc = analyze::analyze_module(mod);
    analyze::compose_interproc(mc);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto &cr : mc.functions) {
        nlohmann::json jf;
        jf["name"] = cr.function;
        jf["partial"] = analyze::cost_class_str(cr.big_o);
        jf["total"] = analyze::cost_class_str(cr.total_class);
        // Confianza: 0=EXACT, 1=HEURISTIC, 2=UNKNOWN.
        jf["confidence"] = static_cast<int>(cr.confidence);
        jf["total_confidence"] = static_cast<int>(cr.total_confidence);
        jf["max_loop_depth"] = cr.max_loop_depth;
        jf["recursive"] = cr.is_recursive;
        jf["divide_conquer"] = cr.is_divide_conquer;
        jf["declared"] = cr.declared_expr; // vacio => sin contrato @complexity.
        jf["contract_mismatch"] = cr.contract_mismatch;
        arr.push_back(std::move(jf));
    }
    nlohmann::json out;
    out["functions"] = std::move(arr);
    return out;
}

nlohmann::json Inspector::diagram(const std::string &uri,
                                  const std::string &kind,
                                  const std::string &format, bool cost) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const std::string &text = docs_.text(uri);

    // Validar kind y format antes de recompilar.
    const bool kind_ok = (kind == "ast" || kind == "ir-pre" ||
                          kind == "ir-post" || kind == "vel");
    if (!kind_ok)
        return {{"error", "kind invalido (use ast|ir-pre|ir-post|vel)"}};
    const bool fmt_ok =
        (format == "mermaid" || format == "graphviz" || format == "html");
    if (!fmt_ok)
        return {{"error", "format invalido (use mermaid|graphviz|html)"}};

    // Cache por (uri, hash, kind, format, cost): la generacion de diagramas
    // recompila con flags concretos -> no repetir peticiones identicas.
    const uint64_t h = fnv1a_hash(text);
    const std::string key = uri + "|" + std::to_string(h) + "|diag:" + kind +
                            ":" + format + ":" + (cost ? "1" : "0");
    auto it = view_cache_.find(key);
    if (it != view_cache_.end())
        return {{"text", it->second}};

    // Activar SOLO el flag de la vista pedida (uno por kind x format).
    vex::CompileOptions opts;
    opts.module_name = "main";
    opts.annotate_cost = cost;
    if (format == "mermaid") {
        if (kind == "ast") opts.dump_mermaid_ast = true;
        else if (kind == "ir-pre") opts.dump_mermaid_ir_pre = true;
        else if (kind == "ir-post") opts.dump_mermaid_ir_post = true;
        else opts.dump_mermaid_vel = true;
    } else if (format == "graphviz") {
        if (kind == "ast") opts.dump_graphviz_ast = true;
        else if (kind == "ir-pre") opts.dump_graphviz_ir_pre = true;
        else if (kind == "ir-post") opts.dump_graphviz_ir_post = true;
        else opts.dump_graphviz_vel = true;
    } else { // html
        if (kind == "ast") opts.dump_html_ast = true;
        else if (kind == "ir-pre") opts.dump_html_ir_pre = true;
        else if (kind == "ir-post") opts.dump_html_ir_post = true;
        else opts.dump_html_vel = true;
    }

    vex::CompileResult res = vex::compile_vex_source(text, uri, opts);
    // Seleccionar el campo del CompileResult que corresponde a la vista.
    std::string out_text;
    if (format == "mermaid") {
        if (kind == "ast") out_text = res.mermaid_ast;
        else if (kind == "ir-pre") out_text = res.mermaid_ir_pre;
        else if (kind == "ir-post") out_text = res.mermaid_ir_post;
        else out_text = res.mermaid_vel;
    } else if (format == "graphviz") {
        if (kind == "ast") out_text = res.graphviz_ast;
        else if (kind == "ir-pre") out_text = res.graphviz_ir_pre;
        else if (kind == "ir-post") out_text = res.graphviz_ir_post;
        else out_text = res.graphviz_vel;
    } else {
        if (kind == "ast") out_text = res.html_ast;
        else if (kind == "ir-pre") out_text = res.html_ir_pre;
        else if (kind == "ir-post") out_text = res.html_ir_post;
        else out_text = res.html_vel;
    }

    if (out_text.empty())
        return {{"error",
                 "el diagrama salio vacio (revisa los diagnosticos del fuente)"}};
    view_cache_[key] = out_text;
    return {{"text", std::move(out_text)}};
}

nlohmann::json Inspector::functions(const std::string &uri) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    nlohmann::json arr = nlohmann::json::array();
    for (const auto &fn : mod.functions) {
        // Saltar stubs nativos y funciones macro-compiladas (no son del
        // codigo del usuario en el sentido habitual).
        if (fn.is_native || fn.is_macro_compiled)
            continue;
        nlohmann::json jf;
        jf["name"] = fn.name;
        jf["line"] = first_source_line(fn);
        arr.push_back(std::move(jf));
    }
    nlohmann::json out;
    out["functions"] = std::move(arr);
    return out;
}

nlohmann::json Inspector::aot_compat(const std::string &uri,
                                     const std::string &tier) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    aot::AotTarget target;
    target.tier = tier_from_str(tier);
    aot::AotCompatReport report = aot::aot_analyze_module(mod, target);

    nlohmann::json issues = nlohmann::json::array();
    for (const auto &iss : report.issues) {
        nlohmann::json ji;
        ji["fn_name"] = iss.fn_name;
        ji["source_line"] = iss.source_line;
        ji["op"] = ir::ir_op_name(iss.op);
        ji["reason"] = iss.reason;
        issues.push_back(std::move(ji));
    }
    nlohmann::json ok = nlohmann::json::array();
    for (const auto &name : report.ok_functions)
        ok.push_back(name);

    nlohmann::json out;
    out["tier"] = tier.empty() ? std::string("bare") : tier;
    out["compatible"] = report.compatible;
    out["issues"] = std::move(issues);
    out["ok_functions"] = std::move(ok);
    return out;
}

nlohmann::json Inspector::jit_asm(const std::string &uri,
                                  const std::string &function) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    const ir::IrFunction *fn = pick_function(mod, function);
    if (!fn && !function.empty()) {
        // Fallback: la funcion puede ser @c comptime (no-macro), que el
        // frontend elide del IR normal.  Recompilar incluyendola para
        // poder inspeccionar su codegen.
        vex::CompileOptions co;
        co.module_name = "main";
        co.emit_comptime_fns = true;
        vex::CompileResult cr2 = vex::compile_vex_source(docs_.text(uri), uri, co);
        ir::IrModule m2;
        if (parse_post_opt_module(cr2, m2)) {
            mod = std::move(m2);
            fn = pick_function(mod, function);
        }
    }
    if (!fn) {
        if (!function.empty())
            return {{"unsupported", true},
                    {"reason",
                     "'" + function +
                         "' no esta en el IO de runtime: puede ser una funcion "
                         "comptime recursiva (se evalua en compilacion; el "
                         "resultado se calcula en el call site) o haber sido "
                         "inlineada/eliminada por el optimizador"}};
        return {{"error", "el modulo no tiene funciones compilables"}};
    }

    // Backend VREG moderno (linear-scan, el mismo que produccion), NO el
    // selector de slots legacy: soporta recursion, llamadas, branches, etc.
    // Para la VISTA usamos el codegen HOST_LEAF (misma seleccion de
    // instrucciones + regalloc que el JIT real; solo difieren prologo y la
    // ABI de las CALL).  Pedimos la tabla linea<->asm para la vista correlada.
    std::vector<jit::NativeReloc> relocs;
    std::vector<jit::LineMapEntry> line_map;
    std::vector<uint8_t> bytes;
    try {
        bytes = jit::vreg_compile_native(
            *fn, /*resolve_call=*/{}, /*ent=*/{}, /*resolve_native=*/{},
            /*resolve_symbol=*/{}, &relocs, /*pic=*/true,
#if defined(_WIN32)
            /*target_sysv=*/false,
#else
            /*target_sysv=*/true,
#endif
            /*mode32=*/false, jit::FloatIsa::SSE2,
            /*emit_line_map=*/true, &line_map);
    } catch (...) {
        return {{"error", "el codegen JIT (vreg) lanzo una excepcion para '" +
                              fn->name + "'"}};
    }
    if (bytes.empty())
        return {{"unsupported", true},
                {"reason", "la funcion '" + fn->name +
                               "' usa operaciones IR aun no soportadas por el "
                               "backend vreg (float/strings/algunos builtins)"}};

    std::string text = disasm_x86_64(bytes.data(), bytes.size(), 0);
    nlohmann::json args = function_args(*fn,
#if defined(_WIN32)
                                        {"rcx", "rdx", "r8", "r9"});
#else
                                        {"rdi", "rsi", "rdx", "rcx", "r8", "r9"});
#endif
    std::vector<std::pair<std::string, std::string>> seed;
    for (const auto &a : args)
        seed.emplace_back(a["reg"].get<std::string>(),
                          a["name"].get<std::string>());
    nlohmann::json frame = nlohmann::json::array();
    nlohmann::json instrs = disasm_x86_64_correlated(
        bytes.data(), bytes.size(), line_map, relocs, seed, &frame);
    nlohmann::json src = function_source_lines(docs_.text(uri), *fn);

    nlohmann::json out;
    out["text"] = std::move(text);
    out["function"] = fn->name;
    out["bytes"] = static_cast<uint64_t>(bytes.size());
    out["instructions"] = static_cast<uint64_t>(line_map.size());
    out["asm_lines"] = std::move(instrs);
    out["source"] = std::move(src);
    out["frame"] = std::move(frame);
    out["ir_by_line"] = ir_by_line(*fn);
    out["args"] = std::move(args);
    return out;
}

nlohmann::json Inspector::aot_asm(const std::string &uri,
                                  const std::string &function) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    ir::IrModule mod;
    if (!parse_post_opt_module(an.result, mod))
        return {{"error", "el modulo no produjo IR (revisa los diagnosticos)"}};

    const ir::IrFunction *fn = pick_function(mod, function);
    if (!fn && !function.empty()) {
        // Fallback: la funcion puede ser @c comptime (no-macro), que el
        // frontend elide del IR normal.  Recompilar incluyendola para
        // poder inspeccionar su codegen.
        vex::CompileOptions co;
        co.module_name = "main";
        co.emit_comptime_fns = true;
        vex::CompileResult cr2 = vex::compile_vex_source(docs_.text(uri), uri, co);
        ir::IrModule m2;
        if (parse_post_opt_module(cr2, m2)) {
            mod = std::move(m2);
            fn = pick_function(mod, function);
        }
    }
    if (!fn) {
        if (!function.empty())
            return {{"unsupported", true},
                    {"reason",
                     "'" + function +
                         "' no esta en el IO de runtime: puede ser una funcion "
                         "comptime recursiva (se evalua en compilacion; el "
                         "resultado se calcula en el call site) o haber sido "
                         "inlineada/eliminada por el optimizador"}};
        return {{"error", "el modulo no tiene funciones compilables"}};
    }

    // Primero comprobar compatibilidad AOT (tier BARE: el subset mas
    // estricto, lo que el codegen nativo aislado puede materializar).
    aot::AotTarget target;
    target.tier = aot::Tier::BARE;
    std::vector<aot::AotIncompat> issues;
    if (!aot::aot_analyze_function(*fn, target, issues)) {
        std::string reason = "la funcion '" + fn->name +
                             "' no es compatible con AOT bare";
        if (!issues.empty()) {
            reason += ": ";
            reason += ir::ir_op_name(issues.front().op);
            reason += " (";
            reason += issues.front().reason;
            reason += ")";
        }
        return {{"incompatible", true}, {"reason", reason}};
    }

    // Compilar a bytes nativos (HOST_LEAF) con resolvers vacios: vista
    // aislada de UNA funcion (las CALL cross-funcion / datos quedan como
    // relocations sin resolver, que se reportan al cliente).
    std::vector<jit::NativeReloc> relocs;
    std::vector<jit::LineMapEntry> line_map;
    std::vector<uint8_t> bytes;
    try {
        bytes = jit::vreg_compile_native(
            *fn, /*resolve_call=*/{}, /*ent=*/{}, /*resolve_native=*/{},
            /*resolve_symbol=*/{}, &relocs, /*pic=*/true,
#if defined(_WIN32)
            /*target_sysv=*/false,
#else
            /*target_sysv=*/true,
#endif
            /*mode32=*/false, jit::FloatIsa::SSE2,
            /*emit_line_map=*/true, &line_map);
    } catch (...) {
        return {{"error", "el codegen AOT lanzo una excepcion para '" +
                              fn->name + "'"}};
    }
    if (bytes.empty())
        return {{"incompatible", true},
                {"reason", "la funcion '" + fn->name +
                               "' no esta soportada por el selector vreg AOT"}};

    std::string text = disasm_x86_64(bytes.data(), bytes.size(), 0);
    nlohmann::json args = function_args(*fn,
#if defined(_WIN32)
                                        {"rcx", "rdx", "r8", "r9"});
#else
                                        {"rdi", "rsi", "rdx", "rcx", "r8", "r9"});
#endif
    std::vector<std::pair<std::string, std::string>> seed;
    for (const auto &a : args)
        seed.emplace_back(a["reg"].get<std::string>(),
                          a["name"].get<std::string>());
    nlohmann::json frame = nlohmann::json::array();
    // Vista correlada (solo-LSP): asm por-instruccion + fuente de la funcion.
    nlohmann::json instrs = disasm_x86_64_correlated(
        bytes.data(), bytes.size(), line_map, relocs, seed, &frame);
    nlohmann::json src = function_source_lines(docs_.text(uri), *fn);

    nlohmann::json jrelocs = nlohmann::json::array();
    for (const auto &r : relocs) {
        nlohmann::json jr;
        jr["offset"] = r.offset;
        jr["kind"] = reloc_kind_str(r.kind);
        jr["symbol"] = r.symbol;
        jr["addend"] = r.addend;
        jrelocs.push_back(std::move(jr));
    }

    nlohmann::json out;
    out["text"] = std::move(text);
    out["function"] = fn->name;
    out["bytes"] = static_cast<uint64_t>(bytes.size());
    out["instructions"] = static_cast<uint64_t>(line_map.size());
    out["relocs"] = std::move(jrelocs);
    // Solo-LSP: correlacion fuente <-> asm para la vista godbolt.
    out["asm_lines"] = std::move(instrs);
    out["source"] = std::move(src);
    out["frame"] = std::move(frame);
    out["ir_by_line"] = ir_by_line(*fn);
    out["args"] = std::move(args);
    return out;
}

nlohmann::json Inspector::macro_expand(const std::string &uri) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    // Las expectaciones de @Macro y las razones de skip se pueblan en la
    // compilacion normal: reutilizar el CompileResult cacheado por el motor.
    const DocAnalysis &an = engine_.analyze_document(uri, docs_.text(uri));
    const vex::CompileResult &res = an.result;

    nlohmann::json expansions = nlohmann::json::array();
    for (const auto &e : res.macro_expectations) {
        nlohmann::json je;
        je["macro_name"] = e.macro_name;
        je["call_site_loc"] = e.src_loc;
        nlohmann::json jargs = nlohmann::json::array();
        for (uint64_t a : e.args)
            jargs.push_back(a);
        je["args"] = std::move(jargs);
        je["generated_code"] = e.expected_str;
        expansions.push_back(std::move(je));
    }

    nlohmann::json skipped = nlohmann::json::array();
    for (const auto &s : res.macro_skip_reasons) {
        nlohmann::json js;
        js["name"] = s.first;
        js["reason"] = s.second;
        skipped.push_back(std::move(js));
    }

    nlohmann::json out;
    out["expansions"] = std::move(expansions);
    out["skipped"] = std::move(skipped);
    return out;
}

nlohmann::json Inspector::comptime_values(const std::string &uri) {
    if (!docs_.has(uri))
        return {{"error", "documento no abierto"}};
    const std::string &text = docs_.text(uri);

    // El snapshot de valores comptime exige recompilar con el flag
    // dump_comptime_values (no se hace en el analyze por pulsacion).  Cachear
    // el JSON por (uri, hash) para no recompilar peticiones identicas.
    const uint64_t h = fnv1a_hash(text);
    const std::string key = uri + "|" + std::to_string(h) + "|comptime-values";
    auto it = view_cache_.find(key);
    if (it != view_cache_.end())
        return nlohmann::json::parse(it->second);

    vex::CompileOptions opts;
    opts.module_name = "main";
    opts.dump_comptime_values = true;
    vex::CompileResult res = vex::compile_vex_source(text, uri, opts);

    nlohmann::json values = nlohmann::json::array();
    for (const auto &v : res.comptime_values) {
        nlohmann::json jv;
        jv["name"] = v.name;
        jv["scope"] = v.scope;
        jv["type_kind"] = v.type_kind;
        jv["value_str"] = v.value_str;
        // Ubicacion (1-based; 0 = sin ubicacion) y clase de builtin, para que
        // el cliente pueda mostrar el valor inline (ghost text) sobre la
        // expresion sizeof/kind/... en su linea.
        jv["line"] = v.loc.line;
        jv["column"] = v.loc.column;
        jv["builtin_kind"] = v.builtin_kind;
        values.push_back(std::move(jv));
    }

    nlohmann::json out;
    out["values"] = std::move(values);
    view_cache_[key] = out.dump();
    return out;
}

} // namespace lsp
