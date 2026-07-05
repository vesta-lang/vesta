# MakeInstallerNSIS.cmake  --  genera el instalador .exe con NSIS.
#
# Se ejecuta en modo script:
#   cmake -DBUILD_DIR=<build> -P cmake/MakeInstallerNSIS.cmake
#
# Automatiza NSIS igual que el CMake principal automatiza OpenSSL: si makensis no
# esta en el sistema, descarga la distribucion portable de NSIS (2.3 MB, hash
# fijado) en <build>/_nsis y la usa.  El usuario NO tiene que instalar nada.
# Despues invoca cpack -G NSIS con makensis en el PATH.

if(NOT BUILD_DIR)
    message(FATAL_ERROR "[installer] falta -DBUILD_DIR=<build>")
endif()

set(NSIS_VERSION "3.10")
set(NSIS_SHA256  "fcdce3229717a2a148e7cda0ab5bdb667f39d8fb33ede1da8dabc336bd5ad110")
set(_nsis_root   "${BUILD_DIR}/_nsis/nsis-${NSIS_VERSION}")
set(_makensis    "${_nsis_root}/makensis.exe")
set(_nsis_bindir "")

# 1) makensis del sistema (si el usuario ya lo tiene instalado).
find_program(_sys_makensis NAMES makensis)
if(_sys_makensis)
    get_filename_component(_nsis_bindir "${_sys_makensis}" DIRECTORY)
    message(STATUS "[NSIS] usando makensis del sistema: ${_sys_makensis}")

# 2) makensis ya descargado en una invocacion previa.
elseif(EXISTS "${_makensis}")
    set(_nsis_bindir "${_nsis_root}")
    message(STATUS "[NSIS] usando makensis descargado: ${_makensis}")

# 3) Descargar la distribucion portable de NSIS.
else()
    set(_url "https://downloads.sourceforge.net/project/nsis/NSIS%203/${NSIS_VERSION}/nsis-${NSIS_VERSION}.zip")
    set(_zip "${BUILD_DIR}/_nsis/nsis-${NSIS_VERSION}.zip")
    message(STATUS "[NSIS] no encontrado; descargando ${_url} (~2.3 MB)...")
    file(MAKE_DIRECTORY "${BUILD_DIR}/_nsis")
    if(NOT EXISTS "${_zip}")
        file(DOWNLOAD "${_url}" "${_zip}"
                SHOW_PROGRESS
                EXPECTED_HASH SHA256=${NSIS_SHA256}
                STATUS _st)
        list(GET _st 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _st 1 _msg)
            message(FATAL_ERROR "[NSIS] descarga fallo (${_code}): ${_msg}\n  URL: ${_url}\n  Alternativa: instala NSIS a mano (https://nsis.sourceforge.io) y reintenta.")
        endif()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${BUILD_DIR}/_nsis")
    if(NOT EXISTS "${_makensis}")
        message(FATAL_ERROR "[NSIS] makensis.exe no encontrado tras extraer en ${_nsis_root}")
    endif()
    set(_nsis_bindir "${_nsis_root}")
    message(STATUS "[NSIS] listo: ${_makensis}")
endif()

# ---------------------------------------------------------------------------
# Overlay "large strings" (NSIS_MAX_STRLEN=8192).  El build estandar de NSIS
# limita las cadenas a 1024 chars: si el PATH del sistema/usuario supera ese
# limite, AddToPath aborta con "PATH too long" (visto en Win10 con PATH largo).
# El build oficial strlen_8192 sube el limite a 8192 y cubre PATHs reales.  Se
# aplica SOLO al NSIS descargado por nosotros (no al makensis del sistema).
# ---------------------------------------------------------------------------
if(_nsis_bindir STREQUAL "${_nsis_root}" AND
   NOT EXISTS "${_nsis_root}/.strlen8192_applied")
    set(_ov_url "https://downloads.sourceforge.net/project/nsis/NSIS%203/${NSIS_VERSION}/nsis-${NSIS_VERSION}-strlen_8192.zip")
    set(_ov_zip "${BUILD_DIR}/_nsis/nsis-${NSIS_VERSION}-strlen_8192.zip")
    set(_ov_dir "${BUILD_DIR}/_nsis/_strlen8192")
    message(STATUS "[NSIS] aplicando overlay large-strings (strlen_8192) para PATHs largos...")
    # Nota: el hash NO se fija (a diferencia del NSIS base) -- es un binario de
    # toolchain build-time del MISMO proyecto NSIS en sourceforge (https).  Si se
    # quiere reproducibilidad estricta, fijar EXPECTED_HASH tras el primer bajado.
    if(NOT EXISTS "${_ov_zip}")
        file(DOWNLOAD "${_ov_url}" "${_ov_zip}" SHOW_PROGRESS STATUS _ovst)
        list(GET _ovst 0 _ovcode)
        if(NOT _ovcode EQUAL 0)
            list(GET _ovst 1 _ovmsg)
            message(WARNING "[NSIS] overlay strlen_8192 no se pudo descargar (${_ovcode}): ${_ovmsg}.\n  Se sigue con el NSIS estandar (limite de PATH 1024).")
        endif()
    endif()
    if(EXISTS "${_ov_zip}")
        file(REMOVE_RECURSE "${_ov_dir}")
        file(MAKE_DIRECTORY "${_ov_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_ov_zip}" DESTINATION "${_ov_dir}")
        # El zip trae (misma estructura que el NSIS base): Bin/makensis.exe (el
        # ejecutable REAL con strlen 8192), makensis.exe (wrapper en la raiz) y
        # Stubs/ (los stubs 8192 que se embeben en cada instalador generado).
        # Se copian SOBRE el NSIS base.
        if(EXISTS "${_ov_dir}/Bin/makensis.exe")
            file(COPY "${_ov_dir}/Bin/makensis.exe" DESTINATION "${_nsis_root}/Bin")
        endif()
        if(EXISTS "${_ov_dir}/makensis.exe")
            file(COPY "${_ov_dir}/makensis.exe" DESTINATION "${_nsis_root}")
        endif()
        if(EXISTS "${_ov_dir}/Stubs")
            file(COPY "${_ov_dir}/Stubs" DESTINATION "${_nsis_root}")
        endif()
        file(WRITE "${_nsis_root}/.strlen8192_applied" "")
        message(STATUS "[NSIS] overlay large-strings aplicado (limite de PATH ~8192).")
    endif()
endif()

# cpack vive junto al cmake que ejecuta este script.
get_filename_component(_cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
find_program(_cpack NAMES cpack PATHS "${_cmake_bin}" NO_DEFAULT_PATH)
if(NOT _cpack)
    set(_cpack "${_cmake_bin}/cpack")
endif()

# Poner makensis en el PATH y lanzar cpack -G NSIS.
#
# La carpeta contenedora (VestaVM-<ver>-win64/) se desactiva SOLO para el NSIS en
# cmake/VestaCPackProjectConfig.cmake (CPACK_PROJECT_CONFIG_FILE), para que el
# .exe instale en `$PROGRAMFILES64\VestaVM\` y no en el subdir anidado
# `...\VestaVM\VestaVM-<ver>-win64\`.
set(ENV{PATH} "${_nsis_bindir};$ENV{PATH}")
message(STATUS "[installer] generando .exe con NSIS...")
execute_process(
        COMMAND "${_cpack}" -G NSIS --config "${BUILD_DIR}/CPackConfig.cmake"
        WORKING_DIRECTORY "${BUILD_DIR}"
        RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "[installer] cpack -G NSIS fallo (codigo ${_rc})")
endif()
message(STATUS "[installer] instalador .exe generado en ${BUILD_DIR}")
