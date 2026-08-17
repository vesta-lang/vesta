/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file emmit/mnemonic.h
 * @brief El mnemonico de una instruccion `.vel` como TIPO, no como cadena.
 *
 * Sale de @c emmit/instr_list.h, que es la lista unica.  Nadie escribe este enum
 * a mano: seria una tercera copia de la misma lista y se separaria de las otras
 * dos igual que ellas ya se separaron entre si.
 *
 * Para que sirve: que el emisor del IR diga QUE instruccion emite en vez de
 * escribir su nombre en una cadena.  Un mnemonico que no existe deja de
 * compilar, en lugar de descubrirse al ensamblar -- el final de la cadena, lejos
 * de donde se escribio.
 *
 * ## Nada se busca
 *
 * El mnemonico ES un indice, asi que su nombre y su categoria salen de tablas
 * planas, no de hashear una cadena.  Y como la lista va agrupada, cada categoria
 * es un rango contiguo: preguntar si algo es un salto son dos comparaciones.
 * Todo @c constexpr, asi que cuando el mnemonico se conoce al compilar la
 * pregunta no llega a ejecutarse.
 */
#ifndef EMMIT_MNEMONIC_H
#define EMMIT_MNEMONIC_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace emmit {

/**
 * @enum Category
 * @brief Que clase de trabajo hace una instruccion.
 *
 * Sale de las secciones en las que YA estaba dividida la tabla del emisor: no se
 * invento aqui una taxonomia nueva.
 */
enum class Category : uint8_t {
    Arithmetic,  ///< suma, resta, producto, division, modulo, ALU de 3 operandos
    Bitwise,     ///< logica bit a bit y desplazamientos
    Branch,      ///< saltos, tablas de salto, cambio de tipo
    Call,        ///< llamadas, closures, tailcall
    Compare,     ///< comparacion con y sin signo
    Concurrency, ///< procesos, monitores, futuros, corrutinas, distribuido
    Exception,   ///< frames de excepcion
    Float,       ///< coma flotante y banco ancho
    Gc,          ///< recoleccion, finalizadores, referencias debiles
    Memory,      ///< transferencia, cursores, asignador crudo
    Object,      ///< sistema de objetos y meta-programacion
    Other,       ///< lo que la tabla no agrupaba en ninguna seccion
    Stack,       ///< pila
    String,      ///< cadenas
    System,      ///< informacion de la VM, nop, argv
    kCount
};

/**
 * @enum Mnemonic
 * @brief Las instrucciones que el `.vel` admite, AGRUPADAS por categoria.
 *
 * El orden es el de @c instr_list.h -- por categoria, no alfabetico -- y de eso
 * depende que los rangos sean contiguos.  Reordenar la lista sin tenerlo en
 * cuenta rompe las preguntas por categoria EN SILENCIO: siguen dando un
 * resultado, el equivocado.  Por eso hay un test que comprueba la contiguidad.
 */
enum class Mnemonic : uint16_t {
#define VX_INSTR(id, text, cat) id,
#include "emmit/instr_list.h"
#undef VX_INSTR
    /// Cuantas hay.  Al final a proposito: dimensiona las tablas planas.
    kCount
};

/// Cuantas instrucciones hay.
inline constexpr uint16_t mnemonic_count() {
    return static_cast<uint16_t>(Mnemonic::kCount);
}

/**
 * @brief El texto del mnemonico @p m (`"jmp.je"`, `"mov"`, ...).
 *
 * Indexado, no buscado: el mnemonico es la posicion.
 */
inline constexpr const char *text_of(Mnemonic m) {
    constexpr const char *kTexts[] = {
#define VX_INSTR(id, text, cat) text,
#include "emmit/instr_list.h"
#undef VX_INSTR
    };
    return static_cast<uint16_t>(m) < mnemonic_count()
               ? kTexts[static_cast<uint16_t>(m)]
               : "";
}

/**
 * @brief La categoria de @p m.
 *
 * Tabla plana de un byte por instruccion.  Se conserva ademas de los rangos
 * porque "de que categoria es" es UNA lectura, mientras que deducirlo de los
 * rangos seria recorrerlos.
 */
inline constexpr Category category_of(Mnemonic m) {
    constexpr Category kCats[] = {
#define VX_INSTR(id, text, cat) Category::cat,
#include "emmit/instr_list.h"
#undef VX_INSTR
    };
    return static_cast<uint16_t>(m) < mnemonic_count()
               ? kCats[static_cast<uint16_t>(m)]
               : Category::Other;
}

/**
 * @brief Primer y ultimo mnemonico de la categoria @p c, ambos incluidos.
 *
 * Para recorrer una categoria entera sin filtrar la lista: sus indices son
 * consecutivos, asi que es un `for` de uno a otro.  Los limites se calculan al
 * COMPILAR recorriendo la tabla una vez.
 */
struct CategoryRange {
    uint16_t first = 0;
    uint16_t last = 0;   ///< incluido
    bool empty = true;
};

inline constexpr CategoryRange range_of(Category c) {
    constexpr Category kCats[] = {
#define VX_INSTR(id, text, cat) Category::cat,
#include "emmit/instr_list.h"
#undef VX_INSTR
    };
    CategoryRange r;
    for (uint16_t i = 0; i < mnemonic_count(); ++i) {
        if (kCats[i] != c) continue;
        if (r.empty) {
            r.first = i;
            r.empty = false;
        }
        r.last = i;
    }
    return r;
}

/**
 * @brief Si @p m es de la categoria @p c, en dos comparaciones.
 *
 * Aprovecha la contiguidad: pertenecer a una categoria es caer en un intervalo.
 */
inline constexpr bool is_in(Mnemonic m, Category c) {
    const CategoryRange r = range_of(c);
    const uint16_t i = static_cast<uint16_t>(m);
    return !r.empty && i >= r.first && i <= r.last;
}

/**
 * @brief El mnemonico que se escribe @p text, o @c kCount si no existe.
 *
 * Es la FRONTERA: el unico sitio donde una cadena se convierte en mnemonico.
 * Del lado de dentro ya nadie compara texto -- se compara el enum, que es un
 * entero --, y por eso esta conversion ocurre UNA vez por token en el lexer y no
 * en cada consulta como pasaba con las tablas indexadas por cadena.
 *
 * Devolver @c kCount y no lanzar es a proposito: "esto no es una instruccion" es
 * una respuesta que el lexer necesita para poder decir DoNDE estaba el nombre
 * mal escrito.
 *
 * La tabla se ordena por texto la primera vez y luego es busqueda binaria.  No
 * puede aprovecharse el orden del enum porque ese es por categoria, y las dos
 * ordenaciones no coinciden -- ni deben: la del enum sirve para las preguntas
 * calientes por categoria, y esta solo se usa al leer texto.
 */
inline Mnemonic mnemonic_from_text(const char *text) {
    struct Entry {
        const char *text;
        Mnemonic m;
    };
    static const std::vector<Entry> kSorted = [] {
        std::vector<Entry> v;
        v.reserve(mnemonic_count());
#define VX_INSTR(id, text, cat) v.push_back({text, Mnemonic::id});
#include "emmit/instr_list.h"
#undef VX_INSTR
        std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
            return std::strcmp(a.text, b.text) < 0;
        });
        return v;
    }();
    if (text == nullptr) return Mnemonic::kCount;
    size_t lo = 0, hi = kSorted.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int c = std::strcmp(kSorted[mid].text, text);
        if (c == 0) return kSorted[mid].m;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return Mnemonic::kCount;
}

/// Si @p m es una instruccion de verdad y no el centinela del final.
inline constexpr bool is_valid(Mnemonic m) {
    return static_cast<uint16_t>(m) < mnemonic_count();
}

/// Nombre de la categoria, para diagnosticos y volcados.
inline constexpr const char *category_name(Category c) {
    switch (c) {
    case Category::Arithmetic:  return "arithmetic";
    case Category::Bitwise:     return "bitwise";
    case Category::Branch:      return "branch";
    case Category::Call:        return "call";
    case Category::Compare:     return "compare";
    case Category::Concurrency: return "concurrency";
    case Category::Exception:   return "exception";
    case Category::Float:       return "float";
    case Category::Gc:          return "gc";
    case Category::Memory:      return "memory";
    case Category::Object:      return "object";
    case Category::Other:       return "other";
    case Category::Stack:       return "stack";
    case Category::String:      return "string";
    case Category::System:      return "system";
    default:                    return "?";
    }
}

} // namespace emmit

#endif // EMMIT_MNEMONIC_H
