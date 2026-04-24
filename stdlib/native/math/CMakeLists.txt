# stdlib/native/math/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)

include(VestaPlugin)

add_vesta_plugin(vesta_math
    SOURCES   vesta_math.c
    SDK_DIR   "${VESTA_SDK_DIR}"
    C_STANDARD 11
    # Enlazar libm en Linux/macOS; en Windows esta incluida en la CRT
    EXTRA_LINK_OPTIONS $<$<NOT:$<BOOL:${WIN32}>>:-lm>
)
