/**
 * @file vesta_io_bare.c
 * @brief I/O nativa minima para Vex AOT (tiers bare/embed) -- SIN la VM.
 *
 * El path de I/O de la VM (vesta_io.c) toma @c (proc, vm_addr, len) porque sus
 * buffers viven en la memoria del proceso VM.  En compilacion nativa (-m aot)
 * NO hay ProcessVM ni vm_mem: los punteros son host directos.  Este modulo
 * provee los servicios que el lowering native_poo necesita, todos con ABI C
 * plana (sin @c proc), implementados sobre libc.
 *
 * El usuario puede ELEGIR como se hace el stdout y como se formatean los args:
 *   - stdout: redefinir @c __vx_write (igual que @AllocatorOverride redefine
 *     malloc) -> el linker toma su version.  En freestanding (kernel) DEBE
 *     proveerlo (a MMIO/UART); aqui solo damos el default libc.
 *   - formato runtime: redefinir cualquier @c __vx_print_* con su propia
 *     version.
 *
 * Los literales y las partes COMPTIME (ANSI, consts, format-specs sobre
 * valores comptime) ya las resuelve el compilador a bytes en .rodata y se
 * emiten via @c __vx_write -- aqui no se tocan.
 */
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Hook de fallo de `unwrap` sobre null (Optional/nonnull vacios).
 *
 * El selector AOT emite el chequeo INLINE (test+branch) y solo llama aqui en
 * el camino frio (valor null).  Default: mensaje + abort.  Redefinible (en
 * freestanding el usuario DEBE proveerlo, p.ej. halt del kernel).
 */
void __vx_panic_null(void) {
    fputs("panic: unwrap de un valor null (Optional/nonnull vacio)\n", stderr);
    abort();
}

/** @brief Sink de stdout: escribe @p len bytes de @p buf. */
void __vx_write(const char *buf, unsigned long long len) {
    if (buf && len) fwrite(buf, 1, (unsigned long)len, stdout);
}

/** @brief Entero con signo en decimal. */
void __vx_print_i64(long long v) { printf("%lld", v); }

/** @brief Entero sin signo en decimal. */
void __vx_print_u64(unsigned long long v) { printf("%llu", v); }

/** @brief Hexadecimal con prefijo 0x (16 nibbles compactos). */
void __vx_print_hex(unsigned long long v) { printf("0x%llx", v); }

/** @brief Octal con prefijo 0o. */
void __vx_print_oct(unsigned long long v) { printf("0o%llo", v); }

/** @brief Puntero: hex compacto con prefijo 0x. */
void __vx_print_ptr(unsigned long long v) { printf("0x%llx", v); }

/** @brief Booleano como "true"/"false". */
void __vx_print_bool(unsigned long long v) {
    fputs(v ? "true" : "false", stdout);
}

/** @brief Binario con prefijo 0b (bits compactos, sin ceros a la izquierda). */
void __vx_print_bin(unsigned long long v) {
    char buf[66];
    int i = 0;
    buf[i++] = '0';
    buf[i++] = 'b';
    if (v == 0) {
        buf[i++] = '0';
    } else {
        char tmp[64];
        int n = 0;
        while (v) {
            tmp[n++] = (char)('0' + (int)(v & 1ULL));
            v >>= 1;
        }
        while (n) buf[i++] = tmp[--n];
    }
    fwrite(buf, 1, (unsigned long)i, stdout);
}

/** @brief Codepoint Unicode -> UTF-8 (1..4 bytes). */
void __vx_print_char(unsigned long long cp) {
    char b[4];
    int n = 0;
    if (cp < 0x80ULL) {
        b[n++] = (char)cp;
    } else if (cp < 0x800ULL) {
        b[n++] = (char)(0xC0 | (cp >> 6));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000ULL) {
        b[n++] = (char)(0xE0 | (cp >> 12));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        b[n++] = (char)(0xF0 | (cp >> 18));
        b[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    }
    fwrite(b, 1, (unsigned long)n, stdout);
}

/** @brief Cadena C nul-terminada (host_ptr). */
void __vx_print_cstr(const char *s) {
    if (s) fputs(s, stdout);
}

/** @brief Vacia el buffer de stdout (flush). */
void __vx_flush(void) { fflush(stdout); }
