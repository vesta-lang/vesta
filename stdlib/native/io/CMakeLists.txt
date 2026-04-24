# stdlib/native/io/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)

include(VestaPlugin)

add_vesta_plugin(vesta_io
    SOURCES   vesta_io.c
    SDK_DIR   "${VESTA_SDK_DIR}"
    C_STANDARD 11
)
