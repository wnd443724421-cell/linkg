cmake_minimum_required(VERSION 3.10)

project(LinkG_Tools C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LINKG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LINKG_OUTPUT_BIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/output/bin)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${LINKG_OUTPUT_BIN_DIR})

add_executable(cmain
    cmain/cmain.c
)

target_include_directories(cmain PRIVATE
    ${LINKG_ROOT}/include
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET cmain POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:cmain>
        COMMENT "Stripping cmain"
    )
endif()
