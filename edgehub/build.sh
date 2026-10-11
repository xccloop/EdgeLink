#!/usr/bin/env bash
# EdgeHub 构建脚本
#
# 用法：
#   ./build.sh                        交叉编译（默认，目标是龙芯 2K0300）
#   ./build.sh --native               原生编译（本机 x86_64，用来验证能否编过）
#   ./build.sh --deploy <板子IP>      交叉编译 + scp 到开发板
#   ./build.sh --deploy <IP> --run    交叉编译 + scp + 前台运行（输出实时回到本终端）
#   ./build.sh --deploy <IP> --bg     交叉编译 + scp + 后台运行（日志写板上文件）
#   ./build.sh --logs <IP>            实时跟随板上的日志（不编译、不传输）
#   ./build.sh --native --run         原生编译 + 本机前台运行
#   ./build.sh -h                     显示这段帮助
#
# 工具链位置：环境变量 LS2K0300_TOOLCHAIN，
#             默认 /opt/ls_2k0300_env/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.6
#             （工具链为什么不放在仓库里，见 tools/README.md）

set -euo pipefail

cd "$(dirname "$0")"

# ============================== 配置 ==============================
PROG="Edgehub"                 # 可执行文件名
BOARD_USER="root"              # 开发板登录用户
BOARD_DIR="/home/root"         # 开发板上的目标目录
BOARD_LOG="/tmp/${PROG}.log"   # 开发板上的日志（后台运行时的输出）
BOARD_PID="/tmp/${PROG}.pid"   # 记录后台进程 PID：开发板是 BusyBox，没有 pgrep/pkill
SSH_OPTS=(-o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)

# ============================== 参数 ==============================
MODE="cross"           # cross | native
DEPLOY_IP=""
RUN_MODE=""            # "" | fg | bg
FOLLOW_LOGS=false

usage() {
    cat <<'EOF'
用法：
  ./build.sh                        交叉编译（默认，目标是龙芯 2K0300）
  ./build.sh --native               原生编译（本机 x86_64，用来验证能否编过）
  ./build.sh --deploy <板子IP>      交叉编译 + scp 到开发板
  ./build.sh --deploy <IP> --run    交叉编译 + scp + 前台运行（输出实时回到本终端）
  ./build.sh --deploy <IP> --bg     交叉编译 + scp + 后台运行（日志写板上文件）
  ./build.sh --logs <IP>            实时跟随板上的日志（不编译、不传输）
  ./build.sh --native --run         原生编译 + 本机前台运行
  ./build.sh -h                     显示这段帮助
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --native)  MODE="native" ;;
        --cross)   MODE="cross" ;;
        --deploy)  shift; DEPLOY_IP="${1:-}" ;;
        --run)     RUN_MODE="fg" ;;
        --bg)      RUN_MODE="bg" ;;
        --logs)    shift; DEPLOY_IP="${1:-}"; FOLLOW_LOGS=true ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数：$1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

# ============================== 只跟随日志 ==============================
# 放在最前面：不编译、不传输，纯看日志
if [ "$FOLLOW_LOGS" = true ]; then
    if [ -z "$DEPLOY_IP" ]; then
        echo "错误：--logs 需要开发板 IP，例如 ./build.sh --logs 192.168.1.6" >&2
        exit 1
    fi
    echo "==> 实时跟随 ${DEPLOY_IP}:${BOARD_LOG}（Ctrl+C 只退出跟随，不影响板上程序）"
    exec ssh "${SSH_OPTS[@]}" "${BOARD_USER}@${DEPLOY_IP}" "tail -f '${BOARD_LOG}'"
fi

if [ -n "$DEPLOY_IP" ] && [ "$MODE" = "native" ]; then
    echo "错误：--deploy 只能跟交叉编译一起用（x86_64 的二进制在龙芯板子上跑不起来）。" >&2
    exit 1
fi

# ============================== 编译 ==============================
BUILD_DIR="build"
MODE_MARKER="${BUILD_DIR}/.cmake-mode"   # 记录这个 build/ 是给哪套编译器配的

# CMake 把"用哪个编译器"缓存在 build/ 里，而且 toolchain file 只在首次配置时生效。
# 所以只在【目录不存在】或【编译模式变了】的时候才重新配置；
# 模式变了必须先清空，否则 CMake 会因为编译器换了而报错。
# （改了 CMakeLists.txt 不用管，cmake --build 会自动触发重新配置。）
NEED_CONFIGURE=true
if [ -d "$BUILD_DIR" ]; then
    if [ "$(cat "$MODE_MARKER" 2>/dev/null || true)" = "$MODE" ]; then
        NEED_CONFIGURE=false
    else
        echo "==> 编译模式变了（$(cat "$MODE_MARKER" 2>/dev/null || echo '未知') -> ${MODE}），清空 ${BUILD_DIR}/ 重建"
        rm -rf "$BUILD_DIR"
    fi
fi

if [ "$NEED_CONFIGURE" = true ]; then
    if [ "$MODE" = "cross" ]; then
        echo "==> 配置：交叉编译 loongarch64"
        cmake -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE=cmake/loongarch64.toolchain.cmake
    else
        echo "==> 配置：原生编译 x86_64"
        cmake -B "$BUILD_DIR"
    fi
    echo "$MODE" > "$MODE_MARKER"
fi

echo "==> 编译 -> ${BUILD_DIR}/"
cmake --build "$BUILD_DIR" -j"$(nproc)"
BIN="${BUILD_DIR}/${PROG}"

if [ "$MODE" = "cross" ]; then
    echo "==> 产物："
    file "$BIN"
fi

# ============================== 本机前台运行 ==============================
if [ "$MODE" = "native" ] && [ "$RUN_MODE" = "fg" ]; then
    echo "==> 前台运行。Ctrl+C 停止；不要用 Ctrl+Z（会暂停进程，但仍占着 8888 端口）。"
    exec "./$BIN"
fi

# ============================== 部署 ==============================
if [ -n "$DEPLOY_IP" ]; then
    # 必须"先停旧进程、再上传"：Linux 不允许覆盖正在执行的可执行文件，
    # 否则 scp 会报 dest open: Failure（ETXTBSY）。
    # 开发板只有 BusyBox，没有 pkill/pgrep，所以用 PID 文件记录。
    echo "==> 停掉开发板上正在运行的旧进程（如果有）"
    ssh "${SSH_OPTS[@]}" "${BOARD_USER}@${DEPLOY_IP}" "
        # 1) 先看 PID 文件（--bg 启动时会写）
        if [ -f '${BOARD_PID}' ]; then
            kill \"\$(cat '${BOARD_PID}')\" 2>/dev/null
            rm -f '${BOARD_PID}'
        fi
        # 2) 再扫 /proc 兜底：抓那些没有 PID 文件的进程
        #    （比如前台跑完 ssh 断开、进程却留下的情况）
        #    不能用 ps|grep —— 会匹配到 ssh 自己发过去的这条命令行，把自己杀掉。
        for d in /proc/[0-9]*; do
            [ -r \"\$d/cmdline\" ] || continue
            cmd=\$(tr '\\0' ' ' < \"\$d/cmdline\" 2>/dev/null)
            case \"\$cmd\" in
                './${PROG}'*) kill \"\${d#/proc/}\" 2>/dev/null ;;
            esac
        done
    "
    sleep 1

    echo "==> 传输到 ${BOARD_USER}@${DEPLOY_IP}:${BOARD_DIR}/"
    scp "${SSH_OPTS[@]}" "$BIN" "${BOARD_USER}@${DEPLOY_IP}:${BOARD_DIR}/"
    # 加可执行位，并确保 data/ 存在（程序要在里面开 sqlite 数据库）
    ssh "${SSH_OPTS[@]}" "${BOARD_USER}@${DEPLOY_IP}" \
        "chmod +x '${BOARD_DIR}/${PROG}'; mkdir -p '${BOARD_DIR}/data'"
    echo "==> 传输完成"
fi

# ============================== 远程前台运行 ==============================
if [ -n "$DEPLOY_IP" ] && [ "$RUN_MODE" = "fg" ]; then
    echo "==> 在开发板上前台运行，输出直接回到本终端。Ctrl+C 停止。"
    echo "    ------------------------------------------------------------"
    # -t 分配伪终端：远端 stdout 变成 tty，Ctrl+C 也能传到远端的进程
    exec ssh -t "${SSH_OPTS[@]}" "${BOARD_USER}@${DEPLOY_IP}" \
        "cd '${BOARD_DIR}' && ./${PROG}"
fi

# ============================== 远程后台运行 ==============================
if [ -n "$DEPLOY_IP" ] && [ "$RUN_MODE" = "bg" ]; then
    echo "==> 在开发板上后台运行，日志写到 ${BOARD_LOG}"
    ssh "${SSH_OPTS[@]}" "${BOARD_USER}@${DEPLOY_IP}" "
        cd '${BOARD_DIR}'
        nohup ./${PROG} > '${BOARD_LOG}' 2>&1 < /dev/null &
        echo \$! > '${BOARD_PID}'
        sleep 2
        printf -- '--- %s ---\n' '${BOARD_LOG}'
        cat '${BOARD_LOG}'
        printf -- '--- PID ---\n'
        PID=\$(cat '${BOARD_PID}')
        if kill -0 \"\$PID\" 2>/dev/null; then
            echo \"运行中，PID \$PID\"
            echo \"看日志：./build.sh --logs ${DEPLOY_IP}\"
        else
            echo '(未在运行 —— 启动失败)'
        fi
    "
fi
