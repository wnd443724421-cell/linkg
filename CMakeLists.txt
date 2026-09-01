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
    "kernel/linkg_fast_nat"
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

# /****************************** ARM内核模块 ******************************/

if(ARCHITECTURE STREQUAL "arm")
    set(LINKG_FAST_NAT_SOURCE_DIR
        "${CMAKE_SOURCE_DIR}/kernel/linkg_fast_nat"
    )

    set(LINKG_FAST_NAT_BUILD_DIR
        "${CMAKE_BINARY_DIR}/kernel/linkg_fast_nat"
    )

    set(LINKG_FAST_NAT_SOURCE
        "${LINKG_FAST_NAT_SOURCE_DIR}/linkg_fast_nat.c"
    )

    set(LINKG_FAST_NAT_UAPI
        "${LINKG_FAST_NAT_SOURCE_DIR}/linkg_fast_nat_uapi.h"
    )

    set(LINKG_FAST_NAT_KBUILD
        "${LINKG_FAST_NAT_SOURCE_DIR}/Kbuild"
    )

    if(DEFINED ENV{LICHEE_TOP_DIR} AND NOT "$ENV{LICHEE_TOP_DIR}" STREQUAL "")
        set(LINKG_TINA_ROOT "$ENV{LICHEE_TOP_DIR}")
    else()
        set(LINKG_TINA_ROOT "/home/dawn/t113_tina5.0_v1.2")
    endif()

    set(LINKG_KERNEL_SOURCE_DIR
        "${LINKG_TINA_ROOT}/kernel/linux-5.4"
        CACHE PATH
        "TinaLinux kernel source directory"
    )

    set(LINKG_KERNEL_BUILD_DIR
        "${LINKG_TINA_ROOT}/out/t113_i/kernel/build"
        CACHE PATH
        "TinaLinux kernel build directory"
    )

    if(NOT EXISTS "${LINKG_FAST_NAT_SOURCE}")
        message(FATAL_ERROR "Fast NAT source not found: ${LINKG_FAST_NAT_SOURCE}")
    endif()

    if(NOT EXISTS "${LINKG_FAST_NAT_UAPI}")
        message(FATAL_ERROR "Fast NAT UAPI not found: ${LINKG_FAST_NAT_UAPI}")
    endif()

    if(NOT EXISTS "${LINKG_FAST_NAT_KBUILD}")
        message(FATAL_ERROR "Fast NAT Kbuild not found: ${LINKG_FAST_NAT_KBUILD}")
    endif()

    if(NOT EXISTS "${LINKG_KERNEL_SOURCE_DIR}/Makefile")
        message(FATAL_ERROR "Kernel source tree invalid: ${LINKG_KERNEL_SOURCE_DIR}")
    endif()

    if(NOT EXISTS "${LINKG_KERNEL_BUILD_DIR}/include/config/auto.conf")
        message(FATAL_ERROR
            "Kernel auto.conf missing: ${LINKG_KERNEL_BUILD_DIR}/include/config/auto.conf"
        )
    endif()

    if(NOT EXISTS "${LINKG_KERNEL_BUILD_DIR}/include/generated/autoconf.h")
        message(FATAL_ERROR
            "Kernel autoconf.h missing: ${LINKG_KERNEL_BUILD_DIR}/include/generated/autoconf.h"
        )
    endif()

    if(NOT EXISTS "${LINKG_KERNEL_BUILD_DIR}/Module.symvers")
        message(FATAL_ERROR
            "Kernel Module.symvers missing: ${LINKG_KERNEL_BUILD_DIR}/Module.symvers"
        )
    endif()

    get_filename_component(
        LINKG_ARM_COMPILER_DIR
        "${CMAKE_C_COMPILER}"
        DIRECTORY
    )

    get_filename_component(
        LINKG_ARM_COMPILER_NAME
        "${CMAKE_C_COMPILER}"
        NAME
    )

    string(
        REGEX REPLACE "gcc$" ""
        LINKG_ARM_COMPILER_PREFIX
        "${LINKG_ARM_COMPILER_NAME}"
    )

    set(LINKG_KERNEL_CROSS_COMPILE
        "${LINKG_ARM_COMPILER_DIR}/${LINKG_ARM_COMPILER_PREFIX}"
    )

    find_program(
        LINKG_KERNEL_MAKE_PROGRAM
        NAMES make gmake
        REQUIRED
    )

    set(LINKG_FAST_NAT_MODULE
        "${LINKG_FAST_NAT_BUILD_DIR}/linkg_fast_nat.ko"
    )

    set(LINKG_FAST_NAT_OUTPUT
        "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/linkg_fast_nat.ko"
    )

    add_custom_command(
        OUTPUT
            "${LINKG_FAST_NAT_OUTPUT}"

        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${LINKG_FAST_NAT_BUILD_DIR}"

        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"

        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "${LINKG_FAST_NAT_SOURCE}"
            "${LINKG_FAST_NAT_BUILD_DIR}/linkg_fast_nat.c"

        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "${LINKG_FAST_NAT_UAPI}"
            "${LINKG_FAST_NAT_BUILD_DIR}/linkg_fast_nat_uapi.h"

        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "${LINKG_FAST_NAT_KBUILD}"
            "${LINKG_FAST_NAT_BUILD_DIR}/Kbuild"

        COMMAND
            "${LINKG_KERNEL_MAKE_PROGRAM}"
            -C "${LINKG_KERNEL_SOURCE_DIR}"
            "O=${LINKG_KERNEL_BUILD_DIR}"
            "M=${LINKG_FAST_NAT_BUILD_DIR}"
            ARCH=arm
            "CROSS_COMPILE=${LINKG_KERNEL_CROSS_COMPILE}"
            modules

        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "${LINKG_FAST_NAT_MODULE}"
            "${LINKG_FAST_NAT_OUTPUT}"

        DEPENDS
            "${LINKG_FAST_NAT_SOURCE}"
            "${LINKG_FAST_NAT_UAPI}"
            "${LINKG_FAST_NAT_KBUILD}"

        COMMENT
            "Building LinkG Fast NAT kernel module"

        VERBATIM
    )

    add_custom_target(linkg_fast_nat ALL
        DEPENDS
            "${LINKG_FAST_NAT_OUTPUT}"
    )
endif()

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

if(ARCHITECTURE STREQUAL "arm")
    message(STATUS "Kernel src:   ${LINKG_KERNEL_SOURCE_DIR}")
    message(STATUS "Kernel build: ${LINKG_KERNEL_BUILD_DIR}")
    message(STATUS "Kernel cross: ${LINKG_KERNEL_CROSS_COMPILE}")
    message(STATUS "Fast NAT:     ${LINKG_FAST_NAT_OUTPUT}")
endif()

message(STATUS "-----------------------------------------------")
