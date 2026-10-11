cmake_minimum_required(VERSION 3.10)

# 需要 C 语言：SQLite 的 amalgamation 是 C 源码，交给 C 编译器编。
project(Edgehub C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 头文件搜索路径
include_directories(${CMAKE_SOURCE_DIR}/inc)
# SQLite amalgamation 的头文件（见 tools/README.md）
include_directories(${CMAKE_SOURCE_DIR}/tools/sqlite)

add_executable(Edgehub
    main.cpp
    src/CRC.cpp
    src/TcpFrame.cpp
    src/Ringbuffer.cpp
    src/Tcp.cpp
    src/Epoll.cpp
    src/Storage.cpp
    src/Message.cpp
    src/Can.cpp
    src/Gateruntime.cpp
    src/Http.cpp
    src/HttpHandle.cpp
    src/OTA.cpp
    # SQLite 直接编进可执行文件，不用 find_package(SQLite3)：
    # 龙芯交叉工具链的 sysroot 里没有 sqlite3，而这样本地和交叉用的是同一份代码。
    tools/sqlite/sqlite3.c
)

# SQLite 用到 dlopen（扩展加载）和数学函数
target_link_libraries(Edgehub PRIVATE pthread dl m)
