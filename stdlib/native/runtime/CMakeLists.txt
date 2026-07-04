cmake_minimum_required(VERSION 3.15)
include(VestaPlugin)
add_vesta_plugin(vx_trace
    SOURCES   vx_trace.c
    SDK_DIR   "${VESTA_SDK_DIR}"
    C_STANDARD 11
)
