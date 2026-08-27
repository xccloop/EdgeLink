#!/usr/bin/env bash
# 编译 + 运行（编译失败则不运行）
set -e

cd "$(dirname "$0")"

# 1. 配置
cmake -B build

# 2. 编译（-j 用全部 CPU 核提升编译速度）
cmake --build build -j"$(nproc)"

# 3. 运行
./build/Edgehub
