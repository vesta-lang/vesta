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
 * @file emmit/operand.h
 * @brief Los OPERANDOS del `.vel`, tipados.
 *
 * Cada uno sabe imprimirse como lo espera el `.vel`, asi que quien emite no
 * cose corchetes, comas ni sufijos: dice QUE es cada cosa.  Un corchete que
 * falta o un sufijo de ancho en el sitio equivocado dejan de ser posibles, en
 * vez de descubrirse al ensamblar.
 *
 * ## Por que viven aqui y no con el emisor del IR
 *
 * Nacieron en `ir/vel_sink.h` porque el emisor del IR fue quien los necesito
 * primero.  Pero un registro, un acceso a memoria o una referencia a simbolo no
 * son del IR: son del lenguaje ensamblador -- del mismo nivel que el mnemonico
 * (@c emmit/mnemonic.h) y la anotacion (@c emmit/directive.h).
 *
 * La diferencia importa porque el CODIFICADOR tiene que poder usarlos, y
 * `emmit` no puede depender de `ir`: seria la dependencia al reves.
 */
#ifndef EMMIT_OPERAND_H
#define EMMIT_OPERAND_H

#include "emmit/directive.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <utility>

namespace ir {

/**
 * @brief Un REGISTRO como operando.
 *
 * El ancho es parte del operando, no un sufijo que se pega a la cadena.  Antes
 * se escribia `out << scratch << "b, "`, y esa `"b"` suelta se podia olvidar, o
 * poner donde no iba, sin que nada lo notara hasta ensamblar.
 */
/**
 * @brief Los registros con nombre PROPIO: los que no siguen ningun patron.
 *
 * Son lista cerrada, asi que van como enum.  Los bancos indexados (`r0..r15`,
 * `f0..f15`, `xmm/ymm/zmm`) NO: escribir sus cien nombres a mano seria una
 * tercera copia del mismo conocimiento -- el error que @c instr_list.h advierte
 * --, cuando lo que los define es el patron.  Para esos hay constructores con
 * el indice ACOTADO, que es lo que de verdad impide un `r99`.
 */
enum class SpecialReg : uint8_t {
    RIP,
    RBP,
    RSP,
    RFLAGS,
    CUR0,
    CUR1,
    CUR2,
    CUR3
};

/// El texto de un registro especial, indexado por el enum (no buscado).
inline const char *text_of(SpecialReg r) {
    constexpr const char *kNames[] = {"rip",  "rbp",  "rsp",  "rflags",
                                      "cur0", "cur1", "cur2", "cur3"};
    return kNames[static_cast<uint8_t>(r)];
}

struct Reg {
    /// Ancho de la vista: el `.vel` lo escribe como sufijo del nombre.
    enum class Width : uint8_t { Q, D, W, B };

    /**
     * @brief De que banco es el registro.
     *
     * Guardar el BANCO y el INDICE, y no el nombre, es lo que quita las
     * asignaciones de memoria: `Reg::gp(13)` construia antes `"r" +
     * std::to_string(13)`, o sea un `std::string` en el monton POR REGISTRO Y
     * POR EMISION, y el emisor emite cientos de miles.  El nombre sale de una
     * tabla indexada cuando hace falta escribirlo, que es una sola vez y sin
     * copiar nada.
     *
     * Cachear los nombres repetidos habria aliviado el sintoma; no crearlos lo
     * quita.
     */
    enum class Bank : uint8_t { GP, FP, XMM, YMM, ZMM, Special };

    /// El sufijo del `.vel` para cada ancho.  Interno: la version publica es
    /// @ref suffix_of, que no puede declararse antes que este tipo.
    static const char *suffix_of_(Width w) {
        switch (w) {
        case Width::B: return "b";
        case Width::W: return "w";
        case Width::D: return "d";
        case Width::Q: break;
        }
        return "";
    }

    Bank bank = Bank::GP;
    uint8_t index = 0; ///< 0..15, o el @ref SpecialReg si @c bank es Special.
    Width width = Width::Q;

    /**
     * @brief Construye desde banco e indice.
     *
     * TRES BYTES y `constexpr`: un registro ES un banco, un numero y un ancho.
     * Tuvo dentro un `std::string` mientras hizo falta el puente desde los
     * nombres que el emisor calculaba a mano; al quedarse ese puente sin
     * usuarios, el campo se fue con el.  Ahora el tipo es trivial y pasarlo por
     * valor no copia nada.
     */
    constexpr Reg(Bank b, uint8_t i, Width w = Width::Q) noexcept
        : bank(b), index(i), width(w) {}

    /**
     * @brief Registro de proposito general `r0`..`r15`.
     * @param n Indice; fuera de 0..15 no es un registro y se ACOTA en vez de
     *          producir un nombre que no existe.
     */
    static constexpr Reg gp(unsigned n, Width w = Width::Q) noexcept {
        return Reg(Bank::GP, static_cast<uint8_t>(n > 15 ? 15 : n), w);
    }
    /// Registro escalar de coma flotante `f0`..`f15`.
    static constexpr Reg fp(unsigned n) noexcept {
        return Reg(Bank::FP, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    /// Vectorial: `xmm`/`ymm`/`zmm` `0`..`15`.
    static constexpr Reg xmm(unsigned n) noexcept {
        return Reg(Bank::XMM, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    static constexpr Reg ymm(unsigned n) noexcept {
        return Reg(Bank::YMM, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    static constexpr Reg zmm(unsigned n) noexcept {
        return Reg(Bank::ZMM, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    /// Uno de los que tienen nombre propio.
    static constexpr Reg special(SpecialReg s) noexcept {
        return Reg(Bank::Special, static_cast<uint8_t>(s));
    }

    /**
     * @brief El nombre del registro, SIN el sufijo de ancho.
     *
     * Devuelve `const char *` y las tablas son `constexpr`: no hay ni una
     * cadena que construir ni que alojar.  Tuvo tablas de `std::string`
     * mientras existio el puente desde nombres calculados a mano -- y esas se
     * alojan en el monton al arrancar el programa, una vez por tabla, sin que
     * nadie las pida.  Al no haber ya puente, sobran.
     */
    constexpr const char *name() const {
        constexpr const char *kGP[16] = {
            "r0", "r1", "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        constexpr const char *kFP[16] = {
            "f0", "f1", "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
            "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15"};
        constexpr const char *kXMM[16] = {"xmm0",  "xmm1",  "xmm2",  "xmm3",
                                          "xmm4",  "xmm5",  "xmm6",  "xmm7",
                                          "xmm8",  "xmm9",  "xmm10", "xmm11",
                                          "xmm12", "xmm13", "xmm14", "xmm15"};
        constexpr const char *kYMM[16] = {"ymm0",  "ymm1",  "ymm2",  "ymm3",
                                          "ymm4",  "ymm5",  "ymm6",  "ymm7",
                                          "ymm8",  "ymm9",  "ymm10", "ymm11",
                                          "ymm12", "ymm13", "ymm14", "ymm15"};
        constexpr const char *kZMM[16] = {"zmm0",  "zmm1",  "zmm2",  "zmm3",
                                          "zmm4",  "zmm5",  "zmm6",  "zmm7",
                                          "zmm8",  "zmm9",  "zmm10", "zmm11",
                                          "zmm12", "zmm13", "zmm14", "zmm15"};
        constexpr const char *kSpecial[8] = {"rip",  "rbp",  "rsp",  "rflags",
                                             "cur0", "cur1", "cur2", "cur3"};
        switch (bank) {
        case Bank::GP: return kGP[index & 15];
        case Bank::FP: return kFP[index & 15];
        case Bank::XMM: return kXMM[index & 15];
        case Bank::YMM: return kYMM[index & 15];
        case Bank::ZMM: return kZMM[index & 15];
        case Bank::Special: break;
        }
        return kSpecial[index & 7];
    }

    /**
     * @brief El registro es el general numero @p n.
     *
     * Pregunta por el REGISTRO, no por la vista: `r14` y `r14b` son el mismo, y
     * quien esto pregunta -- para invalidar una cache de constante, por ejemplo
     * -- se refiere al registro, no al ancho con el que se escribe.
     */
    constexpr bool is_gp(unsigned n) const {
        return bank == Bank::GP && index == n;
    }
};

/**
 * @brief Dos registros son el mismo operando si coinciden banco, numero y
 * vista.
 *
 * Comparaba los NOMBRES, y eso era comparar cadenas para responder a una
 * pregunta que son tres bytes.  Hubo ademas comparaciones contra literales
 * (`r == "r14"`), que es preguntar por como se escribe en vez de por cual es:
 * para eso esta @ref Reg::is_gp, y esas comparaciones ya no existen.
 */
constexpr bool operator==(const Reg &a, const Reg &b) {
    return a.bank == b.bank && a.index == b.index && a.width == b.width;
}
constexpr bool operator!=(const Reg &a, const Reg &b) {
    return !(a == b);
}

/// El sufijo que el `.vel` espera para cada ancho.
inline const char *suffix_of(Reg::Width w) {
    return Reg::suffix_of_(w);
}

/**
 * @brief El ancho de vista que corresponde a un tamano en BYTES.
 *
 * El emisor calculaba el sufijo a mano en cada sitio (`(n == 4) ? "d" : ...`)
 * y lo pegaba al nombre del registro.  Escrito asi, el ancho es una cadena
 * suelta que se puede olvidar, duplicar o poner donde no va; aqui es el mismo
 * valor que el operando ya lleva dentro.
 *
 * @param bytes Tamano del elemento (1, 2, 4 o cualquier otro = palabra).
 * @return El ancho correspondiente.
 */
inline Reg::Width width_for_bytes(unsigned bytes) {
    switch (bytes) {
    case 1: return Reg::Width::B;
    case 2: return Reg::Width::W;
    case 4: return Reg::Width::D;
    default: return Reg::Width::Q;
    }
}

/**
 * @brief Un ACCESO A MEMORIA como operando: `[base]`, `[base+8]`, `[b+i*4]`.
 *
 * Los corchetes los pone el operando, no quien lo escribe.  Cosidos a mano
 * (`<< ", [" << reg << "]"`) es facil dejarse uno, o abrirlo donde el `.vel`
 * espera un registro -- y eso no falla hasta el ensamblador.
 *
 * Base e indice son REGISTROS, no cadenas.  Lo fueron mientras un registro era
 * texto; ahora son tres bytes cada uno, asi que guardarlos como `std::string`
 * era pedir memoria para escribir "r10".  Y "no hay indice" es una bandera: un
 * registro no tiene estado vacio que sirva de centinela.
 */
struct Mem {
    Reg base;       ///< registro base.
    Reg index;      ///< registro indice; solo cuenta si @c hay_index.
    bool hay_index; ///< si el acceso lleva indice.
    unsigned scale; ///< 1/2/4/8; solo cuenta con @c hay_index.
    long long disp; ///< desplazamiento, con signo.

    constexpr explicit Mem(Reg b) noexcept
        : base(b), index(Reg::gp(0)), hay_index(false), scale(1), disp(0) {}
    constexpr Mem(Reg b, long long off) noexcept
        : base(b), index(Reg::gp(0)), hay_index(false), scale(1), disp(off) {}
    constexpr Mem(Reg b, Reg idx, unsigned sc) noexcept
        : base(b), index(idx), hay_index(true), scale(sc), disp(0) {}
};

/**
 * @brief Una ETIQUETA como operando (destino de salto o de llamada).
 *
 * Distinta de un registro a proposito: `jmp r0` y `jmp fin` no son lo mismo, y
 * con cadenas los dos se escriben igual.
 */
struct Lbl {
    std::string name;
    explicit Lbl(std::string n) noexcept : name(std::move(n)) {}
};

/**
 * @brief Una REFERENCIA A SIMBOLO como operando: `@Absolute("code.fin")`.
 *
 * El `.vel` las escribe como anotaciones, y cosidas a mano son cuatro trozos
 * -- la arroba, el nombre de la clase, los parentesis y las comillas -- que hay
 * que acertar en orden.  Una comilla que falta no la ve nadie hasta ensamblar.
 *
 * La clase sale de @c emmit::Directive, que es la lista UNICA de anotaciones
 * del `.vel`.  Tuvo aqui su propio enum durante una tanda, y eso era empezar la
 * quinta copia de la misma lista -- el error que este refactor viene a cerrar.
 */
struct Ann {
    emmit::Directive kind;
    std::string value;

    Ann(emmit::Directive k, std::string v) noexcept
        : kind(k), value(std::move(v)) {}

    /// `@Absolute("<v>")`.
    static Ann absolute(std::string v) {
        return Ann(emmit::Directive::ABS_REF, std::move(v));
    }
    /// `@Method("<v>")`.
    static Ann method(std::string v) {
        return Ann(emmit::Directive::METHOD, std::move(v));
    }
    /// `@Name("<v>")`.
    static Ann name(std::string v) {
        return Ann(emmit::Directive::NAME, std::move(v));
    }
};

/// Una referencia se imprime con su clase, sus parentesis y sus comillas.
inline std::ostream &operator<<(std::ostream &os, const Ann &a) {
    return os << '@' << emmit::text_of(a.kind) << "(\"" << a.value << "\")";
}

/// Un registro se imprime con su ancho pegado, que es como lo espera el `.vel`.
inline std::ostream &operator<<(std::ostream &os, const Reg &r) {
    return os << r.name() << suffix_of(r.width);
}

/// La memoria se imprime con sus corchetes y solo con las partes que tiene.
///
/// Los espacios alrededor del signo NO son cosmetica: es la forma que ya
/// escribia el emisor a mano (`[r15 + r13]`), y cambiarla haria que la misma
/// entrada produjera un `.vel` distinto -- que es justo lo que este refactor
/// tiene que poder descartar para saber que no ha roto nada.
inline std::ostream &operator<<(std::ostream &os, const Mem &m) {
    os << '[' << m.base;
    if (m.hay_index) {
        os << " + " << m.index;
        if (m.scale != 1) os << " * " << m.scale;
    }
    if (m.disp > 0)
        os << " + " << m.disp;
    else if (m.disp < 0)
        os << " - " << -m.disp;
    return os << ']';
}

/// Una etiqueta se imprime por su nombre.
inline std::ostream &operator<<(std::ostream &os, const Lbl &l) {
    return os << l.name;
}

} // namespace ir

#endif // EMMIT_OPERAND_H
