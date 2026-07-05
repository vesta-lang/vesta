# Licencia de VestaVM

Copyright (c) 2026 David Lopez T. (DesmonHak)

VestaVM (el compilador, la maquina virtual, el JIT, el backend AOT, el
runtime, el LSP y las herramientas) se distribuye bajo los terminos de la
**GNU General Public License, version 2** (GPLv2), tal y como la publica la
Free Software Foundation.  El texto completo de la GPLv2 esta en el fichero
[`COPYING`](COPYING).

A esa licencia se le anaden las **dos excepciones** siguientes.

---

## Excepcion 1 -- Salida del compilador (Runtime Exception)

Al estilo de la *GCC Runtime Library Exception*, esta excepcion garantiza que
**el codigo que escribes en el lenguaje Vesta, y los programas que compilas con
VestaVM, son totalmente libres**: NO quedan sujetos a la GPL y puedes
licenciarlos como quieras.

> Como excepcion especial, el codigo que este compilador produce como salida
> (bytecode `.velb`, IR, codigo nativo AOT), asi como las porciones del runtime,
> del recolector de basura y de la biblioteca estandar de Vesta que se enlacen o
> incorporen en dicha salida, NO estan sujetos a la Licencia Publica General
> GNU.  Puedes usar, distribuir y licenciar tus programas compilados con
> VestaVM bajo los terminos que tu elijas, sin ninguna restriccion derivada de
> esta licencia.
>
> Esta excepcion NO otorga permiso para distribuir el propio VestaVM (su codigo
> fuente o sus binarios) fuera de los terminos de la GPLv2; solo afecta a la
> SALIDA que produce y a las partes del runtime/stdlib enlazadas en esa salida.

## Excepcion 2 -- Enlace con OpenSSL (OpenSSL Linking Exception)

> Como excepcion especial adicional, los titulares del copyright conceden
> permiso para enlazar el codigo de este programa con la biblioteca OpenSSL (o
> con una version modificada de dicha biblioteca que use la misma licencia que
> OpenSSL), y para distribuir las combinaciones enlazadas resultantes.  Debes
> cumplir la GPLv2 en todo lo demas respecto al codigo de este programa; si
> modificas este fichero, puedes extender esta excepcion a tu version del
> fichero, pero no estas obligado a hacerlo.  Si no deseas hacerlo, elimina esta
> declaracion de excepcion de tu version.

---

## Dependencias de terceros

VestaVM incorpora componentes de terceros con sus propias licencias, que
prevalecen sobre este fichero para el codigo respectivo:

- **Keystone** (`libs/SourceCode/keystone`) -- GPLv2.
- **Capstone** (`libs/SourceCode/capstone`) -- BSD-3-Clause.
- **nlohmann/json** (`libs/SourceCode/json`) -- MIT.
- **cxxopts** (`libs/SourceCode/cxxopts`) -- MIT.
- **SQLite** (bundled) -- dominio publico.
- **OpenSSL** -- Apache-2.0 (ver Excepcion 2).
- **LibPEparse** (`libs/SourceCode/LibPEparse`) -- MIT (autor: David Lopez T.).
- **DistanciaLevenshtein** (`libs/SourceCode/DistanciaLevenshtein`) -- MIT
  (autor: David Lopez T.).

## Nota

Los textos de las excepciones anteriores siguen las formas estandar de la
*GCC Runtime Library Exception* y de la *OpenSSL Linking Exception*.  Si vas a
apoyarte legalmente en ellos, conviene una revision juridica de la redaccion
final.
