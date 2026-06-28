/**
 * @file util/gc_diag.h
 * @brief Macros de diagnostico (iostream) neutralizables en FREESTANDING.
 *
 * El codigo de infraestructura del GC (arena, TLB, VirtualMemory) emite
 * mensajes de error/debug por @c std::cerr / @c std::cout.  En el build
 * FREESTANDING de @c libvesta_gc no debe haber dependencias de @c <iostream>
 * (que arrastra @c std::ostream, locale, @c basic_string, etc. de libstdc++,
 * impidiendo enlazar con NUESTRO linker AOT sin g++).
 *
 * Con @c VESTA_GC_FREESTANDING las macros @c VGC_CERR/COUT se vuelven un sink
 * nulo (se compilan a nada util, el compilador las elimina) y los
 * manipuladores (@c VGC_ENDL/HEX/DEC/SETW/SETFILL) son enteros inocuos que el
 * @c operator<< plantilla del sink ignora.  Sin la macro, son exactamente
 * @c std::* -> comportamiento identico en la VM/JIT.
 */
#ifndef VESTA_UTIL_GC_DIAG_H
#define VESTA_UTIL_GC_DIAG_H

#if defined(VESTA_GC_FREESTANDING)

namespace util {
/// Sink que descarta cualquier @c operator<< sin tocar iostream.
struct GcNullStream {
    template <typename T> GcNullStream &operator<<(const T &) { return *this; }
};
inline GcNullStream gc_null_stream;
} // namespace util

#define VGC_CERR ::util::gc_null_stream
#define VGC_COUT ::util::gc_null_stream
#define VGC_ENDL 0
#define VGC_HEX 0
#define VGC_DEC 0
#define VGC_SETW(n) 0
#define VGC_SETFILL(c) 0

#else // build normal (VM/JIT): iostream real

#include <iomanip>
#include <iostream>

#define VGC_CERR std::cerr
#define VGC_COUT std::cout
#define VGC_ENDL std::endl
#define VGC_HEX std::hex
#define VGC_DEC std::dec
#define VGC_SETW(n) std::setw(n)
#define VGC_SETFILL(c) std::setfill(c)

#endif

#endif // VESTA_UTIL_GC_DIAG_H
