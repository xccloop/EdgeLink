cmake_minimum_required(VERSION 3.10)
project(Edgehub CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 通过根目录头文件夹，对应 -I 参数
include_directories(${CMAKE_SOURCE_DIR}/inc)

# 列出需要编译的 .cpp 文件，不需要列出 .hpp。
add_executable(Edgehub
    main.cpp
    src/CRC.cpp
    src/Frame.cpp
    src/Ringbuffer.cpp
    src/Tcp.cpp
    src/Epoll.cpp
)

#修复：协议解析属于纯内存逻辑，增加一个独立测试目标验证半包、粘包、错位恢复和Ringbuffer回绕
enable_testing()
add_executable(frame_parser_tests
    tests/frame_parser_test.cpp
    src/CRC.cpp
    src/Frame.cpp
    src/Ringbuffer.cpp
)
add_test(NAME frame_parser_tests COMMAND frame_parser_tests)
