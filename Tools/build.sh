#!/bin/bash

set -e

TOOLS_DIR=$(cd "$(dirname "$0")"; pwd)
PROJECT_ROOT=$(cd "$TOOLS_DIR/.."; pwd)

BUILD_DIR="${TOOLS_DIR}/build_tmp"
OUTPUT_DIR="${TOOLS_DIR}/output"
TOOLCHAIN_FILE="${PROJECT_ROOT}/toolchains/arm.cmake"
DEFAULT_ARM_TOOLCHAIN="/home/dawn/t113_tina5.0_v1.2/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain"

function clean_all()
{
    echo "正在清理 LinkG Tools 编译产物..."

    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
        echo "已移除: $BUILD_DIR"
    fi

    if [ -d "$OUTPUT_DIR" ]; then
        rm -rf "$OUTPUT_DIR"
        echo "已移除: $OUTPUT_DIR"
    fi

    echo "清理完成。"
}

function build_all()
{
    echo "==============================================="
    echo "开始编译 LinkG Tools..."
    echo "工具链: $TOOLCHAIN_FILE"

    export STAGING_DIR="${DEFAULT_ARM_TOOLCHAIN}"

    echo "STAGING_DIR: $STAGING_DIR"

    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    mkdir -p "$OUTPUT_DIR"

    cmake -S "$TOOLS_DIR" \
          -B "$BUILD_DIR" \
          -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
          -DCMAKE_BUILD_TYPE=Release

    cmake --build "$BUILD_DIR" --parallel "$(nproc)"

    echo "==============================================="
    echo "编译成功！"
    echo "输出位于: $OUTPUT_DIR"

    find "$OUTPUT_DIR" -maxdepth 2 -type f -executable -print

    echo "正在自动清理中间文件: $BUILD_DIR"
    rm -rf "$BUILD_DIR"

    echo "清理完成，仅保留输出目录: $OUTPUT_DIR"
}

case "$1" in
    clean)
        clean_all
        ;;
    *)
        build_all
        ;;
esac
