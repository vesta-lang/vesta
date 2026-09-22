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
 * @file byte_buffer.h
 * @brief La parte COMUN de todo buffer de bytes del proyecto.
 *
 * ## Que problema cierra
 *
 * Un `.velb`, la seccion `@ir`, un objeto COFF, un paquete de red y una
 * entrada de cache eran, los cinco, `std::vector<uint8_t>`.  Byte a byte el
 * mismo tipo, y de ahi salian dos fallos que no se parecen pero tienen la
 * misma causa -- que el tipo no dice lo que es --:
 *
 * 1. **No se pueden ATRIBUIR sus reservas.**  El paseo de la pila se queda
 *    dentro de libstdc++ y el informe solo sabe decir `std::vector<unsigned
 *    char>`.  Medido: de 116 millones de reservas al compilar 441.089 lineas,
 *    el 60% no se podia atribuir a codigo nuestro.
 * 2. **Nada impide pasar uno donde iba otro.**  Un `std::move` de los bytes de
 *    una cadena a la funcion de otra compila sin una sola queja.
 *
 * ## Y por que hay UNA parte comun
 *
 * Porque no tenerla ya divergio: hoy conviven CUATRO implementaciones de lo
 * mismo -- `util::serialize.h`, los helpers inline de `ssa_ir_serialize.h`,
 * `emmit/bytewriter.h` + `bytereader.h`, y los lectores sueltos del loader --.
 * Ninguna se escribio para divergir; simplemente escribir la quinta siempre
 * fue el camino corto.
 *
 * ## Como esta hecho, y por que asi
 *
 * Struct plano y funciones libres.  **Ni plantillas ni clases**: una plantilla
 * instancia el tronco entero por cada cadena, y una jerarquia de clases mete
 * una vtable y una indirecta en algo que es, literalmente, un puntero y dos
 * tamanyos.
 *
 * Un tipo concreto EXTIENDE poniendo @ref ByteBuffer como PRIMER miembro, que
 * es lo que permite pasar su direccion donde se pide la parte comun:
 *
 * @code
 * typedef struct VelbImage {
 *     ByteBuffer buf;   // primero, siempre
 *     // ...y lo suyo: cabecera, tabla de secciones, ...
 * } VelbImage;
 * @endcode
 *
 * ## Las dos reglas que impiden que vuelva a divergir
 *
 * No son documentacion, son el diseno:
 *
 * - **No existe un buffer anonimo.**  La unica forma de tener uno es con su
 *   @ref ByteKind (ver @ref byte_buffer_init): no hay inicializacion sin
 *   descriptor.  Un buffer que no dice lo que es no llega a compilar, en vez
 *   de compilar y perderse en el informe.
 * - **Las fronteras solo hablan @ref ByteBuffer.**  Leer un fichero, escribirlo
 *   de forma atomica, cargar un artefacto, enlazar, serializar.  Se puede
 *   seguir escribiendo un `std::vector<uint8_t>` propio, pero no se le puede
 *   PASAR a nadie: el quinto serializador deja de ser el camino corto y pasa a
 *   ser el largo.  Hoy es al reves.
 */

#ifndef VESTA_UTIL_BYTE_BUFFER_H
#define VESTA_UTIL_BYTE_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace util {

/**
 * @struct ByteKind
 * @brief QUE es un buffer: su nombre, su formato y como crece.
 *
 * Hay UNO por cadena en todo el programa -- estatico y constante --, no una
 * copia por buffer: lo que cada buffer guarda es un puntero a el.
 *
 * Lleva la funcion de crecimiento a proposito, y es lo que hace que el informe
 * de reservas pueda atribuirlas.  Sin plantillas ni virtuales, la unica forma
 * de que una reserva salga con nombre propio en el paseo de la pila es que la
 * funcion llamada sea SUYA: `velb_image_grow` y no `std::vector::_M_realloc`.
 * La indirecta se paga una vez por CRECIMIENTO -- amortizado, y son buffers de
 * megabytes --, nunca por byte.
 */
struct ByteKind {
    /// Como se llama para quien lea un volcado o un informe: "velb",
    /// "ir-section", "coff-object".
    const char *name;
    /// Los primeros bytes que lo identifican, o 0 si el formato no tiene.
    /// Tenerlo aqui es lo que permite que el buffer compruebe su propia
    /// cabecera en vez de que cada consumidor se acuerde de hacerlo.
    uint32_t magic;
    /// Version del formato, o 0 si no tiene.
    uint16_t version;
    /**
     * @brief A cuantos bytes se alinea lo que se reserve para el.
     *
     * Va DECLARADO aqui, y no escondido dentro de @ref alloc, por lo mismo que
     * el resto del descriptor: quien mire un buffer tiene que poder saber
     * como esta hecho sin ir a leer una funcion.  Y porque la alineacion no es
     * un detalle -- es lo que decide si una copia puede ir vectorizada
     * alineada o no, y este proyecto ya pago una vez por no poder DEMOSTRAR la
     * de un bloque que el mismo reservaba (ver `gdata_host`).
     *
     * Quien lo aplica es la @ref alloc de cada cadena; esto es el contrato que
     * ella cumple.
     */
    uint16_t align;
    /**
     * @brief Reserva @c n bytes para un buffer de esta clase.
     *
     * La pone cada cadena, y ES el mecanismo de atribucion: su nombre sale en
     * la cadena de llamadas del informe de reservas.
     */
    void *(*alloc)(size_t n);
    /// Suelta lo que dio @ref alloc.  Por la MISMA puerta: mezclarlas no falla
    /// al soltar, falla despues y en otro sitio.
    void (*free)(void *p) noexcept;
};

/**
 * @enum ByteFault
 * @brief QUE salio mal, cuando algo sale mal.
 *
 * No es un booleano, y la diferencia no es cosmetica: "no se pudo" no
 * distingue quedarse sin memoria -- que es del entorno -- de que el artefacto
 * mienta sobre su propio tamanyo -- que es del fichero --.  El primero se
 * reintenta y el segundo se rechaza, asi que un consumidor que solo vea un
 * `false` no puede hacer ninguna de las dos cosas bien.
 *
 * Es la misma leccion que costo aqui el viejo `is_host_ptr`: un bit no puede
 * decir "no lo se", asi que lo que nadie miro y lo que se demostro que no
 * salian iguales.
 *
 * Uno solo para lectura y escritura porque el motivo es del BUFFER, no de la
 * direccion en que se le esta usando.
 */
enum class ByteFault : uint8_t {
    None = 0,    ///< Todo bien.
    OutOfMemory, ///< La reserva no dio; es del entorno, no del dato.
    PastEnd,     ///< Se quiso leer mas alla del final.
    BadLength,   ///< Una longitud declaraba mas bytes de los que hay.
    BadMagic     ///< La cabecera no es la de la clase que dice ser.
};

/**
 * @brief El motivo, en texto, para poder contarlo.
 *
 * Aqui y no en el catalogo multi-idioma a proposito: esto NO es un
 * diagnostico, es el dato con el que quien si emite un diagnostico compone el
 * suyo -- que si sale del catalogo --.  Sirve tambien para un volcado o una
 * traza, que no son texto para el usuario.
 *
 * @param f El motivo.
 * @return Un nombre estable, nunca nulo.
 */
inline const char *byte_fault_name(ByteFault f) noexcept {
    switch (f) {
    case ByteFault::None: return "none";
    case ByteFault::OutOfMemory: return "out-of-memory";
    case ByteFault::PastEnd: return "past-end";
    case ByteFault::BadLength: return "bad-length";
    case ByteFault::BadMagic: return "bad-magic";
    }
    return "unknown";
}

/**
 * @struct ByteBuffer
 * @brief El almacenamiento y nada mas: lo que toda cadena comparte.
 *
 * Deliberadamente sin nada propio de ningun formato.  Lo que sabe de
 * cabeceras, secciones o versiones vive en el tipo que lo extiende.
 */
struct ByteBuffer {
    uint8_t *data;        ///< Los bytes, o nulo si todavia no hay.
    size_t size;          ///< Cuantos valen.
    size_t cap;           ///< Cuantos caben sin volver a reservar.
    const ByteKind *kind; ///< Que ES esto.  Nunca nulo tras inicializar.
    /**
     * @brief Que fue mal la PRIMERA vez que algo fue mal.
     *
     * Se queda con el primero y no con el ultimo: lo que viene detras de un
     * fallo son sus consecuencias, y el motivo que interesa es el de la causa.
     * Mientras no sea @c None, toda escritura posterior no hace nada.
     *
     * Existe para que una secuencia de escrituras se compruebe UNA vez al
     * final en lugar de mirar el resultado de cada una -- que es lo que nadie
     * hace --.  Las dos alternativas son peores: un booleano por llamada que
     * 118 sitios van a ignorar, o lanzar, que es lo que hace hoy `push_back` y
     * lo que convierte quedarse sin memoria serializando en una excepcion a
     * traves de medio compilador.
     */
    ByteFault fault;
};

/**
 * @brief Deja @p b listo y le dice QUE es.
 *
 * @p kind no tiene valor por defecto y no puede ser nulo: es la regla que
 * impide que exista un buffer anonimo.  Todo lo demas de este fichero da por
 * hecho que se ha pasado por aqui.
 *
 * @param b Buffer a inicializar.
 * @param kind Descriptor de su cadena.
 */
inline void byte_buffer_init(ByteBuffer &b, const ByteKind *kind) noexcept {
    b.data = nullptr;
    b.size = 0;
    b.cap = 0;
    b.kind = kind;
    b.fault = ByteFault::None;
}

/**
 * @brief Suelta lo que tenga y lo deja vacio, pero SIGUE siendo de su clase.
 *
 * No se olvida el @ref ByteKind a proposito: un buffer soltado se puede volver
 * a llenar, y perder por el camino lo que era lo convertiria en el buffer
 * anonimo que este fichero existe para que no haya.
 *
 * @param b Buffer a vaciar.
 */
inline void byte_buffer_release(ByteBuffer &b) noexcept {
    if (b.data != nullptr) b.kind->free(b.data);
    b.data = nullptr;
    b.size = 0;
    b.cap = 0;
    b.fault = ByteFault::None;
}

/// Que hacer si alguien intenta reservar sobre un buffer PRESTADO.
/// @param n Ignorado.  @return Siempre nulo: prestado no es suyo.
inline void *byte_borrowed_no_alloc(size_t) { return nullptr; }
/// Y que hacer al soltarlo: nada, porque no es suyo.
/// @param p Ignorado.
inline void byte_borrowed_no_free(void *) noexcept {}

/// La clase de un buffer que solo MIRA memoria de otro.
inline const ByteKind kBorrowedBytes = {
    "borrowed", 0, 0, 0, byte_borrowed_no_alloc, byte_borrowed_no_free};

/**
 * @brief Un buffer que MIRA @p n bytes de @p data sin ser su dueno.
 *
 * Existe para las fronteras: un tramo dentro de un artefacto mas grande, un
 * paquete que llega de la red, una region mapeada.  Ahi los bytes son de
 * otro, y copiarlos solo para poder recorrerlos seria copiar megabytes por
 * nada.
 *
 * Es de SOLO LECTURA por construccion, y no por convenio: su clase no sabe
 * reservar, asi que cualquier escritura falla con
 * @c ByteFault::OutOfMemory en vez de tocar memoria ajena.
 *
 * El buffer vale mientras valgan los bytes prestados, que es responsabilidad
 * de quien presta.
 *
 * @param data Primer byte.
 * @param n Cuantos.
 * @return El buffer prestado.
 */
inline ByteBuffer byte_buffer_borrow(const void *data, size_t n) noexcept {
    ByteBuffer b;
    b.data = const_cast<uint8_t *>(static_cast<const uint8_t *>(data));
    b.size = n;
    b.cap = 0; // cero: no hay sitio propio, asi que no se puede escribir
    b.kind = &kBorrowedBytes;
    b.fault = ByteFault::None;
    return b;
}

/**
 * @brief Asegura sitio para @p n bytes en total.
 *
 * Crece al DOBLE, no a lo justo: llenar un buffer byte a byte reservando lo
 * exacto es cuadratico, y estos se llenan asi -- una instruccion, un campo,
 * una cadena --.
 *
 * @param b Buffer.
 * @param n Capacidad minima que debe quedar.
 * @return false si la reserva fallo; @p b queda intacto.
 */
inline bool byte_buffer_reserve(ByteBuffer &b, size_t n) {
    if (b.fault != ByteFault::None) return false;
    if (n <= b.cap) return true;
    size_t grown = (b.cap < 64) ? 64 : b.cap;
    while (grown < n) grown *= 2;
    void *p = b.kind->alloc(grown);
    if (p == nullptr) {
        b.fault = ByteFault::OutOfMemory;
        return false;
    }
    if (b.size != 0) std::memcpy(p, b.data, b.size);
    if (b.data != nullptr) b.kind->free(b.data);
    b.data = static_cast<uint8_t *>(p);
    b.cap = grown;
    return true;
}

/**
 * @brief Anyade @p n bytes copiados de @p src.
 *
 * @param b Buffer.
 * @param src Origen; puede ser nulo si @p n es cero.
 * @param n Cuantos bytes.
 * @return false si no se pudo reservar.
 */
inline bool byte_buffer_append(ByteBuffer &b, const void *src, size_t n) {
    if (n == 0) return true;
    if (!byte_buffer_reserve(b, b.size + n)) return false;
    std::memcpy(b.data + b.size, src, n);
    b.size += n;
    return true;
}

/** @brief Anyade un byte. @param b Buffer. @param v Valor. @return false si no
 * se pudo reservar. */
inline bool byte_buffer_append_u8(ByteBuffer &b, uint8_t v) {
    return byte_buffer_append(b, &v, 1);
}

/**
 * @brief Anyade un entero de 16 bits en LITTLE-ENDIAN.
 *
 * El orden se escribe a mano y no se copia el entero tal cual: los formatos de
 * este proyecto son little-endian SIEMPRE, y volcar la representacion del
 * anfitrion daria un artefacto que solo vale en las maquinas que se parecen a
 * la que lo produjo -- y eso no falla al escribirlo, falla al leerlo en otra --.
 *
 * @param b Buffer.  @param v Valor.  @return false si no se pudo reservar.
 */
inline bool byte_buffer_append_u16(ByteBuffer &b, uint16_t v) {
    const uint8_t t[2] = {static_cast<uint8_t>(v & 0xFF),
                          static_cast<uint8_t>((v >> 8) & 0xFF)};
    return byte_buffer_append(b, t, sizeof(t));
}

/** @brief Anyade un entero de 32 bits en little-endian.  @param b Buffer.
 * @param v Valor.  @return false si no se pudo reservar. */
inline bool byte_buffer_append_u32(ByteBuffer &b, uint32_t v) {
    const uint8_t t[4] = {
        static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF),
        static_cast<uint8_t>((v >> 24) & 0xFF)};
    return byte_buffer_append(b, t, sizeof(t));
}

/** @brief Anyade un entero de 64 bits en little-endian.  @param b Buffer.
 * @param v Valor.  @return false si no se pudo reservar. */
inline bool byte_buffer_append_u64(ByteBuffer &b, uint64_t v) {
    uint8_t t[8];
    for (int i = 0; i < 8; ++i)
        t[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    return byte_buffer_append(b, t, sizeof(t));
}

/**
 * @brief Anyade una cadena: su longitud en 32 bits y detras sus bytes.
 *
 * Con la longitud DELANTE y no terminada en cero, que es lo que permite que
 * lleve bytes nulos dentro -- y los lleva: por aqui pasan literales de Vesta,
 * que son UTF-8 arbitrario.
 *
 * @param b Buffer.
 * @param s Texto; se copia entero, sea lo que sea lo que tenga.
 * @param n Cuantos bytes tiene @p s.
 * @return false si no se pudo reservar.
 */
inline bool byte_buffer_append_bytes_with_len(ByteBuffer &b, const char *s,
                                              size_t n) {
    if (!byte_buffer_append_u32(b, static_cast<uint32_t>(n))) return false;
    return byte_buffer_append(b, s, n);
}

/**
 * @struct ByteCursor
 * @brief Por donde va la LECTURA de un buffer.
 *
 * Aparte del buffer y no dentro, porque leer no lo modifica: dos sitios pueden
 * recorrer el mismo buffer a la vez, y el propio buffer puede ser constante.
 *
 * Lleva su propio motivo en vez de que cada lectura devuelva un booleano que
 * alguien tenga que mirar.  En cuanto algo va mal se queda ahi y todo lo que
 * venga detras falla tambien, asi que se puede comprobar UNA vez al final de
 * una secuencia larga en lugar de linea por linea -- que es justamente lo que
 * nadie hace --.
 */
struct ByteCursor {
    const ByteBuffer *buf; ///< De donde se lee.
    size_t off;            ///< Por donde va.
    /// Que fue mal la PRIMERA vez.  Salirse del final y que una longitud
    /// mienta no son lo mismo: lo primero puede ser que quien lee se haya
    /// pasado, lo segundo es que el artefacto esta mal.
    ByteFault fault;
};

/**
 * @brief Un cursor al principio de @p b.
 * @param b Buffer a recorrer.
 * @return El cursor.
 */
inline ByteCursor byte_cursor_at_start(const ByteBuffer &b) noexcept {
    ByteCursor c;
    c.buf = &b;
    c.off = 0;
    c.fault = ByteFault::None;
    return c;
}

/**
 * @brief Cuantos bytes quedan por leer.
 * @param c Cursor.
 * @return Los que quedan, o cero si ya fallo.
 */
inline size_t byte_cursor_left(const ByteCursor &c) noexcept {
    if (c.fault != ByteFault::None || c.off > c.buf->size) return 0;
    return c.buf->size - c.off;
}

/**
 * @brief Copia @p n bytes y avanza.
 * @param c Cursor.  @param dst Destino.  @param n Cuantos.
 * @return false si no habia tantos; el cursor queda invalidado.
 */
inline bool byte_cursor_read(ByteCursor &c, void *dst, size_t n) {
    if (c.fault != ByteFault::None) return false;
    if (byte_cursor_left(c) < n) {
        c.fault = ByteFault::PastEnd;
        return false;
    }
    std::memcpy(dst, c.buf->data + c.off, n);
    c.off += n;
    return true;
}

/** @brief Lee un byte.  @param c Cursor.  @param out Destino.  @return false
 * si no quedaba. */
inline bool byte_cursor_read_u8(ByteCursor &c, uint8_t &out) {
    return byte_cursor_read(c, &out, 1);
}

/** @brief Lee 16 bits little-endian.  @param c Cursor.  @param out Destino.
 * @return false si no quedaban. */
inline bool byte_cursor_read_u16(ByteCursor &c, uint16_t &out) {
    uint8_t t[2];
    if (!byte_cursor_read(c, t, sizeof(t))) return false;
    out = static_cast<uint16_t>(t[0] | (static_cast<uint16_t>(t[1]) << 8));
    return true;
}

/** @brief Lee 32 bits little-endian.  @param c Cursor.  @param out Destino.
 * @return false si no quedaban. */
inline bool byte_cursor_read_u32(ByteCursor &c, uint32_t &out) {
    uint8_t t[4];
    if (!byte_cursor_read(c, t, sizeof(t))) return false;
    out = static_cast<uint32_t>(t[0]) |
          (static_cast<uint32_t>(t[1]) << 8) |
          (static_cast<uint32_t>(t[2]) << 16) |
          (static_cast<uint32_t>(t[3]) << 24);
    return true;
}

/** @brief Lee 64 bits little-endian.  @param c Cursor.  @param out Destino.
 * @return false si no quedaban. */
inline bool byte_cursor_read_u64(ByteCursor &c, uint64_t &out) {
    uint8_t t[8];
    if (!byte_cursor_read(c, t, sizeof(t))) return false;
    out = 0;
    for (int i = 7; i >= 0; --i)
        out = (out << 8) | static_cast<uint64_t>(t[i]);
    return true;
}

/**
 * @brief Lee una cadena escrita por @ref byte_buffer_append_bytes_with_len.
 *
 * Devuelve DONDE estan los bytes dentro del buffer, no una copia: quien lea
 * miles de nombres -- el deserializador del intermedio lo hace -- no tiene por
 * que reservar uno por cada uno.  El puntero vale mientras el buffer no
 * crezca ni se suelte.
 *
 * @param c Cursor.
 * @param out_ptr Recibe el comienzo, o nulo si la cadena es vacia.
 * @param out_len Recibe cuantos bytes tiene.
 * @return false si no cabia; el cursor queda invalidado.
 */
inline bool byte_cursor_read_bytes_with_len(ByteCursor &c,
                                            const char **out_ptr,
                                            size_t *out_len) {
    uint32_t len = 0;
    if (!byte_cursor_read_u32(c, len)) return false;
    /* Y aqui NO es PastEnd: la longitud viene DEL PROPIO buffer, asi que si
     * declara mas de lo que hay, el que miente es el artefacto.  Distinguirlo
     * es lo que permite decidir entre rechazarlo y sospechar de quien lee. */
    if (byte_cursor_left(c) < len) {
        c.fault = ByteFault::BadLength;
        return false;
    }
    *out_ptr = (len == 0)
                   ? nullptr
                   : reinterpret_cast<const char *>(c.buf->data + c.off);
    *out_len = len;
    c.off += len;
    return true;
}

/**
 * @brief Comprueba que el buffer empieza por el magic de su propia clase.
 *
 * Esto es lo que el descriptor hace posible: la comprobacion sale del buffer,
 * no de que cada consumidor se acuerde de hacerla con la constante correcta.
 *
 * Y tampoco contesta con un booleano: no cuadrar porque el buffer se acaba
 * antes de los cuatro bytes de la cabecera y no cuadrar porque los bytes son
 * otros son dos cosas distintas -- lo primero es un fichero truncado, lo
 * segundo es que no es de esta clase --.
 *
 * @param b Buffer.
 * @return @c ByteFault::None si su clase no declara magic (nada que
 *         comprobar) o si cuadra.
 */
inline ByteFault byte_buffer_check_magic(const ByteBuffer &b) noexcept {
    if (b.kind->magic == 0) return ByteFault::None;
    if (b.size < 4) return ByteFault::PastEnd;
    const uint32_t m = static_cast<uint32_t>(b.data[0]) |
                       (static_cast<uint32_t>(b.data[1]) << 8) |
                       (static_cast<uint32_t>(b.data[2]) << 16) |
                       (static_cast<uint32_t>(b.data[3]) << 24);
    return (m == b.kind->magic) ? ByteFault::None : ByteFault::BadMagic;
}

/**
 * @struct OwnedBytes
 * @brief Un @ref ByteBuffer que se suelta solo, para vivir DENTRO de algo.
 *
 * ## Por que existe, y por que no esta en @ref ByteBuffer
 *
 * @ref ByteBuffer es un struct plano a proposito: es lo que cruza las
 * fronteras -- se pasa por referencia, se presta, se recorre -- y ahi no debe
 * llevar nada encima.  Su dueno es el ambito de quien lo declara, que lo
 * suelta al acabar.
 *
 * Eso funciona mientras el buffer sea una variable.  En cuanto tiene que ser
 * un CAMPO, el ambito ya no puede soltarlo, y entonces hacen falta dos cosas
 * que un struct plano no tiene: que se suelte cuando muera lo que lo contiene,
 * y que al MOVER ese contenedor el origen se quede sin el.
 *
 * Esto es eso y nada mas: la propiedad, encima del buffer, por composicion.
 * Sin vtable, sin plantilla, sin coste en ejecucion.
 *
 * ## Lo que impide
 *
 * Copiarlo.  Un buffer no se copia sin querer: duplicar megabytes de bytes
 * nunca es lo que alguien queria, y dos duenos del mismo bloque acaban en una
 * doble liberacion muy lejos de donde se cometio.  Se mueve, y el origen se
 * queda vacio -- que es lo mismo que hace el `mvtake` del bytecode con un
 * `unique<T>` del lenguaje --.
 */
struct OwnedBytes {
    /**
     * @brief Estrena uno de la clase @p kind, vacio.
     * @param kind Su descriptor; no puede ser nulo, igual que en
     *        @ref byte_buffer_init.
     */
    explicit OwnedBytes(const ByteKind *kind) noexcept {
        byte_buffer_init(buf, kind);
    }
    /// Suelta lo que tenga.
    ~OwnedBytes() { byte_buffer_release(buf); }

    OwnedBytes(const OwnedBytes &) = delete;
    OwnedBytes &operator=(const OwnedBytes &) = delete;

    /**
     * @brief Toma lo de @p o, que se queda vacio PERO de su misma clase.
     * @param o De donde se toma.
     */
    OwnedBytes(OwnedBytes &&o) noexcept : buf(o.buf) {
        byte_buffer_init(o.buf, o.buf.kind);
    }
    /**
     * @brief Suelta lo suyo y toma lo de @p o.
     * @param o De donde se toma.
     * @return Este mismo.
     */
    OwnedBytes &operator=(OwnedBytes &&o) noexcept {
        if (this != &o) {
            byte_buffer_release(buf);
            buf = o.buf;
            byte_buffer_init(o.buf, o.buf.kind);
        }
        return *this;
    }

    /// El buffer, para pasarlo a cualquier frontera.  Publico porque este
    /// tipo NO es una abstraccion: es la propiedad y nada mas.
    ByteBuffer buf;
};

} // namespace util

#endif // VESTA_UTIL_BYTE_BUFFER_H
