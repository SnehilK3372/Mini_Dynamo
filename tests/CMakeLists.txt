# GoogleTest: prefer a system install, otherwise fetch a pinned release. The
# fetch keeps CI hermetic and lets a fresh checkout build tests with no extra
# setup.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

set(TEST_SOURCES
    base64_test.cpp
    vector_clock_test.cpp
    versioned_value_test.cpp
    router_test.cpp
    coordinator_test.cpp
)

# The RocksDB engine test only makes sense when the durable engine is compiled.
if(HAVE_ROCKSDB)
    list(APPEND TEST_SOURCES rocksdb_storage_test.cpp)
endif()

add_executable(kvstore_tests ${TEST_SOURCES})
target_link_libraries(kvstore_tests PRIVATE kv_core GTest::gtest_main Threads::Threads)

if(HAVE_ROCKSDB)
    target_compile_definitions(kvstore_tests PRIVATE HAVE_ROCKSDB)
    target_sources(kvstore_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/storage/rocksdb_storage.cpp)
    if(RocksDB_FOUND)
        target_link_libraries(kvstore_tests PRIVATE RocksDB::rocksdb)
    else()
        target_include_directories(kvstore_tests PRIVATE ${ROCKSDB_INCLUDE})
        target_link_libraries(kvstore_tests PRIVATE ${ROCKSDB_LIB})
    endif()
endif()

include(GoogleTest)
gtest_discover_tests(kvstore_tests)
