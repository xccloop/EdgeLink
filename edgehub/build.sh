#!/usr/bin/env bash
# 编译 + 运行（编译失败则不运行）
set -e

cd "$(dirname "$0")"

# 1. 配置
cmake -B build

# 2. 编译
cmake --build build -j"$(nproc)"

# 3. 前台运行。Ctrl+C 会结束服务；Ctrl+Z 只会暂停进程且仍占用 8888 端口。
echo "EdgeHub starts in foreground. Press Ctrl+C to stop it; do not use Ctrl+Z."
exec ./build/Edgehub
