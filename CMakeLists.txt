cmake_minimum_required(VERSION 3.20)

project(AsyncLogger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 1. 构建日志库
add_library(async_logger STATIC
        src/logger.cpp
        src/async_logger.cpp
)

target_include_directories(async_logger PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 2. 构建演示程序
add_executable(logger_demo
        examples/basic.cpp
)

target_link_libraries(logger_demo PRIVATE
        async_logger
)

add_executable(async_logger_demo
        examples/async_basic.cpp
)

target_link_libraries(async_logger_demo PRIVATE
        async_logger
)

# 3. 构建基准测试
add_executable(tinylog_single_producer_bench
        benchmarks/single_producer.cpp
)

target_link_libraries(tinylog_single_producer_bench PRIVATE
        async_logger
)

find_package(Threads REQUIRED)

add_executable(tinylog_multi_producer_bench
        benchmarks/multi_producer.cpp
)

target_link_libraries(tinylog_multi_producer_bench PRIVATE
        async_logger
        Threads::Threads
)

add_executable(tinylog_backpressure_bench
        benchmarks/backpressure_analysis.cpp
)

target_link_libraries(tinylog_backpressure_bench PRIVATE
        async_logger
        Threads::Threads
)

# 4. 配置测试
include(CTest)

if(BUILD_TESTING)
    add_executable(logger_test
            tests/logger_test.cpp
    )

    target_link_libraries(logger_test PRIVATE
            async_logger
    )

    add_test(NAME logger_basic COMMAND logger_test)

    add_executable(async_logger_test
            tests/async_logger_test.cpp
    )

    target_link_libraries(async_logger_test PRIVATE
            async_logger
    )

    add_test(NAME async_logger_basic COMMAND async_logger_test)

    add_executable(async_flush_failure_test
            tests/async_flush_failure_test.cpp
            tests/async_logger_test.cpp
            src/async_logger.cpp
    )

    target_include_directories(async_flush_failure_test PRIVATE include tests)
    target_compile_definitions(async_flush_failure_test PRIVATE TINYLOG_FLUSH_FAILURE_TEST)

    add_test(NAME async_flush_failure_rejects_log COMMAND async_flush_failure_test rejects_log)
    add_test(NAME async_flush_failure_releases_waiters COMMAND async_flush_failure_test releases_waiters)
    add_test(NAME releases_same_batch_flush_failure COMMAND async_flush_failure_test releases_same_batch_flush_failure)
    add_test(NAME releases_same_batch_write_failure COMMAND async_flush_failure_test releases_same_batch_write_failure)
    add_test(NAME async_logger_backpressure COMMAND async_flush_failure_test backpressure)
    set_tests_properties(async_flush_failure_rejects_log async_flush_failure_releases_waiters
            releases_same_batch_flush_failure releases_same_batch_write_failure async_logger_backpressure
            PROPERTIES TIMEOUT 10
    )
endif()
