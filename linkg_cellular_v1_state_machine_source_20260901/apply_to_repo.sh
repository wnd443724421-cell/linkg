#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="${1:-}"
FORCE="${2:-}"

if [[ -z "$TARGET_DIR" ]]; then
    echo "usage: $0 <linkg-repository> [--force]" >&2
    exit 2
fi

TARGET_DIR="$(cd "$TARGET_DIR" && pwd)"

if [[ ! -f "$TARGET_DIR/CMakeLists.txt" || ! -d "$TARGET_DIR/modules/cellular" ]]; then
    echo "target is not a LinkG source repository: $TARGET_DIR" >&2
    exit 2
fi

required_paths=(
    "modules/cellular/cellular_status.h"
    "modules/cellular/cellular_status.c"
    "modules/cellular/linkg_cellular.c"
    "platform/cellular/at/at_channel.h"
    "platform/cellular/rg255/rg255_cmd.h"
    "platform/cellular/rg255/rg255_query.h"
)

for path in "${required_paths[@]}"; do
    if [[ ! -e "$TARGET_DIR/$path" ]]; then
        echo "required baseline path is missing: $path" >&2
        exit 1
    fi
done

production_paths=(
    "modules/cellular/cellular_runtime.h"
    "modules/cellular/cellular_runtime.c"
    "modules/cellular/cellular_monitor.h"
    "modules/cellular/cellular_monitor.c"
    "modules/cellular/linkg_cellular.c"
    "platform/cellular/rg255/rg255_runtime_urc.h"
    "platform/cellular/rg255/rg255_runtime_urc.c"
)

if [[ "$FORCE" != "--force" ]] && command -v git >/dev/null 2>&1 && git -C "$TARGET_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if ! git -C "$TARGET_DIR" diff --quiet -- modules/cellular/linkg_cellular.c ||
       ! git -C "$TARGET_DIR" diff --cached --quiet -- modules/cellular/linkg_cellular.c; then
        echo "local changes detected in modules/cellular/linkg_cellular.c; use --force only after reviewing them" >&2
        exit 1
    fi
fi

for path in "${production_paths[@]}"; do
    source_path="$SCRIPT_DIR/files/$path"
    target_path="$TARGET_DIR/$path"

    if [[ ! -f "$source_path" ]]; then
        echo "package source is missing: $path" >&2
        exit 1
    fi

    if [[ -e "$target_path" && "$FORCE" != "--force" ]] && ! cmp -s "$source_path" "$target_path"; then
        if [[ "$path" != "modules/cellular/linkg_cellular.c" ]]; then
            echo "target already contains a different file: $path" >&2
            echo "review it first or rerun with --force" >&2
            exit 1
        fi
    fi

done

for path in "${production_paths[@]}"; do
    mkdir -p "$(dirname "$TARGET_DIR/$path")"
    cp "$SCRIPT_DIR/files/$path" "$TARGET_DIR/$path"
done

echo "Cellular V1 runtime state machine applied to: $TARGET_DIR"
echo "No Git add/commit/push operation was performed."
