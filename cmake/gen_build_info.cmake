# ============================================================================
# gen_build_info.cmake -- escribe la fecha/hora del build + hash de git en un
# header generado (build_info_generated.h).  Se ejecuta en CADA build (target
# ALL siempre out-of-date) para que `vesta --version` muestre el timestamp
# real del momento de compilacion.
#
# El header solo lo consume `src/cli/version_info.cpp` (una TU pequena), asi
# que reescribirlo en cada build solo fuerza a recompilar ese fichero, no todo
# main.cpp.
#
# Argumentos (via -D):
#   OUT     : ruta del header a generar.
#   SRC_DIR : raiz del repo (para leer el hash de git).
# ============================================================================

# La FECHA no se escribe aqui: la pone el compilador con __DATE__/__TIME__ al
# compilar `version_info.cpp`.
#
# Escribirla aqui obligaba a reescribir el header en cada build -- el segundo
# cambia siempre --, y eso recompilaba esa unidad y RE-ENLAZABA el binario
# entero: 6,3 segundos por un build en el que no habia cambiado nada.  Y ni
# siquiera era mas exacta: decia cuando corrio cmake, no cuando se compilo el
# programa.  Con __DATE__/__TIME__ la fecha es la de la compilacion de verdad, y
# esa unidad se recompila exactamente cuando hay algo que recompilar.

# Hash corto del commit actual (si git esta disponible + es un repo).
set(_git_hash "desconocido")
set(_git_known 0)
find_program(_GIT_EXE NAMES git)
if(_GIT_EXE)
    execute_process(
        COMMAND "${_GIT_EXE}" -C "${SRC_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE _h
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _h)
        set(_git_hash "${_h}")
        set(_git_known 1)
        # Marca si el arbol de trabajo tiene cambios sin commitear.
        execute_process(
            COMMAND "${_GIT_EXE}" -C "${SRC_DIR}" diff --quiet
            RESULT_VARIABLE _dirty
            ERROR_QUIET)
        if(NOT _dirty EQUAL 0)
            set(_git_hash "${_h}+")
        endif()
    endif()
endif()

set(_content "#pragma once\n")
string(APPEND _content "// Generado por cmake/gen_build_info.cmake -- NO editar a mano.\n")
string(APPEND _content "// Hash de git para `vesta --version`; la fecha la pone el compilador.\n")
string(APPEND _content "#define VESTA_BUILD_DATE (__DATE__ \" \" __TIME__)\n")
string(APPEND _content "#define VESTA_GIT_HASH \"${_git_hash}\"\n")
string(APPEND _content "#define VESTA_GIT_HASH_KNOWN ${_git_known}\n")

# Escribir SOLO si cambia.  El target corre en cada build a proposito -- hay que
# volver a mirar el hash y si el arbol esta sucio --, pero tocar el fichero
# cuando el contenido es el mismo arrastra una recompilacion y un enlace que no
# hacian falta.
set(_prev "")
if (EXISTS "${OUT}")
    file(READ "${OUT}" _prev)
endif()
if (NOT _prev STREQUAL _content)
    file(WRITE "${OUT}" "${_content}")
endif()
