// @vx-snippet: vx_string
// @vx-requires: vx_macros
// @vx-includes: stdint.h, stdlib.h, string.h, stdio.h
// @vx-freestanding-skip: yes

/* =========================================================================
 * Mini runtime VxString (mode=managed).
 *
 * Layout: header 16 bytes + buffer flexible.  Operaciones de strings
 * (concat/equals/length/raw/bytes) usan estos helpers.  El usuario debe
 * llamar @c vx_str_free(s) cuando termine con un VxString* heap-alloc'd
 * (los marcados @c is_owned=1).  Al exit del programa, los unfreed se
 * reportan a @c stderr.
 *
 * Para programacion freestanding/bootloader: este snippet se OMITE.  Si
 * el programa usa strings y se compila con @c --port-freestanding, error.
 * El programador debe implementar sus propios helpers @c vx_str_make /
 * @c vx_str_concat / etc. con su asignador.
 * =========================================================================
 */
typedef struct VxString {
    struct VxString *__prev;
    struct VxString *__next;
    uint32_t byte_len;
    uint32_t code_points;
    uint8_t is_owned; /* 1=heap (free at vx_str_free), 0=literal */
    uint8_t __pad[3];
    char *data;
} VxString;

/* Self-ref + TLS no compatible en todos los toolchains; lazy-init. */
static VX_TLS VxString vx_str_head_;
static VX_TLS int vx_str_head_init_ = 0;
static VX_TLS int vx_str_live_count_ = 0;
static int vx_str_atexit_registered_ = 0;

static void vx_str_head_ensure_(void) {
    if (!vx_str_head_init_) {
        vx_str_head_.__prev = &vx_str_head_;
        vx_str_head_.__next = &vx_str_head_;
        vx_str_head_init_ = 1;
    }
}

static void vx_str_track_(VxString *s) {
    vx_str_head_ensure_();
    s->__next = vx_str_head_.__next;
    s->__prev = &vx_str_head_;
    vx_str_head_.__next->__prev = s;
    vx_str_head_.__next = s;
    vx_str_live_count_++;
}

static void vx_str_untrack_(VxString *s) {
    s->__prev->__next = s->__next;
    s->__next->__prev = s->__prev;
    s->__prev = s->__next = 0;
    vx_str_live_count_--;
}

static void vx_str_atexit_warn_(void) {
    if (vx_str_live_count_ > 0) {
        fprintf(stderr,
                "[vx] warning: %d VxString blocks leaked at thread exit\n",
                vx_str_live_count_);
    }
}

static void vx_str_register_atexit_(void) {
    if (!vx_str_atexit_registered_) {
        vx_str_atexit_registered_ = 1;
        atexit(vx_str_atexit_warn_);
    }
}

/* Cuenta code points UTF-8 en un buffer. */
static uint32_t vx_str_count_cp_(const char *buf, uint32_t byte_len) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < byte_len;) {
        unsigned char c = (unsigned char)buf[i];
        uint32_t adv = 1;
        if ((c & 0x80) == 0x00)
            adv = 1;
        else if ((c & 0xE0) == 0xC0)
            adv = 2;
        else if ((c & 0xF0) == 0xE0)
            adv = 3;
        else if ((c & 0xF8) == 0xF0)
            adv = 4;
        i += adv;
        n++;
    }
    return n;
}

static VX_UNUSED VxString *vx_str_from_lit(const char *lit,
                                              uint32_t byte_len) {
    VxString *s = (VxString *)malloc(sizeof(VxString));
    if (!s) return 0;
    s->byte_len = byte_len;
    s->code_points = vx_str_count_cp_(lit, byte_len);
    s->is_owned = 0;
    s->data = (char *)lit;
    vx_str_track_(s);
    vx_str_register_atexit_();
    return s;
}

static VX_UNUSED VxString *vx_str_make(const char *buf, uint32_t byte_len) {
    VxString *s = (VxString *)malloc(sizeof(VxString));
    if (!s) return 0;
    s->byte_len = byte_len;
    s->code_points = vx_str_count_cp_(buf, byte_len);
    s->is_owned = 1;
    s->data = (char *)malloc(byte_len + 1);
    if (!s->data) {
        free(s);
        return 0;
    }
    memcpy(s->data, buf, byte_len);
    s->data[byte_len] = 0;
    vx_str_track_(s);
    vx_str_register_atexit_();
    return s;
}

static VX_UNUSED VxString *vx_str_concat(VxString *a, VxString *b) {
    if (!a || !b) return 0;
    uint32_t na = a->byte_len, nb = b->byte_len;
    char *buf = (char *)malloc(na + nb + 1);
    if (!buf) return 0;
    memcpy(buf, a->data, na);
    memcpy(buf + na, b->data, nb);
    buf[na + nb] = 0;
    VxString *r = (VxString *)malloc(sizeof(VxString));
    if (!r) {
        free(buf);
        return 0;
    }
    r->byte_len = na + nb;
    r->code_points = a->code_points + b->code_points;
    r->is_owned = 1;
    r->data = buf;
    vx_str_track_(r);
    return r;
}

static VX_UNUSED int vx_str_eq(VxString *a, VxString *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->byte_len != b->byte_len) return 0;
    return memcmp(a->data, b->data, a->byte_len) == 0;
}
static VX_UNUSED uint32_t vx_str_len(VxString *s) {
    return s ? s->code_points : 0;
}
static VX_UNUSED uint32_t vx_str_byte_len(VxString *s) {
    return s ? s->byte_len : 0;
}
static VX_UNUSED const char *vx_str_raw(VxString *s) {
    return s ? s->data : (const char *)"";
}

static VX_UNUSED void vx_str_free(VxString *s) {
    if (!s) return;
    vx_str_untrack_(s);
    if (s->is_owned) free(s->data);
    free(s);
}
