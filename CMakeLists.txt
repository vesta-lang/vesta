cmake_minimum_required(VERSION 3.20)

# Evitar warning CMP0063 de librerias externas
if (POLICY CMP0063)
    cmake_policy(SET CMP0063 NEW)
endif ()

#if (WIN32)
#    message(STATUS "Compilando en Windows: activando linkado estático")
#
#    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
#else ()
#    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
#endif ()

set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "Enable verbose build output" FORCE)


# helper para imprimir propiedades relevantes de un target
function(print_target_flags tgt)
    get_target_property(_inc_dirs ${tgt} INCLUDE_DIRECTORIES)
    get_target_property(_compile_opts ${tgt} COMPILE_OPTIONS)
    get_target_property(_compile_defs ${tgt} COMPILE_DEFINITIONS)
    get_target_property(_link_opts ${tgt} LINK_OPTIONS)
    get_target_property(_link_libs ${tgt} LINK_LIBRARIES)

    message(STATUS "---- Target: ${tgt} ----")
    message(STATUS "INCLUDE_DIRECTORIES: ${_inc_dirs}")
    message(STATUS "COMPILE_OPTIONS: ${_compile_opts}")
    message(STATUS "COMPILE_DEFINITIONS: ${_compile_defs}")
    message(STATUS "LINK_OPTIONS: ${_link_opts}")
    message(STATUS "LINK_LIBRARIES: ${_link_libs}")
endfunction()

# si no necesitamos los ejecutables de fuzzing,
# deshabilitarlos usando opciones de Keystone/LLVM
set(ENABLE_FUZZING OFF CACHE BOOL "" FORCE)


# Configuracion Capstone
set(CAPSTONE_ARM_SUPPORT ON CACHE BOOL "Enable ARM")
set(CAPSTONE_AARCH64_SUPPORT ON CACHE BOOL "Enable ARM64")
set(CAPSTONE_X86_SUPPORT ON CACHE BOOL "Enable X86/x64")

# Desactivar todos los demas backends
set(CAPSTONE_MIPS_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_PPC_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_SPARC_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_SYSTEMZ_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_XCORE_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_TMS320C64X_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_M680X_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_EVM_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_MOS65XX_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_WASM_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_BPF_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_RISCV_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_SH_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_TRICORE_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_ALPHA_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_HPPA_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_LOONGARCH_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_XTENSA_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_ARC_SUPPORT OFF CACHE BOOL "")


# Configuracion Keystone
set(ENABLE_ARM_TARGET ON CACHE BOOL "")
set(ENABLE_AARCH64_TARGET ON CACHE BOOL "")
set(ENABLE_X86_TARGET ON CACHE BOOL "")

# Desactivar los demas backends
set(ENABLE_MIPS_TARGET OFF CACHE BOOL "")
set(ENABLE_PPC_TARGET OFF CACHE BOOL "")
set(ENABLE_SPARC_TARGET OFF CACHE BOOL "")
set(ENABLE_SYSTEMZ_TARGET OFF CACHE BOOL "")
set(ENABLE_HEXAGON_TARGET OFF CACHE BOOL "")
set(ENABLE_M68K_TARGET OFF CACHE BOOL "")
set(ENABLE_TMS320C64X_TARGET OFF CACHE BOOL "")
set(ENABLE_M680X_TARGET OFF CACHE BOOL "")
set(ENABLE_EVM_TARGET OFF CACHE BOOL "")
set(ENABLE_MOS65XX_TARGET OFF CACHE BOOL "")
set(ENABLE_WASM_TARGET OFF CACHE BOOL "")
set(ENABLE_BPF_TARGET OFF CACHE BOOL "")
set(ENABLE_RISCV_TARGET OFF CACHE BOOL "")
set(ENABLE_SH_TARGET OFF CACHE BOOL "")
set(ENABLE_TRICORE_TARGET OFF CACHE BOOL "")
set(ENABLE_ALPHA_TARGET OFF CACHE BOOL "")
set(ENABLE_HPPA_TARGET OFF CACHE BOOL "")
set(ENABLE_LOONGARCH_TARGET OFF CACHE BOOL "")
set(ENABLE_XTENSA_TARGET OFF CACHE BOOL "")
set(ENABLE_ARC_TARGET OFF CACHE BOOL "")

project(VMProject
        VERSION 1.0.0
        DESCRIPTION "VestaVM: register-based VM + Vex frontend"
        LANGUAGES C CXX)

# versiones del compilador y la VM expuestas al @Target.
# Permite que usuarios escriban @Target("compiler>=1.0") y obtengan
# resolucion en compile-time sin hardcodear los numeros en C++.  Se
# bumpean al subir mayor/minor; un sprint futuro puede separar
# vx-compiler vs runtime-VM si divergen.
set(VX_COMPILER_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(VX_COMPILER_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(VX_COMPILER_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(VX_VM_VERSION_MAJOR       ${PROJECT_VERSION_MAJOR})
set(VX_VM_VERSION_MINOR       ${PROJECT_VERSION_MINOR})
set(VX_VM_VERSION_PATCH       ${PROJECT_VERSION_PATCH})

# Generar include/vx/version_generated.h con las constantes para que
# el frontend lea las versiones desde un solo punto.  Build incremental
# regenera automaticamente cuando cambia el numero en este CMakeLists.
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version_generated.h.in"
    "${CMAKE_BINARY_DIR}/generated/include/vx/version_generated.h"
    @ONLY
)
include_directories(BEFORE "${CMAKE_BINARY_DIR}/generated/include")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Codigo posicion-independiente (PIC) en TODOS los objetos.  El proyecto
# produce ademas de los ejecutables una libreria COMPARTIDA embebible
# (vesta_ffi -> libvesta.so/.dll) que enlaza las libs estaticas del proyecto y
# las vendored (vx_lib, capstone, keystone, sqlite3, ...).  En ELF, enlazar un
# objeto NO-PIC dentro de un .so falla (relocaciones R_X86_64_PC32 /
# R_X86_64_TPOFF32 "recompile with -fPIC"), p.ej. el TLS `thread_local` de
# vx_lib o simbolos de capstone.  Con PIC global el shared object enlaza en
# Linux; en x86-64 el coste de PIC es despreciable y en Windows/MinGW el flag
# es inocuo para los .a.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# GCC 16 (UCRT64) tiene un ICE en el pase de var-tracking a -O3 sobre algunas
# funciones del frontend.  var-tracking es info de DEBUG -> en Release
# (-DNDEBUG, sin -g) no aporta NADA y se puede desactivar SIN tocar el nivel
# de optimizacion: -O3 sigue intacto (calidad del codigo identica).  Solo GNU.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -fno-var-tracking")
    string(APPEND CMAKE_C_FLAGS_RELEASE " -fno-var-tracking")
endif ()


# SQLite library (statica)
add_library(sqlite3 STATIC
        libs/SourceCode/sqlite/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC
        libs/SourceCode/sqlite
)

# Preprocesador vpp (opcional — activado por defecto)
option(VESTA_BUILD_PREPROCESSOR "Compilar el preprocesador vpp e integrarlo en el ensamblador" ON)
if(VESTA_BUILD_PREPROCESSOR)
    # deshabilitar el ejecutable y los tests propios del preprocesador cuando se compila
    # como submodulo del proyecto principal, para no duplicar binarios
    set(VPP_BUILD_EXE   OFF CACHE BOOL "" FORCE)
    set(VPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(preprocessor)
endif()

# Include directories globales
set(VM_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        libs/SourceCode/json
        libs/SourceCode/cxxopts
        libs/SourceCode/capstone/include
        libs/SourceCode/keystone/include
        libs/SourceCode/DistanciaLevenshtein
)
if(VESTA_BUILD_PREPROCESSOR)
    list(APPEND VM_INCLUDES ${CMAKE_SOURCE_DIR}/preprocessor/include)
endif()

# ---------------------------------------------------------------------------
# OpenSSL centralizado en el target INTERFACE `vesta_openssl`.
# ---------------------------------------------------------------------------
# UNICO punto de verdad de la linkadura de OpenSSL: cada consumidor (vm,
# vx_lib, vmcore, vesta_lsp, vesta_ffi, vesta_rt, tests) enlaza `vesta_openssl`
# y nada mas.  Se define AQUI, antes del primer add_subdirectory que lo use.
#
# Windows: se PREFIERE el enlace ESTATICO (libssl.a + libcrypto.a del sysroot
# UCRT64/MinGW) para que el binario sea STANDALONE -- OpenSSL embebido, sin
# acompanar libssl-3-x64.dll / libcrypto-3-x64.dll.  Si no hay .a, se cae a
# FireDaemon (import libs + DLLs que el empaquetado incluye en ese caso).
add_library(vesta_openssl INTERFACE)

if (WIN32)
    set(OPENSSL_ROOT_DIR "${CMAKE_SOURCE_DIR}/libs/Windows/OpenSSL-Win64")
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD")

    get_filename_component(_cc_bin    "${CMAKE_C_COMPILER}" DIRECTORY)  # .../ucrt64/bin
    get_filename_component(_ucrt_root "${_cc_bin}"          DIRECTORY)  # .../ucrt64
    set(_ossl_a_ssl    "${_ucrt_root}/lib/libssl.a")
    set(_ossl_a_crypto "${_ucrt_root}/lib/libcrypto.a")

    if (MINGW AND EXISTS "${_ossl_a_ssl}" AND EXISTS "${_ossl_a_crypto}")
        message(STATUS "[OpenSSL] enlace ESTATICO (standalone, sin DLLs): ${_ossl_a_ssl}")
        target_include_directories(vesta_openssl INTERFACE "${_ucrt_root}/include")
        # Orden: ssl antes que crypto; dependencias Win32 de OpenSSL al final
        # (crypt32: almacen de certificados; gdi32/ws2_32: RNG y sockets).
        target_link_libraries(vesta_openssl INTERFACE
                "${_ossl_a_ssl}" "${_ossl_a_crypto}" crypt32 gdi32 ws2_32)
        set(VESTA_OPENSSL_STATIC ON  CACHE INTERNAL "OpenSSL embebido en el binario")
    else ()
        set(VESTA_OPENSSL_STATIC OFF CACHE INTERNAL "OpenSSL embebido en el binario")
        # --- Fallback: OpenSSL precompilado (FireDaemon) ---------------------
        # Las .lib/.dll NO se versionan en git (solo headers + applink.c).  En un
        # clone limpio se descargan aqui.  Idempotente + hash SHA-256 fijado.
        if (NOT EXISTS "${OPENSSL_LIB_DIR}/libssl.lib")
            set(_ossl_ver "3.6.1")
            set(_ossl_url "https://download.firedaemon.com/FireDaemon-OpenSSL/openssl-${_ossl_ver}.zip")
            set(_ossl_sha "48f53e49fda1ac9600f6356ef7a88cf9a3a1a4f69abf8e4918453f167884ddf8")
            set(_ossl_zip "${CMAKE_BINARY_DIR}/openssl-${_ossl_ver}.zip")
            set(_ossl_dir "${CMAKE_BINARY_DIR}/openssl-${_ossl_ver}-extract")
            message(STATUS "[OpenSSL] ${OPENSSL_LIB_DIR}/libssl.lib no encontrado; descargando FireDaemon OpenSSL ${_ossl_ver}...")
            if (NOT EXISTS "${_ossl_zip}")
                file(DOWNLOAD "${_ossl_url}" "${_ossl_zip}"
                        SHOW_PROGRESS
                        EXPECTED_HASH SHA256=${_ossl_sha}
                        STATUS _ossl_st)
                list(GET _ossl_st 0 _ossl_code)
                if (NOT _ossl_code EQUAL 0)
                    list(GET _ossl_st 1 _ossl_msg)
                    message(FATAL_ERROR "[OpenSSL] descarga fallo (${_ossl_code}): ${_ossl_msg}\n  URL: ${_ossl_url}\n  Alternativa: descarga el zip a mano y copia x64/lib/{libssl,libcrypto}.lib en ${OPENSSL_LIB_DIR} y x64/bin/*-x64.dll junto al ejecutable.")
                endif()
            endif()
            file(ARCHIVE_EXTRACT INPUT "${_ossl_zip}" DESTINATION "${_ossl_dir}")
            file(MAKE_DIRECTORY "${OPENSSL_LIB_DIR}")
            file(COPY "${_ossl_dir}/x64/lib/libssl.lib" "${_ossl_dir}/x64/lib/libcrypto.lib"
                    DESTINATION "${OPENSSL_LIB_DIR}")
            file(COPY "${_ossl_dir}/x64/bin/libssl-3-x64.dll" "${_ossl_dir}/x64/bin/libcrypto-3-x64.dll"
                    DESTINATION "${OPENSSL_ROOT_DIR}/bin")
            if (NOT EXISTS "${OPENSSL_INCLUDE_DIR}/openssl/ssl.h")
                file(COPY "${_ossl_dir}/x64/include/openssl" DESTINATION "${OPENSSL_INCLUDE_DIR}")
            endif()
            file(COPY "${_ossl_dir}/x64/bin/libssl-3-x64.dll" "${_ossl_dir}/x64/bin/libcrypto-3-x64.dll"
                    DESTINATION "${CMAKE_BINARY_DIR}")
            message(STATUS "[OpenSSL] instalado en ${OPENSSL_ROOT_DIR} (lib/VC/x64/MD + bin)")
        endif()
        # applink solo es necesario al mezclar OpenSSL-DLL con el CRT del .exe.
        add_library(applink STATIC
                libs/Windows/OpenSSL-Win64/include/openssl/applink.c)
        target_include_directories(vesta_openssl INTERFACE ${OPENSSL_INCLUDE_DIR})
        target_link_directories(vesta_openssl INTERFACE ${OPENSSL_LIB_DIR})
        target_link_libraries(vesta_openssl INTERFACE libssl libcrypto ws2_32 applink)
    endif ()
else ()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(vesta_openssl INTERFACE OpenSSL::SSL OpenSSL::Crypto)
    set(VESTA_OPENSSL_STATIC OFF CACHE INTERNAL "OpenSSL embebido en el binario")
endif ()

# Frontend del lenguaje Vex como libreria estatica aislada (vx_lib).
# Decision arquitectonica: el frontend del HLL se compila
# fuera del GLOB principal y se enlaza al ejecutable como dependencia
# explicita, prefigurando la futura libvesta_rt.
add_subdirectory(src/vx)

# Sources del ejecutable principal.  Se EXCLUYEN los .cpp de src/vx/
# porque ya se compilan dentro de vx_lib (incluirlos en ambos lados
# causaria definicion multiple de simbolos en el linker).
file(GLOB_RECURSE VM_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein_c.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein.cpp
)
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/vx/")
# Port transpiler IR -> codigo (C, futuro Java/JS) vive en vx_lib (ver
# src/vx/CMakeLists.txt).  Excluir para evitar definicion duplicada.
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/port/")
# Subsistema de coste (analyze::bigo) vive en vx_lib para que los renderers
# de diagramas (tambien en vx_lib) puedan anotar el coste por funcion.  Solo
# depende de ir::IrModule.  Excluir aqui para evitar definicion duplicada.
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/analyze/")
# La API C embebible (src/capi/) solo forma parte de la libreria compartida
# vesta_ffi (con VESTA_BUILD_DLL); excluirla del ejecutable `vm` y de vmcore
# para no compilar sus definiciones dllexport/dllimport fuera de contexto.
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/capi/")
# El servidor LSP (src/lsp/) es un binario aparte (vesta_lsp) con su propio
# main(); excluirlo del ejecutable `vm` y de vmcore para no duplicar main ni
# arrastrar el dispatcher LSP donde no se usa.
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/lsp/")
# El driver de compilacion reutilizable (src/toolchain/) es su propia libreria
# estatica (vesta_toolchain): pega el frontend (vx_lib) con el back-end de
# ensamblado (vmcore) sin meterlo en vmcore (que no debe depender de vx_lib).
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/toolchain/")
# main.cpp se agrega explicitamente (GLOB_RECURSE con "main.cpp" encontraria
# tambien preprocessor/src/main.cpp y causaria definicion multiple de main)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_closure.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_spimm.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_pattern.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_async.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_weak.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_sync.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_generic.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_distrib.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_exc.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_string.cpp)

# Modulo de programacion distribuida (VDP protocol)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/mailbox.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/dirty_tracker.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/node_registry.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/vdp_session.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/dist_runtime.cpp)


set(CAPSTONE_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CAPSTONE_USE_LTO OFF CACHE BOOL "" FORCE)

# Nota: las reglas install() de estos submodulos de terceros (keystone instala
# kstool.exe + keystone.pc, etc.) NO entran en el instalador de VestaVM porque el
# empaquetado (cmake/VestaPackaging.cmake) usa instalacion POR-COMPONENTE: CPack
# solo procesa los componentes de CPACK_COMPONENTS_ALL, nunca el "Unspecified"
# donde caen los terceros.

# Capstone (desensamblador)
add_subdirectory(libs/SourceCode/capstone)

# Keystone (desensamblador)
add_subdirectory(libs/SourceCode/keystone)

# LibPEparse (emisor/parser de PE/COFF/ELF, C puro generico) -- usado por el
# backend AOT (Phase AOT.4) para emitir objetos/ejecutables nativos.  Los
# ejemplos de la lib quedan OFF al incluirse como subdirectorio.
add_subdirectory(libs/SourceCode/LibPEparse)

# Shim C del emisor AOT (aot_emit_shim.c): frontera entre el AOT C++ y
# LibPEparse.  Se compila como C (donde las cabeceras de LibPEparse -- con
# uniones anonimas y macros ELF -- compilan sin friccion C/C++) y expone la ABI
# plana de include/aot/aot_emit_shim.h.  Enlaza LibPEparse de forma PUBLIC, asi
# que linkar aot_emit arrastra LibPEparse transitivamente.
# Desacople multi-arch de la emision: el shim se parte en aot/common/ (arch-
# neutral) + aot/x86_64 + aot/x86_32 + aot/x86_16 + aot/arm64.  Los .c de
# emision incluyen aot/common/aot_emit_internal.h RELATIVO a su fichero (no se
# mete src/ en el include-path).
add_library(aot_emit STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aot/common/aot_emit_common.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aot/x86_64/aot_emit_x86_64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aot/x86_32/aot_emit_x86_32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aot/x86_16/aot_emit_x86_16.c)
target_include_directories(aot_emit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aot_emit PUBLIC LibPEparse)


add_executable(vm ${VM_SOURCES})

# Fecha/hora del build + hash de git para `vesta --version`.  Un target ALL
# que corre SIEMPRE (out-of-date por diseno) regenera el header con el
# timestamp del momento; solo lo consume src/cli/version_info.cpp, asi que el
# rebuild constante afecta a esa unica TU pequena, no a todo main.cpp.
set(VESTA_BUILD_INFO_HEADER
    "${CMAKE_BINARY_DIR}/generated/include/version/build_info_generated.h")
add_custom_target(vm_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${VESTA_BUILD_INFO_HEADER}
        -DSRC_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_build_info.cmake
    BYPRODUCTS ${VESTA_BUILD_INFO_HEADER}
    COMMENT "Generando build_info_generated.h (fecha de build + git hash)"
    VERBATIM)
add_dependencies(vm vm_build_info)

# Sprint edge-bugs (2026-06-02): exec_instruction_float.cpp implementa
# semantica IEEE 754 estricta (NaN, Inf).  -ffast-math global asume no
# NaN/Inf, lo que folda isnan(x) a false y rompe FCMP con NaN.
# Override por-archivo: -fno-fast-math + -fno-finite-math-only.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_float.cpp
    PROPERTIES COMPILE_FLAGS "-fno-fast-math -fno-finite-math-only"
)

target_include_directories(vm
        PRIVATE ${VM_INCLUDES}
)
target_link_libraries(vm
        PRIVATE sqlite3
        PRIVATE capstone
        PRIVATE keystone
        PRIVATE vx_lib
        PRIVATE vesta_toolchain
        PRIVATE aot_emit
)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(vm PRIVATE vpp_lib)
    target_compile_definitions(vm PRIVATE VESTA_HAS_PREPROCESSOR=1)

    # copiar include_lib junto al ejecutable para que #import <vesta/...> funcione
    # sin necesidad de instalar ni configurar rutas manualmente
    add_custom_command(TARGET vm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/preprocessor/include_lib"
            "$<TARGET_FILE_DIR:vm>/include_lib"
        COMMENT "Copiando stdlib vpp (include_lib) al directorio de salida"
    )
endif()

# OpenSSL: vm enlaza el target INTERFACE `vesta_openssl` (definido arriba, antes
# de add_subdirectory(src/vx)).  Estatico (standalone) o FireDaemon segun el
# entorno; un unico punto de verdad para todos los consumidores.
if (WIN32)
    target_link_libraries(vm PRIVATE
            vesta_openssl
            ws2_32           # sockets en Windows
            synchronization  # WaitOnAddress / WakeByAddress* (ThreadPool)
    )
else ()
    target_link_libraries(vm PRIVATE vesta_openssl dl)
endif ()

# Opciones globales para todos los builds
target_compile_options(vm PRIVATE
        -D__STDC_LIMIT_MACROS
        -D__STDC_CONSTANT_MACROS
        -pedantic-errors
)

# LTO (Link Time Optimization)
#set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

# global
if (WIN32)
target_link_options(vm PRIVATE
        -static
        # Stack reserve 128 MB para el thread principal (donde corre el
        # scheduler single-thread bypass + JIT que aloca buffers grandes
        # via host_alloca, arrays nativos `u8[N]` con N grande, etc).
        # Default Win64 era 1 MB que limita programas con recursion profunda
        # o arrays nativos grandes en stack.  La memoria es VirtualAlloc
        # RESERVE (no commit) -- solo paginas tocadas consumen memoria real.
        -Wl,--stack,134217728   # 128 MB reserve (commit on touch)
)
endif()

# release
target_link_options(vm PRIVATE
        $<$<CONFIG:Release>:

        # --- Eliminación de código muerto ---
        -Wl,--gc-sections          # Elimina funciones/datos no usados

        # --- Reducción de tamaño ---
        -Wl,--strip-all            # Elimina todos los símbolos
        -Wl,--as-needed            # Solo enlaza libs realmente usadas
        -Wl,--build-id=none        # No genera build-id (reduce tamaño)

        # --- Flags NO soportados por tu MinGW (comentados) ---
        #-Wl,--icf=all             # Identical Code Folding (solo gold/lld)
        #-Wl,--compress-debug-sections=zlib   # Solo binutils modernos
        #-flto=thin                # mi MinGW no soporta LTO
        >
)

# Enlace estático solo en Windows + Release
target_link_options(vm PRIVATE
        $<$<AND:$<CONFIG:Release>,$<BOOL:${WIN32}>>:
        # --- Enlace estático ---
        -static                    # Binario autónomo
        -static-libgcc             # Libgcc estática
        -static-libstdc++          # Libstdc++ estática
        >
)

# Opciones por configuracion
target_compile_options(vm PRIVATE
        $<$<CONFIG:Debug>:
        -g -O0 -Wall -Wextra -fno-omit-frame-pointer -DMODE_DEBUG_ENABLE
        >
        $<$<CONFIG:Release>:
        -O3
        -march=x86-64
        -mtune=native
        -ffast-math
        -fstrict-aliasing
        -fno-plt
        -fomit-frame-pointer
        -fvisibility=hidden
        -ffunction-sections
        -fdata-sections
        -DNDEBUG
        >
        $<$<CONFIG:RelWithDebInfo>:-fpermissive -g -O1 -fno-omit-frame-pointer>
)


add_library(vmcore STATIC ${VM_SOURCES})
target_include_directories(vmcore PRIVATE ${VM_INCLUDES})
target_link_libraries(vmcore
        PRIVATE sqlite3
        PRIVATE capstone
        PRIVATE keystone
        PRIVATE aot_emit
)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(vmcore PRIVATE vpp_lib)
    target_compile_definitions(vmcore PRIVATE VESTA_HAS_PREPROCESSOR=1)
endif()

if (WIN32)
    target_link_libraries(vmcore PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(vmcore PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif ()

# ---------------------------------------------------------------------------
# vesta_toolchain: driver de compilacion reutilizable (.vx -> .velb).
#
# Pega el frontend (vx_lib: compile_vx_source/_project) con el back-end de
# ensamblado/linkado (vmcore: run_worker) en una funcion unica que comparten el
# ejecutable `vm`, el servidor `vesta_lsp` (metodos vesta/compile*) y, via el
# LSP, la libreria cliente Python.  Es una libreria aparte porque vx_lib es
# standalone (no puede depender de vmcore) y vmcore no debe depender de vx_lib.
# Como libreria estatica solo necesita los headers para compilar; los simbolos
# de vx_lib/vmcore se resuelven al enlazar el ejecutable final (que ya enlaza
# ambos).
# ---------------------------------------------------------------------------
add_library(vesta_toolchain STATIC
        src/toolchain/toolchain.cpp
        src/toolchain/aot_build.cpp
        src/toolchain/native_backend.cpp)
target_include_directories(vesta_toolchain PRIVATE ${VM_INCLUDES})
target_compile_features(vesta_toolchain PUBLIC cxx_std_17)

# ---------------------------------------------------------------------------
# vesta_lsp: servidor Language Server Protocol para Vex (Fase 1).
#
# Binario independiente que habla LSP por stdio (JSON-RPC enmarcado por
# Content-Length) y enlaza el core del compilador EN PROCESO para producir
# diagnosticos en vivo.  Reutiliza:
#   - vx_lib  -> compile_vx_source / compile_vx_project + analyze::bigo.
#   - vmcore   -> ir:: (parse_ir_module_cache) + runtime que el frontend
#                 arrastra transitivamente.
#
# El motor de analisis (src/lsp/analysis_engine.cpp) es el punto unico por el
# que pasaran las futuras fases (semantic tokens, hover con Big-O, ver
# IR/bytecode/JIT/AOT, diagramas), reutilizando el CompileResult cacheado.
# ---------------------------------------------------------------------------
add_executable(vesta_lsp
        src/lsp/json_rpc.cpp
        src/lsp/document_store.cpp
        src/lsp/analysis_engine.cpp
        src/lsp/semantic_tokens.cpp
        src/lsp/param_hints.cpp
        src/lsp/symbol_index.cpp
        src/lsp/inspector.cpp
        src/lsp/builtin_docs.cpp
        src/lsp/lsp_server.cpp
        src/lsp/lsp_main.cpp
)
target_include_directories(vesta_lsp PRIVATE ${VM_INCLUDES})
target_link_libraries(vesta_lsp
        PRIVATE vx_lib
        PRIVATE vesta_toolchain
        PRIVATE vmcore
        PRIVATE sqlite3
        PRIVATE capstone
        PRIVATE keystone
        PRIVATE aot_emit
)
if (VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(vesta_lsp PRIVATE vpp_lib)
    target_compile_definitions(vesta_lsp PRIVATE VESTA_HAS_PREPROCESSOR=1)
endif ()
if (WIN32)
    target_link_libraries(vesta_lsp PRIVATE
            vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(vesta_lsp PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Reduccion de tamano del LSP (espejo de `vm`).  Sin esto el .exe pesaba
# ~327MB: el 91% era DWARF de depuracion sin estripar (las libs estaticas
# vendored la emiten por defecto aunque no haya -g).  Con --strip-all +
# --gc-sections baja a ~26MB (-92%).  No afecta -O3.
target_link_options(vesta_lsp PRIVATE
        $<$<CONFIG:Release>:
        -Wl,--gc-sections
        -Wl,--strip-all
        -Wl,--as-needed
        -Wl,--build-id=none
        >
        $<$<AND:$<CONFIG:Release>,$<BOOL:${WIN32}>>:
        -static -static-libgcc -static-libstdc++
        >
)

# Test unitario de la conversion de posiciones byte->UTF-16.  Solo necesita
# document_store.cpp (autocontenido, sin deps del compilador).
add_executable(test_lsp_position_conv
        tests/lsp/test_position_conv.cpp
        src/lsp/document_store.cpp
)
target_include_directories(test_lsp_position_conv PRIVATE ${VM_INCLUDES})

# Inc 0 de libvesta_gc (GC opt-in de AOT): valida la C-ABI del GcHeap global
# sin ProcessVM.  Linka vmcore (el motor GC vive en src/vesta_gc/, globbeado).
# libvesta_gc: el motor GC (mismo que interp/JIT) empaquetado STANDALONE como
# libreria estatica enlazable en ejecutables AOT que usan gc<T>.  Set minimo:
# gc_lib (C-ABI) + gc_heap + arena + jit_registry/stack_scan (frames precisos).
# Inc 0b dejo gc_heap.cpp sin dependencia de ProcessVM -> esto compila sin la VM.
add_library(vesta_gc STATIC
        src/vesta_gc/gc_lib.cpp
        src/vesta_gc/freestanding_shim.cpp
        src/gc/gc_heap.cpp
        src/gc/wait_table.cpp
        src/gc/shared_handle_table.cpp
        src/arena/arena_manager.cpp
        src/arena/VirtualMemory.cpp
        src/arena/TLB.cpp
        src/jit/jit_registry.cpp
        src/jit/stack_scan.cpp
)
target_include_directories(vesta_gc PUBLIC ${VM_INCLUDES})
# Freestanding-ish: reducir las dependencias de libstdc++ para que el .a se
# enlace SIN g++ (solo libc, que el AOT ya resuelve via IAT/PLT).  -fno-exceptions
# /-rtti/-threadsafe-statics quitan _Unwind/__cxa_*/guards; el define apaga el
# logging GC (std::ostringstream/cerr).  Lo que quede de libstdc++ (operator
# new/delete, unordered_map) se cubre con el shim libvesta_gc_shim.
target_compile_options(vesta_gc PRIVATE
        -fno-exceptions -fno-rtti -fno-threadsafe-statics
        # -g1: simbolos de depuracion minimos (linea + funcion) sobre el -O3
        # heredado de Release, por si hay que depurar el GC en un binario AOT.
        -g1)
target_compile_definitions(vesta_gc PRIVATE
        VESTA_GC_DEBUG_DISABLE=1 VESTA_GC_FREESTANDING=1)

# ---------------------------------------------------------------------------
# Variantes ESTATICAS de los plugins de stdlib (colecciones / math) para AOT
# STANDALONE.  Mismo .c que la DLL, pero VESTA_PLUGIN_EXPORT vacio (sin
# dllexport): el AOT enlaza estos .a (libvesta_collections.a / libvesta_math.a)
# en vez de importar la DLL cuando el programa usa colecciones/math, produciendo
# un .exe sin DLLs.  Solo se enlazan si el programa las usa (auto-link en main).
# -ffunction-sections + -fdata-sections para que el linker descarte lo no usado.
add_library(vesta_collections_a STATIC
        stdlib/native/collections/vesta_collections.c)
set_target_properties(vesta_collections_a PROPERTIES
        OUTPUT_NAME vesta_collections C_STANDARD 11)
target_include_directories(vesta_collections_a PRIVATE
        ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(vesta_collections_a PRIVATE "VESTA_PLUGIN_EXPORT=")
# -O3 (heredado de Release) para codigo optimo + -g1 para conservar simbolos de
# depuracion MINIMOS (tablas de linea + nombres de funcion): suficiente para
# backtraces / addr2line / gdb, sin el peso de -g2/-g3 (variables locales, macros).
target_compile_options(vesta_collections_a PRIVATE
        -ffunction-sections -fdata-sections -g1)

add_library(vesta_math_a STATIC stdlib/native/math/vesta_math.c)
set_target_properties(vesta_math_a PROPERTIES
        OUTPUT_NAME vesta_math C_STANDARD 11)
target_include_directories(vesta_math_a PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(vesta_math_a PRIVATE "VESTA_PLUGIN_EXPORT=")
target_compile_options(vesta_math_a PRIVATE
        -ffunction-sections -fdata-sections -g1)

add_executable(test_gc_lib tests/gc/test_gc_lib.cpp)
target_include_directories(test_gc_lib PRIVATE ${VM_INCLUDES})
# vmcore es STATIC y sus deps son PRIVATE -> replicar el set de enlace.
target_link_libraries(test_gc_lib PRIVATE vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_gc_lib PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_gc_lib PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_gc_lib PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Driver incremental granular (claves Merkle + CAS + plan_rebuild + fragmentos
# de IR).  vx_lib aporta el indice + el driver; vmcore aporta ir:: (IrModule,
# serializacion).  vx_lib va PRIMERO: referencia simbolos ir:: de vmcore.
add_executable(test_vx_incremental tests/vx/test_incremental.cpp)
target_include_directories(test_vx_incremental PRIVATE ${VM_INCLUDES})
target_link_libraries(test_vx_incremental PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_vx_incremental PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_vx_incremental PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_vx_incremental PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Huella computacional por-funcion (recursos + efectos + composicion).
add_executable(test_analyze_fingerprint tests/analyze/test_fingerprint.cpp)
target_include_directories(test_analyze_fingerprint PRIVATE ${VM_INCLUDES})
target_link_libraries(test_analyze_fingerprint PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_analyze_fingerprint PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_analyze_fingerprint PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_analyze_fingerprint PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Infra de analisis: AnalysisManager (type-erasure + dependencias + invalidacion).
add_executable(test_analysis_manager tests/analysis/test_manager.cpp)
target_include_directories(test_analysis_manager PRIVATE ${VM_INCLUDES})

# Sistema unico de efectos: reticulo AbstractLoc, combinadores seq/join,
# contratos declarativos.
add_executable(test_effects tests/effects/test_effects.cpp)
target_include_directories(test_effects PRIVATE ${VM_INCLUDES})
target_link_libraries(test_effects PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_effects PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_effects PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_effects PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Modelo de memoria UNICO: alias por rangos (may/must/no) + resolvedor
# points-to compartido (raiz + offset const).
add_executable(test_mem_loc tests/analysis/test_mem_loc.cpp)
target_include_directories(test_mem_loc PRIVATE ${VM_INCLUDES})
target_link_libraries(test_mem_loc PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_mem_loc PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_mem_loc PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_mem_loc PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# DB de instrucciones embebida: matcher texto->FormID.
add_executable(test_instr_db tests/vx/instr_db_test.cpp)
target_include_directories(test_instr_db PRIVATE ${VM_INCLUDES})
target_link_libraries(test_instr_db PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_instr_db PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_instr_db PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_instr_db PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# CFG del asm inline (bloques basicos + aristas).
add_executable(test_asm_cfg tests/vx/asm_cfg_test.cpp)
target_include_directories(test_asm_cfg PRIVATE ${VM_INCLUDES})
target_link_libraries(test_asm_cfg PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_asm_cfg PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_asm_cfg PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_asm_cfg PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Diagnosticos estructurales del asm sobre el CFG.
add_executable(test_asm_diag tests/vx/asm_diag_test.cpp)
target_include_directories(test_asm_diag PRIVATE ${VM_INCLUDES})
target_link_libraries(test_asm_diag PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_asm_diag PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_asm_diag PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_asm_diag PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Diagramas del CFG del asm anotados con coste.
add_executable(test_asm_diagram tests/vx/asm_diagram_test.cpp)
target_include_directories(test_asm_diagram PRIVATE ${VM_INCLUDES})
target_link_libraries(test_asm_diagram PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_asm_diagram PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_asm_diagram PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_asm_diagram PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Lift de patrones de asm a IR tipado (atomicos).
add_executable(test_asm_lift tests/vx/asm_lift_test.cpp)
target_include_directories(test_asm_lift PRIVATE ${VM_INCLUDES})
target_link_libraries(test_asm_lift PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_asm_lift PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_asm_lift PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_asm_lift PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Catalogo multi-idioma de diagnosticos.
add_executable(test_diag_catalog tests/vx/diag_catalog_test.cpp)
target_include_directories(test_diag_catalog PRIVATE ${VM_INCLUDES})
target_link_libraries(test_diag_catalog PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_diag_catalog PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_diag_catalog PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_diag_catalog PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Selector IR -> AArch64 (backend arm64, Phase H.2).
add_executable(test_arm64_select tests/jit/test_arm64_select.cpp)
target_include_directories(test_arm64_select PRIVATE ${VM_INCLUDES})
target_link_libraries(test_arm64_select PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_arm64_select PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_arm64_select PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_arm64_select PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Banco de registros fisico unico (allocator de banco ancho, Fase 0):
# consolidacion de TargetRegInfo + BackendCaps en un modelo comun (Lane/View/
# SavePolicy/AliasSet/Reserve).  Test de round-trip contra los targets.
add_executable(test_rbank_physical tests/jit/test_rbank_physical.cpp)
target_include_directories(test_rbank_physical PRIVATE ${VM_INCLUDES})
target_link_libraries(test_rbank_physical PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_rbank_physical PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_rbank_physical PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_rbank_physical PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# ValueRequirements (banco ancho, Fase 0 nivel 1): que necesita un valor +
# satisfacibilidad contra el banco fisico (motivos como DATO i18n-ready).
add_executable(test_rbank_value_req tests/jit/test_rbank_value_req.cpp)
target_include_directories(test_rbank_value_req PRIVATE ${VM_INCLUDES})
target_link_libraries(test_rbank_value_req PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_rbank_value_req PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_rbank_value_req PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_rbank_value_req PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Constraints (banco ancho, Fase 0 nivel 2): feasibilidad DURA + verificador de
# soluciones (validate_assignment), con violaciones como DATO i18n-ready.
add_executable(test_rbank_constraints tests/jit/test_rbank_constraints.cpp)
target_include_directories(test_rbank_constraints PRIVATE ${VM_INCLUDES})
target_link_libraries(test_rbank_constraints PRIVATE vx_lib vmcore sqlite3 capstone keystone aot_emit)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(test_rbank_constraints PRIVATE vpp_lib)
endif()
if (WIN32)
    target_link_libraries(test_rbank_constraints PRIVATE vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(test_rbank_constraints PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# ---------------------------------------------------------------------------
# libvesta: DLL/.so con C-ABI estable para embeber VestaVM en cualquier
# lenguaje (Python via ctypes, Rust FFI, C#, C, ...).  Expone
# vesta_compile / vesta_run / vesta_eval / vesta_version / vesta_free.
#
# Se construye a partir del MISMO conjunto de fuentes del ejecutable `vm`
# (VM_SOURCES) PERO sin main.cpp (un main() en una libreria compartida no
# tiene sentido y colisiona con el CRT) + el wrapper C de la API.  Enlaza
# exactamente las mismas dependencias que el target `vm`.
# ---------------------------------------------------------------------------
option(VESTA_BUILD_FFI "Construir libvesta (DLL/.so con C-ABI)" ON)
if (VESTA_BUILD_FFI)
    # Lista de fuentes = VM_SOURCES menos main.cpp (no queremos un entry
    # point en la libreria compartida).
    set(VESTA_FFI_SOURCES ${VM_SOURCES})
    list(REMOVE_ITEM VESTA_FFI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

    add_library(vesta_ffi SHARED
            ${VESTA_FFI_SOURCES}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/capi/vesta_capi.cpp
    )

    # OUTPUT_NAME "vesta" -> vesta.dll (Windows) / libvesta.so (POSIX).
    set_target_properties(vesta_ffi PROPERTIES OUTPUT_NAME "vesta")

    # VESTA_BUILD_DLL activa __declspec(dllexport) en la cabecera publica.
    target_compile_definitions(vesta_ffi PRIVATE VESTA_BUILD_DLL)

    # exec_instruction_float.cpp tambien aqui necesita IEEE estricto (igual
    # que en el target `vm`).
    set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_float.cpp
            PROPERTIES COMPILE_FLAGS "-fno-fast-math -fno-finite-math-only"
    )

    target_include_directories(vesta_ffi PRIVATE ${VM_INCLUDES})

    target_link_libraries(vesta_ffi
            PRIVATE sqlite3
            PRIVATE capstone
            PRIVATE keystone
            PRIVATE vx_lib
            PRIVATE aot_emit
    )
    if (VESTA_BUILD_PREPROCESSOR)
        target_link_libraries(vesta_ffi PRIVATE vpp_lib)
        target_compile_definitions(vesta_ffi PRIVATE VESTA_HAS_PREPROCESSOR=1)
    endif ()

    if (WIN32)
        target_link_libraries(vesta_ffi PRIVATE
                vesta_openssl ws2_32 synchronization)
        # DLL autocontenido: enlazar TODO estaticamente (libgcc, libstdc++ y
        # libwinpthread via -static, mas OpenSSL via vesta_openssl) para que el
        # consumidor de vesta.dll no necesite NINGUN runtime DLL adicional de
        # MinGW ni de OpenSSL.  Sin -static (solo -static-libgcc/-libstdc++),
        # libwinpthread-1.dll quedaba como dependencia dinamica -> el consumidor
        # fallaba con "no se encuentra el punto de entrada clock_gettime64" en un
        # sistema con un libwinpthread distinto.
        target_link_options(vesta_ffi PRIVATE
                -static -static-libgcc -static-libstdc++)
    else ()
        target_link_libraries(vesta_ffi PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
    endif ()

    # Auto-build: construir el ejecutable `vm` arrastra la construccion de
    # libvesta.  Asi `cmake --build . --target vm` produce SIEMPRE el DLL/.so
    # junto al .exe sin que el usuario tenga que pedir el target explicito.
    add_dependencies(vm vesta_ffi)
endif ()

# ---------------------------------------------------------------------------
# Mejora III: libvesta_rt como libreria independiente.
#
# Subset estricto de @c vmcore que contiene SOLO los componentes del runtime
# puro (GC, scheduler, mailbox, futures, monitores, FFI, class_registry,
# arena, distribucion VDP).  No incluye:
#   - frontend del lenguaje (vx, ir, lexer, parser, emmit)
#   - linker / loader principal del .velb (solo class_registry)
#   - CLI / VSH / REPL
#   - jit, optimizer, profiler, install
#
# Objetivo: el codigo JIT-eado o AOT-compilado en fases futuras 
# podra enlazar contra vesta_rt sin arrastrar el frontend.  Mismo binary
# layout y misma calling convention que las funciones de runtime que el
# bytecode invoca via CALLN.
#
# Estado: MVP -- la libreria compila independientemente y expone los
# simbolos del runtime.  El target @c vm sigue compilando monoliticamente
# todo (sin regresion).  Una integracion mas profunda (vm enlazado contra
# vesta_rt + romper circular loader.h <-> runtime.h) queda para Phase C
# del roadmap, donde se desacople tambien el loader.
# ---------------------------------------------------------------------------
set(VESTA_RT_SOURCES
        # GC - heap generacional + raw allocator
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gc/gc_heap.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gc/raw_allocator.cpp

        # Arena manager - VirtualMemory + TLB + ArenaManager
        ${CMAKE_CURRENT_SOURCE_DIR}/src/arena/VirtualMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/arena/TLB.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/arena/arena_manager.cpp

        # Runtime core - ProcessVM + Scheduler + ManageVM + decoder/exec
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/proceso_runtime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/runtime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/manager_runtime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/decode_instruction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/decode_table.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exception_runtime.cpp

        # Runtime - exec instructions (todas las familias del set)
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_alu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_gc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_oop.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_closure.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_pattern.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_async.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_spimm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_weak.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_sync.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_generic.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_distrib.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_exc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_string.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_meta.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_coro.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_float.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/host_alloca_tracker.cpp
        # Sprint D.6 (2026-06-03): profile counters runtime para PGO + C2.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/profile.cpp
        # Sprint edge-bugs (2026-06-02): exec_instruction_float.cpp implementa
        # operaciones IEEE 754 (FCMP con NaN, etc.) y NO puede usar
        # -ffast-math (que rompe isnan/isinf + asume no NaN).  Override
        # con flags por-archivo: -fno-fast-math + -fno-finite-math-only.
        # set_source_files_properties debe ir DESPUES de listar las sources.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/native_callback.cpp

        # Distribucion - VDP protocol + mailbox + dirty tracker
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/mailbox.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/dirty_tracker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/node_registry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/vdp_session.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/dist_runtime.cpp

        # FFI - puente plugins nativos
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ffi/native_ffi.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ffi/virtual_lib_registry.cpp

        # Class registry (POO dinamica + reflexion + AOP)
        ${CMAKE_CURRENT_SOURCE_DIR}/src/loader/class_registry.cpp

        # Net (TCP server / TLS) - usado por dist_runtime para VDP
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net/connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net/net.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net/tcp_server.cpp

        # Util compartido necesario por runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/src/util/ThreadPool.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/util/sqlite_singleton.cpp

        # Controller TLS (usado por VDP)
        ${CMAKE_CURRENT_SOURCE_DIR}/src/controller/tls_connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/controller/tls_context.cpp

        # Phase C/D.0 (snapshot 2026-05-13): C ABI estable + ABI checks +
        # infraestructura JIT inicial (code cache, runtime entries, bridge).
        # vesta_rt incluye estos archivos para que el binario JIT-eado
        # pueda enlazarse contra la libreria sin arrastrar el frontend.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vesta_rt/public_wrapper.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vesta_rt/abi_checks.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/code_cache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/runtime_entries.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/x86_encoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/jit_branch_prof.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/jit_registry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/stack_scan.cpp

        # Opcion W Fase 3b: serializer IR.  Necesario para que el Loader
        # (en vesta_rt) deserialize la seccion @ir del .velb v3.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ir/ssa_ir.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ir/ssa_ir_serialize.cpp
)

add_library(vesta_rt STATIC ${VESTA_RT_SOURCES})
target_include_directories(vesta_rt PUBLIC ${VM_INCLUDES})
target_link_libraries(vesta_rt
        PUBLIC sqlite3
)
if (WIN32)
    target_link_libraries(vesta_rt PUBLIC vesta_openssl ws2_32 synchronization)
else ()
    target_link_libraries(vesta_rt PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif ()
target_compile_options(vesta_rt PRIVATE
        -D__STDC_LIMIT_MACROS
        -D__STDC_CONSTANT_MACROS
        $<$<CONFIG:Release>:-O3 -march=x86-64 -mtune=native -ffast-math
                            -fstrict-aliasing -fno-plt -fomit-frame-pointer
                            -fvisibility=hidden -ffunction-sections
                            -fdata-sections -DNDEBUG>
)

# Anadir subdirectorio de tests
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Configuracion global del SDK de plugins nativos
# Debe quedar fuera de los bloques opcionales para que VESTA_SDK_DIR y
# CMAKE_MODULE_PATH esten disponibles en stdlib, examples y cualquier
# subdirectorio futuro que use add_vesta_plugin().
# ---------------------------------------------------------------------------
set(VESTA_SDK_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Raiz del SDK VestaVM (directorio con include/ffi/vesta_plugin.h)")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Libreria estandar nativa (vesta_io, vesta_math, ...)
option(VESTA_BUILD_STDLIB   "Compilar la libreria estandar nativa de Vesta" ON)
if(VESTA_BUILD_STDLIB)
    add_subdirectory(stdlib/native)
endif()

# Ejemplos y plugins de demostracion
option(VESTA_BUILD_EXAMPLES "Compilar los ejemplos y plugins de ejemplo"    ON)
if(VESTA_BUILD_EXAMPLES)
    add_subdirectory(examples_codes_vm/plugin_ejemplo)
endif()

# ---------------------------------------------------------------------------
# Empaquetado (CPack): reglas install() + generadores NSIS/.exe, WiX/.msi, ZIP.
# Genera el instalador con:  cpack --config <build>/CPackConfig.cmake -G NSIS
# ---------------------------------------------------------------------------
include(VestaPackaging)