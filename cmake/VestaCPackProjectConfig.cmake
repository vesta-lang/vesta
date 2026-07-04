# ---------------------------------------------------------------------------
# VestaCPackProjectConfig.cmake -- overrides de CPack POR GENERADOR.
#
# CPack ejecuta este fichero UNA VEZ POR CADA generador que se empaqueta, con la
# variable ${CPACK_GENERATOR} ya fijada al generador actual.  Es el mecanismo
# soportado para dar valores distintos segun el formato de salida (a diferencia
# de un `-D` en la linea de cpack, que el CPackConfig.cmake generado pisa con su
# propio `set()`).
#
# Aqui ajustamos la "carpeta contenedora" (TOPLEVEL_DIRECTORY):
#   - ZIP / archivos: SI queremos la carpeta VestaVM-<ver>-win64/ dentro del zip
#     para que al extraer no se vuelquen los archivos sueltos en el cwd.
#   - NSIS (.exe): NO la queremos -- el instalador ya pide un <prefix>
#     (`$PROGRAMFILES64\VestaVM`); anadir la carpeta la ANIDA
#     (`...\VestaVM\VestaVM-<ver>-win64\`), que es justo el bug reportado.
# ---------------------------------------------------------------------------

if(CPACK_GENERATOR STREQUAL "NSIS" OR CPACK_GENERATOR STREQUAL "WIX")
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 0)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY 0)
else()
    # ZIP, TGZ, 7Z, etc.: conservar la carpeta contenedora.
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 1)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY 1)
endif()
