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
    src/OtaImageVerify.c
    tools/ota/ota_signature.c
    tools/ota/ota_public_key.c
    tools/ota/crc32.c
    tools/crypto/sha256/sha256.c
    tools/crypto/micro-ecc/uECC.c
    # SQLite 直接编进可执行文件，不用 find_package(SQLite3)：
    # 龙芯交叉工具链的 sysroot 里没有 sqlite3，而这样本地和交叉用的是同一份代码。
    tools/sqlite/sqlite3.c
)

# SQLite 用到 dlopen（扩展加载）和数学函数
target_link_libraries(Edgehub PRIVATE pthread dl m)

# Hub独立保存验签代码和公钥，构建不依赖Node目录；库由C编译器编译。
target_include_directories(Edgehub PRIVATE ${CMAKE_SOURCE_DIR}/tools/ota)
set_source_files_properties(tools/crypto/micro-ecc/uECC.c PROPERTIES
    COMPILE_DEFINITIONS "uECC_SUPPORTS_secp160r1=0;uECC_SUPPORTS_secp192r1=0;uECC_SUPPORTS_secp224r1=0;uECC_SUPPORTS_secp256k1=0;uECC_SUPPORT_COMPRESSED_POINT=0"
)
