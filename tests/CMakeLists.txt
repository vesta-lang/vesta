cmake_minimum_required(VERSION 3.20)
project(VMTests)

# Forzar Debug si no se especifica
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Fuentes para ${FINAL_NAME}: ${ALL_PROJECT_SOURCES}")

# Helper function para crear tests individuales
function(add_single_test PREFIX TEST_FILE ALL_SOURCES)
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    set(FINAL_NAME "${PREFIX}_${TEST_NAME}")
    
    add_executable(${FINAL_NAME} ${TEST_FILE} ${ALL_SOURCES})
    
    target_include_directories(${FINAL_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/json
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/cxxopts
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/ftxui/include
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/capstone/include
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/keystone/include
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein
    )
    
    # SQLite
    target_link_libraries(${FINAL_NAME}
            PRIVATE sqlite3
            PRIVATE capstone
            PRIVATE keystone
            PRIVATE ftxui::screen
            PRIVATE ftxui::dom
            PRIVATE ftxui::component
    )
    
    # OpenSSL
    if(WIN32)
        target_include_directories(${FINAL_NAME} PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_link_directories(${FINAL_NAME} PRIVATE ${OPENSSL_LIB_DIR})
        target_link_libraries(${FINAL_NAME} PRIVATE libssl libcrypto ws2_32)
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(${FINAL_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    
    # pthread solo Linux/macOS
    if(NOT WIN32)
        target_link_libraries(${FINAL_NAME} PRIVATE pthread)
    endif()
    
    target_compile_options(${FINAL_NAME} PRIVATE
            -pedantic-errors -fpermissive -g -O0 -Wall -Wextra -DMODE_DEBUG_ENABLE
    )
endfunction()

# Todos los sources de src/
file(GLOB_RECURSE ALL_PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein_c.c
        ${CMAKE_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein.cpp
)

# Buscar todos los archivos .cpp dentro de tests/ y subcarpetas
file(GLOB_RECURSE TEST_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.cpp)

foreach(TEST_FILE ${TEST_FILES})
    # Crear un prefijo seguro basado en la ruta relativa
    get_filename_component(TEST_DIR ${TEST_FILE} DIRECTORY)
    string(REPLACE "/" "_" REL_DIR_SAFE ${TEST_DIR})
    add_single_test("test_${REL_DIR_SAFE}" ${TEST_FILE} "${ALL_PROJECT_SOURCES}")
endforeach()