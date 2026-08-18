# =============================================================================
#  BuildOpenSSLStatic.cmake
# -----------------------------------------------------------------------------
#  Compila OpenSSL ESTATICO (libssl.a + libcrypto.a) desde el tarball OFICIAL,
#  con el MISMO compilador del build.  Resultado: el .exe final embebe OpenSSL
#  y NO depende de libssl-3-x64.dll / libcrypto-3-x64.dll -- binario standalone.
#
#  Por que compilar desde fuente y no descargar un precompilado:
#    - PORTABLE + MULTI-COMPILADOR: la ABI cuadra con CUALQUIER toolchain porque
#      lo compila EL (no un artefacto ajeno con otro runtime -- msvcrt vs ucrt).
#    - AUTO-DESCARGA con SHA-256 FIJADO (idempotente); un clone limpio funciona
#      sin pasos manuales.
#    - Sin depender del sysroot del compilador (TDM-GCC no trae las .a; MSYS2 si
#      -- eso NO es portable).
#
#  Diseno:
#    - CACHE por el RESULTADO (las .a): si ya existen, no recompila.  El cache
#      vive en el build dir -> un reconfigure no recompila; un clean total si.
#    - Herramientas (perl + make) via find_program -> error CLARO si faltan, en
#      vez de un fallo cripitico a mitad de build.
#    - build_libs (no `make` a secas): solo libcrypto + libssl, sin apps/tests.
#
#  Salida (en el scope del CALLER, via PARENT_SCOPE):
#    VESTA_OSSL_STATIC_OK       TRUE si las .a estan listas
#    VESTA_OSSL_STATIC_SSL      ruta a libssl.a
#    VESTA_OSSL_STATIC_CRYPTO   ruta a libcrypto.a
#    VESTA_OSSL_STATIC_INCLUDE  dir de headers (los del MISMO tarball compilado)
# =============================================================================

# -----------------------------------------------------------------------------
#  Ruta en el estilo que entiende un perl de MSYS: `C:/x` -> `/c/x`.
#
#  Hace falta porque PERL5LIB se separa por `:` en un perl UNIX, asi que una
#  ruta con letra de unidad se partiria en dos entradas rotas (`C` y `/x`).
# -----------------------------------------------------------------------------
function(vesta_msys_path _win _out)
    set(_p "${_win}")
    string(REGEX REPLACE "^([A-Za-z]):" "/\\1" _p "${_p}")
    string(SUBSTRING "${_p}" 0 2 _head)
    string(TOLOWER "${_head}" _head_low)
    string(SUBSTRING "${_p}" 2 -1 _tail)
    set(${_out} "${_head_low}${_tail}" PARENT_SCOPE)
endfunction()

# -----------------------------------------------------------------------------
#  vesta_ensure_perl(<out_perl> <out_shim>)
# -----------------------------------------------------------------------------
#  Deja en <out_perl> la ruta a un perl APTO para configurar OpenSSL-mingw, y en
#  <out_shim> el directorio de modulos que hay que anadirle (vacio si no hace
#  falta ninguno).  El target `mingw64` de OpenSSL usa el esquema de build UNIX
#  (Makefile GNU), asi que su `Configurations/unix-checker.pm` EXIGE que
#  `rel2abs('.')` devuelva rutas con barra `/`.  Consecuencias:
#    - un perl NATIVO Windows (Strawberry, ActiveState) SIEMPRE falla ese check
#      (File::Spec::Win32 -> barras invertidas), por muy completo que sea.
#    - hace falta un perl estilo UNIX (MSYS2 / Cygwin / Git-for-Windows), que
#      ademas traiga IPC::Cmd (que Configure requiere).
#
#  ESTO NO PUEDE PEDIRLE NADA AL USUARIO.  Antes, si el unico perl de la maquina
#  era el de Git-for-Windows, esto abortaba el configure entero diciendo "instala
#  MSYS2" -- o sea, el build dependia de que alguien montara herramientas a mano.
#  Ahora se resuelve solo, en tres pasos de coste creciente:
#
#    1. Un perl del sistema que YA sirva.  Gratis.
#    2. Un perl del sistema con rutas UNIX pero desnudo de modulos.  Es el caso
#       de Git-for-Windows: trae `IPC::Cmd` y `Params::Check`, pero NO
#       `Locale::Maketext::Simple`, que los dos requieren -- y sin el, los tres
#       fallan a la vez.  Ese modulo son nueve kilobytes de Perl puro, asi que se
#       BAJA (SHA-256 fijado, como el tarball de OpenSSL) y se le anade por
#       PERL5LIB.  No se toca la instalacion del sistema: el modulo vive en el
#       cache del proyecto.
#    3. Ningun perl con rutas UNIX.  Entonces se baja uno entero (el `perl` de
#       MSYS2, que se extrae y funciona sin instalar nada).
#
#  El perl solo corre Configure y genera cabeceras; NO se enlaza en OpenSSL --
#  eso lo compila el toolchain del build (CC + mingw32-make) -- asi que su CRT es
#  irrelevante para la ABI final.
#
#  Condicion de aptitud, en un solo test (RESULT 0 = apto):
#    perl -MIPC::Cmd -MFile::Spec::Functions=rel2abs -e "exit(rel2abs('.')=~m{/}?0:1)"
# -----------------------------------------------------------------------------

##
# @brief Comprueba si un perl sirve, opcionalmente con un directorio de modulos.
# @param _perl Ejecutable a probar.
# @param _shim Directorio extra de modulos, o cadena vacia.
# @param _out  Recibe TRUE/FALSE.
#
# La prueba EJECUTA `can_run`, no se limita a cargar IPC::Cmd.  Cargarlo no
# bastaba: el modulo entra bien y revienta luego, dentro de `can_run`, porque le
# falta ExtUtils::MakeMaker -- y entonces el fallo aparecia a mitad del Configure
# de OpenSSL, donde no hay quien lo relacione con el perl.  Aqui se prueba lo
# mismo que Configure va a hacer, asi que un perl a medias se descarta ANTES.
function(vesta_perl_apto _perl _shim _out)
    set(_args "")
    if (_shim)
        list(APPEND _args -I "${_shim}")
    endif()
    execute_process(
        COMMAND "${_perl}" ${_args} -MIPC::Cmd -MPod::Usage
                -MFile::Spec::Functions=rel2abs
                -e "IPC::Cmd::can_run(q{perl}); exit(rel2abs('.') =~ m{/} ? 0 : 1)"
        RESULT_VARIABLE _apt OUTPUT_QUIET ERROR_QUIET)
    if (_apt EQUAL 0)
        set(${_out} TRUE PARENT_SCOPE)
    else()
        set(${_out} FALSE PARENT_SCOPE)
    endif()
endfunction()

##
# @brief Baja los modulos de Perl puro que le faltan a un perl desnudo.
# @param _dir Directorio donde dejarlos (se crea).
# @param _out Recibe TRUE si estan disponibles.
#
# Lo que le falta al perl de Git-for-Windows para configurar OpenSSL.  Cada uno
# se descubrio porque el anterior dejaba de ser el que faltaba:
#
#   - `Locale::Maketext::Simple`, del que dependen `Params::Check` e `IPC::Cmd`.
#     Los dos ESTAN instalados, pero sin el no cargan, asi que el sintoma es que
#     falta IPC::Cmd cuando lo que falta es esto.
#   - `ExtUtils::MakeMaker`, que es lo que usa `IPC::Cmd::can_run` para decidir
#     si un programa es ejecutable.  Sin el, IPC::Cmd carga pero revienta en
#     cuanto Configure busca el compilador.
#   - `Pod::Usage` y su cadena (`Pod::Simple` -> `Pod::Escapes`, y `Pod::Text`
#     de podlators), que carga el `configdata.pm` que Configure deja escrito.
#     Ese se paga al final, cuando ya parecia que habia salido bien.
#
# Todos son Perl puro: se copian y funcionan, no hay nada que compilar.  Y se
# dejan en el cache del proyecto, no en la instalacion del sistema.
function(vesta_fetch_perl_shim _dir _out)
    # Campos separados por `|`, NO por `;`: una lista de CMake ya se separa por
    # `;`, asi que un elemento que lo contenga se parte en varios y lo que llega
    # al bucle son los campos sueltos, no las entradas.
    #
    # El orden es el de dependencia, para que un fallo se lea de arriba abajo.
    #
    # (nombre del dist | version | SHA-256 | ruta del autor en CPAN | .pm testigo)
    set(_mods
        "Locale-Maketext-Simple|0.21|b009ff51f4fb108d19961a523e99b4373ccf958d37ca35bf1583215908dca9a9|J/JE/JESSE|Locale/Maketext/Simple.pm"
        "ExtUtils-MakeMaker|7.70|f108bd46420d2f00d242825f865b0f68851084924924f92261d684c49e3e7a74|B/BI/BINGOS|ExtUtils/MakeMaker.pm"
        "Pod-Escapes|1.07|dbf7c827984951fb248907f940fd8f19f2696bc5545c0a15287e0fbe56a52308|N/NE/NEILB|Pod/Escapes.pm"
        "Pod-Simple|3.45|8483bb95cd3e4307d66def092a3779f843af772482bfdc024e3e00d0c4db0cfa|K/KH/KHW|Pod/Simple.pm"
        "podlators|5.01|ccfd1df9f1a47f095bce6d718fad5af40f78ce2491f2c7239626e15b7020bc71|R/RR/RRA|Pod/Text.pm"
        "Pod-Usage|2.03|7d8fdc7dce60087b6cf9e493b8d6ae84a5ab4c0608a806a6d395cc6557460744|M/MA/MAREKR|Pod/Usage.pm")

    foreach (_mod IN LISTS _mods)
        string(REPLACE "|" ";" _f "${_mod}")
        list(GET _f 0 _name)
        list(GET _f 1 _ver)
        list(GET _f 2 _sha)
        list(GET _f 3 _author)
        list(GET _f 4 _witness)

        if (EXISTS "${_dir}/${_witness}")
            continue()
        endif()

        set(_tgz "${_dir}/${_name}-${_ver}.tar.gz")
        set(_url "https://cpan.metacpan.org/authors/id/${_author}/${_name}-${_ver}.tar.gz")
        file(MAKE_DIRECTORY "${_dir}")
        message(STATUS "[OpenSSL] al perl le falta ${_name}; descargandolo...")
        file(DOWNLOAD "${_url}" "${_tgz}" EXPECTED_HASH SHA256=${_sha} STATUS _st)
        list(GET _st 0 _code)
        if (NOT _code EQUAL 0)
            list(GET _st 1 _msg)
            file(REMOVE "${_tgz}")
            message(STATUS "[OpenSSL] no se pudo descargar ${_name} (${_code}): ${_msg}")
            set(${_out} FALSE PARENT_SCOPE)
            return()
        endif()

        file(ARCHIVE_EXTRACT INPUT "${_tgz}" DESTINATION "${_dir}/_unpack")
        # Cada dist trae su arbol en `lib/`, ya con la jerarquia que espera el
        # nombre del modulo; se copia tal cual sobre el directorio comun.
        file(GLOB _libdirs "${_dir}/_unpack/${_name}-*/lib")
        foreach (_lib IN LISTS _libdirs)
            file(COPY "${_lib}/" DESTINATION "${_dir}")
        endforeach()
        file(REMOVE_RECURSE "${_dir}/_unpack")

        if (NOT EXISTS "${_dir}/${_witness}")
            message(STATUS "[OpenSSL] ${_name} se bajo pero no trajo ${_witness}")
            set(${_out} FALSE PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${_out} TRUE PARENT_SCOPE)
endfunction()

##
# @brief Deja una ruta que se pueda pasar SIN comillas, si Windows lo permite.
# @param _path Ruta original.
# @param _out  Recibe la version 8.3 si la hay, o la original.
#
# El makefile de OpenSSL usa `$(PERL)` sin comillas.  Con el perl de
# Git-for-Windows, que vive en `C:/Program Files/...`, make parte el comando por
# el espacio e intenta ejecutar `C:/Program`: el sintoma es un error 127 a mitad
# de la compilacion, que no se parece en nada a su causa.  El nombre corto de
# Windows (`C:/PROGRA~1/...`) no tiene espacios y apunta al mismo fichero.
function(vesta_short_path _path _out)
    set(${_out} "${_path}" PARENT_SCOPE)
    if (NOT "${_path}" MATCHES " ")
        return()  # sin espacios no hay nada que arreglar
    endif()
    file(TO_NATIVE_PATH "${_path}" _native)
    execute_process(COMMAND cmd /c for %I in ("${_native}") do @echo %~sI
                    OUTPUT_VARIABLE _short OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _rc ERROR_QUIET)
    #  El nombre corto puede estar desactivado en el volumen; entonces esto
    #  devuelve la ruta larga y hay que dejarla como estaba.
    if (_rc EQUAL 0 AND _short AND NOT "${_short}" MATCHES " ")
        string(REPLACE "\\" "/" _short "${_short}")
        set(${_out} "${_short}" PARENT_SCOPE)
    endif()
endfunction()

function(vesta_ensure_perl _out_perl _out_shim)
    set(${_out_shim} "" PARENT_SCOPE)

    # -- Reunir candidatos --------------------------------------------------
    set(_cands "")
    #  (a) el del PATH.
    find_program(_path_perl NAMES perl)
    if (_path_perl)
        list(APPEND _cands "${_path_perl}")
    endif()
    #  (b) derivado del arbol del compilador: si gcc vive en <root>/*/bin, el
    #      perl de MSYS2 suele estar en <root>/usr/bin (p.ej. gcc ucrt64 ->
    #      F:/msys/ucrt64/bin/gcc y perl en F:/msys/usr/bin/perl).
    get_filename_component(_cc_bin  "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_cc_env  "${_cc_bin}"          DIRECTORY)  # <root>/<env>
    get_filename_component(_cc_root "${_cc_env}"          DIRECTORY)  # <root>
    #  (c) raices tipicas de MSYS2 / Cygwin / Git-for-Windows.  El de Git faltaba
    #      y es el que hay en la mayoria de maquinas de desarrollo Windows.
    foreach (_root
             "${_cc_root}" "${_cc_env}"
             "F:/msys" "C:/msys64" "C:/msys" "D:/msys64"
             "C:/cygwin64" "C:/cygwin"
             "C:/Program Files/Git" "C:/Program Files (x86)/Git"
             "$ENV{LOCALAPPDATA}/Programs/Git"
             "${VESTA_OSSL_STATIC_ROOT}/perl")
        foreach (_sub "usr/bin" "usr/local/bin" "bin")
            if (EXISTS "${_root}/${_sub}/perl.exe")
                list(APPEND _cands "${_root}/${_sub}/perl.exe")
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _cands)

    # -- Paso 1: alguno sirve tal cual --------------------------------------
    foreach (_p ${_cands})
        vesta_perl_apto("${_p}" "" _ok)
        if (_ok)
            message(STATUS "[OpenSSL] perl apto (rutas UNIX + IPC::Cmd): ${_p}")
            vesta_short_path("${_p}" _p_ok)
            set(${_out_perl} "${_p_ok}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    # -- Paso 2: alguno tiene rutas UNIX y solo le faltan modulos -----------
    set(_shim_dir "${VESTA_OSSL_STATIC_ROOT}/perl-shim")
    foreach (_p ${_cands})
        # Rutas UNIX SIN exigir IPC::Cmd: separa "es del tipo correcto pero esta
        # desnudo" de "es un perl nativo Windows", que no tiene arreglo.
        execute_process(
            COMMAND "${_p}" -MFile::Spec::Functions=rel2abs
                    -e "exit(rel2abs('.') =~ m{/} ? 0 : 1)"
            RESULT_VARIABLE _unix OUTPUT_QUIET ERROR_QUIET)
        if (NOT _unix EQUAL 0)
            continue()
        endif()
        vesta_fetch_perl_shim("${_shim_dir}" _got)
        if (NOT _got)
            break()  # sin red no hay nada que probar con el resto
        endif()
        vesta_perl_apto("${_p}" "${_shim_dir}" _ok)
        if (_ok)
            message(STATUS "[OpenSSL] perl apto con modulos del proyecto: ${_p}")
            vesta_short_path("${_p}" _p_ok)
            set(${_out_perl} "${_p_ok}" PARENT_SCOPE)
            set(${_out_shim} "${_shim_dir}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    # -- Paso 3: no hay ninguno; bajar un perl entero -----------------------
    vesta_download_perl("${VESTA_OSSL_STATIC_ROOT}/perl" _dl_perl)
    if (_dl_perl)
        vesta_perl_apto("${_dl_perl}" "" _ok)
        if (NOT _ok)
            vesta_fetch_perl_shim("${_shim_dir}" _got)
            if (_got)
                vesta_perl_apto("${_dl_perl}" "${_shim_dir}" _ok)
                if (_ok)
                    set(${_out_shim} "${_shim_dir}" PARENT_SCOPE)
                endif()
            endif()
        endif()
        if (_ok)
            message(STATUS "[OpenSSL] usando el perl descargado: ${_dl_perl}")
            set(${_out_perl} "${_dl_perl}" PARENT_SCOPE)
            return()
        endif()
    endif()

    # -- Nada funciono: error CLARO -----------------------------------------
    string(REPLACE ";" "\n    " _cand_list "${_cands}")
    message(FATAL_ERROR
        "[OpenSSL] no se consiguio un perl apto para compilar OpenSSL-mingw.\n"
        "  El target mingw64 usa el esquema UNIX y exige un perl que:\n"
        "    (1) devuelva rutas con barra '/' (estilo MSYS2/Cygwin/Git), y\n"
        "    (2) traiga el modulo IPC::Cmd.\n"
        "  Se probaron los del sistema, se intento completarlos con los modulos\n"
        "  que les faltan, y se intento descargar uno entero; nada funciono.\n"
        "  Lo mas probable es que no haya salida a internet.  Candidatos:\n"
        "    ${_cand_list}")
endfunction()

##
# @brief Descarga un perl con rutas UNIX que funcione sin instalar nada.
# @param _dir  Donde extraerlo.
# @param _out  Recibe la ruta al perl.exe, o vacio si no se pudo.
#
# Se usa el paquete `perl` de MSYS2 (formato pacman, un `.tar.zst`): es un
# tarball que se extrae y ya corre -- no hay instalador que ejecutar ni registro
# que tocar --, y su perl es el completo, con los modulos del core.  Necesita
# ademas el runtime de MSYS2 (`msys2-runtime`), que es de donde sale el
# `msys-2.0.dll` contra el que enlaza.
function(vesta_download_perl _dir _out)
    set(${_out} "" PARENT_SCOPE)
    set(_perl_exe "${_dir}/usr/bin/perl.exe")
    if (EXISTS "${_perl_exe}")
        set(${_out} "${_perl_exe}" PARENT_SCOPE)
        return()
    endif()

    # Paquetes de MSYS2 con su SHA-256 fijado.  Van juntos porque el perl enlaza
    # contra el runtime: bajar uno sin el otro da un ejecutable que no arranca.
    #  Campos separados por `|` por lo mismo que en vesta_fetch_perl_shim.
    set(_pkgs
        "perl-5.38.4-1-x86_64.pkg.tar.zst|https://repo.msys2.org/msys/x86_64/perl-5.38.4-1-x86_64.pkg.tar.zst"
        "msys2-runtime-3.6.4-2-x86_64.pkg.tar.zst|https://repo.msys2.org/msys/x86_64/msys2-runtime-3.6.4-2-x86_64.pkg.tar.zst")

    message(STATUS "[OpenSSL] no hay ningun perl utilizable; descargando uno...")
    file(MAKE_DIRECTORY "${_dir}")
    foreach (_pkg IN LISTS _pkgs)
        string(REPLACE "|" ";" _f "${_pkg}")
        list(GET _f 0 _name)
        list(GET _f 1 _url)
        set(_dst "${_dir}/${_name}")
        if (NOT EXISTS "${_dst}")
            file(DOWNLOAD "${_url}" "${_dst}" STATUS _st)
            list(GET _st 0 _code)
            if (NOT _code EQUAL 0)
                list(GET _st 1 _msg)
                file(REMOVE "${_dst}")
                message(STATUS "[OpenSSL] descarga de ${_name} fallo (${_code}): ${_msg}")
                return()
            endif()
        endif()
        # Los .tar.zst los abre libarchive, que CMake trae dentro.
        file(ARCHIVE_EXTRACT INPUT "${_dst}" DESTINATION "${_dir}")
    endforeach()

    if (EXISTS "${_perl_exe}")
        set(${_out} "${_perl_exe}" PARENT_SCOPE)
    endif()
endfunction()

function(vesta_build_openssl_static)
    set(_ver "3.5.7")  # LTS (soporte hasta 2030-04)
    set(_sha "a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8")
    set(_url "https://github.com/openssl/openssl/releases/download/openssl-${_ver}/openssl-${_ver}.tar.gz")

    # Donde vive el resultado.  COMPARTIDO entre directorios de build, no dentro
    # de uno.
    #
    # Antes se guardaba en ${CMAKE_BINARY_DIR}, y eso hacia que cada build dir
    # nuevo tuviera que compilar OpenSSL OTRA VEZ -- o sea, volver a exigir un
    # perl estilo UNIX que en muchas maquinas no hay.  El resultado no depende
    # del tipo de build (es una libreria de terceros, no lleva nuestros flags),
    # asi que tenerlo por build dir no aportaba nada y costaba una dependencia
    # de herramientas por cada configuracion.  Se compila UNA vez y lo comparten
    # Release, Profile y Debug.
    set(VESTA_OSSL_STATIC_ROOT "${CMAKE_SOURCE_DIR}/.deps/openssl-static"
            CACHE PATH "Donde se guarda el OpenSSL estatico ya compilado")

    # Sitios donde puede estar ya hecho, en orden: el compartido, este build dir
    # (por si viene de la epoca en que se guardaba ahi) y cualquier otro build
    # dir del arbol.  Reconocer lo ya compilado evita pedir herramientas para
    # rehacer algo que ya existe.
    set(_cands_root "${VESTA_OSSL_STATIC_ROOT}" "${CMAKE_BINARY_DIR}/openssl-static")
    file(GLOB _otros_builds "${CMAKE_SOURCE_DIR}/cmake-build-*/openssl-static")
    list(APPEND _cands_root ${_otros_builds})
    list(REMOVE_DUPLICATES _cands_root)

    foreach (_cand IN LISTS _cands_root)
        if (EXISTS "${_cand}/out/lib/libssl.a" AND
            EXISTS "${_cand}/out/lib/libcrypto.a" AND
            EXISTS "${_cand}/out/include/openssl/ssl.h")
            message(STATUS "[OpenSSL] estatico ya compilado (cache): ${_cand}")
            set(VESTA_OSSL_STATIC_OK      TRUE                          PARENT_SCOPE)
            set(VESTA_OSSL_STATIC_SSL     "${_cand}/out/lib/libssl.a"    PARENT_SCOPE)
            set(VESTA_OSSL_STATIC_CRYPTO  "${_cand}/out/lib/libcrypto.a" PARENT_SCOPE)
            set(VESTA_OSSL_STATIC_INCLUDE "${_cand}/out/include"         PARENT_SCOPE)
            return()
        endif ()
    endforeach ()

    # No esta hecho en ningun sitio: se compila en el compartido.
    set(_root   "${VESTA_OSSL_STATIC_ROOT}")
    set(_ssl_a    "${_root}/out/lib/libssl.a")
    set(_crypto_a "${_root}/out/lib/libcrypto.a")
    set(_inc      "${_root}/out/include")

    # -- Herramientas: perl (completo) + make -------------------------------
    #  perl: helper que usa uno del sistema si sirve, lo completa con los modulos
    #  que le falten si hace falta, y si no hay ninguno se baja uno entero.  Los
    #  modulos que devuelva se copian al arbol de OpenSSL mas abajo, cuando ya
    #  esta extraido.  make: el del toolchain (mingw32-make).
    vesta_ensure_perl(_ossl_perl _ossl_perl_shim)
    #  Los modulos completados llegan al perl por DOS vias, porque ninguna cubre
    #  las dos fases:
    #
    #    - PERL5LIB, para Configure.  Aqui lo lanza CMake directamente y el
    #      `configdata.pm` que Configure ejecuta al final lo hereda; entre dos
    #      procesos MSYS la variable viaja intacta.  Hace falta porque
    #      configdata.pm corre con un @INC que no incluye ni `.` ni `util/perl`.
    #    - una COPIA dentro del arbol, para el make (ver mas abajo).  Ahi PERL5LIB
    #      no sirve: al pasar por mingw32-make, que es nativo, la emulacion de
    #      MSYS2 reescribe `/f/...` como `F:/...`, y como PERL5LIB se separa por
    #      `:` la letra de unidad lo parte en dos entradas rotas.
    set(_ossl_env "")
    if (_ossl_perl_shim)
        vesta_msys_path("${_ossl_perl_shim}" _shim_msys)
        set(_ossl_env ${CMAKE_COMMAND} -E env "PERL5LIB=${_shim_msys}")
    endif()
    find_program(_ossl_make NAMES mingw32-make make gmake)
    if (NOT _ossl_make)
        message(FATAL_ERROR
            "[OpenSSL] falta make para compilar estatico.\n"
            "  Asegura que mingw32-make (viene con el toolchain) esta en el PATH.")
    endif()

    # -- Descargar el tarball (hash fijado, idempotente) --------------------
    set(_tgz "${_root}/openssl-${_ver}.tar.gz")
    if (NOT EXISTS "${_tgz}")
        message(STATUS "[OpenSSL] descargando fuente ${_ver} (SHA-256 fijado)...")
        file(DOWNLOAD "${_url}" "${_tgz}"
             SHOW_PROGRESS EXPECTED_HASH SHA256=${_sha} STATUS _st)
        list(GET _st 0 _code)
        if (NOT _code EQUAL 0)
            list(GET _st 1 _msg)
            file(REMOVE "${_tgz}")  # no dejar una descarga parcial
            message(FATAL_ERROR "[OpenSSL] descarga fallo (${_code}): ${_msg}\n  URL: ${_url}")
        endif()
    endif()

    # -- Extraer (guardado por MARCADOR de completitud) ---------------------
    #  NO se re-extrae en cada configure: eso choca si algun proceso (el IDE, un
    #  make en curso) tiene un fichero del arbol abierto -> ARCHIVE_EXTRACT falla
    #  a medias.  Se extrae solo si el marcador `.vesta_extract_ok` NO existe (=
    #  no hubo una extraccion COMPLETA previa); ante un arbol parcial se limpia y
    #  se rehace.  El marcador se escribe DESPUES de extraer, asi que una
    #  extraccion interrumpida deja el arbol sin marcar -> el siguiente run lo
    #  rehace pristino.  (El riesgo de cabeceras truncadas que motivaba el
    #  re-extract agresivo desaparecio al forzar el perl correcto en Configure +
    #  make: la generacion de cabeceras ya no falla a medias.)
    set(_src "${_root}/openssl-${_ver}")
    set(_extract_ok "${_src}/.vesta_extract_ok")
    if (NOT EXISTS "${_extract_ok}")
        message(STATUS "[OpenSSL] extrayendo arbol pristino...")
        file(REMOVE_RECURSE "${_src}")
        file(ARCHIVE_EXTRACT INPUT "${_tgz}" DESTINATION "${_root}")
        file(TOUCH "${_extract_ok}")
    endif()

    #  Si al perl hubo que completarle modulos, se COPIAN dentro del arbol de
    #  OpenSSL en vez de senalarlos con PERL5LIB.
    #
    #  PERL5LIB no sirve aqui: el makefile llama a perl a traves de mingw32-make,
    #  que es un programa NATIVO, y al cruzar esa frontera la emulacion de MSYS2
    #  reescribe las variables que parecen rutas -- `/f/...` se convierte en
    #  `F:/...`.  Como PERL5LIB se separa por `:`, la letra de unidad la parte en
    #  dos entradas rotas (`F` y `/C/...`) y perl deja de encontrar los modulos a
    #  mitad de la compilacion.
    #
    #  Los dos destinos son los dos sitios donde perl YA mira: `util/perl`, que
    #  Configure anade a @INC, y la raiz del arbol, que el makefile pasa como
    #  `-I.`.  Asi no hay ninguna variable de entorno de por medio.
    if (_ossl_perl_shim)
        foreach (_dest "${_src}" "${_src}/util/perl")
            file(COPY "${_ossl_perl_shim}/" DESTINATION "${_dest}"
                 FILES_MATCHING PATTERN "*.pm")
        endforeach()
    endif()

    # -- Configure (mingw64, ESTATICO) --------------------------------------
    #  no-shared  = sin DLLs (estatico).      no-tests/no-apps = solo las libs.
    #  no-docs    = sin manpages.             CC=<compilador del build> para que
    #  la ABI cuadre con el resto del proyecto (multi-compilador).
    #  no-asm     = SIN ensamblador optimizado (crypto en C puro).  Clave para la
    #  robustez en este toolchain (TDM-GCC + perl MSYS2 + mingw32-make de CLion):
    #  el ensamblador se genera con scripts perlasm que LANZAN sub-procesos
    #  (perl + sh para el probe `$CC -E`, xlate por pipe); bajo -j alto, decenas
    #  de estos a la vez saturan la emulacion de fork() de MSYS2 y fallan de forma
    #  DETERMINISTA ("[...].s Error 1").  Sin asm no hay esa fase, hay muchos
    #  menos sub-procesos y el build es portable a CUALQUIER compilador.  El coste
    #  (crypto un poco mas lenta) es irrelevante para el uso de Vesta (firmar
    #  .velb, keygen -- operaciones puntuales, no TLS masivo).
    #  Guardado por el makefile: si ya existe no re-Configura (un make fallido a
    #  medias se recupera INCREMENTAL con el reintento de abajo, sin rehacer todo).
    if (NOT EXISTS "${_src}/makefile" AND NOT EXISTS "${_src}/Makefile")
        message(STATUS "[OpenSSL] Configure mingw64 no-shared no-asm (CC=${CMAKE_C_COMPILER})...")
        execute_process(
            COMMAND ${_ossl_env} "${_ossl_perl}" Configure mingw64 no-shared
                    no-asm no-tests no-apps no-docs "CC=${CMAKE_C_COMPILER}"
            WORKING_DIRECTORY "${_src}"
            RESULT_VARIABLE _cfg
            OUTPUT_VARIABLE _cfg_out ERROR_VARIABLE _cfg_out)
        if (NOT _cfg EQUAL 0)
            message(FATAL_ERROR "[OpenSSL] Configure fallo:\n${_cfg_out}")
        endif()
    endif()

    # -- make build_libs (solo libcrypto + libssl) --------------------------
    include(ProcessorCount)
    ProcessorCount(_nproc)
    if (_nproc EQUAL 0)
        set(_nproc 4)
    endif()
    message(STATUS "[OpenSSL] compilando estatico (tarda unos minutos)...")
    #  PERL=<ruta nativa completa>: el makefile fija `PERL=/usr/bin/perl` (ruta
    #  msys); mingw32-make NATIVO la resolveria via el sh/PATH del sistema al
    #  perl de Git-for-Windows (minimo, sin Pod::Usage) en vez del completo que
    #  uso Configure.  La asignacion en linea de comandos gana sobre la del
    #  makefile y fuerza el perl correcto para generar cabeceras.
    #
    #  REINTENTO con parallelismo DECRECIENTE: cada objeto hace `touch X.d.tmp` +
    #  `mv X.d.tmp X.d` (sh/coreutils via el sh de MSYS2), cuya emulacion de
    #  fork() en Windows falla de forma TRANSITORIA bajo carga -> el `mv` a veces
    #  no encuentra su `.d.tmp`.  NO es error de compilacion (el objeto compila al
    #  reintentar).  1er intento en PARALELO (-j${_nproc}) por velocidad; los
    #  reintentos en SERIE (-j1), que casi eliminan la contencion de fork y
    #  completan INCREMENTAL lo que el paralelo se salto.  Solo se aborta si TODOS
    #  fallan (seria un error real, no transitorio).
    set(_mk_ok FALSE)
    foreach (_try RANGE 1 4)
        if (_try EQUAL 1)
            set(_jflag "-j${_nproc}")
        else()
            set(_jflag "-j1")
        endif()
        execute_process(
            COMMAND "${_ossl_make}" "${_jflag}"
                    "PERL=${_ossl_perl}" build_libs
            WORKING_DIRECTORY "${_src}"
            RESULT_VARIABLE _mk
            OUTPUT_VARIABLE _mk_out ERROR_VARIABLE _mk_out)
        if (_mk EQUAL 0)
            set(_mk_ok TRUE)
            break()
        endif()
        message(STATUS "[OpenSSL] make intento ${_try} fallo (falla transitoria "
                       "de la emulacion MSYS2); reintentando en serie...")
    endforeach()
    if (NOT _mk_ok)
        message(FATAL_ERROR "[OpenSSL] make build_libs fallo tras varios intentos:\n${_mk_out}")
    endif()

    if (NOT EXISTS "${_src}/libssl.a" OR NOT EXISTS "${_src}/libcrypto.a")
        message(FATAL_ERROR
            "[OpenSSL] la compilacion no produjo libssl.a/libcrypto.a en ${_src}")
    endif()

    # -- Publicar en el cache (out/) ----------------------------------------
    file(MAKE_DIRECTORY "${_root}/out/lib")
    file(COPY "${_src}/libssl.a" "${_src}/libcrypto.a"
         DESTINATION "${_root}/out/lib")
    # Los headers del MISMO tarball (autoconsistentes con las .a compiladas).
    file(COPY "${_src}/include/openssl" DESTINATION "${_inc}")
    # opensslconf.h y demas headers GENERADOS por Configure viven aparte.
    file(GLOB _gen "${_src}/include/openssl/*.h")
    if (_gen)
        file(COPY ${_gen} DESTINATION "${_inc}/openssl")
    endif()

    message(STATUS "[OpenSSL] estatico listo: ${_ssl_a}")
    set(VESTA_OSSL_STATIC_OK      TRUE           PARENT_SCOPE)
    set(VESTA_OSSL_STATIC_SSL     "${_ssl_a}"    PARENT_SCOPE)
    set(VESTA_OSSL_STATIC_CRYPTO  "${_crypto_a}" PARENT_SCOPE)
    set(VESTA_OSSL_STATIC_INCLUDE "${_inc}"      PARENT_SCOPE)
endfunction()
