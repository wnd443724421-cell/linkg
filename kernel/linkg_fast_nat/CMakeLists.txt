# LinkG Fast NAT内核模块通过Linux Kbuild构建。

find_program(LINKG_KERNEL_MAKE NAMES make gmake REQUIRED)
# CMake仅负责传递目标内核目录、架构和交叉编译器，并复制最终.ko。

if(NOT ARCHITECTURE STREQUAL "arm")
    return()
endif()

set(LINKG_FAST_NAT_MODULE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(DEFINED ENV{LICHEE_KERN_DIR} AND NOT "$ENV{LICHEE_KERN_DIR}" STREQUAL "")
    set(_LINKG_DEFAULT_KERNEL_DIR "$ENV{LICHEE_KERN_DIR}")
elseif(DEFINED ENV{LICHEE_TOP_DIR} AND NOT "$ENV{LICHEE_TOP_DIR}" STREQUAL "")
    set(_LINKG_DEFAULT_KERNEL_DIR "$ENV{LICHEE_TOP_DIR}/kernel/linux-5.4")
else()
    set(_LINKG_DEFAULT_KERNEL_DIR "")
endif()

set(LINKG_KERNEL_DIR
    "${_LINKG_DEFAULT_KERNEL_DIR}"
    CACHE PATH
    "Linux kernel build tree used to build linkg_fast_nat.ko"
)

if(DEFINED ENV{LINKG_KERNEL_CROSS_COMPILE} AND
   NOT "$ENV{LINKG_KERNEL_CROSS_COMPILE}" STREQUAL "")
    set(_LINKG_DEFAULT_CROSS_COMPILE "$ENV{LINKG_KERNEL_CROSS_COMPILE}")
else()
    get_filename_component(_LINKG_COMPILER_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_LINKG_COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)

    string(REGEX REPLACE "gcc$" "" _LINKG_COMPILER_PREFIX "${_LINKG_COMPILER_NAME}")

    set(_LINKG_DEFAULT_CROSS_COMPILE
        "${_LINKG_COMPILER_DIR}/${_LINKG_COMPILER_PREFIX}"
    )
endif()

set(LINKG_KERNEL_CROSS_COMPILE
    "${_LINKG_DEFAULT_CROSS_COMPILE}"
    CACHE STRING
    "Kernel CROSS_COMPILE prefix"
)

set(LINKG_FAST_NAT_OUTPUT_DIR
    "${CMAKE_SOURCE_DIR}/output/kernel/${ARCHITECTURE}"
)

if(LINKG_KERNEL_DIR STREQUAL "")
    message(FATAL_ERROR
        "LINKG_KERNEL_DIR is empty. Set LICHEE_KERN_DIR or pass "
        "-DLINKG_KERNEL_DIR=/path/to/linux-5.4"
    )
endif()

if(NOT EXISTS "${LINKG_KERNEL_DIR}/Makefile")
    message(FATAL_ERROR
        "Invalid LINKG_KERNEL_DIR=${LINKG_KERNEL_DIR}; Linux kernel Makefile not found"
    )
endif()

add_custom_target(linkg_fast_nat ALL
    COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${LINKG_FAST_NAT_OUTPUT_DIR}"

    COMMAND
        "${LINKG_KERNEL_MAKE}"
        -C "${LINKG_KERNEL_DIR}"
        M="${LINKG_FAST_NAT_MODULE_DIR}"
        ARCH=arm
        CROSS_COMPILE="${LINKG_KERNEL_CROSS_COMPILE}"
        modules

    COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "${LINKG_FAST_NAT_MODULE_DIR}/linkg_fast_nat.ko"
        "${LINKG_FAST_NAT_OUTPUT_DIR}/linkg_fast_nat.ko"

    WORKING_DIRECTORY
        "${LINKG_FAST_NAT_MODULE_DIR}"

    BYPRODUCTS
        "${LINKG_FAST_NAT_MODULE_DIR}/linkg_fast_nat.ko"

    COMMENT
        "Building LinkG Fast NAT kernel module"

    VERBATIM
)

add_custom_target(linkg_fast_nat_clean
    COMMAND
        "${LINKG_KERNEL_MAKE}"
        -C "${LINKG_KERNEL_DIR}"
        M="${LINKG_FAST_NAT_MODULE_DIR}"
        ARCH=arm
        CROSS_COMPILE="${LINKG_KERNEL_CROSS_COMPILE}"
        clean

    COMMAND
        "${CMAKE_COMMAND}" -E rm -f
        "${LINKG_FAST_NAT_OUTPUT_DIR}/linkg_fast_nat.ko"

    WORKING_DIRECTORY
        "${LINKG_FAST_NAT_MODULE_DIR}"

    COMMENT
        "Cleaning LinkG Fast NAT kernel module"

    VERBATIM
)

message(STATUS "Fast NAT kernel: ${LINKG_KERNEL_DIR}")
message(STATUS "Fast NAT cross:  ${LINKG_KERNEL_CROSS_COMPILE}")
message(STATUS "Fast NAT output: ${LINKG_FAST_NAT_OUTPUT_DIR}/linkg_fast_nat.ko")
