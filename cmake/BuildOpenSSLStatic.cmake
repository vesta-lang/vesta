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
#  vesta_ensure_perl(<out_var>)
# -----------------------------------------------------------------------------
#  Deja en <out_var> la ruta a un perl APTO para configurar OpenSSL-mingw.  El
#  target `mingw64` de OpenSSL usa el esquema de build UNIX (Makefile GNU), asi
#  que su `Configurations/unix-checker.pm` EXIGE que `rel2abs('.')` devuelva
#  rutas con barra `/`.  Consecuencias:
#    - un perl NATIVO Windows (Strawberry, ActiveState) SIEMPRE falla ese check
#      (File::Spec::Win32 -> barras invertidas), por muy completo que sea.
#    - hace falta un perl estilo UNIX (MSYS2 / Cygwin), que ademas traiga
#      IPC::Cmd (que Configure requiere).  El de Git-for-Windows suele venir
#      MINIMO (sin Locale::Maketext::Simple -> IPC::Cmd roto).
#
#  Por eso NO se descarga Strawberry: no serviria.  Se busca un perl que pase
#  AMBAS condiciones (rutas `/` + IPC::Cmd) entre los candidatos habituales.  El
#  perl solo corre Configure (un script); NO se enlaza en OpenSSL -- eso lo
#  compila el toolchain del build (CC + mingw32-make) -- asi que su CRT es
#  irrelevante para la ABI final.
#
#  Condicion de aptitud, en un solo test (RESULT 0 = apto):
#    perl -MIPC::Cmd -MFile::Spec::Functions=rel2abs -e "exit(rel2abs('.')=~m{/}?0:1)"
# -----------------------------------------------------------------------------
function(vesta_ensure_perl _out)
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
    #  (c) raices tipicas de MSYS2 / Cygwin.
    foreach (_root
             "${_cc_root}" "${_cc_env}"
             "F:/msys" "C:/msys64" "C:/msys" "D:/msys64"
             "C:/cygwin64" "C:/cygwin")
        foreach (_sub "usr/bin" "usr/local/bin" "bin")
            if (EXISTS "${_root}/${_sub}/perl.exe")
                list(APPEND _cands "${_root}/${_sub}/perl.exe")
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _cands)

    # -- Probar cada candidato: rutas UNIX (`/`) + IPC::Cmd ------------------
    foreach (_p ${_cands})
        execute_process(
            COMMAND "${_p}" -MIPC::Cmd -MFile::Spec::Functions=rel2abs
                    -e "exit(rel2abs('.') =~ m{/} ? 0 : 1)"
            RESULT_VARIABLE _apt OUTPUT_QUIET ERROR_QUIET)
        if (_apt EQUAL 0)
            message(STATUS "[OpenSSL] perl apto (rutas UNIX + IPC::Cmd): ${_p}")
            set(${_out} "${_p}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    # -- Ninguno sirve: error CLARO y accionable ----------------------------
    string(REPLACE ";" "\n    " _cand_list "${_cands}")
    message(FATAL_ERROR
        "[OpenSSL] no se encontro un perl apto para compilar OpenSSL-mingw.\n"
        "  El target mingw64 usa el esquema UNIX y exige un perl que:\n"
        "    (1) devuelva rutas con barra '/' (estilo MSYS2/Cygwin), y\n"
        "    (2) traiga el modulo IPC::Cmd.\n"
        "  Un perl NATIVO Windows (Strawberry/ActiveState) NO sirve (rutas '\\').\n"
        "  El de Git-for-Windows suele venir minimo (sin IPC::Cmd).\n"
        "  Solucion: instala MSYS2 y su perl (pacman -S perl) o Cygwin, o pon un\n"
        "  perl estilo UNIX en el PATH.  Candidatos probados:\n    ${_cand_list}")
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
    #  perl: helper que prefiere uno del sistema que sirva y, si no, baja
    #  Strawberry portable.  make: el del toolchain (mingw32-make).
    vesta_ensure_perl(_ossl_perl)
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
            COMMAND "${_ossl_perl}" Configure mingw64 no-shared no-asm no-tests
                    no-apps no-docs "CC=${CMAKE_C_COMPILER}"
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
            COMMAND "${_ossl_make}" "${_jflag}" "PERL=${_ossl_perl}" build_libs
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
