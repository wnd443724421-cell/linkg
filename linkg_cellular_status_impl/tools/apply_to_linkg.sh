#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <linkg-repo-root>" >&2
    exit 2
fi

repo_root=$(cd "$1" && pwd)
package_root=$(cd "$(dirname "$0")/.." && pwd)
expected_head="1aa7ea9d526d3ace09ccebe972cae39cd0d08b70"

if ! git -C "$repo_root" rev-parse --git-dir >/dev/null 2>&1; then
    echo "target is not a Git working tree: $repo_root" >&2
    exit 1
fi

actual_head=$(git -C "$repo_root" rev-parse HEAD)
if [[ "$actual_head" != "$expected_head" ]]; then
    echo "baseline mismatch" >&2
    echo "  expected: $expected_head" >&2
    echo "  actual:   $actual_head" >&2
    exit 1
fi

check_blob()
{
    local path="$1"
    local expected="$2"
    local actual

    if [[ ! -f "$repo_root/$path" ]]; then
        echo "missing baseline file: $path" >&2
        exit 1
    fi

    actual=$(git -C "$repo_root" hash-object "$path")
    if [[ "$actual" != "$expected" ]]; then
        echo "baseline file changed: $path" >&2
        echo "  expected blob: $expected" >&2
        echo "  actual blob:   $actual" >&2
        exit 1
    fi
}

check_blob "modules/cellular/cellular_status.c" "334e10ae22e29ca41f153fc2efa06ba74ea8791d"
check_blob "modules/cellular/cellular_status.h" "908500bac6403f8198f06d422b45fb57162eb8c1"
check_blob "platform/cellular/rg255/rg255_query.c" "6e903a0fde6badc8749a9e7f4e18fea933a231ce"
check_blob "platform/cellular/rg255/rg255_query.h" "97aa7943f245cff284a638405b3c6a801254f695"
check_blob "include/linkg/modules/cellular/linkg_cellular_status.h" "d07e5d8d11b43f49e03a66da312b26209320cccf"

if [[ -f "$repo_root/test/cellular/rg255_query_test.c" ]]; then
    check_blob "test/cellular/rg255_query_test.c" "1b575802e823cdd9fb361ce4402a9ce94adc2336"
fi

if [[ -f "$repo_root/test/cellular/rg255_query_fixture_test.c" ]]; then
    check_blob "test/cellular/rg255_query_fixture_test.c" "3250fe882c8a841569fe11ffdae2f8e476401e3d"
fi

(
    cd "$repo_root"
    git apply --check "$package_root/patches/rg255_query.c.patch"

    if [[ -f test/cellular/rg255_query_test.c ]]; then
        git apply --check "$package_root/patches/rg255_query_test.c.patch"
    fi

    if [[ -f test/cellular/rg255_query_fixture_test.c ]]; then
        git apply --check "$package_root/patches/rg255_query_fixture_test.c.patch"
    fi
)

cp "$package_root/modules/cellular/cellular_status.c" "$repo_root/modules/cellular/cellular_status.c"
cp "$package_root/modules/cellular/cellular_status.h" "$repo_root/modules/cellular/cellular_status.h"
cp "$package_root/platform/cellular/rg255/rg255_query.h" "$repo_root/platform/cellular/rg255/rg255_query.h"
cp "$package_root/include/linkg/modules/cellular/linkg_cellular_status.h" "$repo_root/include/linkg/modules/cellular/linkg_cellular_status.h"

(
    cd "$repo_root"
    git apply "$package_root/patches/rg255_query.c.patch"

    if [[ -f test/cellular/rg255_query_test.c ]]; then
        git apply "$package_root/patches/rg255_query_test.c.patch"
    fi

    if [[ -f test/cellular/rg255_query_fixture_test.c ]]; then
        git apply "$package_root/patches/rg255_query_fixture_test.c.patch"
    fi
)

echo "Cellular Status source applied. No commit or push was performed."
