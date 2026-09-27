/**
 * @file type_fingerprints.h
 * @brief La huella de cada tipo agregado (struct, clase, enum): lo que el
 *        comprobador sabe de su forma, para contrastar los contratos de tipo
 *        (`@pod`, `@no_heap`, `@size`) y para el informe de `--analyze`.
 *
 * Vivia como funcion privada del camino de fichero suelto, y el de proyecto no
 * la tenia: los contratos de tipo no se comprobaban para ningun programa con
 * `namespace` o `import`.  Ahora la calcula cada modulo al compilarse y el
 * resultado se une.
 */
#ifndef VX_TYPE_FINGERPRINTS_H
#define VX_TYPE_FINGERPRINTS_H

#include "analyze/fingerprint.h"

namespace vx {

class TypeChecker;

/**
 * @brief La huella de cada struct, clase (no interfaz) y enum que conoce
 *        @p tc, a partir de sus layouts ya resueltos.
 *
 * `@pod` y `@no_heap` se componen con los clasificadores del comprobador
 * (`type_is_managed`, `type_is_c_representable`), que son la verdad de la
 * frontera C.  Una clase es un tipo por REFERENCIA: nunca es `@pod` ni
 * `@no_heap`.
 *
 * Solo de los tipos que el modulo DECLARA: los que importa (marcados con
 * `TypeChecker::is_imported`) los calcula su propio modulo.
 *
 * @param tc El comprobador, ya ejecutado.
 * @return Una huella por tipo.
 */
analyze::TypeFingerprints compute_type_fingerprints(const TypeChecker &tc);

} // namespace vx

#endif // VX_TYPE_FINGERPRINTS_H
