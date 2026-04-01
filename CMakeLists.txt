cmake_minimum_required(VERSION 3.20)

# Evitar warning CMP0063 de librerias externas
if(POLICY CMP0063)
    cmake_policy(SET CMP0063 NEW)
endif()

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")

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

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()


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
if(WIN32)
    set(OPENSSL_ROOT_DIR "${CMAKE_SOURCE_DIR}/libs/Windows/OpenSSL-Win64")
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD")
    
    target_include_directories(vm PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_directories(vm PRIVATE ${OPENSSL_LIB_DIR})
    
    target_link_libraries(vm PRIVATE
            libssl
            libcrypto
            ws2_32        # Esto es crucial para sockets en windwos
    )
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(vm PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# Opciones globales para todos los builds
target_compile_options(vm PRIVATE
        -std=c++17
        -D__STDC_LIMIT_MACROS
        -D__STDC_CONSTANT_MACROS
        -static
        -static-libgcc -static-libstdc++
)

target_compile_options(vm PRIVATE
        $<$<CONFIG:Release>:-Os -ffunction-sections -fdata-sections>
)
target_link_options(vm PRIVATE
        $<$<CONFIG:Release>:-Wl,--gc-sections>
)

# Opciones por configuracion
target_compile_options(vm PRIVATE
        $<$<CONFIG:Debug>:-fpermissive -g -O0 -Wall -Wextra -DMODE_DEBUG_ENABLE>
        $<$<CONFIG:Release>:-fpermissive -O3 -DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-fpermissive -g -O1 -fno-omit-frame-pointer>
)

# hace que el linker strippee simbolos innecesarios,
# bajando el tamaño final
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Elimina simbolos de depuracion y reduce binario
    set_target_properties(vm PROPERTIES
            LINK_FLAGS "-s"
    )
endif()

# Añadir subdirectorio de tests
add_subdirectory(tests)