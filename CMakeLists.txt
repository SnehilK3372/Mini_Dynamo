cmake_minimum_required(VERSION 3.10)
project(distributed_kv_store)

set(CMAKE_CXX_STANDARD 17)

set(SOURCES
    src/main.cpp
    src/node.cpp
    src/router.cpp
    src/net/tcp_server.cpp
    src/net/tcp_client.cpp
    src/message.cpp
    src/storage/in_memory_storage.cpp
)

include_directories(src)

add_executable(kvstore ${SOURCES})
