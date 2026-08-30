cmake_minimum_required(VERSION 3.12)

project(linkg VERSION 1.0.0 LANGUAGES C)

set(ARCHITECTURE "x86" CACHE STRING "Target architecture")
set_property(CACHE ARCHITECTURE PROPERTY STRINGS "x86" "arm")

if(NOT ARCHITECTURE MATCHES "^(x86|arm)$")
    message(FATAL_ERROR "Invalid ARCHITECTURE=${ARCHITECTURE}; expected x86 or arm")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output/bin/${ARCHITECTURE}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output/lib/${ARCHITECTURE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output/lib/${ARCHITECTURE}")

find_package(Threads REQUIRED)

# /****************************** 头文件目录 ******************************/

set(LINKG_INCLUDE_ROOTS
    "include"
    "core"
    "infra/config"
    "modules/cellular"
    "modules/wifi"
    "platform"
    "service"
    "third_party/cjson"
)

set(LINKG_INCLUDE_DIRS)

foreach(root IN LISTS LINKG_INCLUDE_ROOTS)
    file(GLOB_RECURSE root_headers CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${root}/*.h")

    foreach(header IN LISTS root_headers)
        get_filename_component(header_dir "${header}" DIRECTORY)
        list(APPEND LINKG_INCLUDE_DIRS "${header_dir}")
    endforeach()
endforeach()

list(REMOVE_DUPLICATES LINKG_INCLUDE_DIRS)

# /****************************** 源文件 ******************************/

file(GLOB_RECURSE LINKG_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/core/*.c"
)

file(GLOB_RECURSE LINKG_INFRA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/infra/*.c"
)

file(GLOB_RECURSE LINKG_MODULE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/modules/*.c"
)

file(GLOB_RECURSE LINKG_PLATFORM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/platform/*.c"
)

file(GLOB_RECURSE LINKG_SERVICE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/service/*.c"
)

file(GLOB_RECURSE LINKG_THIRD_PARTY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/third_party/cjson/*.c"
)

set(LINKG_MAIN_SOURCE "${CMAKE_SOURCE_DIR}/app/linkg_main.c")

if(NOT EXISTS "${LINKG_MAIN_SOURCE}")
    message(FATAL_ERROR "Main source not found: ${LINKG_MAIN_SOURCE}")
endif()

set(LINKG_SOURCES
    "${LINKG_MAIN_SOURCE}"
    ${LINKG_CORE_SOURCES}
    ${LINKG_INFRA_SOURCES}
    ${LINKG_MODULE_SOURCES}
    ${LINKG_PLATFORM_SOURCES}
    ${LINKG_SERVICE_SOURCES}
    ${LINKG_THIRD_PARTY_SOURCES}
)

# /****************************** 可执行程序 ******************************/

add_executable(${PROJECT_NAME}
    ${LINKG_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${LINKG_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Threads::Threads
    m
    rt
)

# /****************************** 编译选项 ******************************/

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -O2
    )
endif()

set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/third_party/cjson/cJSON.c"
    PROPERTIES
    COMPILE_OPTIONS "-Wno-misleading-indentation;-Wno-parentheses;-Wno-implicit-fallthrough;-Wno-unused-function"
)

# /****************************** 平台配置 ******************************/

# platform/wifi/wpa_ctrl_unix.c 是x86与ARM共用的控制接口实现。
# ARM不再链接SDK的libwpa_client.a，避免同一API存在两套实现。
if(ARCHITECTURE STREQUAL "arm")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PRODUCT_RELEASE
    )
elseif(ARCHITECTURE STREQUAL "x86")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        DEBUG_MODE
    )
endif()

# /****************************** 配置信息 ******************************/

message(STATUS "-----------------------------------------------")
message(STATUS "Project:      ${PROJECT_NAME}")
message(STATUS "Version:      ${PROJECT_VERSION}")
message(STATUS "Architecture: ${ARCHITECTURE}")
message(STATUS "Compiler:     ${CMAKE_C_COMPILER}")
message(STATUS "Output:       ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "-----------------------------------------------")
