# ============================================================================
# vex_lib: frontend del lenguaje Vex (lexer, parser, type checker, lowering,
# facade del compilador).  Diseno aislado segun la decision arquitectonica
# del roadmap: el frontend es una libreria estatica separada que prefigura
# la futura libvesta_rt y permite reutilizarla desde tests, herramientas
# externas o un futuro servicio LSP sin arrastrar todo el binario `vm`.
#
# Dependencias internas:
#  - Ninguna sobre vm/vmcore: el frontend solo conoce ir::IrModule
#    (definido en include/ir/), y consume cabeceras de vex/.  El IR se
#    enlaza desde el ejecutable principal porque sus .cpp se incluyen
#    en el GLOB principal de src/.
#
# Dependencias externas:
#  - Ninguna mas alla de la stdlib de C++17.
# ============================================================================

# ---------------------------------------------------------------------------
# Version de CODEGEN para invalidacion de cache (.vexi / .vexir / .vpc).
#
# vexi_compiler_version_hash() usa VEXI_COMPILER_BUILD_ID si esta definido.
# Aqui generamos ese identificador como un hash de las fuentes que afectan la
# SALIDA del compilador (frontend Vex + IR + emisores de bytecode + AOT/JIT).
# El header solo se reescribe cuando ese hash cambia, de modo que:
#   - cambiar lowering/IR/emisores -> nuevo hash -> caches invalidados.
#   - recompilar sin tocar codegen  -> mismo hash -> caches conservados.
# Esto evita el bug de "edite el compilador pero el .vexir viejo se reuso".
file(GLOB_RECURSE VEX_CODEGEN_SRCS
    ${CMAKE_SOURCE_DIR}/src/vex/*.cpp     ${CMAKE_SOURCE_DIR}/src/vex/*.h
    ${CMAKE_SOURCE_DIR}/include/vex/*.h
    ${CMAKE_SOURCE_DIR}/src/ir/*.cpp      ${CMAKE_SOURCE_DIR}/include/ir/*.h
    ${CMAKE_SOURCE_DIR}/src/emmit/*.cpp   ${CMAKE_SOURCE_DIR}/include/emmit/*.h
    ${CMAKE_SOURCE_DIR}/src/aot/*.c       ${CMAKE_SOURCE_DIR}/src/aot/*.cpp
    ${CMAKE_SOURCE_DIR}/include/aot/*.h
    ${CMAKE_SOURCE_DIR}/src/jit/*.cpp     ${CMAKE_SOURCE_DIR}/include/jit/*.h
    ${CMAKE_SOURCE_DIR}/src/port/transpiler_base.cpp
    ${CMAKE_SOURCE_DIR}/src/port/c/c_backend.cpp
)
set(VEX_CODEGEN_VERSION_HDR ${CMAKE_CURRENT_BINARY_DIR}/vex_codegen_version.h)
add_custom_command(
    OUTPUT ${VEX_CODEGEN_VERSION_HDR}
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${VEX_CODEGEN_VERSION_HDR}
        "-DSRCS=${VEX_CODEGEN_SRCS}"
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_codegen_version.cmake
    DEPENDS ${VEX_CODEGEN_SRCS} ${CMAKE_SOURCE_DIR}/cmake/gen_codegen_version.cmake
    COMMENT "Calculando version de codegen Vex (invalidacion de cache)"
    VERBATIM
)

add_library(vex_lib STATIC
    ${VEX_CODEGEN_VERSION_HDR}
    lexer.cpp
    ast.cpp
    parser.cpp
    asm_effects.cpp
    asm_backend.cpp
    type_checker.cpp
    # Fase 0 interop C / ownership: clasificador is_c_representable / is_managed.
    type_classify.cpp
    lombok.cpp
    lowering.cpp
    # Auto-vectorizacion de bucles (idiomas memcpy/copy; futuro unroll+SIMD).
    # Metodos de Lowering separados de lowering.cpp para mantenerlos agrupados.
    vectorize.cpp
    borrow_checker.cpp
    compiler.cpp
    compiler_project.cpp
    mermaid_diagrams.cpp
    graphviz_diagrams.cpp
    html_diagrams.cpp
    # Subsistema de coste Big-O (modo --analyze).  Solo depende de
    # ir::IrModule; vive aqui para que los renderers de diagramas puedan
    # anotar el coste por funcion (--diagram-cost).
    ${CMAKE_SOURCE_DIR}/src/analyze/bigo.cpp
    comptime_introspect.cpp
    # Sistema de modulos.
    module_resolver.cpp
    module_interop.cpp
    namespace_flatten.cpp
    vexi_format.cpp
    project_cache.cpp
    velb_signature.cpp
    # comptime_vm.cpp vive en src/runtime/ (depende del runtime VM).
    # Transpiler IR -> codigo fuente (C inicial; Java/JS/etc futuro).
    # Vive aqui porque compiler.cpp lo invoca como ultimo paso opcional.
    # Si en el futuro se quiere usar el transpiler standalone, se extrae
    # a su propia libreria port_lib en CMakeLists del root.
    ${CMAKE_SOURCE_DIR}/src/port/transpiler_base.cpp
    ${CMAKE_SOURCE_DIR}/src/port/c/c_backend.cpp
)

# La libreria expone sus headers desde include/vex/; PUBLIC para que los
# consumidores (vm, tests) los hereden automaticamente al linkear.
target_include_directories(vex_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
# El header generado con VEXI_COMPILER_BUILD_ID es PRIVATE (solo lo usa
# vexi_format.cpp).  Dependencia explicita para que CMake genere el header
# antes de compilar ese TU y lo recompile cuando el header cambie.
target_include_directories(vex_lib PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_source_files_properties(vexi_format.cpp PROPERTIES
    OBJECT_DEPENDS ${VEX_CODEGEN_VERSION_HDR})

# C++17 minimo (consistente con el resto del proyecto).
target_compile_features(vex_lib PUBLIC cxx_std_17)

# velb_signature.cpp usa OpenSSL EVP_DigestSign / EVP_DigestVerify.
# Enlace centralizado en `vesta_openssl` (estatico/standalone o FireDaemon).
target_link_libraries(vex_lib PRIVATE vesta_openssl)

# Avisos estrictos en builds nuestros para mantener la regla de "no warnings".
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vex_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()
