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
    # El allocator central (rbank) + estructuras generales del codegen (RegAlloc,
    # LinearPos, AllocationTimeline...): cambiarlos cambia el codigo emitido -> deben
    # entrar al hash que invalida el cache de codegen.
    ${CMAKE_SOURCE_DIR}/include/codegen/*.h  ${CMAKE_SOURCE_DIR}/src/codegen/*.cpp
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
    # Sistema UNICO de efectos del compilador: modelo (SemanticEffects/
    # MachineEffects/reticulo AbstractLoc + combinadores seq/join), summaries por
    # funcion/modulo, contratos DECLARATIVOS por reglas, y el gestor de analisis
    # de dos niveles (local por nodo + summary por funcion).  Fuente unica para
    # optimizer, scheduler, --analyze, diagramas y LSP.
    ${CMAKE_SOURCE_DIR}/src/analysis/effects/effects.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/effects/contracts.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/effects/ir_effects.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/effects/effect_analysis.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/effects/effects_report.cpp
    # Hechos IR objetivos (def-use, call-sites, estructura) para la infra de
    # analisis: base compartida que consumen los analisis sin re-recorrer el IR.
    ${CMAKE_SOURCE_DIR}/src/analysis/facts/ir_facts.cpp
    # De cuanto es multiplo un valor: el hecho con el que se pliega una
    # comprobacion de alineacion y se comprueba lo que una instruccion EXIGE.
    ${CMAKE_SOURCE_DIR}/src/analysis/facts/alignment.cpp
    # De que valor del programa habla cada operando de un bloque de asm.  Es el
    # puente entre el texto (donde un operando se llama `$0`) y el IR (donde es
    # un valor), y lo comparten el modelo de efectos, el eliminador de
    # escrituras muertas y la comprobacion de precondiciones.
    ${CMAKE_SOURCE_DIR}/src/analysis/facts/asm_bindings.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/memory/points_to.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/memory/memory_access.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/escape/escape.cpp
    asm/asm_effects.cpp
    # Resumen de efectos a nivel de bloque de un inline asm (memoria/atomica/
    # call/flags/rama/marco de pila explicito) sobre la tabla plana por
    # instruccion de asm_effects; base del compilador consciente del asm.
    asm/asm_analyze.cpp
    asm/asm_backend.cpp
    # CFG del asm inline (bloques basicos + aristas desde labels/saltos); base
    # del dataflow, los diagnosticos y los diagramas del asm.
    asm/asm_cfg.cpp
    # Diagnosticos estructurales del asm sobre el CFG (codigo muerto, salto no
    # resuelto, bucle sin salida).
    asm/asm_diag.cpp
    # Diagramas del CFG del asm anotados con coste/latencia/cuellos de botella/
    # flags/diagnosticos (mermaid + graphviz), para embeber en los diagramas.
    asm/asm_diagram.cpp
    # Lift de patrones de asm a IR tipado (lock cmpxchg/xadd -> ATOMIC_CAS/ADD):
    # atomicos escritos en asm que bajan al mismo op optimizable que el opcode.
    asm/asm_lift.cpp
    # Emision del IR tipado del lift (separada del reconocimiento, para mantener
    # el lowering minimo).
    asm/asm_lift_emit.cpp
    # Lift de asm opaco sin operandos de registro a ASM_MICRO (barreras/nop/...).
    asm/asm_lift_micro.cpp
    # Lift GENERAL de asm inline a IR: dispatcher NEUTRO por ISA + frontend x86.
    # El core neutro (register-file + CFG driver + emisores) vive en el header
    # asm_lift_core.h; cada ISA aporta su lift_<isa> sin tocar el core.
    asm/asm_lift_general.cpp
    asm/asm_lift_x86.cpp
    # Catalogo multi-idioma de diagnosticos (codigo estable VXNNNN -> mensaje por
    # idioma).  La tabla se genera desde catalog/diagnostics.toml con
    # tools/import/gen_diag_catalog.py.  Seleccion de idioma por entorno.
    # Renderizado de diagnosticos en formato maquina (JSON/SARIF) para IDEs/CI.
    diag/diag_format.cpp
    # DB de instrucciones EMBEBIDA (matcher texto->FormID) + tablas generadas
    # por tools/import/gen_cpp_db.py (autocontenido, sin archivos externos).
    # La consumen ASA (asm-aware), el LSP y el optimizer JIT/AOT.
    asm/instr_db.cpp
    gen/instr_db_x86_gen.cpp
    gen/instr_db_arm64_gen.cpp
    gen/instr_db_arm32_gen.cpp
    gen/instr_db_riscv_gen.cpp
    gen/instr_db_x86_cost_gen.cpp
    gen/instr_db_arm64_cost_gen.cpp
    gen/instr_db_arm32_cost_gen.cpp
    gen/instr_db_riscv_cost_gen.cpp
    gen/instr_db_x86_feat_gen.cpp
    gen/instr_db_arm_feat_gen.cpp
    gen/instr_db_riscv_feat_gen.cpp
    type_checker.cpp
    # El `when:` de un contrato: gramatica, atomos y la regla de prioridad
    # entre varios que casan a la vez.  Aparte porque lo necesitan DOS TUs
    # (parser.cpp resuelve los atomos de target, type_checker.cpp los que
    # hablan del parametro de tipo) y tener dos copias del clasificador era
    # justo lo que hacia falta evitar.
    contract_when.cpp
    # Utilidades de clonacion de AST con sustitucion de type-params,
    # extraidas de type_checker.cpp (monolito) para mantenerlo manejable.
    generics/generic_clone.cpp
    # Monomorphizacion de metodos genericos `obj.metodo<U>()` (#4).
    generics/generic_methods.cpp
    # Conceptos / constraints de genericos `<T: Concepto>` / where (#6).
    generics/concepts.cpp
    # Especializacion total + parcial de structs genericos (#7).
    generics/specialization.cpp
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
    # Traductor de los tipos de Vesta a entidades de la base de conocimiento de
    # depuracion (vxdbg).  Vive en el frontend porque la correspondencia entre
    # el vocabulario de Vesta y el modelo generico solo la conoce quien declara
    # los tipos; vxdbg no sabe de ningun lenguaje.
    vxdbg_emit.cpp
    diagram/mermaid_diagrams.cpp
    diagram/graphviz_diagrams.cpp
    diagram/html_diagrams.cpp
    # Subsistema de coste Big-O (modo --analyze).  Solo depende de
    # ir::IrModule; vive aqui para que los renderers de diagramas puedan
    # anotar el coste por funcion (--diagram-cost).
    ${CMAKE_SOURCE_DIR}/src/analyze/bigo.cpp
    # Huella computacional por-declaracion (recursos + efectos): base del
    # modelo dirigido por resumenes y de las anotaciones comprobables.
    ${CMAKE_SOURCE_DIR}/src/analyze/fingerprint.cpp
    ${CMAKE_SOURCE_DIR}/src/analyze/asm_report.cpp
    # El asm como dominio del ASA (productor de hechos): vive junto al informe
    # porque comparte la base de datos de instrucciones.
    ${CMAKE_SOURCE_DIR}/src/analyze/asa_asm.cpp
    comptime/comptime_introspect.cpp
    # Recolector del conjunto comptime (P1 fase 1: base del artefacto comptime
    # separado que compilara/ejecutara todo el comptime en la ComptimeVM).
    comptime/comptime_collect.cpp
    # Constructor `comptime T(expr)` de structs: literales de tipo usuario (F1b).
    comptime/literal_ctor.cpp
    # Sistema de modulos.
    module/module_resolver.cpp
    module/module_interop.cpp
    module/namespace_flatten.cpp
    # Indice semantico por-declaracion (hash de contenido + grafo de deps):
    # sustrato de la compilacion incremental granular y distribuida.
    semantic_index.cpp
    # Huella del contenido con SIGNIFICADO de un fuente (tokens, no bytes).
    source_hash.cpp
    # Driver incremental granular: claves Merkle + CAS + plan_rebuild.
    incremental.cpp
    module/vxi_format.cpp
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
# Los codecs de vxdbg comparten un header interno que no forma parte de su API
# publica: se ve desde su propia carpeta y de ningun otro sitio.
target_include_directories(vx_lib PRIVATE ${CMAKE_SOURCE_DIR}/src/vxdbg)
set_source_files_properties(module/vxi_format.cpp PROPERTIES
    OBJECT_DEPENDS ${VX_CODEGEN_VERSION_HDR})

# C++17 minimo (consistente con el resto del proyecto).
target_compile_features(vx_lib PUBLIC cxx_std_17)
# PIC se activa globalmente en el CMakeLists raiz (CMAKE_POSITION_INDEPENDENT_
# CODE ON) para que vx_lib enlace tambien en la libreria compartida embebible
# vesta_ffi (libvesta.so).  No hace falta fijarlo aqui.

# velb_signature.cpp usa OpenSSL EVP_DigestSign / EVP_DigestVerify.
# Enlace centralizado en `vesta_openssl` (estatico/standalone o FireDaemon).
target_link_libraries(vx_lib PRIVATE vesta_openssl)
# El grafo de conocimiento del programa es una libreria aparte: ni es de Vesta
# ni de la maquina, y el runtime tambien lo necesita.
target_link_libraries(vx_lib PUBLIC vxdbg_lib diag_lib)

# Avisos estrictos en builds nuestros para mantener la regla de "no warnings".
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vx_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()
