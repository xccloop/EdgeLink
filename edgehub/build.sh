#!/usr/bin/env bash
# 编译 + 运行（编译失败则不运行）
set -e

cd "$(dirname "$0")"

# 1. 配置
cmake -B build

# 2. 编译
cmake --build build -j"$(nproc)"

# can0 不在这里配：接口已经在 up 状态时不能在线改波特率，ip 会报
# "RTNETLINK answers: Device or resource busy" 让 set -e 掐断脚本；
# 而为了绕开它去 down/up，又会在每次编译运行时把总线断开一下。
# 开机配一次就够了，需要时自己跑：
#   sudo ip link set can0 up type can bitrate 500000

# 3. 前台运行。Ctrl+C 会结束服务；Ctrl+Z 只会暂停进程且仍占用 8888 端口。
echo "EdgeHub starts in foreground. Press Ctrl+C to stop it; do not use Ctrl+Z."
exec ./build/Edgehub
