# ============================================================================
# vx_lib: frontend del lenguaje Vesta (lexer, parser, type checker, lowering,
# facade del compilador).  Diseno aislado segun la decision arquitectonica
# del roadmap: el frontend es una libreria estatica separada que prefigura
# la futura libvesta_rt y permite reutilizarla desde tests, herramientas
# externas o un futuro servicio LSP sin arrastrar todo el binario `vm`.
#
# Dependencias internas:
#  - Ninguna sobre vm/vmcore: el frontend solo conoce ir::IrModule
#    (definido en include/ir/), y consume cabeceras de vx/.  El IR se
#    enlaza desde el ejecutable principal porque sus .cpp se incluyen
#    en el GLOB principal de src/.
#
# Dependencias externas:
#  - Ninguna mas alla de la stdlib de C++17.
# ============================================================================

# ---------------------------------------------------------------------------
# Version de CODEGEN para invalidacion de cache (.vxi / .vxir / .vpc).
#
# vxi_compiler_version_hash() usa VXI_COMPILER_BUILD_ID si esta definido.
# Aqui generamos ese identificador como un hash de las fuentes que afectan la
# SALIDA del compilador.  El header solo se reescribe cuando ese hash cambia:
#   - cambiar lowering/IR/emisores/linker -> nuevo hash -> caches invalidados.
#   - recompilar sin tocar codegen        -> mismo hash -> caches conservados.
# Esto evita el bug de "edite el compilador pero el .vxir viejo se reuso".
#
# El criterio es TODO lo que decide los bytes del artefacto, no solo el
# frontend.  El `.vpc` cachea el `.velb` FINAL, asi que la cadena entera cuenta:
#
#   .vx --[preprocesador]--> .vx --[frontend vx]--> IR --[ir_emitter]--> .vel
#       --[lexer + parser + emmit + linker]--> .velb        (o AOT/JIT/port)
#
# Faltaban los cuatro ultimos: un cambio del LINKER (layout de secciones,
# relocs, @sym) no invalidaba nada y el `.vpc` seguia sirviendo un `.velb`
# emitido por el linker viejo.  Ante la duda, sobre-invalidar: reemitir es
# barato y estas fuentes cambian poco; servir un artefacto stale no lo es.
file(GLOB_RECURSE VX_CODEGEN_SRCS
    ${CMAKE_SOURCE_DIR}/src/vx/*.cpp     ${CMAKE_SOURCE_DIR}/src/vx/*.h
    ${CMAKE_SOURCE_DIR}/include/vx/*.h
    ${CMAKE_SOURCE_DIR}/src/ir/*.cpp      ${CMAKE_SOURCE_DIR}/include/ir/*.h
    ${CMAKE_SOURCE_DIR}/src/emmit/*.cpp   ${CMAKE_SOURCE_DIR}/include/emmit/*.h
    ${CMAKE_SOURCE_DIR}/src/aot/*.c       ${CMAKE_SOURCE_DIR}/src/aot/*.cpp
    ${CMAKE_SOURCE_DIR}/include/aot/*.h
    ${CMAKE_SOURCE_DIR}/src/jit/*.cpp     ${CMAKE_SOURCE_DIR}/include/jit/*.h
    ${CMAKE_SOURCE_DIR}/src/port/transpiler_base.cpp
    ${CMAKE_SOURCE_DIR}/src/port/c/c_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/port/snippet_loader.cpp
    # --- .vel -> .velb: lo que ensambla y enlaza el bytecode final ---
    ${CMAKE_SOURCE_DIR}/src/lexer/*.cpp   ${CMAKE_SOURCE_DIR}/include/lexer/*.h
    ${CMAKE_SOURCE_DIR}/src/parser/*.cpp  ${CMAKE_SOURCE_DIR}/include/parser/*.h
    ${CMAKE_SOURCE_DIR}/src/linker/*.cpp  ${CMAKE_SOURCE_DIR}/include/linker/*.h
    ${CMAKE_SOURCE_DIR}/src/util/assembler_multiprocess.cpp
    ${CMAKE_SOURCE_DIR}/include/util/assembler_multiprocess.h
    # --- preprocesador: corre como primera etapa sobre cada .vx ---
    ${CMAKE_SOURCE_DIR}/preprocessor/src/*.c
    ${CMAKE_SOURCE_DIR}/preprocessor/src/*.cpp
    ${CMAKE_SOURCE_DIR}/preprocessor/include/*.h
)
set(VX_CODEGEN_VERSION_HDR ${CMAKE_CURRENT_BINARY_DIR}/vx_codegen_version.h)
add_custom_command(
    OUTPUT ${VX_CODEGEN_VERSION_HDR}
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${VX_CODEGEN_VERSION_HDR}
        "-DSRCS=${VX_CODEGEN_SRCS}"
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_codegen_version.cmake
    DEPENDS ${VX_CODEGEN_SRCS} ${CMAKE_SOURCE_DIR}/cmake/gen_codegen_version.cmake
    COMMENT "Calculando version de codegen Vesta (invalidacion de cache)"
    VERBATIM
)

add_library(vx_lib STATIC
    ${VX_CODEGEN_VERSION_HDR}
    lexer.cpp
    ast.cpp
    parser.cpp
    asm_effects.cpp
    # Resumen de efectos a nivel de bloque de un inline asm (memoria/atomica/
    # call/flags/rama/marco de pila explicito) sobre la tabla plana por
    # instruccion de asm_effects; base del compilador consciente del asm.
    asm_analyze.cpp
    asm_backend.cpp
    type_checker.cpp
    # El `when:` de un contrato: gramatica, atomos y la regla de prioridad
    # entre varios que casan a la vez.  Aparte porque lo necesitan DOS TUs
    # (parser.cpp resuelve los atomos de target, type_checker.cpp los que
    # hablan del parametro de tipo) y tener dos copias del clasificador era
    # justo lo que hacia falta evitar.
    contract_when.cpp
    # Utilidades de clonacion de AST con sustitucion de type-params,
    # extraidas de type_checker.cpp (monolito) para mantenerlo manejable.
    generic_clone.cpp
    # Monomorphizacion de metodos genericos `obj.metodo<U>()` (#4).
    generic_methods.cpp
    # Conceptos / constraints de genericos `<T: Concepto>` / where (#6).
    concepts.cpp
    # Especializacion total + parcial de structs genericos (#7).
    specialization.cpp
    # Fase 0 interop C / ownership: clasificador is_c_representable / is_managed.
    type_classify.cpp
    # Fase 4 interop C: generador de header C publico (vx --emit-header).
    c_header_gen.cpp
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
    # Huella computacional por-declaracion (recursos + efectos): base del
    # modelo dirigido por resumenes y de las anotaciones comprobables.
    ${CMAKE_SOURCE_DIR}/src/analyze/fingerprint.cpp
    comptime_introspect.cpp
    # Recolector del conjunto comptime (P1 fase 1: base del artefacto comptime
    # separado que compilara/ejecutara todo el comptime en la ComptimeVM).
    comptime_collect.cpp
    # Sistema de modulos.
    module_resolver.cpp
    module_interop.cpp
    namespace_flatten.cpp
    # Indice semantico por-declaracion (hash de contenido + grafo de deps):
    # sustrato de la compilacion incremental granular y distribuida.
    semantic_index.cpp
    # Driver incremental granular: claves Merkle + CAS + plan_rebuild.
    incremental.cpp
    vxi_format.cpp
    project_cache.cpp
    velb_signature.cpp
    # comptime_vm.cpp vive en src/runtime/ (depende del runtime VM).
    # Transpiler IR -> codigo fuente (C inicial; Java/JS/etc futuro).
    # Vive aqui porque compiler.cpp lo invoca como ultimo paso opcional.
    # Si en el futuro se quiere usar el transpiler standalone, se extrae
    # a su propia libreria port_lib en CMakeLists del root.
    ${CMAKE_SOURCE_DIR}/src/port/transpiler_base.cpp
    ${CMAKE_SOURCE_DIR}/src/port/c/c_backend.cpp
    # Cargador de snippets compartido (stdlib/port/c) -- backend C + header gen.
    ${CMAKE_SOURCE_DIR}/src/port/snippet_loader.cpp
)

# La libreria expone sus headers desde include/vx/; PUBLIC para que los
# consumidores (vm, tests) los hereden automaticamente al linkear.
target_include_directories(vx_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
# El header generado con VXI_COMPILER_BUILD_ID es PRIVATE (solo lo usa
# vxi_format.cpp).  Dependencia explicita para que CMake genere el header
# antes de compilar ese TU y lo recompile cuando el header cambie.
target_include_directories(vx_lib PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_source_files_properties(vxi_format.cpp PROPERTIES
    OBJECT_DEPENDS ${VX_CODEGEN_VERSION_HDR})

# C++17 minimo (consistente con el resto del proyecto).
target_compile_features(vx_lib PUBLIC cxx_std_17)
# PIC se activa globalmente en el CMakeLists raiz (CMAKE_POSITION_INDEPENDENT_
# CODE ON) para que vx_lib enlace tambien en la libreria compartida embebible
# vesta_ffi (libvesta.so).  No hace falta fijarlo aqui.

# velb_signature.cpp usa OpenSSL EVP_DigestSign / EVP_DigestVerify.
# Enlace centralizado en `vesta_openssl` (estatico/standalone o FireDaemon).
target_link_libraries(vx_lib PRIVATE vesta_openssl)

# Avisos estrictos en builds nuestros para mantener la regla de "no warnings".
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vx_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()
