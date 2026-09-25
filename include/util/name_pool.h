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
 * @file util/name_pool.h
 * @brief Pozo de nombres compartidos, para lo que se repite millones de veces.
 *
 * Un nombre de fichero se guarda en CADA posicion de fuente, y una posicion va
 * en cada nodo del arbol, en cada token y en cada instruccion del intermedio.
 * Guardarlo por valor reserva memoria SIEMPRE -- una ruta pasa de los ochenta
 * caracteres y no cabe en el buffer pequeno de `std::string` --, y no una vez
 * sino en cada COPIA, que es lo que de verdad pasa mucho.
 *
 * El pozo lo reparte una sola vez y devuelve un puntero estable: a partir de
 * ahi una posicion se copia sin tocar el monton.
 *
 * Se interna UNA VEZ POR FICHERO, no por nodo.  Quien lexa pide aqui el nombre
 * al empezar y se lo pasa a todo lo que produzca; por eso el cerrojo que hace
 * falta para compartir el pozo entre hilos no se nota.
 *
 * El pozo NO se vacia.  Esta acotado por el numero de ficheros distintos, y los
 * punteros tienen que seguir valiendo mientras viva algo que los apunte --
 * incluido un diagnostico ya entregado a quien llamo.
 *
 * El preprocesador tiene el suyo (`vpp::intern_file_name`) por la misma razon y
 * con los mismos numeros; no se comparten porque son proyectos distintos y
 * acoplarlos por una tabla de cadenas seria peor que tener dos.
 */
#ifndef UTIL_NAME_POOL_H
#define UTIL_NAME_POOL_H

#include <cstddef>
#include <functional> // std::hash
#include <iosfwd>
#include <string>

namespace util {

/**
 * @brief Da el puntero compartido y estable de @p name, internandolo si hace
 *        falta.
 *
 * Toma un cerrojo, asi que NO debe llamarse por nodo ni por token: se llama al
 * abrir un fichero y el puntero se reparte desde ahi.
 *
 * @param name Nombre a internar.
 * @return Puntero al nombre compartido; nunca nulo.
 */
const std::string *intern_name(const std::string &name);

/**
 * @brief El nombre vacio compartido.
 *
 * Variable EN LINEA y no un `static` dentro de la funcion: un estatico local se
 * inicializa la primera vez que se pasa por el, asi que el compilador mete una
 * comprobacion de guarda -- y ademas con cerrojo, por si dos hilos llegan a la
 * vez -- en CADA llamada.  Y aqui se llama al construir cualquier nombre por
 * defecto, que es lo que hace todo tipo sin nombre y toda posicion de fuente.
 *
 * Medido: costaba 15 millones de instrucciones, el 1 % de compilar.
 */
inline const std::string kEmptyName;

/**
 * @brief El nombre vacio compartido.
 *
 * Va aparte de @c intern_name para que construir un nombre por defecto -- lo
 * que hace cada nodo antes de que nadie le diga como se llama -- no toque el
 * pozo ni su cerrojo.
 *
 * @return Puntero a la cadena vacia compartida; nunca nulo.
 */
inline const std::string *empty_name() noexcept {
    return &kEmptyName;
}

/**
 * @brief Un nombre INTERNADO, como tipo propio y no como puntero suelto.
 *
 * Guardado como `const std::string*`, un nombre internado es para el
 * compilador un puntero cualquiera: se escribe en un flujo -- y sale una
 * DIRECCION en vez del nombre, sin aviso --, se convierte a `bool`, se compara
 * con cualquier otro puntero.  Paso al internar las rutas de los modulos: nueve
 * trazas compilaban e imprimian direcciones, y solo se encontraron buscandolas
 * a mano.  Con este tipo:
 *
 *  - escribirlo en un flujo escribe el TEXTO;
 *  - no se convierte implicitamente en nada;
 *  - `==`, `!=` y el hash van por IDENTIDAD, que es para lo que se interna;
 *  - concatenarlo o compararlo con una `std::string` no compila: quien quiere
 *    el texto lo pide con @ref str, y queda escrito que lo hace.
 *
 * Nunca es nulo: por defecto es el nombre vacio.  Cabe en un registro y se pasa
 * por valor.
 */
class InternedName {
  public:
    /// El nombre vacio.
    InternedName() noexcept : p_(empty_name()) {}

    /**
     * @brief Interna @p name.  Toma el cerrojo del pozo: ver @ref intern_name.
     * @param name Texto del nombre.
     * @return El nombre internado.
     */
    static InternedName intern(const std::string &name) {
        return InternedName(intern_name(name));
    }

    /**
     * @brief Adopta un puntero que YA salio del pozo (o de @ref empty_name).
     * @param p Puntero internado; nulo se toma como el nombre vacio.
     * @return El nombre.
     */
    static InternedName from_interned(const std::string *p) noexcept {
        return InternedName(p != nullptr ? p : empty_name());
    }

    /// @brief El texto.  @return Referencia estable, vive lo que el proceso.
    const std::string &str() const noexcept { return *p_; }
    /// @brief El texto como cadena de C.  @return Estable.
    const char *c_str() const noexcept { return p_->c_str(); }
    /// @brief Si es el nombre vacio.  @return true si no tiene texto.
    bool empty() const noexcept { return p_->empty(); }
    /// @brief El puntero internado, para quien indexa por el.
    /// @return Nunca nulo.
    const std::string *ptr() const noexcept { return p_; }

    /// Igualdad por IDENTIDAD: dos nombres internados iguales son el mismo.
    friend bool operator==(InternedName a, InternedName b) noexcept {
        return a.p_ == b.p_;
    }
    friend bool operator!=(InternedName a, InternedName b) noexcept {
        return a.p_ != b.p_;
    }

  private:
    explicit InternedName(const std::string *p) noexcept : p_(p) {}
    const std::string *p_;
};

/**
 * @brief Escribe el TEXTO del nombre, no su direccion.
 * @param os Flujo.
 * @param n  Nombre.
 * @return El flujo.
 */
std::ostream &operator<<(std::ostream &os, InternedName n);

/// Hash de @ref InternedName por identidad: el puntero, sin mirar el texto.
struct InternedNameHash {
    size_t operator()(InternedName n) const noexcept {
        return std::hash<const std::string *>()(n.ptr());
    }
};

} // namespace util

#endif // UTIL_NAME_POOL_H
