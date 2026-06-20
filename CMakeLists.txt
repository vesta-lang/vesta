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
# vex-compiler vs runtime-VM si divergen.
set(VEX_COMPILER_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(VEX_COMPILER_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(VEX_COMPILER_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(VEX_VM_VERSION_MAJOR       ${PROJECT_VERSION_MAJOR})
set(VEX_VM_VERSION_MINOR       ${PROJECT_VERSION_MINOR})
set(VEX_VM_VERSION_PATCH       ${PROJECT_VERSION_PATCH})

# Generar include/vex/version_generated.h con las constantes para que
# el frontend lea las versiones desde un solo punto.  Build incremental
# regenera automaticamente cuando cambia el numero en este CMakeLists.
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version_generated.h.in"
    "${CMAKE_BINARY_DIR}/generated/include/vex/version_generated.h"
    @ONLY
)
include_directories(BEFORE "${CMAKE_BINARY_DIR}/generated/include")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
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
        libs/SourceCode/ftxui/include
        libs/SourceCode/capstone/include
        libs/SourceCode/keystone/include
        libs/SourceCode/DistanciaLevenshtein
)
if(VESTA_BUILD_PREPROCESSOR)
    list(APPEND VM_INCLUDES ${CMAKE_SOURCE_DIR}/preprocessor/include)
endif()

# Frontend del lenguaje Vex como libreria estatica aislada (vex_lib).
# Decision arquitectonica: el frontend del HLL se compila
# fuera del GLOB principal y se enlaza al ejecutable como dependencia
# explicita, prefigurando la futura libvesta_rt.
add_subdirectory(src/vex)

# Sources del ejecutable principal.  Se EXCLUYEN los .cpp de src/vex/
# porque ya se compilan dentro de vex_lib (incluirlos en ambos lados
# causaria definicion multiple de simbolos en el linker).
file(GLOB_RECURSE VM_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein_c.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein.cpp
)
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/vex/")
# Port transpiler IR -> codigo (C, futuro Java/JS) vive en vex_lib (ver
# src/vex/CMakeLists.txt).  Excluir para evitar definicion duplicada.
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/port/")
# Subsistema de coste (analyze::bigo) vive en vex_lib para que los renderers
# de diagramas (tambien en vex_lib) puedan anotar el coste por funcion.  Solo
# depende de ir::IrModule.  Excluir aqui para evitar definicion duplicada.
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/analyze/")
# La API C embebible (src/capi/) solo forma parte de la libreria compartida
# vesta_ffi (con VESTA_BUILD_DLL); excluirla del ejecutable `vm` y de vmcore
# para no compilar sus definiciones dllexport/dllimport fuera de contexto.
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/capi/")
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
add_library(aot_emit STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/aot/aot_emit_shim.c)
target_include_directories(aot_emit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aot_emit PUBLIC LibPEparse)


add_executable(vm ${VM_SOURCES})

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
        PRIVATE vex_lib
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

# OpenSSL
if (WIN32)
    set(OPENSSL_ROOT_DIR "${CMAKE_SOURCE_DIR}/libs/Windows/OpenSSL-Win64")
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD")

    # --- Auto-descarga de OpenSSL precompilado (FireDaemon) -----------------
    # Las .lib/.dll de OpenSSL son binarios precompilados que NO se versionan
    # en git (solo los headers + applink.c estan trackeados).  En un clone
    # limpio -- o si los binarios se pierden -- se descargan aqui y se colocan
    # en el layout que el resto del CMake espera (lib/VC/x64/MD + bin).
    # Idempotente: si libssl.lib ya existe, no hace nada.  Reproducible: la
    # version y el hash SHA-256 del zip estan fijados.
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
        # Layout FireDaemon ${_ossl_ver}: x64/{lib,bin,include}.  El proyecto
        # espera las .lib en lib/VC/x64/MD (layout historico reorganizado).
        file(MAKE_DIRECTORY "${OPENSSL_LIB_DIR}")
        file(COPY "${_ossl_dir}/x64/lib/libssl.lib" "${_ossl_dir}/x64/lib/libcrypto.lib"
                DESTINATION "${OPENSSL_LIB_DIR}")
        file(COPY "${_ossl_dir}/x64/bin/libssl-3-x64.dll" "${_ossl_dir}/x64/bin/libcrypto-3-x64.dll"
                DESTINATION "${OPENSSL_ROOT_DIR}/bin")
        # Headers: ya estan versionados en git; si faltaran, restaurarlos.
        if (NOT EXISTS "${OPENSSL_INCLUDE_DIR}/openssl/ssl.h")
            file(COPY "${_ossl_dir}/x64/include/openssl" DESTINATION "${OPENSSL_INCLUDE_DIR}")
        endif()
        # DLLs junto al ejecutable (runtime: vm.exe las carga al arrancar).
        file(COPY "${_ossl_dir}/x64/bin/libssl-3-x64.dll" "${_ossl_dir}/x64/bin/libcrypto-3-x64.dll"
                DESTINATION "${CMAKE_BINARY_DIR}")
        message(STATUS "[OpenSSL] instalado en ${OPENSSL_ROOT_DIR} (lib/VC/x64/MD + bin)")
    endif()

    target_include_directories(vm PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_directories(vm PRIVATE ${OPENSSL_LIB_DIR})

    target_link_libraries(vm PRIVATE
            libssl
            libcrypto
            ws2_32           # sockets en Windows
            synchronization  # WaitOnAddress / WakeByAddress* (ThreadPool)
    )

    # es necesario en windows
    add_library(applink STATIC
            libs/Windows/OpenSSL-Win64/include/openssl/applink.c
    )

    target_link_libraries(vm PRIVATE applink)
else ()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(vm PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
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

add_library(vesta_install STATIC
        src/install/install.cpp
        src/install/install_options.cpp
        src/install/manifest.cpp
)

if(WIN32)
    target_sources(vesta_install PRIVATE src/install/platform_win.cpp)
    target_link_libraries(vesta_install PRIVATE Shell32 Advapi32 Ole32 User32 Shlwapi)
else()
    target_sources(vesta_install PRIVATE src/install/platform_linux.cpp)
endif()

target_include_directories(vesta_install PUBLIC include)

# Linkar al binario principal:
target_link_libraries(vm PRIVATE vesta_install)

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
    target_include_directories(vmcore PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_directories(vmcore PRIVATE ${OPENSSL_LIB_DIR})
    target_link_libraries(vmcore PRIVATE libssl libcrypto ws2_32 synchronization applink)
else ()
    target_link_libraries(vmcore PRIVATE OpenSSL::SSL OpenSSL::Crypto)
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
            PRIVATE vex_lib
            PRIVATE aot_emit
            PRIVATE vesta_install
    )
    if (VESTA_BUILD_PREPROCESSOR)
        target_link_libraries(vesta_ffi PRIVATE vpp_lib)
        target_compile_definitions(vesta_ffi PRIVATE VESTA_HAS_PREPROCESSOR=1)
    endif ()

    if (WIN32)
        target_include_directories(vesta_ffi PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_link_directories(vesta_ffi PRIVATE ${OPENSSL_LIB_DIR})
        target_link_libraries(vesta_ffi PRIVATE
                libssl libcrypto ws2_32 synchronization applink)
        # DLL autocontenido: enlazar estaticamente libgcc/libstdc++ para que
        # el consumidor no necesite los runtime DLLs de MinGW.  OpenSSL sigue
        # siendo DLL (libssl-3-x64.dll / libcrypto-3-x64.dll deben acompanar).
        target_link_options(vesta_ffi PRIVATE
                -static-libgcc -static-libstdc++)
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
#   - frontend del lenguaje (vex, ir, lexer, parser, emmit)
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
    target_include_directories(vesta_rt PUBLIC ${OPENSSL_INCLUDE_DIR})
    target_link_directories(vesta_rt PUBLIC ${OPENSSL_LIB_DIR})
    target_link_libraries(vesta_rt PUBLIC libssl libcrypto ws2_32 synchronization applink)
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