# VestaPlugin.cmake
# Modulo CMake reutilizable para construir plugins nativos de VestaVM.
#
# Uso:
#   list(APPEND CMAKE_MODULE_PATH "${VESTA_SDK_DIR}/cmake")
#   include(VestaPlugin)
#
#   add_vesta_plugin(mi_plugin
#       SOURCES mi_plugin.c otro_archivo.c
#       SDK_DIR  /ruta/al/sdk/vestavm      # directorio con include/ffi/vesta_plugin.h
#   )
#
# El target resultante es una libreria compartida con el nombre correcto
# por plataforma:  mi_plugin.dll (Windows), libmi_plugin.so (Linux),
# libmi_plugin.dylib (macOS).
#
# Variables de salida:
#   <TARGET>_OUTPUT_FILE  -- ruta completa al artefacto generado.

cmake_minimum_required(VERSION 3.15)

# ---------------------------------------------------------------------------
# Deteccion de compilador y plataforma
# ---------------------------------------------------------------------------

# Identificar familia de compilador una vez y exportar como variables de cache
# internas para que otros modulos puedan consultarlas.
if(NOT DEFINED VESTA_COMPILER_FAMILY)
    if(MSVC)
        set(VESTA_COMPILER_FAMILY "MSVC"   CACHE INTERNAL "")
    elseif(MINGW)
        set(VESTA_COMPILER_FAMILY "MinGW"  CACHE INTERNAL "")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(VESTA_COMPILER_FAMILY "Clang"  CACHE INTERNAL "")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(VESTA_COMPILER_FAMILY "GCC"    CACHE INTERNAL "")
    else()
        set(VESTA_COMPILER_FAMILY "Unknown" CACHE INTERNAL "")
    endif()
    message(STATUS "[VestaPlugin] Compilador detectado: ${VESTA_COMPILER_FAMILY}")
endif()

# ---------------------------------------------------------------------------
# Funcion principal: add_vesta_plugin
# ---------------------------------------------------------------------------

# add_vesta_plugin(<target>
#     SOURCES  <archivo.c> [<archivo.c> ...]   -- fuentes del plugin (C o C++)
#     [SDK_DIR  <ruta>]                         -- directorio raiz del SDK VestaVM
#                                                  (debe contener include/ffi/vesta_plugin.h)
#                                                  Por defecto: CMAKE_CURRENT_SOURCE_DIR/../../..
#     [OUTPUT_NAME <nombre>]                    -- nombre del artefacto sin extension
#                                                  Por defecto: igual al target
#     [C_STANDARD   <std>]                      -- estandar C (99 | 11 | 17); por defecto 11
#     [CXX_STANDARD <std>]                      -- estandar C++ (11 | 14 | 17 | 20); por defecto 17
#     [EXTRA_COMPILE_OPTIONS <opts...>]         -- flags adicionales de compilacion
#     [EXTRA_LINK_OPTIONS    <opts...>]         -- flags adicionales de enlace
# )

function(add_vesta_plugin TARGET)
    cmake_parse_arguments(
        _P                          # prefijo de variables locales
        ""                          # opciones booleanas (ninguna)
        "SDK_DIR;OUTPUT_NAME;C_STANDARD;CXX_STANDARD"  # args de un solo valor
        "SOURCES;EXTRA_COMPILE_OPTIONS;EXTRA_LINK_OPTIONS"  # args de lista
        ${ARGN}
    )

    # -- Validar argumentos obligatorios --
    if(NOT _P_SOURCES)
        message(FATAL_ERROR "[VestaPlugin] add_vesta_plugin(${TARGET}): se requiere SOURCES")
    endif()

    # -- Resolver SDK_DIR --
    if(NOT _P_SDK_DIR)
        if(DEFINED VESTA_SDK_DIR)
            # Preferir la variable de cache inyectada por el proyecto padre
            set(_P_SDK_DIR "${VESTA_SDK_DIR}")
        elseif(DEFINED CMAKE_SOURCE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/include/ffi/vesta_plugin.h")
            # Cuando se compila como subproyecto, CMAKE_SOURCE_DIR es la raiz del SDK
            set(_P_SDK_DIR "${CMAKE_SOURCE_DIR}")
        else()
            # Ultimo recurso: la convencion examples_codes_vm/plugin_xxx/ -> subir dos niveles
            get_filename_component(_P_SDK_DIR
                "${CMAKE_CURRENT_SOURCE_DIR}/../.."
                ABSOLUTE
            )
        endif()
    endif()

    set(_PLUGIN_HEADER "${_P_SDK_DIR}/include/ffi/vesta_plugin.h")
    if(NOT EXISTS "${_PLUGIN_HEADER}")
        message(WARNING
            "[VestaPlugin] No se encontro vesta_plugin.h en '${_P_SDK_DIR}/include/ffi/'.\n"
            "Ajusta SDK_DIR o copia el header al directorio correcto."
        )
    endif()

    # -- Estandares de lenguaje --
    if(NOT _P_C_STANDARD)
        set(_P_C_STANDARD 11)
    endif()
    if(NOT _P_CXX_STANDARD)
        set(_P_CXX_STANDARD 17)
    endif()

    # -- Crear el target de libreria compartida --
    add_library(${TARGET} SHARED ${_P_SOURCES})

    # Incluir cabeceras del SDK
    target_include_directories(${TARGET} PRIVATE
        "${_P_SDK_DIR}/include"
    )

    # Nombre de salida (sin prefijo "lib" en Windows para compatibilidad con LoadLibraryA)
    if(_P_OUTPUT_NAME)
        set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME "${_P_OUTPUT_NAME}")
    endif()

    # Sin prefijo "lib" en todas las plataformas (los plugins se cargan por nombre exacto)
    set_target_properties(${TARGET} PROPERTIES PREFIX "")

    # Estandares de lenguaje
    set_target_properties(${TARGET} PROPERTIES
        C_STANDARD   ${_P_C_STANDARD}
        CXX_STANDARD ${_P_CXX_STANDARD}
        C_STANDARD_REQUIRED   ON
        CXX_STANDARD_REQUIRED ON
        C_EXTENSIONS   OFF
        CXX_EXTENSIONS OFF
    )

    # -- Flags por compilador y plataforma --
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE
            /W3             # nivel de aviso moderado
            /WX-            # no tratar avisos como errores (plugins externos)
            /utf-8          # interpretar fuentes como UTF-8
            ${_P_EXTRA_COMPILE_OPTIONS}
        )
        # Exportar todos los simbolos automaticamente en Windows con MSVC
        set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

    elseif(MINGW)
        target_compile_options(${TARGET} PRIVATE
            -Wall
            -Wextra
            -Wno-unused-parameter
            ${_P_EXTRA_COMPILE_OPTIONS}
        )
        # Exportar todos los simbolos en MinGW igual que MSVC
        set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
        target_link_options(${TARGET} PRIVATE
            -Wl,--enable-auto-import
            ${_P_EXTRA_LINK_OPTIONS}
        )

    else()
        # GCC / Clang en Linux y macOS
        target_compile_options(${TARGET} PRIVATE
            -Wall
            -Wextra
            -Wno-unused-parameter
            -fvisibility=hidden      # ocultar simbolos por defecto; solo los marcados visible se exportan
            ${_P_EXTRA_COMPILE_OPTIONS}
        )
        # -fPIC es obligatorio para .so en Linux; en macOS es el comportamiento por defecto
        set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_link_options(${TARGET} PRIVATE
            ${_P_EXTRA_LINK_OPTIONS}
        )
    endif()

    # -- Definicion del macro de exportacion --
    # El plugin puede usar VESTA_PLUGIN_EXPORT delante de sus funciones para
    # marcarlas explicitamente como visibles; es opcional cuando se usa
    # WINDOWS_EXPORT_ALL_SYMBOLS o -fvisibility=hidden con atributo default.
    if(MSVC OR MINGW)
        target_compile_definitions(${TARGET} PRIVATE
            "VESTA_PLUGIN_EXPORT=__declspec(dllexport)"
        )
    else()
        target_compile_definitions(${TARGET} PRIVATE
            "VESTA_PLUGIN_EXPORT=__attribute__((visibility(\"default\")))"
        )
    endif()

    # -- Tipo de build por defecto si no se especifico --
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    # -- Mostrar resumen --
    message(STATUS
        "[VestaPlugin] Target '${TARGET}' configurado"
        "  | Compilador: ${VESTA_COMPILER_FAMILY}"
        "  | SDK: ${_P_SDK_DIR}"
    )

    # -- Auto-instalacion (CPack) --
    # Los plugins bajo stdlib/ se empaquetan en la MISMA ruta relativa al SDK que
    # ocupan en el arbol (p.ej. stdlib/native/io -> <prefix>/stdlib/native/io),
    # que es donde el runtime los busca (relativo a vm.exe).  Asi un plugin nuevo
    # entra al instalador sin tocar nada.  Los plugins de ejemplo no se instalan.
    if(DEFINED VESTA_SDK_DIR)
        file(RELATIVE_PATH _vp_rel "${VESTA_SDK_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
        if(_vp_rel MATCHES "^stdlib/")
            install(TARGETS ${TARGET}
                RUNTIME DESTINATION "${_vp_rel}" COMPONENT stdlib
                LIBRARY DESTINATION "${_vp_rel}" COMPONENT stdlib)
        endif()
    endif()

    # -- Variable de salida con la ruta del artefacto --
    set(${TARGET}_OUTPUT_FILE
        "$<TARGET_FILE:${TARGET}>"
        PARENT_SCOPE
    )
endfunction()
