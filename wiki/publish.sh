#!/usr/bin/env bash

set -euo pipefail

if (( $# > 1 )); then
    printf '用法: %s [提交说明]\n' "$0" >&2
    exit 2
fi

wiki_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir="$wiki_dir/products"
remote='git@gitee.com:amovlab1/amov-linkg-wiki.git'
branch='master'
message="${1:-更新 LinkG Wiki}"

if [[ ! -d "$source_dir" ]]; then
    printf '找不到 Wiki 内容目录: %s\n' "$source_dir" >&2
    exit 1
fi

publish_dir=$(mktemp -d "${TMPDIR:-/tmp}/linkg-wiki-publish.XXXXXXXX")
trap 'rm -rf -- "$publish_dir"' EXIT

git clone --quiet --depth 1 --single-branch --branch "$branch" -- "$remote" "$publish_dir"

# 发布仓库的 products 在根目录，内容以开发仓库的 wiki/products 为准。
mkdir -p -- "$publish_dir/products"
rsync -a --checksum --delete -- "$source_dir/" "$publish_dir/products/"
git -C "$publish_dir" add -A -- products

if git -C "$publish_dir" diff --cached --quiet; then
    printf 'Wiki 内容没有变化。\n'
    exit 0
fi

git -C "$publish_dir" diff --cached --stat
git -C "$publish_dir" commit -m "$message"
git -C "$publish_dir" push origin "HEAD:refs/heads/$branch"

printf 'Wiki 已推送: %s\n' "$(git -C "$publish_dir" rev-parse --short HEAD)"
