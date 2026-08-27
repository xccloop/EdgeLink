cmake_minimum_required(VERSION 3.10)
project(Edgehub CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 告诉编译器头文件在哪（对应 -I 参数）
include_directories(${CMAKE_SOURCE_DIR}/inc)

# 列出所有 .cpp 文件（不是 .hpp）
add_executable(Edgehub
    main.cpp
    src/Can.cpp
    src/Ringbuffer.cpp
    src/Tcp.cpp
    src/Epoll.cpp
)