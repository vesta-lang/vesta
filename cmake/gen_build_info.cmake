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

# Fecha/hora local del build (formato legible ASCII).
string(TIMESTAMP _now "%Y-%m-%d %H:%M:%S")

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
string(APPEND _content "// Fecha/hora del build + hash de git para `vesta --version`.\n")
string(APPEND _content "#define VESTA_BUILD_DATE \"${_now}\"\n")
string(APPEND _content "#define VESTA_GIT_HASH \"${_git_hash}\"\n")
string(APPEND _content "#define VESTA_GIT_HASH_KNOWN ${_git_known}\n")

# Escribir siempre (el timestamp cambia en cada build).
file(WRITE "${OUT}" "${_content}")
