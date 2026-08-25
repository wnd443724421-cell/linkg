#!/usr/bin/env bash

set -Eeuo pipefail

readonly COLOR_GREEN='\033[0;32m'
readonly COLOR_RED='\033[0;31m'
readonly COLOR_YELLOW='\033[0;33m'
readonly COLOR_RESET='\033[0m'

readonly PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly BUILD_ROOT="${PROJECT_ROOT}/build"
readonly OUTPUT_ROOT="${PROJECT_ROOT}/output"
readonly SDK_ROOT="${LICHEE_TOP_DIR:-/home/dawn/t113_tina5.0_v1.2}"
readonly DEFAULT_ARM_TOOLCHAIN="${T113_TOOLCHAIN_DIR:-${SDK_ROOT}/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain}"
readonly ARM_COMPILER="${DEFAULT_ARM_TOOLCHAIN}/bin/arm-openwrt-linux-gcc"
readonly ARM_TOOLCHAIN_FILE="${PROJECT_ROOT}/toolchains/arm.cmake"

ARCHITECTURE="arm"
ARCHITECTURE_SPECIFIED=false
CLEAN_BUILD=false

print_info()
{
    printf "${COLOR_GREEN}%s${COLOR_RESET}\n" "$1"
}

print_warn()
{
    printf "${COLOR_YELLOW}%s${COLOR_RESET}\n" "$1"
}

print_error()
{
    printf "${COLOR_RED}%s${COLOR_RESET}\n" "$1" >&2
}

usage()
{
    cat <<EOF
用法:
  $0 [x86|arm] [clean]

默认架构:
  arm

示例:
  $0
  $0 x86
  $0 arm
  $0 clean
  $0 x86 clean
  $0 arm clean
EOF
}

parse_arguments()
{
    local argument

    for argument in "$@"
    do
        case "${argument}" in
            x86|arm)
                ARCHITECTURE="${argument}"
                ARCHITECTURE_SPECIFIED=true
                ;;
            clean)
                CLEAN_BUILD=true
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                print_error "无效参数: ${argument}"
                usage
                exit 1
                ;;
        esac
    done
}

safe_remove_directory()
{
    local target="$1"

    if [[ -z "${target}" || "${target}" == "${PROJECT_ROOT}" || "${target}" != "${PROJECT_ROOT}/"* ]]
    then
        print_error "拒绝清理非项目构建目录: ${target}"
        exit 1
    fi

    rm -rf -- "${target}"
}

clean_build()
{
    if [[ "${ARCHITECTURE_SPECIFIED}" == true ]]
    then
        print_info "正在清理 ${ARCHITECTURE} 构建产物..."

        safe_remove_directory "${BUILD_ROOT}/${ARCHITECTURE}"
        safe_remove_directory "${OUTPUT_ROOT}/bin/${ARCHITECTURE}"
        safe_remove_directory "${OUTPUT_ROOT}/lib/${ARCHITECTURE}"
    else
        print_info "正在清理全部构建产物..."

        safe_remove_directory "${BUILD_ROOT}"
        safe_remove_directory "${OUTPUT_ROOT}"
    fi
}

prepare_arm_environment()
{
    if [[ ! -x "${ARM_COMPILER}" ]]
    then
        print_error "未找到 T113 编译器: ${ARM_COMPILER}"
        print_error "请检查 LICHEE_TOP_DIR 或 T113_TOOLCHAIN_DIR。"
        exit 1
    fi

    if [[ ! -f "${ARM_TOOLCHAIN_FILE}" ]]
    then
        print_error "未找到 CMake 工具链文件: ${ARM_TOOLCHAIN_FILE}"
        exit 1
    fi

    export T113_TOOLCHAIN_DIR="${DEFAULT_ARM_TOOLCHAIN}"
    export STAGING_DIR="${DEFAULT_ARM_TOOLCHAIN}"
    export PATH="${DEFAULT_ARM_TOOLCHAIN}/bin:${PATH}"
}

configure_project()
{
    local build_directory="${BUILD_ROOT}/${ARCHITECTURE}"
    local cmake_arguments=(
        -S "${PROJECT_ROOT}"
        -B "${build_directory}"
        -DARCHITECTURE="${ARCHITECTURE}"
        -DCMAKE_BUILD_TYPE=Release
    )

    if [[ "${ARCHITECTURE}" == "arm" ]]
    then
        prepare_arm_environment
        cmake_arguments+=("-DCMAKE_TOOLCHAIN_FILE=${ARM_TOOLCHAIN_FILE}")
    fi

    print_info "正在配置 ${ARCHITECTURE} 构建环境..."

    cmake "${cmake_arguments[@]}"
}

build_project()
{
    local build_directory="${BUILD_ROOT}/${ARCHITECTURE}"
    local cpu_count

    cpu_count="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"

    print_info "开始编译 ${ARCHITECTURE}..."

    cmake --build "${build_directory}" --parallel "${cpu_count}"

    print_info "编译成功"
    print_info "输出目录: ${OUTPUT_ROOT}/bin/${ARCHITECTURE}"
}

main()
{
    parse_arguments "$@"

    if [[ "${CLEAN_BUILD}" == true ]]
    then
        clean_build
        exit 0
    fi

    configure_project
    build_project
}

main "$@"
