# 龙芯 2K0300（loongarch64）交叉编译工具链文件
#
# 用法：
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/loongarch64.toolchain.cmake
# 或者直接用：
#   ./build.sh --cross
#
# 工具链位置用环境变量 LS2K0300_TOOLCHAIN 覆盖，默认 /opt/ls_2k0300_env/...
# （工具链为什么不放在仓库里，见 tools/README.md）

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR loongarch64)

if(DEFINED ENV{LS2K0300_TOOLCHAIN})
    set(LS2K0300_TOOLCHAIN "$ENV{LS2K0300_TOOLCHAIN}")
else()
    set(LS2K0300_TOOLCHAIN
        "/opt/ls_2k0300_env/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.6")
endif()

if(NOT EXISTS "${LS2K0300_TOOLCHAIN}/bin/loongarch64-linux-gnu-gcc")
    message(FATAL_ERROR
        "找不到龙芯交叉工具链：${LS2K0300_TOOLCHAIN}\n"
        "用环境变量 LS2K0300_TOOLCHAIN 指向工具链根目录即可。")
endif()

set(CMAKE_C_COMPILER   "${LS2K0300_TOOLCHAIN}/bin/loongarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${LS2K0300_TOOLCHAIN}/bin/loongarch64-linux-gnu-g++")

# 只在工具链自带的 sysroot 里找头文件和库，不要跑到宿主机（x86_64）上去找。
# 这几个变量必须在第一次 project() 之前生效，所以只能写在 toolchain file 里 ——
# 写在 CMakeLists.txt 里 project() 之后是不生效的。
set(CMAKE_FIND_ROOT_PATH "${LS2K0300_TOOLCHAIN}/loongarch64-linux-gnu/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
