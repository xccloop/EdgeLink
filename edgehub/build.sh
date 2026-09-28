#!/usr/bin/env bash
# 编译 + 运行（编译失败则不运行）
set -e

cd "$(dirname "$0")"

# 1. 配置
cmake -B build

# 2. 编译
cmake --build build -j"$(nproc)"

# 设置 can0 为 500kbps。
# 注意：接口已经在 up 状态时，不能在线改波特率，ip 会直接报
# "RTNETLINK answers: Device or resource busy"。所以先 down 再 up。
# 而且这里失败了也不能让 set -e 掐断脚本——那样 Edgehub 就永远起不来了。
sudo ip link set can0 down 2>/dev/null || true
if ! sudo ip link set can0 up type can bitrate 500000; then
    echo "[警告] can0 没配起来，CAN 收不到数据（不影响 TCP）"
fi

# 3. 前台运行。Ctrl+C 会结束服务；Ctrl+Z 只会暂停进程且仍占用 8888 端口。
echo "EdgeHub starts in foreground. Press Ctrl+C to stop it; do not use Ctrl+Z."
exec ./build/Edgehub
