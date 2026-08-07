/**
 * @file test_asm_analyze.cpp
 * @brief Tests del resumen de efectos de bloque de un cuerpo de inline asm.
 *        Ver vx/asm_analyze.h.  Llama a la API C++ directamente -- el mismo uso
 *        que hara el servidor de lenguaje (linkado contra el compilador).
 *
 * Cubre las cuatro arquitecturas: x86_64, x86 (32-bit), x86_16 y arm64.
 */
#include "vx/asm/asm_analyze.h"

#include <cstdio>
#include <string>

using namespace vx;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_fail;                                                           \
            std::printf("  FAIL: %s (linea %d)\n", (msg), __LINE__);            \
        }                                                                       \
    } while (0)

int main() {
    // --- x86-64: lock cmpxchg (CAS atomico) ------------------------------
    {
        AsmBlockEffects e =
            asm_analyze_block("  mov rax, rsi\n  lock cmpxchg [rdi], rdx\n",
                              "x86_64");
        CHECK(e.known(), "x86_64 cas: mnemonicos conocidos");
        CHECK(e.has_atomic, "x86_64 cas: lock -> atomica");
        CHECK(e.touches_mem, "x86_64 cas: toca memoria");
        CHECK(!e.is_call, "x86_64 cas: sin call");
        CHECK(e.explicit_stack_bytes == 0, "x86_64 cas: sin marco explicito");
    }

    // --- x86-64/32/16: el mismo push mide 8/4/2 bytes --------------------
    {
        const char *body = "  push rbp\n";
        CHECK(asm_analyze_block(body, "x86_64").explicit_stack_bytes == 8,
              "x86_64: push = 8B");
        CHECK(asm_analyze_block(body, "x86").explicit_stack_bytes == 4,
              "x86-32: push = 4B");
        CHECK(asm_analyze_block(body, "x86_16").explicit_stack_bytes == 2,
              "x86_16: push = 2B");
    }

    // --- x86: prologo con marco explicito (pico, no neto) ----------------
    {
        AsmBlockEffects e = asm_analyze_block(
            "  push rbp\n  sub rsp, 32\n  mov [rsp], rax\n  add rsp, 32\n"
            "  pop rbp\n",
            "x86_64");
        CHECK(e.known(), "x86 prologo: conocido");
        CHECK(e.explicit_stack_bytes == 40, "x86 prologo: marco pico = 40");
        CHECK(e.touches_mem, "x86 prologo: toca memoria");
    }

    // --- x86: rama + flags -----------------------------------------------
    {
        AsmBlockEffects e =
            asm_analyze_block("  cmp rax, rbx\n  jne .otro\n.otro:\n", "x86_64");
        CHECK(e.has_branch, "x86 rama: jne -> rama");
        CHECK(e.touches_flags, "x86 rama: cmp -> flags");
        CHECK(!e.touches_mem, "x86 rama: sin memoria");
    }

    // --- arm64: bucle LL/SC (ldaxr/stlxr) TODO reconocido ----------------
    {
        AsmBlockEffects e = asm_analyze_block(
            ".retry:\n  ldaxr x3, [x0]\n  cmp x3, x1\n  b.ne .done\n"
            "  stlxr w4, x2, [x0]\n  cbnz w4, .retry\n.done:\n",
            "arm64");
        CHECK(e.known(), "arm64 cas: TODO reconocido (0 desconocidos)");
        CHECK(e.has_atomic, "arm64 cas: ldaxr/stlxr -> atomica");
        CHECK(e.touches_mem, "arm64 cas: [x0] -> memoria");
        CHECK(e.has_branch, "arm64 cas: b.ne/cbnz -> rama");
        CHECK(e.touches_flags, "arm64 cas: cmp -> flags");
        CHECK(e.explicit_stack_bytes == 0, "arm64 cas: sin marco explicito");
    }

    // --- arm64: CAS de armv8.1 + marco explicito sub sp ------------------
    {
        AsmBlockEffects e = asm_analyze_block(
            "  sub sp, sp, #16\n  casal x0, x1, [x2]\n  add sp, sp, #16\n",
            "arm64");
        CHECK(e.known(), "arm64 casal: reconocido");
        CHECK(e.has_atomic, "arm64 casal: -> atomica");
        CHECK(e.touches_mem, "arm64 casal: -> memoria");
        CHECK(e.explicit_stack_bytes == 16, "arm64: sub sp,#16 -> 16B");
    }

    // --- arm64: un mnemonico x86 NO existe en arm64 (error claro) --------
    {
        AsmBlockEffects e = asm_analyze_block("  cpuid\n", "arm64");
        CHECK(!e.known(), "arm64: cpuid no es de arm64 -> desconocido");
        CHECK(e.unknown_mnemonics == std::vector<std::string>{"cpuid"},
              "arm64: nombra cpuid");
    }

    // --- desconocido en x86: error claro ---------------------------------
    {
        AsmBlockEffects e =
            asm_analyze_block("  chorradadesconocida rax, rbx\n", "x86_64");
        CHECK(!e.known(), "desconocido: known()==false");
        CHECK(e.unknown_mnemonics ==
                  std::vector<std::string>{"chorradadesconocida"},
              "desconocido: nombra el mnemonico exacto");
    }

    // --- leer memoria NO es escribirla -----------------------------------
    // Decirlo de menos convertiria el bloque en una barrera para todo lo que
    // lo rodea; decirlo de mas dejaria reordenar algo que no se puede.  Por
    // eso ante cualquier duda se marcan las dos.
    {
        AsmBlockEffects e = asm_analyze_block("  mov rax, [rdi]\n", "x86_64");
        CHECK(e.reads_mem, "carga: lee memoria");
        CHECK(!e.writes_mem, "carga: NO escribe memoria");
        CHECK(e.touches_mem, "carga: toca memoria (compatibilidad)");
    }
    {
        AsmBlockEffects e = asm_analyze_block("  mov [rdi], rax\n", "x86_64");
        CHECK(e.writes_mem, "almacen: escribe memoria");
    }
    {
        AsmBlockEffects e = asm_analyze_block("  cmp rax, [rdi]\n", "x86_64");
        CHECK(e.reads_mem && !e.writes_mem, "comparar: solo lee");
    }
    {
        // Modo de direccionamiento con coma: la coma de dentro de los
        // corchetes no separa operandos.
        AsmBlockEffects e =
            asm_analyze_block("  mov rax, [rbx + rcx*8]\n", "x86_64");
        CHECK(e.reads_mem && !e.writes_mem,
              "modo [base+indice*escala]: sigue siendo solo lectura");
    }
    {
        AsmBlockEffects e = asm_analyze_block("  add [rdi], rax\n", "x86_64");
        CHECK(e.reads_mem && e.writes_mem,
              "acumular en memoria: lee Y escribe");
    }
    {
        AsmBlockEffects e =
            asm_analyze_block("  lock xadd [rdi], rax\n", "x86_64");
        CHECK(e.reads_mem && e.writes_mem, "atomica: lee Y escribe");
    }
    {
        AsmBlockEffects e = asm_analyze_block("  add rax, rbx\n", "x86_64");
        CHECK(!e.reads_mem && !e.writes_mem && !e.touches_mem,
              "aritmetica de registros: no toca memoria");
    }

    // --- por que registro se llega a la memoria ---------------------------
    // Sirve para decir QUE memoria toca el bloque en vez de "cualquiera".
    {
        AsmBlockEffects e = asm_analyze_block("  mov [rdi], rax\n", "x86_64");
        CHECK(e.accesos.size() == 1 && !e.accesos_incompletos,
              "un acceso, atribuido");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "rdi",
              "la base es rdi");
        CHECK(!e.accesos.empty() && e.accesos[0].escribe, "y se escribe");
    }
    {
        // El nombre se canonicaliza: el registro es el mismo aunque se nombre
        // por su mitad baja.
        AsmBlockEffects e = asm_analyze_block("  mov eax, [ebx]\n", "x86_64");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "rbx",
              "ebx canonicaliza a rbx");
        CHECK(!e.accesos.empty() && !e.accesos[0].escribe, "y solo se lee");
    }
    {
        AsmBlockEffects e =
            asm_analyze_block("  mov rax, [rbx + rcx*8]\n", "x86_64");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "rbx",
              "la base de [base+indice*escala] es la base");
    }
    {
        /* Si el bloque REESCRIBE el registro base antes de usarlo, el acceso ya
         * no va a donde apuntaba la variable ligada: no se puede atribuir. */
        AsmBlockEffects e =
            asm_analyze_block("  mov rdi, rsi\n  mov [rdi], rax\n", "x86_64");
        CHECK(e.accesos_incompletos,
              "base reescrita por el propio bloque: no se atribuye");
    }
    {
        // Direccion absoluta: no hay registro base que seguir.
        AsmBlockEffects e = asm_analyze_block("  mov rax, [0x1000]\n", "x86_64");
        CHECK(e.accesos_incompletos, "direccion absoluta: sin base");
    }

    // --- memoria IMPLICITA pero CONOCIDA ----------------------------------
    // Una instruccion de cadena no escribe los corchetes, pero la arquitectura
    // dice por donde accede.  Decir "toca memoria en algun sitio" seria dejar
    // el analisis a medias, no ser prudente.
    {
        AsmBlockEffects e = asm_analyze_block("  movsb\n", "x86_64");
        CHECK(e.reads_mem && e.writes_mem, "movsb: lee y escribe memoria");
        CHECK(!e.accesos_incompletos, "movsb: sus accesos SI se atribuyen");
        CHECK(e.accesos.size() == 2, "movsb: dos accesos (origen y destino)");
        bool lee_rsi = false, escribe_rdi = false;
        for (const auto &a : e.accesos) {
            if (a.base == "rsi" && !a.escribe) lee_rsi = true;
            if (a.base == "rdi" && a.escribe) escribe_rdi = true;
        }
        CHECK(lee_rsi, "movsb: lee por rsi");
        CHECK(escribe_rdi, "movsb: escribe por rdi");
    }
    {
        AsmBlockEffects e = asm_analyze_block("  stosb\n", "x86_64");
        CHECK(e.accesos.size() == 1 && e.accesos[0].base == "rdi" &&
                  e.accesos[0].escribe,
              "stosb: solo escribe, por rdi");
    }
    {
        AsmBlockEffects e = asm_analyze_block("  lodsb\n", "x86_64");
        CHECK(e.accesos.size() == 1 && e.accesos[0].base == "rsi" &&
                  !e.accesos[0].escribe,
              "lodsb: solo lee, por rsi");
    }

    // --- la forma moderna: el operando es un marcador ---------------------
    // `asm ( reg d = p, reg t, ) { mov t, [d] }` llega con el registro sin
    // elegir todavia (lo elige el asignador), pero el marcador YA identifica
    // el operando -- que es lo unico que hace falta, y ademas no depende de
    // nombres de registro.
    {
        AsmBlockEffects e = asm_analyze_block("  mov $1, [$0]\n", "x86_64");
        CHECK(e.reads_mem && !e.writes_mem, "marcador: solo lee");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "$0",
              "marcador: la base es $0");
    }
    {
        AsmBlockEffects e = asm_analyze_block("  mov [$0], $1\n", "x86_64");
        CHECK(!e.accesos.empty() && e.accesos[0].escribe,
              "marcador: el almacen escribe");
    }
    {
        // Y la misma guarda: si el bloque pisa el marcador base, no se atribuye.
        AsmBlockEffects e =
            asm_analyze_block("  mov $0, $1\n  mov [$0], $2\n", "x86_64");
        CHECK(e.accesos_incompletos,
              "marcador base reescrito: no se atribuye");
    }

    // --- lo mismo en arm64, analizado desde un build de x86 ---------------
    // El arch es un DATO del analisis, no del entorno: una variante por
    // @Target se compila con los registros de SU arquitectura.  Si esto
    // canonicalizara con los del objetivo activo, la base saldria vacia.
    {
        AsmBlockEffects e = asm_analyze_block("  str x1, [x0]\n", "arm64");
        CHECK(e.writes_mem, "arm64: str escribe memoria");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "x0",
              "arm64: la base es x0");
        CHECK(!e.accesos.empty() && e.accesos[0].escribe,
              "arm64: el acceso escribe");
    }
    {
        AsmBlockEffects e = asm_analyze_block("  ldr x1, [x0, #8]\n", "arm64");
        CHECK(e.reads_mem && !e.writes_mem, "arm64: ldr solo lee");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "x0",
              "arm64: base de [x0, #8]");
    }
    {
        AsmBlockEffects e =
            asm_analyze_block("  ldr x2, [x0, x1, lsl #3]\n", "arm64");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "x0",
              "arm64: base de [base, indice, desplazamiento]");
    }
    {
        // El registro de 32 bits canonicaliza al mismo fisico.
        AsmBlockEffects e = asm_analyze_block("  ldr w1, [x0]\n", "arm64");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "x0",
              "arm64: w1/x1 comparten canonico");
    }

    if (g_fail == 0)
        std::printf("=== test_asm_analyze: %d checks OK, 0 fallidos ===\n",
                    g_checks);
    else
        std::printf("=== test_asm_analyze: %d checks, %d FALLIDOS ===\n",
                    g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
