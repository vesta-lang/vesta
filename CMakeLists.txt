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

# no necesitas los ejecutables de fuzzing,
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

project(VMProject)

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

# Sources del ejecutable principal
file(GLOB_RECURSE VM_SOURCES
        main.cpp
        src/*.cpp
        libs/SourceCode/DistanciaLevenshtein/Levenshtein_c.c
        libs/SourceCode/DistanciaLevenshtein/Levenshtein.cpp
)


set(CAPSTONE_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CAPSTONE_USE_LTO OFF CACHE BOOL "" FORCE)

# Capstone (desensamblador)
add_subdirectory(libs/SourceCode/capstone)

# Keystone (desensamblador)
add_subdirectory(libs/SourceCode/keystone)


add_executable(vm ${VM_SOURCES})
target_include_directories(vm
        PRIVATE ${VM_INCLUDES}
)
target_link_libraries(vm
        PRIVATE sqlite3
        PRIVATE capstone
        PRIVATE keystone
)

# OpenSSL
if (WIN32)
    set(OPENSSL_ROOT_DIR "${CMAKE_SOURCE_DIR}/libs/Windows/OpenSSL-Win64")
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD")

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
    target_link_libraries(vm PRIVATE OpenSSL::SSL OpenSSL::Crypto)
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

# Añadir subdirectorio de tests
add_subdirectory(tests)