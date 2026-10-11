# tools/ —— 第三方依赖

这个目录放**跟着仓库走**的第三方东西：源码形式、跟平台无关、没有二进制产物。

## sqlite/ —— SQLite amalgamation

| | |
|---|---|
| 版本 | 3.46.1 |
| 来源 | https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip |
| 文件 | `sqlite3.c`（9.1MB）、`sqlite3.h`（644KB） |

**为什么用 amalgamation 源码，而不是 `-lsqlite3`：**

本机 WSL 装了 `libsqlite3-dev`，所以原生编译用 `-lsqlite3` 是能过的。但**龙芯交叉工具链的
sysroot 里没有 sqlite3**（头文件和库都没有），交叉编译时 `#include <sqlite3.h>` 直接报
`No such file or directory`。

用源码自己编，可以一举解决三件事：

1. 本地编译和交叉编译**用同一份代码**，不再依赖宿主机装没装 `libsqlite3-dev`
2. 不用往开发板部署 `.so`，也不用设 `LD_LIBRARY_PATH`
3. 版本锁死在仓库里，换机器不会因为系统 sqlite 版本不同而行为不一致

更新方法：从 sqlite.org 下载新的 amalgamation，替换这两个文件即可。

## 龙芯交叉工具链**不在**这个目录

工具链装在 WSL 原生盘上：

```
/opt/ls_2k0300_env/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.6
```

**为什么不放进来：** 这套工具链 304MB / 4038 个文件，里面有 **61 个符号链接**，而且都是必需的：

```
sysroot/lib    -> usr/lib
sysroot/lib64  -> usr/lib64
sysroot/usr/lib64/libstdc++.so -> libstdc++.so.6.0.25
.../libgcc_eh.a -> libgcc.a
```

而本仓库放在 `/mnt/c`（Windows 盘）上，那是 **v9fs** 文件系统 —— 权限位是假的
（`ls -l` 出来全是 777），**符号链接保存不住**。链接一断，编译器的 sysroot 查找就崩，
交叉编译直接不能用。

所以工具链必须待在 WSL 原生盘。`/opt/ls_2k0300_env/` 这个路径跟逐飞官方
`buildroot-2405/build.sh` 里的约定一致：

```bash
export PATH=/opt/ls_2k0300_env/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.6/bin:$PATH
```

`build.sh` 通过环境变量 `LS2K0300_TOOLCHAIN` 定位它，默认值就是上面那个路径。
换机器的话，把工具链解压到任意位置，然后 `export LS2K0300_TOOLCHAIN=<你的路径>` 即可。

**这套工具链为什么合适：** 它就是编出板子那个 rootfs 的同一套 ——
编译器版本串 `LoongArch GNU toolchain rc1.6 (20241115)`、glibc 2.28、
`libstdc++.so.6.0.25`，跟板子 `/lib/` 里的完全对得上。所以**动态链接可以直接用**，
不需要 `-static`。
