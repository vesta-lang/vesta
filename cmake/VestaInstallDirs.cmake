# =============================================================================
#  VestaInstallDirs.cmake -- donde va cada cosa de una instalacion de Vesta.
# -----------------------------------------------------------------------------
#  Se incluye PRONTO (antes que los plugins de la stdlib), porque
#  `add_vesta_plugin` necesita saber donde colocar su .dll/.so, y tambien desde
#  VestaPackaging.cmake, que declara el resto de reglas install().  Tener las
#  rutas en un solo sitio es lo que impide que las dos mitades de la
#  instalacion acaben apuntando a directorios distintos.
#
#  Incluirlo dos veces no hace nada: los `set()` son los mismos.
# =============================================================================

# ---------------------------------------------------------------------------
# DONDE va cada cosa, segun la plataforma.
# ---------------------------------------------------------------------------
# El ejecutable localiza su stdlib, su include_lib y sus plugins RELATIVOS A SI
# MISMO (`exe_dir/../stdlib`, ...), desde una quincena de sitios del codigo.
# Eso fija el layout, y las dos plataformas lo resuelven distinto:
#
#   Windows -- todo bajo un prefijo propio (`C:\Program Files\VestaVM`), con el
#     ejecutable en `bin/` y el arbol colgando de la raiz.
#
#   Linux -- una distribucion no admite un prefijo propio para cada programa,
#     pero SI un arbol privado en `/usr/lib/<paquete>` con un enlace simbolico
#     en `/usr/bin`.  Es lo que hacen Go (`/usr/lib/go-1.x`), Clang
#     (`/usr/lib/llvm-N`), OpenJDK (`/usr/lib/jvm/...`) y .NET
#     (`/usr/lib/dotnet`), y por la misma razon: `/proc/self/exe` RESUELVE el
#     enlace, asi que el binario se ve a si mismo dentro de su arbol y lo
#     encuentra todo sin que haya que compilarle ninguna ruta dentro.
#
# Asi el mismo juego de reglas install() sirve para el .exe de Windows y para
# el .deb, y no hay dos ideas de "que forma una instalacion".
if (WIN32)
    set(VESTA_INSTALL_BINDIR   "bin")      # ejecutables y bibliotecas de AOT
    set(VESTA_INSTALL_PRIVDIR  ".")        # stdlib/, include_lib/
    set(VESTA_INSTALL_DOCDIR   ".")        # README, LICENSE
    set(VESTA_INSTALL_DATADIR  ".")        # examples/, tools/
    set(VESTA_INSTALL_INCDIR   "include")  # cabeceras del SDK
    set(VESTA_INSTALL_LIBDIR   "lib")      # import library del SDK
    set(VESTA_INSTALL_CMAKEDIR "cmake")    # helper de CMake para plugins
    set(VESTA_EXE_NAME         "vesta.exe")
    set(VESTA_LSP_EXE_NAME     "vesta_lsp.exe")
else()
    set(VESTA_INSTALL_BINDIR   "lib/vesta/bin")
    set(VESTA_INSTALL_PRIVDIR  "lib/vesta")
    set(VESTA_INSTALL_DOCDIR   "share/doc/vesta")
    set(VESTA_INSTALL_DATADIR  "share/vesta")
    set(VESTA_INSTALL_INCDIR   "include/vesta")
    set(VESTA_INSTALL_LIBDIR   "lib/vesta")
    set(VESTA_INSTALL_CMAKEDIR "lib/vesta/cmake")
    set(VESTA_EXE_NAME         "vesta")
    set(VESTA_LSP_EXE_NAME     "vesta_lsp")
endif()
