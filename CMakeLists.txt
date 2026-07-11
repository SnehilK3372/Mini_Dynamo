cmake_minimum_required(VERSION 3.14)  # FetchContent + modern GoogleTest support
project(mini_dynamo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# --- kv_core: pure coordination/versioning logic ---------------------------
# No POSIX sockets, no RocksDB, so it builds on any C++17 compiler (including the
# Windows/MSYS2 dev box) and is what the unit tests link against.
add_library(kv_core STATIC
    src/base64.cpp
    src/vector_clock.cpp
    src/versioned_value.cpp
    src/router.cpp
    src/message.cpp
    src/coordinator.cpp
    src/storage/in_memory_storage.cpp
)
target_include_directories(kv_core PUBLIC src)
target_link_libraries(kv_core PUBLIC Threads::Threads)

# --- Optional RocksDB detection --------------------------------------------
# Debian's librocksdb-dev ships no CMake package config, so fall back to a plain
# library+header probe. HAVE_ROCKSDB gates the durable engine and the node
# binary's default storage.
find_package(RocksDB QUIET CONFIG)
if(RocksDB_FOUND)
    set(HAVE_ROCKSDB TRUE)
else()
    find_library(ROCKSDB_LIB rocksdb)
    find_path(ROCKSDB_INCLUDE rocksdb/db.h)
    if(ROCKSDB_LIB AND ROCKSDB_INCLUDE)
        set(HAVE_ROCKSDB TRUE)
    endif()
endif()

# --- kvstore: the node binary (POSIX sockets; built on Linux/Docker) --------
if(UNIX)
    set(NODE_SOURCES
        src/main.cpp
        src/node.cpp
        src/net/tcp_server.cpp
        src/net/tcp_client.cpp
        src/net/framing.cpp
        src/net/tcp_replica_client.cpp
    )
    if(HAVE_ROCKSDB)
        list(APPEND NODE_SOURCES src/storage/rocksdb_storage.cpp)
    endif()

    add_executable(kvstore ${NODE_SOURCES})
    target_link_libraries(kvstore PRIVATE kv_core Threads::Threads)

    if(HAVE_ROCKSDB)
        target_compile_definitions(kvstore PRIVATE HAVE_ROCKSDB)
        if(RocksDB_FOUND)
            target_link_libraries(kvstore PRIVATE RocksDB::rocksdb)
        else()
            target_include_directories(kvstore PRIVATE ${ROCKSDB_INCLUDE})
            target_link_libraries(kvstore PRIVATE ${ROCKSDB_LIB})
        endif()
    endif()
endif()

# --- Tests ------------------------------------------------------------------
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
