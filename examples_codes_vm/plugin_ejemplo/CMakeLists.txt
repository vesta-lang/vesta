# CMakeLists.txt - Plugin de ejemplo para VestaVM
#
# Construccion autonoma (sin necesitar el proyecto principal):
#
#   Windows (MinGW):
#     cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -B build .
#     cmake --build build
#     => build/mi_plugin.dll
#
#   Linux / macOS:
#     cmake -DCMAKE_BUILD_TYPE=Release -B build .
#     cmake --build build
#     => build/mi_plugin.so  (Linux)
#     => build/mi_plugin.dylib  (macOS)
#
#   Con Clang en lugar de GCC:
#     cmake -DCMAKE_C_COMPILER=clang -B build .
#     cmake --build build
#
#   Con MSVC (Visual Studio):
#     cmake -G "Visual Studio 17 2022" -A x64 -B build .
#     cmake --build build --config Release
#
# Para apuntar a un SDK en otra ruta:
#   cmake -DVESTA_SDK_DIR=/ruta/al/sdk -B build .

cmake_minimum_required(VERSION 3.15)

# project() solo se declara en build autonomo; cuando viene del proyecto
# principal ya existe un proyecto activo y re-declararlo es innecesario.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(mi_plugin C)
endif()

# ---------------------------------------------------------------------------
# Localizar el modulo VestaPlugin.cmake
# ---------------------------------------------------------------------------
if(NOT DEFINED VESTA_SDK_DIR)
    # plugin_ejemplo/ -> examples_codes_vm/ -> SDK root
    get_filename_component(VESTA_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

list(APPEND CMAKE_MODULE_PATH "${VESTA_SDK_DIR}/cmake")

include(VestaPlugin)

# ---------------------------------------------------------------------------
# Declarar el plugin
# ---------------------------------------------------------------------------
add_vesta_plugin(mi_plugin
    SOURCES
        mi_plugin.c
    SDK_DIR
        "${VESTA_SDK_DIR}"
    C_STANDARD
        11
)

# ---------------------------------------------------------------------------
# Targets de utilidad
# ---------------------------------------------------------------------------

# 'cmake --build build --target help_plugin' muestra instrucciones de uso
add_custom_target(help_plugin
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "=== Plugin compilado ==="
    COMMAND ${CMAKE_COMMAND} -E echo "Artefacto: $<TARGET_FILE:mi_plugin>"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Copiar al directorio del ejecutable .velb y ejecutar:"
    COMMAND ${CMAKE_COMMAND} -E echo "  vesta --run usar_plugin.velb"
    VERBATIM
)
