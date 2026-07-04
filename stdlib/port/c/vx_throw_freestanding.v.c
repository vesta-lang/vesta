// @vx-snippet: vx_throw_freestanding
// @vx-requires: vx_macros
// @vx-includes:
// @vx-freestanding-skip: no

/* Modo freestanding: @c vx_throw NO se implementa aqui.  El usuario
 * debe proveer una funcion @c VX_NORETURN @c "void vx_throw(int)"
 * en su propio codigo (e.g. que escriba a un puerto serie + halt).
 * El transpiler la referencia como @c extern. */
extern VX_NORETURN void vx_throw(int code);
extern int vx_exc_code; /* opcional; el usuario decide si tracker */
