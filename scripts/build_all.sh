#!/usr/bin/env bash
# build_all.sh —— 编译全部用户态实验，并做一次最小冒烟
set -euo pipefail
cd "$(dirname "$0")/.."

for t in gcc make; do
    command -v "$t" >/dev/null || { echo "缺少 $t，先 apt install build-essential"; exit 1; }
done

echo "编译（$(nproc) 并发）..."
make -j"$(nproc)"

echo
echo "冒烟：raw_syscall 前 12 行"
./build/01_syscall/01_raw_syscall | head -12

echo
echo "可执行文件："
make -s list
