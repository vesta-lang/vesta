// @vx-snippet: vx_exception_freestanding
// @vx-requires: vx_macros
// @vx-includes: stdint.h, setjmp.h
// @vx-freestanding-skip: no

/* Variante freestanding del runtime de excepciones.  Misma estructura
 * de @c vx_exc_frame pero @c vx_panic_with_str NO escribe a stderr
 * (no hay stdio).  El usuario en su codigo decide que hacer en
 * uncaught panic via su propia @c vx_throw (e.g. puerto serie + hlt). */
typedef struct vx_exc_frame {
    jmp_buf buf;
    int64_t type_tag;
    struct vx_exc_frame *prev;
} vx_exc_frame;

static VX_TLS vx_exc_frame *vx_exc_top = 0;
static VX_TLS int64_t vx_exc_value = 0;

static VX_UNUSED VX_NORETURN VX_COLD void
vx_panic_with_str(const void *msg) {
    vx_exc_value = (int64_t)(intptr_t)msg;
    if (vx_exc_top != 0) {
        longjmp(vx_exc_top->buf, 1);
    }
    /* Uncaught en freestanding: delegar al usuario via vx_throw. */
    vx_throw(1);
}
