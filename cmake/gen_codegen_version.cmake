# ============================================================================
# gen_codegen_version.cmake -- calcula la version del CODEGEN del compilador
# Vex y la escribe como VEXI_COMPILER_BUILD_ID en un header generado.
#
# Proposito: invalidar los caches .vexi / .vexir / .vpc SOLO cuando cambia el
# codigo que afecta la salida del compilador (lowering, IR, emisores, AOT...),
# NO en cada recompilacion.  Un rebuild sin cambios en esas fuentes produce el
# mismo hash -> el header no se reescribe -> vexi_format.cpp no se recompila ->
# el cvh no cambia -> el cache sigue valido.
#
# Se invoca via `cmake -P` desde un add_custom_command cuya lista DEPENDS son
# justamente esas fuentes; CMake lo re-ejecuta cuando alguna cambia.
#
# Argumentos (vía -D):
#   OUT  : ruta del header a generar.
#   SRCS : lista (separada por ;) de ficheros fuente del codegen.
# ============================================================================

set(_combined "")
# Orden estable: ordenar para que el hash no dependa del orden del glob.
list(SORT SRCS)
foreach(_f IN LISTS SRCS)
    if(EXISTS "${_f}")
        file(SHA256 "${_f}" _h)
        string(APPEND _combined "${_h}")
    endif()
endforeach()
string(SHA256 _final "${_combined}")

set(_content "#pragma once\n")
string(APPEND _content "// Generado por cmake/gen_codegen_version.cmake -- NO editar a mano.\n")
string(APPEND _content "// Hash de las fuentes de codegen; invalida los caches .vexi/.vexir/.vpc\n")
string(APPEND _content "// cuando el codigo de generacion del compilador cambia.\n")
string(APPEND _content "#define VEXI_COMPILER_BUILD_ID \"vexcg-${_final}\"\n")

# Escribir solo si cambia, para no forzar recompilaciones espurias.
set(_old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _content)
    file(WRITE "${OUT}" "${_content}")
    message(STATUS "[vex-codegen-version] actualizado: vexcg-${_final}")
endif()
