# VestaPackaging.cmake
#
# Reglas de instalacion (CPack) para VestaVM.  UNICA fuente de verdad de "que
# archivos forman una instalacion": el instalador generado (NSIS .exe / WiX .msi
# / ZIP portable) empaqueta EXACTAMENTE lo que aqui se declara con install().
# Asi nunca se desincroniza ni "se olvida" copiar un archivo.
#
# Generar el instalador con UN comando (NSIS se AUTO-DESCARGA si falta -- el
# usuario no instala nada a mano):
#   cmake --build <build> --target installer        # -> VestaVM-<ver>-win64.exe
#   cmake --build <build> --target installer-zip     # -> VestaVM-<ver>-win64.zip
#
# Alternativa manual (si ya tienes la herramienta instalada):
#   cpack --config <build>/CPackConfig.cmake -G NSIS    # .exe bonito
#   cpack --config <build>/CPackConfig.cmake -G WIX     # .msi (requiere WiX Toolset)
#   cpack --config <build>/CPackConfig.cmake -G ZIP     # portable (sin herramientas)
#
# Layout instalado (PLANO, espejo del runtime: el ejecutable resuelve stdlib/ e
# include_lib/ relativos a su propia ubicacion).  Por COMPONENTE:
#
#   <prefix>/
#     vesta.exe                    <- [core] ejecutable principal (en el PATH)
#     libvesta_gc.a                <- [core] GC estatico para AOT (gc<T>)
#     libvesta_collections.a       <- [core] colecciones estaticas para AOT
#     libvesta_math.a              <- [core] math estatico para AOT
#     include_lib/                 <- [core] stdlib del preprocesador VPP
#     README.md, LICENSE.txt       <- [core]
#     stdlib/native/<mod>/*.dll    <- [stdlib] plugins nativos (io, math, ...)
#     stdlib/{vx,vel,port,vsh}/   <- [stdlib] modulos fuente del lenguaje
#     vesta_lsp.exe                <- [lsp] servidor LSP para editores
#     examples/vx/, examples/vsh/ <- [examples] ejemplos Vex y VSH
#     tools/                       <- [tools] scripts del lenguaje
#     vesta.dll + lib/ + include/ffi/ + cmake/  <- [sdk] embeber / plugins
#     libssl-3-x64.dll, libcrypto-3-x64.dll     <- [core] SOLO si OpenSSL no se embebio

if (NOT WIN32)
    return()  # de momento el empaquetado grafico es Windows-only
endif()

# ---------------------------------------------------------------------------
# Reglas de instalacion (fuente de verdad) organizadas por COMPONENTE.
# El instalador NSIS ofrece marcar/desmarcar cada componente (instalacion
# personalizada).  `core` es obligatorio; el resto es opcional.
# ---------------------------------------------------------------------------

# === core (OBLIGATORIO): el lenguaje en si =================================
# El RUNTIME (vesta.exe + todo lo que el ejecutable resuelve relativo a su
# propia ubicacion: include_lib/, stdlib/, libvesta_gc.a, DLLs) se instala en
# <prefix>/bin.  Asi el acceso directo del Menu Inicio y el PATH del sistema
# (ambos gestionados por CPack, que asume `bin/`) apuntan correctamente a
# <prefix>/bin/vesta.exe y `vesta` funciona desde cualquier shell.
# Ejecutable principal instalado como vesta.exe (no vm.exe).
install(PROGRAMS "$<TARGET_FILE:vm>"
        DESTINATION bin RENAME vesta.exe COMPONENT core)
# stdlib del preprocesador VPP (fuente, NO binario): va en la RAIZ; el
# ejecutable en bin/ la resuelve relativo a su padre (exe_dir/../include_lib).
install(DIRECTORY "${CMAKE_SOURCE_DIR}/preprocessor/include_lib"
        DESTINATION . COMPONENT core)
# Documentacion (LICENSE en texto plano para que el asistente lo muestre bien).
install(FILES
        "${CMAKE_SOURCE_DIR}/README.md"
        "${CMAKE_SOURCE_DIR}/LICENSE"
        DESTINATION . COMPONENT core)
# OpenSSL: si NO se embebio (fallback FireDaemon), enviar sus DLLs.  Con enlace
# estatico (VESTA_OPENSSL_STATIC) no se envia nada -- el .exe es standalone.
if (NOT VESTA_OPENSSL_STATIC)
    install(FILES
            "$<TARGET_FILE_DIR:vm>/libssl-3-x64.dll"
            "$<TARGET_FILE_DIR:vm>/libcrypto-3-x64.dll"
            DESTINATION bin COMPONENT core)
endif()
# GC estatico para AOT: al compilar un programa con `gc<T>` en modo AOT, el
# enlazador interno busca libvesta_gc.a JUNTO a vesta.exe.  Sin esto, gc<T> en
# AOT falla con "no se encontro libvesta_gc.a".
if (TARGET vesta_gc)
    install(FILES "$<TARGET_FILE:vesta_gc>" DESTINATION bin COMPONENT core)
endif()
# Variantes ESTATICAS de los plugins de stdlib (colecciones / math): el AOT las
# auto-enlaza JUNTO a vesta.exe cuando el programa las usa -> .exe standalone sin
# DLLs.  Se instalan en core (parte del toolchain AOT).
if (TARGET vesta_collections_a)
    install(FILES "$<TARGET_FILE:vesta_collections_a>"
            DESTINATION bin COMPONENT core)
endif()
if (TARGET vesta_math_a)
    install(FILES "$<TARGET_FILE:vesta_math_a>" DESTINATION bin COMPONENT core)
endif()

# === stdlib (opcional): biblioteca estandar del lenguaje ===================
# Modulos fuente Vesta/Vex (vx/, vel/, port/, vsh/).  Los plugins nativos
# (stdlib/native/*/*.dll) los instala add_vesta_plugin con COMPONENT stdlib.
install(DIRECTORY
        "${CMAKE_SOURCE_DIR}/stdlib/vx"
        "${CMAKE_SOURCE_DIR}/stdlib/vel"
        "${CMAKE_SOURCE_DIR}/stdlib/port"
        "${CMAKE_SOURCE_DIR}/stdlib/vsh"
        DESTINATION "stdlib" COMPONENT stdlib
        PATTERN ".gitignore" EXCLUDE)

# === lsp (opcional): servidor de lenguaje para editores ====================
if (TARGET vesta_lsp)
    install(PROGRAMS "$<TARGET_FILE:vesta_lsp>" DESTINATION bin COMPONENT lsp)
endif()

# === examples (opcional): programas de ejemplo de AMBOS lenguajes ==========
# Vesta trae dos lenguajes: Vex (compilado) y VSH (scripting).
install(DIRECTORY "${CMAKE_SOURCE_DIR}/examples_codes_vx/"
        DESTINATION "examples/vx" COMPONENT examples
        FILES_MATCHING
            PATTERN "*.vx"
            PATTERN "*.md"
            PATTERN "*.toml")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/examples_codes_vsh/"
        DESTINATION "examples/vsh" COMPONENT examples
        FILES_MATCHING
            PATTERN "*.vsh"
            PATTERN "*.md")

# === tools (opcional): herramientas del lenguaje ===========================
# Scripts (cobertura AOT/JIT, benchmarks, cliente de depuracion VSH).  Se
# excluyen los artefactos de build (.velb/.vel/.vx de scratch).
install(DIRECTORY "${CMAKE_SOURCE_DIR}/tools/"
        DESTINATION "tools" COMPONENT tools
        FILES_MATCHING
            PATTERN "*.py"
            PATTERN "*.vsh"
            PATTERN "*.sh"
            PATTERN "*.md")

# === sdk (opcional): embeber Vesta o escribir plugins nativos ==============
# libvesta.dll (FFI, OpenSSL + libstdc++ estaticos -> autocontenida) + header
# C + helper CMake.
if (TARGET vesta_ffi)
    install(TARGETS vesta_ffi
            RUNTIME DESTINATION bin  COMPONENT sdk
            ARCHIVE DESTINATION lib  COMPONENT sdk)
endif()
install(FILES "${CMAKE_SOURCE_DIR}/include/ffi/vesta_plugin.h"
        DESTINATION "include/ffi" COMPONENT sdk)
install(FILES "${CMAKE_SOURCE_DIR}/cmake/VestaPlugin.cmake"
        DESTINATION "cmake" COMPONENT sdk)

# ---------------------------------------------------------------------------
# Metadatos CPack (comunes a todos los generadores)
# ---------------------------------------------------------------------------
# Escapado correcto de las variables CPACK_* en el config generado (paths con
# espacios, parentesis en descripciones, etc.).  Recomendado por CMake.
set(CPACK_VERBATIM_VARIABLES ON)

set(CPACK_PACKAGE_NAME              "VestaVM")
set(CPACK_PACKAGE_VENDOR           "David Lopez T. (DesmonHak)")
set(CPACK_PACKAGE_VERSION          "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR    "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR    "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH    "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "VestaVM: maquina virtual y compilador del lenguaje Vex (bytecode/JIT/AOT)")
set(CPACK_PACKAGE_DESCRIPTION
        "VestaVM: el compilador y la maquina virtual de los lenguajes Vex (compilado: bytecode/JIT/AOT nativo) y VSH (scripting), con su biblioteca estandar, servidor LSP, ejemplos y herramientas.")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "VestaVM")
set(CPACK_PACKAGE_FILE_NAME        "VestaVM-${PROJECT_VERSION}-win64")

# Empaquetar SOLO nuestros componentes.  CPack instala CADA componente de
# CPACK_COMPONENTS_ALL por separado (con CMAKE_INSTALL_COMPONENT definido), de
# modo que las reglas install() de los submodulos de terceros (keystone/
# capstone/...), que caen en el componente "Unspecified", NUNCA se disparan.  Sin
# CPACK_MONOLITHIC_INSTALL -> el instalador NSIS muestra la pagina de seleccion
# de componentes (instalacion personalizada).
set(CPACK_COMPONENTS_ALL core stdlib lsp examples tools sdk)
# El generador ZIP por defecto haria una instalacion MONOLITICA (incluiria el
# "Unspecified" de los terceros y fallaria).  Con esto, el ZIP tambien instala
# por-componente; ALL_COMPONENTS_IN_ONE agrupa todo en UN solo .zip (no uno por
# componente) y NSIS mantiene la pagina de seleccion.
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
# Mantener la carpeta contenedora (VestaVM-<ver>-win64/) dentro del .zip para
# que al extraer no se vuelquen los archivos sueltos en el directorio actual.
# NOTA: este valor por defecto (1) es el correcto para el ZIP; para el NSIS lo
# forzamos a 0 en CPACK_PROJECT_CONFIG_FILE (si no, el .exe instalaria en
# `...\VestaVM\VestaVM-<ver>-win64\` en vez de `...\VestaVM\`).
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 1)
# Override por-generador (NSIS=0, ZIP=1).  CPack lo ejecuta una vez por
# generador con ${CPACK_GENERATOR} fijado -> es el modo soportado de dar valores
# distintos segun el formato (un `-D` de cpack lo pisaria el CPackConfig).
set(CPACK_PROJECT_CONFIG_FILE
        "${CMAKE_SOURCE_DIR}/cmake/VestaCPackProjectConfig.cmake")

# Descripciones de cada componente (lo que ve el usuario al elegir que instalar).
set(CPACK_COMPONENT_CORE_DISPLAY_NAME     "Lenguaje Vesta (vesta.exe)")
set(CPACK_COMPONENT_CORE_DESCRIPTION
        "Compilador + maquina virtual del lenguaje Vex.  Obligatorio.")
set(CPACK_COMPONENT_CORE_REQUIRED ON)

set(CPACK_COMPONENT_STDLIB_DISPLAY_NAME   "Biblioteca estandar")
set(CPACK_COMPONENT_STDLIB_DESCRIPTION
        "Modulos de la stdlib (io, math, async, sync, mem, colecciones, strings).  Necesaria para print/IO y la mayoria de los programas.")
set(CPACK_COMPONENT_STDLIB_DEPENDS core)

set(CPACK_COMPONENT_LSP_DISPLAY_NAME      "Servidor LSP (editores)")
set(CPACK_COMPONENT_LSP_DESCRIPTION
        "vesta_lsp.exe: autocompletado, hover con Big-O, ir-a-definicion y diagnosticos en VS Code y otros editores LSP.")
set(CPACK_COMPONENT_LSP_DEPENDS core)

set(CPACK_COMPONENT_EXAMPLES_DISPLAY_NAME "Ejemplos (Vex + VSH)")
set(CPACK_COMPONENT_EXAMPLES_DESCRIPTION
        "Programas de ejemplo de los dos lenguajes de Vesta: Vex (compilado, examples/vx) y VSH (scripting, examples/vsh).")

set(CPACK_COMPONENT_TOOLS_DISPLAY_NAME    "Herramientas")
set(CPACK_COMPONENT_TOOLS_DESCRIPTION
        "Scripts del lenguaje: cobertura AOT/JIT, benchmarks y cliente de depuracion VSH.")

set(CPACK_COMPONENT_SDK_DISPLAY_NAME      "SDK (plugins / embeber)")
set(CPACK_COMPONENT_SDK_DESCRIPTION
        "libvesta.dll + header C + helper CMake para escribir plugins nativos o embeber Vesta en otros programas.")
set(CPACK_COMPONENT_SDK_DISABLED ON)   # desmarcado por defecto

# Recursos mostrados por el asistente (licencia en texto plano + readme).
# El repo mantiene UN solo fichero de licencia (LICENSE, sin extension).  El
# asistente NSIS espera un .txt, asi que generamos una copia en el build dir
# (no versionada) a partir del LICENSE canonico.
configure_file("${CMAKE_SOURCE_DIR}/LICENSE"
               "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")
set(CPACK_RESOURCE_FILE_README  "${CMAKE_SOURCE_DIR}/README.md")

# Acceso directo del Menu Inicio -> abre el REPL de Vesta.
set(CPACK_PACKAGE_EXECUTABLES "vesta" "Vesta REPL")

# Icono de la aplicacion (si existe en la raiz del repo).
set(_vesta_icon "${CMAKE_SOURCE_DIR}/icono.ico")

# ---------------------------------------------------------------------------
# NSIS  -- instalador .exe con asistente moderno (recomendado)
# ---------------------------------------------------------------------------
set(CPACK_NSIS_DISPLAY_NAME        "VestaVM ${PROJECT_VERSION}")
set(CPACK_NSIS_PACKAGE_NAME        "VestaVM ${PROJECT_VERSION}")
set(CPACK_NSIS_INSTALL_ROOT        "$PROGRAMFILES64")
# Anade <prefix> al PATH (con pagina de eleccion) -> `vesta` desde cualquier shell.
set(CPACK_NSIS_MODIFY_PATH         ON)
# NO auto-desinstalar la version previa.  El `.onInit` de CPack lee el registro
# y ejecuta el Uninstall.exe viejo; si el usuario borro la carpeta a mano, ese
# .exe ya no existe -> "Uninstall failed" + Abort (la instalacion se bloquea).
# Con esto OFF, reinstalar SOBRESCRIBE los archivos y REGENERA el registro y el
# desinstalador (auto-reparable), sin depender de un Uninstall.exe que pudo
# borrarse.  CPack no expone un hook en .onInit para hacerlo tolerante.
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL OFF)
set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\vesta.exe")
set(CPACK_NSIS_BRANDING_TEXT       "VestaVM ${PROJECT_VERSION}")
if (EXISTS "${_vesta_icon}")
    set(CPACK_NSIS_MUI_ICON   "${_vesta_icon}")  # icono del instalador
    set(CPACK_NSIS_MUI_UNIICON "${_vesta_icon}")  # icono del desinstalador
endif()
# Imagenes opcionales del asistente (si las dejas en packaging/windows/):
#   welcome.bmp 164x314  |  header.bmp 150x57   (formato BMP)
if (EXISTS "${CMAKE_SOURCE_DIR}/packaging/windows/welcome.bmp")
    set(CPACK_NSIS_MUI_WELCOMEFINISHPAGE_BITMAP
            "${CMAKE_SOURCE_DIR}/packaging/windows/welcome.bmp")
    set(CPACK_NSIS_MUI_UNWELCOMEFINISHPAGE_BITMAP
            "${CMAKE_SOURCE_DIR}/packaging/windows/welcome.bmp")
endif()
# Enlace en el Menu Inicio al README (se abre con el editor por defecto).
set(CPACK_NSIS_MENU_LINKS
        "README.md" "VestaVM - Leeme")

# ---------------------------------------------------------------------------
# WiX  -- instalador .msi (despliegue corporativo / GPO / Intune)
# ---------------------------------------------------------------------------
# UpgradeGuid FIJO: identifica el producto entre versiones (no cambiar nunca).
set(CPACK_WIX_UPGRADE_GUID    "5C9D2E14-7A3B-4F08-9E61-2B7C0A4D8F35")
set(CPACK_WIX_PROPERTY_ARPHELPLINK "https://github.com")
if (EXISTS "${_vesta_icon}")
    set(CPACK_WIX_PRODUCT_ICON "${_vesta_icon}")
endif()

# ---------------------------------------------------------------------------
# Generador por defecto: ZIP (no requiere herramientas externas).  NSIS / WIX
# se eligen con `cpack -G NSIS` / `cpack -G WIX`.
# ---------------------------------------------------------------------------
if (NOT CPACK_GENERATOR)
    set(CPACK_GENERATOR "ZIP")
endif()

include(CPack)

# ---------------------------------------------------------------------------
# Targets de conveniencia: generan el instalador con UN comando.  NSIS se
# AUTO-DESCARGA (portable, 2.3 MB) si no esta en el sistema -- el usuario no
# instala nada a mano (igual que la auto-descarga de OpenSSL).
#
#   cmake --build <build> --target installer        # -> VestaVM-<ver>-win64.exe
#   cmake --build <build> --target installer-zip     # -> VestaVM-<ver>-win64.zip
# ---------------------------------------------------------------------------

# Binarios que el instalador empaqueta (se construyen antes de cpack).
set(_vesta_pkg_targets vm)
foreach(_t vesta_lsp vesta_ffi vesta_gc vesta_collections_a vesta_math_a
           vesta_io vesta_math vesta_collections vx_trace)
    if (TARGET ${_t})
        list(APPEND _vesta_pkg_targets ${_t})
    endif()
endforeach()

# .exe NSIS (con auto-descarga de NSIS si falta).
add_custom_target(installer
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR}
                -P "${CMAKE_SOURCE_DIR}/cmake/MakeInstallerNSIS.cmake"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        COMMENT "Generando instalador .exe (NSIS auto-descargado si no esta)")
add_dependencies(installer ${_vesta_pkg_targets})

# .zip portable (no requiere herramientas externas).
add_custom_target(installer-zip
        COMMAND ${CMAKE_CPACK_COMMAND} -G ZIP --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
        COMMENT "Generando paquete .zip portable")
add_dependencies(installer-zip ${_vesta_pkg_targets})

# El target estandar `package` (creado por include(CPack)) tambien debe construir
# los binarios antes de empaquetar -- vesta_lsp/vesta_gc/vesta_ffi NO estan en el
# target `all`, asi que sin esto `cmake --build --target package` empaquetaria
# binarios obsoletos o inexistentes.
if (TARGET package)
    add_dependencies(package ${_vesta_pkg_targets})
endif()
