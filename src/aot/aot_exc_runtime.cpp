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
 * @file aot_exc_runtime.cpp
 * @brief Emision de __vx_setjmp / __vx_longjmp nativos (hand-rolled).
 */

#include "aot/aot_exc_runtime.h"

namespace aot {

namespace {

/// Anade los bytes de @c b al final de @c out.
inline void put(std::vector<uint8_t> &out,
                std::initializer_list<uint8_t> b) {
    out.insert(out.end(), b);
}

/// MOV [base_reg + disp8], src_reg  (REX.W; base = rdi(7)/rcx(1); regs 0-15).
/// rex_b/rex_r calculados; disp8 cabe en [-128,127] (offsets del buffer <128).
inline void mov_mem_reg(std::vector<uint8_t> &out, uint8_t base, uint8_t src,
                        int disp) {
    const uint8_t rex = 0x48 | ((src >= 8) ? 0x04 : 0) | ((base >= 8) ? 0x01 : 0);
    out.push_back(rex);
    out.push_back(0x89); // MOV r/m64, r64
    // ModRM: si disp==0 y base no es rbp/r13 -> mod=00; usamos disp8 siempre
    // (offsets pequenos) salvo disp==0 con base!=rbp.
    const uint8_t b3 = base & 7, s3 = src & 7;
    if (disp == 0 && b3 != 5) {
        out.push_back(static_cast<uint8_t>((0x00 << 6) | (s3 << 3) | b3));
    } else {
        out.push_back(static_cast<uint8_t>((0x01 << 6) | (s3 << 3) | b3));
        out.push_back(static_cast<uint8_t>(disp & 0xFF));
    }
}

/// MOV dst_reg, [base_reg + disp8]  (REX.W).
inline void mov_reg_mem(std::vector<uint8_t> &out, uint8_t dst, uint8_t base,
                        int disp) {
    const uint8_t rex = 0x48 | ((dst >= 8) ? 0x04 : 0) | ((base >= 8) ? 0x01 : 0);
    out.push_back(rex);
    out.push_back(0x8B); // MOV r64, r/m64
    const uint8_t b3 = base & 7, d3 = dst & 7;
    if (disp == 0 && b3 != 5) {
        out.push_back(static_cast<uint8_t>((0x00 << 6) | (d3 << 3) | b3));
    } else {
        out.push_back(static_cast<uint8_t>((0x01 << 6) | (d3 << 3) | b3));
        out.push_back(static_cast<uint8_t>(disp & 0xFF));
    }
}

// IDs de registro x86-64.
enum : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15
};

// Conjunto de GP callee-saved a salvar, por ABI, en orden de offset.
struct SaveSet {
    const uint8_t *regs;
    int count;
};

const uint8_t kSysvSaved[] = {RBX, RBP, R12, R13, R14, R15};         // 6
const uint8_t kWin64Saved[] = {RBX, RBP, RDI, RSI, R12, R13, R14, R15}; // 8

SaveSet save_set(bool sysv) {
    return sysv ? SaveSet{kSysvSaved, 6} : SaveSet{kWin64Saved, 8};
}

} // namespace

ExcBufLayout aot_exc_buf_layout(bool sysv) {
    const int n = save_set(sysv).count;       // GP callee-saved
    const int rsp_off = n * 8;                // tras los GP
    const int rip_off = rsp_off + 8;
    return ExcBufLayout{rsp_off, rip_off, rip_off + 8};
}

std::vector<uint8_t> aot_exc_setjmp_bytes(bool sysv) {
    std::vector<uint8_t> out;
    const uint8_t buf = sysv ? RDI : RCX; // arg0 = buffer
    const SaveSet ss = save_set(sysv);
    const ExcBufLayout L = aot_exc_buf_layout(sysv);
    // Salvar los GP callee-saved en offsets 0, 8, 16, ...
    for (int i = 0; i < ss.count; ++i)
        mov_mem_reg(out, buf, ss.regs[i], i * 8);
    // rsp del CALLER (tras el ret): lea rax, [rsp+8]; mov [buf+rsp_off], rax.
    put(out, {0x48, 0x8D, 0x44, 0x24, 0x08}); // lea rax, [rsp+8]
    mov_mem_reg(out, buf, RAX, L.rsp_off);
    // rip (return addr en [rsp]): mov rax, [rsp]; mov [buf+rip_off], rax.
    put(out, {0x48, 0x8B, 0x04, 0x24}); // mov rax, [rsp]
    mov_mem_reg(out, buf, RAX, L.rip_off);
    // return 0.
    put(out, {0x31, 0xC0}); // xor eax, eax
    put(out, {0xC3});       // ret
    return out;
}

std::vector<uint8_t> aot_exc_longjmp_bytes(bool sysv) {
    std::vector<uint8_t> out;
    const uint8_t buf = sysv ? RDI : RCX; // arg0 = buffer
    const uint8_t val = sysv ? RSI : RDX; // arg1 = valor de retorno
    const SaveSet ss = save_set(sysv);
    const ExcBufLayout L = aot_exc_buf_layout(sysv);
    // Restaurar los GP callee-saved.
    for (int i = 0; i < ss.count; ++i)
        mov_reg_mem(out, ss.regs[i], buf, i * 8);
    // Restaurar rsp.
    mov_reg_mem(out, RSP, buf, L.rsp_off);
    // rax = val; if (rax == 0) rax = 1.
    {
        const uint8_t rex = 0x48 | ((val >= 8) ? 0x04 : 0); // mov rax, val
        out.push_back(rex);
        out.push_back(0x89);
        out.push_back(static_cast<uint8_t>(0xC0 | ((val & 7) << 3))); // ModRM 11 val rax
    }
    put(out, {0x48, 0x85, 0xC0}); // test rax, rax
    put(out, {0x75, 0x03});       // jnz +3 (saltar el inc)
    put(out, {0x48, 0xFF, 0xC0}); // inc rax
    // jmp [buf + rip_off]  (FF /4).
    {
        const uint8_t rex = ((buf >= 8) ? 0x41 : 0x00);
        if (rex) out.push_back(rex);
        out.push_back(0xFF);
        const uint8_t b3 = buf & 7;
        out.push_back(static_cast<uint8_t>((0x01 << 6) | (4 << 3) | b3)); // mod01 /4
        out.push_back(static_cast<uint8_t>(L.rip_off & 0xFF));
    }
    return out;
}

} // namespace aot
