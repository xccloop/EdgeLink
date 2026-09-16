cmake_minimum_required(VERSION 3.10)
project(Edgehub CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 SQLite3 开发包（如果系统未安装，会报错）
find_package(SQLite3 REQUIRED)

# 通过根目录头文件夹，对应 -I 参数
include_directories(${CMAKE_SOURCE_DIR}/inc)

# 列出需要编译的 .cpp 文件，不需要列出 .hpp。
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
)

# 为 Edgehub 链接 SQLite 库
target_link_libraries(Edgehub PRIVATE SQLite::SQLite3)