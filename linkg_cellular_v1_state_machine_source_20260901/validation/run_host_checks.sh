#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_DIR="$PACKAGE_DIR/files"
BUILD_DIR="${BUILD_DIR:-$(mktemp -d)}"
CC="${CC:-gcc}"

mkdir -p "$BUILD_DIR"

common_flags=(
    -std=gnu17
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -pthread
)

owner_includes=(
    -I"$SCRIPT_DIR/stubs"
    -I"$SOURCE_DIR/modules/cellular"
    -I"$SOURCE_DIR/platform/cellular/rg255"
)

"$CC" "${common_flags[@]}" \
    -I"$SOURCE_DIR/modules/cellular" \
    "$SOURCE_DIR/modules/cellular/cellular_runtime.c" \
    "$SCRIPT_DIR/tests/cellular_runtime_test.c" \
    -o "$BUILD_DIR/cellular_runtime_test"

"$CC" "${common_flags[@]}" \
    -I"$SCRIPT_DIR/monitor_stubs" \
    -I"$SOURCE_DIR/modules/cellular" \
    "$SOURCE_DIR/modules/cellular/cellular_monitor.c" \
    "$SCRIPT_DIR/tests/cellular_monitor_test.c" \
    -o "$BUILD_DIR/cellular_monitor_test"

"$CC" "${common_flags[@]}" \
    -I"$SCRIPT_DIR/stubs" \
    -I"$SOURCE_DIR/platform/cellular/rg255" \
    "$SOURCE_DIR/platform/cellular/rg255/rg255_runtime_urc.c" \
    "$SCRIPT_DIR/tests/rg255_runtime_urc_test.c" \
    -o "$BUILD_DIR/rg255_runtime_urc_test"

for test_name in \
    bootstrap_single_restart_test \
    owner_happy_path_test \
    owner_pin_missing_test \
    owner_pin_once_test \
    owner_sim_removal_test \
    owner_verify_failure_test
do
    "$CC" "${common_flags[@]}" \
        "${owner_includes[@]}" \
        "$SOURCE_DIR/modules/cellular/linkg_cellular.c" \
        "$SOURCE_DIR/modules/cellular/cellular_runtime.c" \
        "$SCRIPT_DIR/tests/${test_name}.c" \
        -o "$BUILD_DIR/$test_name"
done

for binary in "$BUILD_DIR"/*_test
do
    echo "RUN $(basename "$binary")"
    "$binary"
done

echo "All host checks passed."
