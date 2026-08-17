/**
 * @file test_asm_analyze.cpp
 * @brief Tests del resumen de efectos de bloque de un cuerpo de inline asm.
 *        Ver vx/asm_analyze.h.  Llama a la API C++ directamente -- el mismo uso
 *        que hara el servidor de lenguaje (linkado contra el compilador).
 *
 * Cubre las cuatro arquitecturas: x86_64, x86 (32-bit), x86_16 y arm64.
 */
#include "vx/asm/asm_analyze.h"
#include "vx/asm/instr_db.h"

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
            asm_analyze_block_no_classes("  mov rax, rsi\n  lock cmpxchg [rdi], rdx\n",
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
        CHECK(asm_analyze_block_no_classes(body, "x86_64").explicit_stack_bytes == 8,
              "x86_64: push = 8B");
        CHECK(asm_analyze_block_no_classes(body, "x86").explicit_stack_bytes == 4,
              "x86-32: push = 4B");
        CHECK(asm_analyze_block_no_classes(body, "x86_16").explicit_stack_bytes == 2,
              "x86_16: push = 2B");
    }

    // --- x86: prologo con marco explicito (pico, no neto) ----------------
    {
        AsmBlockEffects e = asm_analyze_block_no_classes(
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
            asm_analyze_block_no_classes("  cmp rax, rbx\n  jne .otro\n.otro:\n", "x86_64");
        CHECK(e.has_branch, "x86 rama: jne -> rama");
        CHECK(e.touches_flags, "x86 rama: cmp -> flags");
        CHECK(!e.touches_mem, "x86 rama: sin memoria");
    }

    // --- arm64: bucle LL/SC (ldaxr/stlxr) TODO reconocido ----------------
    {
        AsmBlockEffects e = asm_analyze_block_no_classes(
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
        AsmBlockEffects e = asm_analyze_block_no_classes(
            "  sub sp, sp, #16\n  casal x0, x1, [x2]\n  add sp, sp, #16\n",
            "arm64");
        CHECK(e.known(), "arm64 casal: reconocido");
        CHECK(e.has_atomic, "arm64 casal: -> atomica");
        CHECK(e.touches_mem, "arm64 casal: -> memoria");
        CHECK(e.explicit_stack_bytes == 16, "arm64: sub sp,#16 -> 16B");
    }

    // --- arm64: un mnemonico x86 NO existe en arm64 (error claro) --------
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  cpuid\n", "arm64");
        CHECK(!e.known(), "arm64: cpuid no es de arm64 -> desconocido");
        CHECK(e.unknown_mnemonics == std::vector<std::string>{"cpuid"},
              "arm64: nombra cpuid");
    }

    // --- desconocido en x86: error claro ---------------------------------
    {
        AsmBlockEffects e =
            asm_analyze_block_no_classes("  chorradadesconocida rax, rbx\n", "x86_64");
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
        AsmBlockEffects e = asm_analyze_block_no_classes("  mov rax, [rdi]\n", "x86_64");
        CHECK(e.reads_mem, "carga: lee memoria");
        CHECK(!e.writes_mem, "carga: NO escribe memoria");
        CHECK(e.touches_mem, "carga: toca memoria (compatibilidad)");
    }
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  mov [rdi], rax\n", "x86_64");
        CHECK(e.writes_mem, "almacen: escribe memoria");
    }
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  cmp rax, [rdi]\n", "x86_64");
        CHECK(e.reads_mem && !e.writes_mem, "comparar: solo lee");
    }
    {
        // Modo de direccionamiento con coma: la coma de dentro de los
        // corchetes no separa operandos.
        AsmBlockEffects e =
            asm_analyze_block_no_classes("  mov rax, [rbx + rcx*8]\n", "x86_64");
        CHECK(e.reads_mem && !e.writes_mem,
              "modo [base+indice*escala]: sigue siendo solo lectura");
    }
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  add [rdi], rax\n", "x86_64");
        CHECK(e.reads_mem && e.writes_mem,
              "acumular en memoria: lee Y escribe");
    }
    {
        AsmBlockEffects e =
            asm_analyze_block_no_classes("  lock xadd [rdi], rax\n", "x86_64");
        CHECK(e.reads_mem && e.writes_mem, "atomica: lee Y escribe");
    }
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  add rax, rbx\n", "x86_64");
        CHECK(!e.reads_mem && !e.writes_mem && !e.touches_mem,
              "aritmetica de registros: no toca memoria");
    }

    // --- por que registro se llega a la memoria ---------------------------
    // Sirve para decir QUE memoria toca el bloque en vez de "cualquiera".
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  mov [rdi], rax\n", "x86_64");
        CHECK(e.accesos.size() == 1 && !e.accesos_incompletos,
              "un acceso, atribuido");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "rdi",
              "la base es rdi");
        CHECK(!e.accesos.empty() && e.accesos[0].escribe, "y se escribe");
    }
    {
        // El nombre se canonicaliza: el registro es el mismo aunque se nombre
        // por su mitad baja.
        AsmBlockEffects e = asm_analyze_block_no_classes("  mov eax, [ebx]\n", "x86_64");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "rbx",
              "ebx canonicaliza a rbx");
        CHECK(!e.accesos.empty() && !e.accesos[0].escribe, "y solo se lee");
    }
    {
        AsmBlockEffects e =
            asm_analyze_block_no_classes("  mov rax, [rbx + rcx*8]\n", "x86_64");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "rbx",
              "la base de [base+indice*escala] es la base");
    }
    {
        /* Si el bloque REESCRIBE el registro base antes de usarlo, el acceso ya
         * no va a donde apuntaba ese registro AL ENTRAR -- pero eso no es
         * perderlo: se sabe de donde salio el valor, y el acceso se atribuye a su
         * ORIGEN.
         *
         * Este caso exigia lo contrario: que el analisis se rindiera.  Se quedo
         * atras cuando el seguimiento del origen entro, y no se noto porque el
         * fichero llevaba tiempo sin compilar -- llamaba a `asm_analyze_block` con
         * dos argumentos, y la version de dos se llama distinto --.  Un test que
         * no compila no falla: desaparece. */
        AsmBlockEffects e =
            asm_analyze_block_no_classes("  mov rdi, rsi\n  mov [rdi], rax\n", "x86_64");
        CHECK(!e.accesos_incompletos && e.accesos.size() == 1 &&
                  e.accesos[0].base == "rsi" && e.accesos[0].escribe,
              "base reescrita: el acceso se atribuye a su ORIGEN, no se abandona");
    }
    {
        // Direccion absoluta: no hay registro base que seguir.
        AsmBlockEffects e = asm_analyze_block_no_classes("  mov rax, [0x1000]\n", "x86_64");
        CHECK(e.accesos_incompletos, "direccion absoluta: sin base");
    }

    // --- memoria IMPLICITA pero CONOCIDA ----------------------------------
    // Una instruccion de cadena no escribe los corchetes, pero la arquitectura
    // dice por donde accede.  Decir "toca memoria en algun sitio" seria dejar
    // el analisis a medias, no ser prudente.
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  movsb\n", "x86_64");
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
        AsmBlockEffects e = asm_analyze_block_no_classes("  stosb\n", "x86_64");
        CHECK(e.accesos.size() == 1 && e.accesos[0].base == "rdi" &&
                  e.accesos[0].escribe,
              "stosb: solo escribe, por rdi");
    }
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  lodsb\n", "x86_64");
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
        AsmBlockEffects e = asm_analyze_block_no_classes("  mov $1, [$0]\n", "x86_64");
        CHECK(e.reads_mem && !e.writes_mem, "marcador: solo lee");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "$0",
              "marcador: la base es $0");
    }
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  mov [$0], $1\n", "x86_64");
        CHECK(!e.accesos.empty() && e.accesos[0].escribe,
              "marcador: el almacen escribe");
    }
    {
        /* Y lo mismo con un marcador: si el bloque pisa el que sirve de base, el
         * acceso se atribuye al operando de donde salio el valor.  Vale igual que
         * con un registro con nombre, y ademas es el camino que importa: los
         * marcadores son lo que emite el compilador. */
        AsmBlockEffects e =
            asm_analyze_block_no_classes("  mov $0, $1\n  mov [$0], $2\n", "x86_64");
        CHECK(!e.accesos_incompletos && e.accesos.size() == 1 &&
                  e.accesos[0].base == "$1" && e.accesos[0].escribe,
              "marcador base reescrito: se atribuye a su ORIGEN ($1)");
    }

    // --- lo mismo en arm64, analizado desde un build de x86 ---------------
    // El arch es un DATO del analisis, no del entorno: una variante por
    // @Target se compila con los registros de SU arquitectura.  Si esto
    // canonicalizara con los del objetivo activo, la base saldria vacia.
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  str x1, [x0]\n", "arm64");
        CHECK(e.writes_mem, "arm64: str escribe memoria");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "x0",
              "arm64: la base es x0");
        CHECK(!e.accesos.empty() && e.accesos[0].escribe,
              "arm64: el acceso escribe");
    }
    {
        AsmBlockEffects e = asm_analyze_block_no_classes("  ldr x1, [x0, #8]\n", "arm64");
        CHECK(e.reads_mem && !e.writes_mem, "arm64: ldr solo lee");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "x0",
              "arm64: base de [x0, #8]");
    }
    {
        AsmBlockEffects e =
            asm_analyze_block_no_classes("  ldr x2, [x0, x1, lsl #3]\n", "arm64");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "x0",
              "arm64: base de [base, indice, desplazamiento]");
    }
    {
        // El registro de 32 bits canonicaliza al mismo fisico.
        AsmBlockEffects e = asm_analyze_block_no_classes("  ldr w1, [x0]\n", "arm64");
        CHECK(!e.accesos.empty() && e.accesos[0].base == "x0",
              "arm64: w1/x1 comparten canonico");
    }

    /* --- Dependencias POR BANDERA -------------------------------------------
     *
     * Saber que banderas toca cada instruccion solo sirve si alguien lo usa, y
     * quien lo usa es esto: decidir si dos instrucciones se estorban.  Con un
     * solo bit de "toca banderas", cualquier par que las tocara chocaba, y eso
     * impide reordenar cosas que no tienen nada que ver.
     *
     * Los casos estan elegidos para que fallen si se vuelve al bit grueso. */
    {
        using vx::instr_db::asm_dep_conflict;
        using vx::instr_db::asm_insn_sem;
        using vx::instr_db::Isa;
        auto choca = [](const char *a, const char *b) {
            return asm_dep_conflict(asm_insn_sem(Isa::X86, a, 0),
                                    asm_insn_sem(Isa::X86, b, 0));
        };
        CHECK(!choca("cld", "adc rax, rdx"),
              "cld toca `df` y adc el acarreo: no se estorban");
        CHECK(choca("stc", "adc rax, rdx"),
              "stc pone el acarreo y adc lo lee: si se estorban");
        CHECK(!choca("bt rax, 3", "setz cl"),
              "bt no toca `zf`, que es lo que setz lee: no se estorban");
        CHECK(choca("cmp rax, rbx", "setz cl"),
              "cmp deja `zf` y setz la lee: dependencia real");
        CHECK(!choca("mov rax, rbx", "add rcx, rdx"),
              "mover no toca banderas: nada que compartir");
        /* Y este SI choca aunque la de la izquierda no toque el acarreo: las dos
         * escriben `zf`, asi que quien la lea despues ve una u otra segun el
         * orden.  Es la parte que no se puede relajar. */
        CHECK(choca("inc rbx", "adc rax, rdx"),
              "inc y adc escriben las mismas banderas menos el acarreo: chocan");
    }

    if (g_fail == 0)
        std::printf("=== test_asm_analyze: %d checks OK, 0 fallidos ===\n",
                    g_checks);
    else
        std::printf("=== test_asm_analyze: %d checks, %d FALLIDOS ===\n",
                    g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
