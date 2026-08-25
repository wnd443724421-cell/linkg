set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# T113 目标为 32 位 ARMv7，构建架构名称统一使用 arm。
if(DEFINED ENV{T113_TOOLCHAIN_DIR} AND NOT "$ENV{T113_TOOLCHAIN_DIR}" STREQUAL "")
    set(TOOLCHAIN_DIR "$ENV{T113_TOOLCHAIN_DIR}")
elseif(DEFINED ENV{LICHEE_TOP_DIR} AND NOT "$ENV{LICHEE_TOP_DIR}" STREQUAL "")
    set(TOOLCHAIN_DIR "$ENV{LICHEE_TOP_DIR}/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain")
else()
    set(TOOLCHAIN_DIR "/home/dawn/t113_tina5.0_v1.2/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain")
endif()

set(CMAKE_C_COMPILER "${TOOLCHAIN_DIR}/bin/arm-openwrt-linux-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_DIR}/bin/arm-openwrt-linux-g++")
set(CMAKE_SYSROOT "${TOOLCHAIN_DIR}")
set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfloat-abi=hard -mfpu=neon-vfpv4")

set(CMAKE_FIND_ROOT_PATH "${TOOLCHAIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
