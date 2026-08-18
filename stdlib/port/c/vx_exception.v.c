// @vx-snippet: vx_exception
// @vx-requires: vx_macros
// @vx-includes: stdint.h, setjmp.h, stdio.h
// @vx-freestanding-skip: yes

/* Excepciones de usuario: stack de @c jmp_buf via linked list TLS.
 * @c panic almacena el mensaje en @c vx_exc_value y hace @c longjmp
 * al frame top.  El handler de @c try/catch hace @c goto * a la
 * direccion del label del bloque @c try_handler_N (labels-as-values
 * GCC: @c &&bb_<id>).
 *
 * Para freestanding: el snippet se omite y el programador debe proveer
 * @c vx_panic_with_str + @c vx_exc_top + @c vx_exc_value en su
 * propio codigo si quiere try/catch. */
typedef struct vx_exc_frame {
    jmp_buf buf;
    int64_t type_tag; /* tipo del catch (sentinel para FatalError) */
    struct vx_exc_frame *prev;
} vx_exc_frame;

static VX_TLS vx_exc_frame *vx_exc_top = 0;
static VX_TLS int64_t vx_exc_value = 0;

static VX_UNUSED VX_NORETURN VX_COLD void vx_panic_with_str(const void *msg) {
    vx_exc_value = (int64_t)(intptr_t)msg;
    if (vx_exc_top != 0) {
        longjmp(vx_exc_top->buf, 1);
    }
    /* Uncaught panic: print msg + abort cleanly. */
    fputs("[vx] uncaught panic: ", stderr);
    if (msg) fputs((const char *)msg, stderr);
    fputc('\n', stderr);
    vx_throw(1);
}
