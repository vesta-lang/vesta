// @vx-snippet: vx_throw_hosted
// @vx-requires: vx_macros
// @vx-includes: setjmp.h
// @vx-freestanding-skip: yes

/* Manejo de excepciones de runtime via setjmp/longjmp (modo hosted).
 * El usuario en modo freestanding debe proveer su propia @c vx_throw
 * declarada como @c VX_NORETURN. */
static jmp_buf vx_exc_buf;
static int vx_exc_code;
static VX_UNUSED VX_NORETURN VX_COLD void vx_throw(int code) {
    vx_exc_code = code;
    longjmp(vx_exc_buf, 1);
}
