# EdgeHub 移植到龙芯 2K0300：从零到 CAN 打通

> 记录时间：2026-10-11
> 目标板：逐飞 seekfree 2K0300 核心板（loongarch64），`root@192.168.1.6`
> 结果：EdgeHub 在板子上跑通 CAN + SQLite + TCP + HTTP

---

## 0. 一页纸流程

```
WSL (x86_64)                              板子 (loongarch64)
──────────────────────────────────────    ──────────────────────────
[1] 装交叉工具链  → /opt/ls_2k0300_env/
[2] 编 edgehub    → build/Edgehub ──scp──→ /home/root/Edgehub
[3] 编内核(开CAN) → vmlinuz                 ──scp──→ /boot/vmlinuz_can
                                              改 /boot/boot.cfg（default 指向它）
                                              重启生效
[4] 编 cansetup   → cansetup                ──scp──→ /usr/bin/cansetup
                                              开机由 /etc/init.d/S97_cansetup 调用
```

三条产物线：**应用**（edgehub 本身）、**内核**（提供 can0 驱动）、**工具**（cansetup 配波特率）。

---

## 1. 环境准备

### 1.1 交叉工具链

工具链在 SDK 里：`LS2K0300_Library-master/tools/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.6`

装到 **WSL 原生盘**（不要放 `/mnt/c`，原因见第 8 节）：

```bash
mkdir -p /opt/ls_2k0300_env
cp -a <工具链目录> /opt/ls_2k0300_env/
```

版本核对：

```bash
/opt/ls_2k0300_env/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.6/bin/loongarch64-linux-gnu-g++ --version
# → loongarch64-linux-gnu-g++ (LoongArch GNU toolchain rc1.6 (20241115)) 8.3.0
```

**这套工具链为什么合适**：它就是编出板子 rootfs 的同一套。板子上 `ldd --version` 报的
`LoongArch GNU toolchain rc1.6 (20241115)) 2.28` 跟它完全一致，`libstdc++.so.6.0.25`
也同名同版本。所以**动态链接可以直接用，不需要 `-static`**。

### 1.2 内核编译依赖

```bash
apt-get install -y bison flex bc libssl-dev libelf-dev
```

`bison` 缺失会导致 Kconfig 解析器编不出来（`zconf.tab.c` 失败）。

### 1.3 SDK 过 Windows 的三个坑 ⚠️

SDK 是从 Windows 解压的，**Linux 文件系统语义丢了**。这三条如果不先知会浪费很多时间：

| 丢的东西 | 报错现象 | 修法 |
|---|---|---|
| **exec 位** | `scripts/config: Permission denied` | `chmod -R a+x scripts` |
| **符号链接** | `dt-bindings/interrupt-controller/irq.h: No such file` | 重建 `scripts/dtc/include-prefixes/` 下的链接 |
| — | `bison: not found` | 见 1.2 |

**符号链接这条特别值得记**：`D:\` 上那份源头就已经是 0 个符号链接了——说明往 Windows
解压时就丢了。内核树的 `scripts/dtc/include-prefixes/` 目录**是空的**，里面本应是
`dt-bindings -> ../../../include/dt-bindings` 这类链接。

重建（`fix_symlinks` 一次性）：

```bash
cd <内核树>/scripts/dtc/include-prefixes
ln -sfn ../../../include/dt-bindings dt-bindings
# 其余架构的按需补：arm/ arm64/ mips/ powerpc/ ...
```

LoongArch 的设备树只用到 `dt-bindings/`：

```bash
grep -rh '#include' arch/loongarch/boot/dts/ | grep -oE '<[^>]+>' | sort -u
# <dt-bindings/gpio/gpio.h>
# <dt-bindings/interrupt-controller/irq.h>
# <dt-bindings/pinctrl/ls2k300-pinfunc.h>
# <dt-bindings/thermal/thermal.h>
```

---

## 2. 内核侧：把 CAN 打开

### 2.1 根因

板子原来的 `.config`：

```
CONFIG_CAN=y                      ← CAN 核心编了（所以 dmesg 有 "can core"）
# CONFIG_CAN_RAW is not set       ← SocketCAN raw，EdgeHub 的 socket() 依赖它
# CONFIG_CAN_DEV is not set       ← 总闸！
# CONFIG_CAN_PEAK_USB is not set
```

`drivers/net/can/Kconfig` 里的依赖结构：

```
50:  config CAN_DEV        tristate，default y      ← 总闸
58:  if CAN_DEV            ← 从这行起
171:     source "drivers/net/can/usb/Kconfig"       ← USB CAN 驱动在这区间内
184: endmenu               ← 到这行止
```

**`CAN_DEV` 一关，这 126 行整段被排除** → 片内 CAN、vcan、slcan、peak_usb、gs_usb 一个都不存在。
再叠加 `CAN_RAW is not set`，就算有驱动，EdgeHub 的 `socket(PF_CAN, SOCK_RAW, CAN_RAW)` 照样建不起来。

### 2.2 为什么必须全编成内置 `=y`

```
CONFIG_MODVERSIONS=y
```

开了符号 CRC 校验。"只编 3 个 `.ko` 丢上去 modprobe"这条路走不通——模块的 CRC 要和内核的
`Module.symvers` 对上，而改配置本身可能改变核心符号的 CRC。

**所以全编 `=y`，只需要部署一个 `vmlinuz`，一个模块都不用传。**

### 2.3 改配置

```bash
cd <内核树>
cp .config .config.orig                    # 备份
scripts/config --file .config \
    --enable CAN_RAW \
    --enable CAN_DEV \
    --enable CAN_PEAK_USB \
    --enable CAN_VCAN                      # 虚拟 CAN，白送的保险
make ARCH=loongarch CROSS_COMPILE=loongarch64-linux-gnu- olddefconfig
```

核对（`diff .config.orig .config` 应当**只有 CAN 相关行**）：

```
CONFIG_CAN=y
CONFIG_CAN_RAW=y
CONFIG_CAN_VCAN=y
CONFIG_CAN_DEV=y
CONFIG_CAN_CALC_BITTIMING=y
CONFIG_CAN_PEAK_USB=y
```

### 2.4 编译

内核树自带 `build.sh`，命令是：

```bash
export PATH=/opt/ls_2k0300_env/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.6/bin:$PATH
make ARCH=loongarch CROSS_COMPILE=loongarch64-linux-gnu- -j$(nproc)
```

- 产物：**源码树根目录**的 `vmlinuz`（还有未压缩的 `vmlinux`，用来验证符号）
- 16 线程约 **2 分 20 秒**
- `CONFIG_KERNEL_LZMA=y`，所以 `vmlinuz` 是个内部 LZMA 压缩的自解压 ELF —— **不能直接 `strings` 找符号，要看 `vmlinux`**

### 2.5 部署前的静态验证

```bash
nm vmlinux | grep -icE 'pcan_usb|peak_usb'          # 89，驱动符号在
strings vmlinux | grep -i 'PCAN-USB'                # 驱动字符串在
```

---

## 3. 应用侧：edgehub 交叉编译适配

### 3.1 `inc/Can.hpp` 缺 `<cstdint>`

```diff
  #pragma once
+ #include <cstdint>
  #include <linux/can.h>
```

**为什么原来在树莓派上能编过**：老 GCC 的 `<string>` / `<vector>` 会"顺带"把 `<cstdint>`
包含进来，所以 `uint32_t` 白用。**GCC 13 之后 libstdc++ 清理了这种捎带行为**，就露出来了。

验证机制：

```cpp
#include <string>
uint32_t x;      // GCC 15 报错：'uint32_t' does not name a type
```

> ⚠️ 反过来说：逐飞的工具链是 **GCC 8.3**（老版本，还带捎带行为），所以**交叉编译时反而可能编得过**。
> 同一份代码在 WSL 本地编不过、交叉编却编过，这种"编译器替我兜底"的状态最危险。加那一行是唯一稳定的做法。

### 3.2 数据库路径硬编码

`src/Gateruntime.cpp` 原来是：

```cpp
_storage.open("/home/qxc/Desktop/EdgeLink/edgehub/data/edgehub.db")   // 树莓派的路径！
```

改成相对路径：

```cpp
// 文件顶部
static const char *const kDbPath = "data/edgehub.db";

// init() 里
if(_storage.open(kDbPath) == false)
{
    printf("Failed to open SQLite database: %s\n", kDbPath);   // 用 %s，打印和实际不会再分叉
    return false;
}
```

配套：部署时 `mkdir -p /home/root/data`，运行时 `cd /home/root`。

### 3.3 SQLite：用 amalgamation，不用 `-lsqlite3`

工具链的 sysroot 里**没有 sqlite3**（`sqlite3.h` / `libsqlite3*` 都没有）。

| 方案 | 评价 |
|---|---|
| **A. amalgamation（采用）** | 自包含；不用往板子部署 `.so`；本地和交叉用同一份代码 |
| B. 交叉编静态库 | 多一个构建步骤 |
| C. 交叉编共享库 | 多一个部署物 + `LD_LIBRARY_PATH` |

从 sqlite.org 下 `sqlite-amalgamation-*.zip`，把 `sqlite3.c` / `sqlite3.h` 放进
`edgehub/tools/sqlite/`。

### 3.4 CMake 交叉配置

**不要**照抄 SDK 里 Nova 的写法——它在 `project()` **之后**才 `set(CMAKE_CXX_COMPILER ...)`，
而 CMake 要求编译器必须在第一次 `project()` 之前指定（它的 `CMakeCache.txt` 里至今记的还是
`/usr/bin/c++`，就是证据）。

用标准做法：独立 toolchain file `edgehub/cmake/loongarch64.toolchain.cmake`

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR loongarch64)

# 工具链路径可用环境变量 LS2K0300_TOOLCHAIN 覆盖
if(DEFINED ENV{LS2K0300_TOOLCHAIN})
    set(LS2K0300_TOOLCHAIN "$ENV{LS2K0300_TOOLCHAIN}")
else()
    set(LS2K0300_TOOLCHAIN "/opt/ls_2k0300_env/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.6")
endif()

set(CMAKE_C_COMPILER   "${LS2K0300_TOOLCHAIN}/bin/loongarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${LS2K0300_TOOLCHAIN}/bin/loongarch64-linux-gnu-g++")

# 只在工具链 sysroot 里找头文件和库
set(CMAKE_FIND_ROOT_PATH "${LS2K0300_TOOLCHAIN}/loongarch64-linux-gnu/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

### 3.5 `CMakeLists.txt`

```cmake
project(Edgehub C CXX)          # 需要 C：sqlite3.c 是 C 源码

include_directories(${CMAKE_SOURCE_DIR}/inc)
include_directories(${CMAKE_SOURCE_DIR}/tools/sqlite)

add_executable(Edgehub
    main.cpp
    src/*.cpp 展开...
    tools/sqlite/sqlite3.c      # 直接编进来，不用 find_package
)

target_link_libraries(Edgehub PRIVATE pthread dl m)
```

### 3.6 `build.sh`

```bash
./build.sh                              # 交叉编译（默认）
./build.sh --native                     # 原生编译 x86_64（验证能否编过 / 跑测试）
./build.sh --deploy 192.168.1.6         # 交叉编译 + scp
./build.sh --deploy 192.168.1.6 --run   # 交叉编译 + scp + 板上后台运行
```

---

## 4. 部署到板子：PMON 引导菜单

### 4.1 板子的引导结构

- 引导器是 **PMON2000**（不是 U-Boot）
- 内核和 `boot.cfg` 都在 **eMMC 的 rootfs 里**（`/boot/` 就是个普通目录，`/dev/mmcblk0p1`）
- **不用烧录、不用拔卡**——把新内核 scp 进 `/boot/` 再改 `boot.cfg` 就行

`/boot/boot.cfg` 格式：

```
timeout 15          ← 倒计时秒数
default 1           ← 默认菜单项，0 起数（0 = 第一个 title）
showmenu 1

title <菜单显示名>
        kernel (emmc0,0)/boot/<内核文件名>
        args console=tty console=ttyS0,115200 rdinit=/sbin/init rootdelay=3 root=/dev/mmcblk0p1
```

菜单实际显示是 **1 起数**，且：

```
Use the UP and DOWN keys to select the entry.
Press ENTER to boot selected OS.
Press 'c' to command-line.
```

### 4.2 安全的部署策略

**`default` 步步不动，新内核先当"手动可选项"**，验证通过再改默认：

1. `cp /boot/vmlinuz /boot/vmlinuz.stable`（备份）
2. `cp /boot/boot.cfg /boot/boot.cfg.orig`
3. scp 新内核 → `/boot/vmlinuz_can`
4. `boot.cfg` 加一条 title，`timeout` 3 → 15，**`default 0` 不动**
5. 重启 → 串口选第 2 项 → 验证
6. 验证通过后再把 `default` 改成 1

这样每一步都有保命退路：万一按键时机不对或新内核有问题，等 15 秒它自己进旧内核。

### 4.3 用串口驱动 PMON 菜单

串口 **COM12 @ 115200**。Windows 侧用 PowerShell 的 `System.IO.Ports.SerialPort`：

```powershell
$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = "COM12"; $sp.BaudRate = 115200
$sp.Parity = [System.IO.Ports.Parity]::None; $sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Open()
# ... ReadExisting() 循环读 ...
$sp.Write("`e[B`r")      # 下箭头 + 回车
```

**两个坑**：
- ⚠️ `.ps1` 脚本必须**纯 ASCII**——PS 5.1 按 ANSI 读文件，中文注释会被读乱导致语法错误
- ⚠️ 用无参构造 + 逐属性赋值。`New-Object System.IO.Ports.SerialPort $Port,$Baud,[System.IO.Ports.Parity]::None,...`
  在 `Start-Job` 里枚举引用不会解析

### 4.4 通过 ssh 往板子写文件

板子只有 **BusyBox `sh`**，没有 `bash`、没有 `pkill`、没有 `bc`。写文件用 base64 最稳：

```powershell
$b64 = (Get-Content local.b64 -Raw).Trim()
ssh root@<IP> "echo '$b64' | base64 -d > /目标路径/file"
```

避免引号地狱和 CRLF 污染。**文件必须是 LF 行尾**。

---

## 5. can0 配置：`cansetup`

### 5.1 为什么需要自己写

真实 CAN 控制器 **必须先设波特率才能 up**，否则：

```
ip: SIOCSIFFLAGS: Invalid argument
```

正常 Linux 上是 `ip link set can0 type can bitrate 500000`，但：

- 板子的 `ip` 是 **BusyBox 版**，不支持 `type can`
- `/sys/class/net/can0/` 下**没有 `can_bittiming`**，没有 sysfs 后门
- 板上**没有编译器**，也没有 iproute2

所以只能自己用 rtnetlink 写一个。

### 5.2 `edgehub/tools/cansetup/cansetup.c`

```
cansetup can0 500000
```

做两件事（对应上面那条 ip 命令的等价物）：

1. `RTM_NEWLINK` + `IFLA_INFO_DATA { IFLA_CAN_BITTIMING { bitrate } }` —— 设波特率
2. `RTM_NEWLINK` + `ifi_flags = IFF_UP` —— 启动接口

交叉编译：

```bash
loongarch64-linux-gnu-gcc -O2 -Wall -Wextra -o cansetup cansetup.c
# 21KB，只依赖 libc
```

### 5.3 ⚠️ 最大的一个坑：`IFLA_LINKINFO`

**`IFLA_INFO_KIND` / `IFLA_INFO_DATA` 必须包在 `IFLA_LINKINFO` 里面**，它们是它的子属性，
不是顶层的：

```
正确：                                第一版写的（错）：
nlmsghdr                             nlmsghdr
  ifinfomsg                            ifinfomsg
  IFLA_LINKINFO (nested)               IFLA_INFO_KIND = "can"      ← 顶层
    IFLA_INFO_KIND = "can"             IFLA_INFO_DATA (nested)     ← 顶层
    IFLA_INFO_DATA (nested)              IFLA_CAN_BITTIMING
      IFLA_CAN_BITTIMING
```

放顶层时，`IFLA_INFO_DATA` 的枚举值刚好是 **2 = `IFLA_ADDRESS`**，内核 netlink 校验层直接
返回 **`-ERANGE`**，而且 **dmesg 一片安静**（因为根本没走到波特率计算）。

**定位手法**（值得记住）：厂家在内核 `drivers/net/can/dev.c:229` 加了一句 `pr_info`，
打印计算出的时序和时钟频率。**dmesg 里没有这行 = 没走到 `can_calc_bittiming`** →
说明是更早的 netlink 校验层拒的，而不是驱动算不出时序。

对比：真的走进去之后 dmesg 是这样的——

```
[261.614385] bt->sjw=1,bt->brp=1,bt->bitrate=500000,priv->clock.freq=8000000,...
[261.626002] peak_usb 3-1:1.0 can0: setting BTR0=0x00 BTR1=0x1c
[261.658854] can0: link becomes ready
```

### 5.4 驱动侧确认

```
[2.465129] usbcore: registered new interface driver peak_usb
[6.331472] peak_usb 3-1:1.0: PEAK-System PCAN-USB adapter hwrev 84 serial FFFFFFFF (1 channel)
[6.951678] peak_usb 3-1:1.0 can0: attached to PCAN-USB channel 0 (device 255)
```

适配器 `0c72:000c`（XCAN-USB，PEAK PCAN-USB 兼容）。驱动设备表
`pcan_usb_core.c:38` 里有 `{USB_DEVICE(PCAN_USB_VENDOR_ID, PCAN_USB_PRODUCT_ID)}`，
且 `bcdDevice` **只被读成硬件版本号、不做匹配过滤**，所以克隆设备也能绑上。

---

## 6. 开机自动配置 can0

板子是 **sysvinit**，`/etc/init.d/rcS` 按字母序执行 `/etc/init.d/S??*`（每个脚本以 `start` 为参数调用）。

新建 `/etc/init.d/S97_cansetup`（排在 `S96_wifi_start` 之后）：

```sh
#!/bin/sh
CAN_IF=can0
CAN_BITRATE=500000

# USB 枚举 + 驱动绑定可能比 init 脚本晚，等最多 20 秒
i=0
while [ $i -lt 20 ]; do
    [ -d "/sys/class/net/$CAN_IF" ] && break
    sleep 1
    i=$((i + 1))
done

[ -d "/sys/class/net/$CAN_IF" ] || { echo "cansetup: $CAN_IF 没出现，跳过"; exit 0; }
[ -x /usr/bin/cansetup ]       || { echo "cansetup: 工具不存在，跳过"; exit 0; }

/usr/bin/cansetup "$CAN_IF" "$CAN_BITRATE"
```

```bash
chmod +x /etc/init.d/S97_cansetup
```

---

## 7. 验证

### 7.1 静态验证（部署前）

```bash
nm vmlinux | grep -icE 'pcan_usb|peak_usb'      # 89
diff .config.orig .config                        # 只应有 CAN 相关差异
```

### 7.2 板上验证

```bash
uname -a                         # 应显示 "root@<你的WSL主机名> ... #1"
ip link show can0                # 应为 UP,LOWER_UP
dmesg | grep -iE 'peak_usb|BTR'
```

### 7.3 EdgeHub 端到端

```bash
cd /home/root && mkdir -p data && ./Edgehub &
netstat -tln | grep -E '8888|8080'      # 两个端口都在监听
curl http://127.0.0.1:8080/hub/status   # {"running":true}
ls -l /home/root/data/edgehub.db        # 16384 字节，建库成功
```

EdgeHub 的路由（`src/HttpHandle.cpp`）：

```
/hub/status
/hub/firmware/query/node/<n>
/hub/firmware/send/node/<n>
```

---

## 8. 为什么工具链不能放进仓库

`edgehub/tools/` 只放**跟着仓库走的第三方源码**（如 SQLite amalgamation）。

**龙芯工具链放在 `/opt/ls_2k0300_env/`**，理由：

| 事实 | 后果 |
|---|---|
| 工具链 **304MB / 4038 个文件** | 放仓库会让每次 clone 都拖着 |
| 有 **61 个符号链接**，且是必需的 | `sysroot/lib → usr/lib`、`libstdc++.so → libstdc++.so.6.0.25` 等 |
| 仓库在 `/mnt/c`，是 **v9fs** 文件系统 | 权限位是假的；**符号链接保存不住** |

链接一断，编译器的 sysroot 查找就崩，交叉编译直接不能用。**这跟内核源码过 Windows 时
丢符号链接是同一个问题。**

`/opt/ls_2k0300_env/` 这个路径跟逐飞官方 `buildroot-2405/build.sh` 里的约定一致。

---

## 9. 踩坑速查表

| # | 坑 | 现象 | 修法 |
|---|---|---|---|
| 1 | SDK 过 Windows 丢 exec 位 | `scripts/config: Permission denied` | `chmod -R a+x scripts` |
| 2 | SDK 过 Windows 丢符号链接 | `dt-bindings/.../irq.h: No such file` | 重建 `scripts/dtc/include-prefixes/` |
| 3 | 缺 bison | `zconf.tab.c` 编译失败 | `apt install bison flex bc` |
| 4 | 大目录从 `/mnt/d` 拷进 WSL 极慢 | 30MB/分钟，992MB 要 33 分钟 | **Windows 侧 `tar -cf` 打成一个文件，WSL 顺序读，18 秒** |
| 5 | GCC 13+ 不再捎带 `<cstdint>` | `'uint32_t' has not been declared` | 显式 `#include <cstdint>` |
| 6 | `IFLA_LINKINFO` 少一层 | 设波特率报 `-ERANGE`，**dmesg 无输出** | `IFLA_INFO_KIND`/`IFLA_INFO_DATA` 必须在 `IFLA_LINKINFO` 内 |
| 7 | BusyBox `ip` 不支持 CAN | `SIOCSIFFLAGS: Invalid argument` | 用 `cansetup`（rtnetlink） |
| 8 | BusyBox 没有 `bash`/`pkill`/`pgrep` | `sh: bash: not found` | 用 `sh -s` + `kill <PID>` |
| 9 | PS 5.1 按 ANSI 读 `.ps1` | 脚本中文注释导致语法错误 | 串口脚本纯 ASCII |
| 10 | PowerShell 拆带空格的参数 | `cp: missing destination file operand` | 把命令写进脚本文件再执行 |
| 11 | 脚本带 BOM 传给 `sh -s` | `sh: echo: not found` | `.TrimStart([char]0xFEFF)` |
| 12 | **`printf` 全缓冲，程序不退出就看不到** | 板上跑 EdgeHub，日志文件 **0 字节**（但程序在跑） | `setvbuf(stdout, NULL, _IONBF, 0)`（板子没有 `stdbuf`） |
| 13 | 覆盖正在运行的可执行文件 | `scp: dest open "...": Failure`（ETXTBSY） | **先停旧进程，再 scp** |
| 14 | 用 `ps \| grep <名字>` 找/杀进程 | 匹配到 ssh 自己发过去的命令行，**把自己杀掉** | 扫 `/proc/*/cmdline`，或记 PID 文件 |
| 15 | 重复传 `CMAKE_TOOLCHAIN_FILE` | `CMake Warning: ... not used by the project` | 只在**首次配置**时传（看着像"交叉没生效"，其实只是噪声） |
| 16 | 同一个 build 目录换编译器 | CMake 报编译器变更错误 | 清空 `build/` 重建（`build.sh` 已自动做） |

---

## 9.1 第 12 条展开：那个"没有输出"的坑

最容易误判的一个。表象是"程序在板上跑但什么都不打印"，很容易怀疑是编译/部署出错，**其实是 C 运行库的缓冲**：

```
printf("...")
  → 写进 stdio 缓冲区，不是直接写文件
  → stdout 连文件/管道时默认【全缓冲】，攒够 4096 字节才真正写出去
  → 程序进入 epoll 事件循环，永不退出
  → 缓冲区永远不满 → 永远不落盘
```

**只有程序退出时（`return`/`exit()`）才强制冲刷。** 所以现象很反直觉：

> init 失败时会看到报错（因为后面 `return` 了），init 成功时反而一片安静。

两个常见误解：

- ❌ "加个 `\n` 就好了" —— 全缓冲**不看换行符**，那是行缓冲（仅连终端时）的规则
- ❌ "用 Nova 的 `stdbuf -o0 -e0`" —— **这块板子的 BusyBox 里没有 `stdbuf`**

只能程序内部解决：`setvbuf(stdout, NULL, _IONBF, 0)`，放在 `main()` 最开头、任何 `printf` 之前。

---

## 10. 常用命令

```bash
# ---- WSL：编 edgehub ----
cd /mnt/c/Users/memory/Desktop/EdgeLink/edgehub
./build.sh                               # 交叉编译
./build.sh --deploy 192.168.1.6 --run    # 编译 + 部署 + 运行

# ---- WSL：编内核 ----
cd /root/ls2k0300/linux-4.19-202506
export PATH=/opt/ls_2k0300_env/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.6/bin:$PATH
make ARCH=loongarch CROSS_COMPILE=loongarch64-linux-gnu- -j$(nproc)

# ---- 部署内核 ----
scp vmlinuz root@192.168.1.6:/boot/vmlinuz_can
ssh root@192.168.1.6 "sync"

# ---- 板上 ----
can0 状态:        ip link show can0
手动配 can0:      cansetup can0 500000
CAN 驱动日志:     dmesg | grep -iE 'peak_usb|BTR|can0'
跑 edgehub:       cd /home/root && ./Edgehub
查端口:           netstat -tln
```

---

## 11. 待办

- [ ] `cansetup` 接进 `build.sh`，`--deploy` 时顺带传过去
- [ ] `timeout 15` 可以按需调小（它是给启动菜单留的反应时间）
- [ ] 片内 CAN（4 路 `can@0x1611xxxx`）仍然没启用：DT 里是 `status="disabled"`，
      需要内核开 `CONFIG_CAN_LSCANFD` + `CONFIG_CAN_LSCANFD_PLATFORM` 并改设备树
- [ ] `/boot/vmlinuz.stable`、`/boot/boot.cfg.orig` 是本次的备份，确认稳定后可清理

---

## 12. 本次为 LS2K0300 适配 edgehub，具体改了哪些东西

### 12.1 修改已有文件

| 文件 | 改了什么 | 为什么 |
|---|---|---|
| `inc/Can.hpp` | 加 `#include <cstdint>` | GCC 13+ 不再从 `<string>`/`<vector>` 捎带 `<cstdint>`，`uint32_t` 直接找不到。树莓派上的老 GCC 一直替我们兜着 |
| `main.cpp` | 加 `#include <csignal>` / `<cstdio>`；开头加 `setvbuf(stdout, NULL, _IONBF, 0)`；启动横幅改成 `printf("EdgeHub starting...\n")` | ① 显式依赖，不再靠传递包含碰巧能编；② 见第 9.1 节——stdout 重定向到文件时全缓冲，程序不退出日志就永远出不来 |
| `src/Gateruntime.cpp` | 数据库路径从硬编码 `/home/qxc/Desktop/EdgeLink/edgehub/data/edgehub.db` 改成文件顶部常量 `static const char *const kDbPath = "data/edgehub.db"`；错误打印改用 `%s` 带上实际路径 | 那是**树莓派的绝对路径**，板子上不存在。改相对路径后本地和板上同一份代码。原 `printf` 里写的还是 `Mini_Edgehub`（复制粘贴留下的错），用 `%s` 后不会再分叉 |
| `CMakeLists.txt` | `project(Edgehub C CXX)`（原来只有 `CXX`）；去掉 `find_package(SQLite3 REQUIRED)`，改成把 `tools/sqlite/sqlite3.c` 直接编进目标；链接改成 `pthread dl m` | 龙芯交叉工具链的 sysroot 里**没有 sqlite3**（头文件和库都没有） |
| `build.sh` | 整体重写（见 12.3） | 支持交叉编译 + 一键部署 |
| `.gitignore` | 去掉 `/build-loongarch/`（现在只有一个 `build/`） | |

### 12.2 新增文件

| 路径 | 作用 |
|---|---|
| `cmake/loongarch64.toolchain.cmake` | 标准的 CMake 交叉工具链文件。**注意不要照抄 Nova 的写法**——它在 `project()` **之后**才 `set(CMAKE_CXX_COMPILER ...)`，按 CMake 规范这是不生效的（Nova 的 `CMakeCache.txt` 里至今记的还是 `/usr/bin/c++`，就是证据） |
| `tools/README.md` | 说明这个目录放什么、以及**工具链为什么不放在仓库里**（v9fs 存不住符号链接，见第 8 节） |
| `tools/sqlite/sqlite3.c`、`sqlite3.h` | SQLite 3.46.1 amalgamation（9.1MB + 644KB）。选它而不是 `-lsqlite3`，是为了让本地编译和交叉编译用同一份代码，且不用往板子部署 `.so` |
| `tools/cansetup/cansetup.c` | 板子上的 CAN 配置工具。板子的 `ip` 是 BusyBox 版、不支持 `type can`，也没有 sysfs 后门，所以用 rtnetlink 自己实现 |

### 12.3 `build.sh` 的行为

```
./build.sh                              交叉编译（默认，目标是板子）
./build.sh --native                     原生编译 x86_64（验证能否编过）
./build.sh --deploy <IP>                交叉编译 + scp + chmod + mkdir data/
./build.sh --deploy <IP> --run          再前台运行，输出实时回到本终端
./build.sh --deploy <IP> --bg           再后台运行，日志写板上 /tmp/Edgehub.log
./build.sh --logs <IP>                  实时跟随板上日志（不编译不传输）
```

内部几个关键点：

1. **单一 `build/` 目录**，用 `build/.cmake-mode` 记住它是给哪套编译器配的；模式一变就清空重建
2. **只在首次配置时传 toolchain file**，避免重复配置时那句 `CMAKE_TOOLCHAIN_FILE not used` 的误导性警告
3. **部署前先停旧进程**（Linux 不允许覆盖正在运行的可执行文件，ETXTBSY）
4. **停止进程用「PID 文件 + 扫 `/proc/*/cmdline`」双保险**，不用 `ps | grep`——那会匹配到 ssh 自己发过去的命令行，把自己杀掉

### 12.4 板上环境配套改动（不在仓库里）

| 位置 | 内容 |
|---|---|
| `/boot/vmlinuz_can` | 重编的内核（CAN 全套内置） |
| `/boot/boot.cfg` | 加了一条 "seekfree_2k300 CAN" 菜单项，`default` 改指它，`timeout` 3 → 15 |
| `/usr/bin/cansetup` | 上面的工具，交叉编译后 scp 过去 |
| `/etc/init.d/S97_cansetup` | 开机自动配置 can0（sysvinit，`rcS` 按字母序执行） |
| `/home/root/Edgehub` | 部署的可执行文件 |
| `/home/root/data/edgehub.db` | SQLite 数据库（eMMC，重启不丢） |

### 12.5 一句话总结

**改动集中在三类**：

1. **可移植性**——去掉写死的路径、补上显式的头文件包含（`<cstdint>`、`<csignal>`、`<cstdio>`）
2. **构建系统**——SQLite 改为源码内嵌，新增交叉工具链文件，`build.sh` 支持一键交叉编译+部署
3. **运行时可见性**——关掉 stdout 缓冲，让日志在板上真实可见

业务逻辑（协议解析、epoll、存储、HTTP、OTA）**一行没动**——这次纯粹是移植适配。
